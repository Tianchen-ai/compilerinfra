#include "compilerinfra/Proofs.h"
#include "mlir/Analysis/AliasAnalysis/LocalAliasAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/IntegerRangeAnalysis.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>

using namespace compilerinfra;
using namespace mlir;

namespace {
Operation *findEnclosingProofScope(Operation *op) {
  if (!op)
    return nullptr;
  Operation *cur = op;
  while (cur->getParentOp() && !cur->hasTrait<OpTrait::IsIsolatedFromAbove>())
    cur = cur->getParentOp();
  return cur;
}

Operation *findEnclosingProofScope(Value v) {
  if (!v)
    return nullptr;
  if (Operation *def = v.getDefiningOp())
    return findEnclosingProofScope(def);
  auto barg = llvm::cast<BlockArgument>(v);
  Region *reg = barg.getOwner() ? barg.getOwner()->getParent() : nullptr;
  return findEnclosingProofScope(reg ? reg->getParentOp() : nullptr);
}

bool contextCoversScope(const IntegerRangeContext *ctx, Operation *scope) {
  if (!ctx || !ctx->valid() || !ctx->root() || !scope)
    return false;
  return ctx->root() == scope || ctx->root()->isAncestor(scope);
}

std::optional<int64_t> matchScalarConstInt(Value v) {
  if (!v)
    return std::nullopt;
  if (auto c = v.getDefiningOp<arith::ConstantOp>())
    if (auto ia = llvm::dyn_cast<IntegerAttr>(c.getValue()))
      if (ia.getValue().getSignificantBits() <= 64)
        return ia.getValue().getSExtValue();
  return std::nullopt;
}

ConstantIntRanges makeNonNegRange(unsigned bw, const APInt &lo128,
                                  const APInt &hi128) {
  APInt maxS = APInt::getSignedMaxValue(bw).sext(128);
  APInt maxU = APInt::getMaxValue(bw).zext(128);
  APInt clampedLo = llvm::APIntOps::smax(APInt(128, 0, true), lo128);
  APInt sHi = llvm::APIntOps::smin(hi128, maxS);
  APInt uHi = llvm::APIntOps::umin(hi128, maxU);
  if (clampedLo.ugt(uHi))
    return ConstantIntRanges::maxRange(bw);
  auto uRng = ConstantIntRanges::fromUnsigned(clampedLo.trunc(bw), uHi.trunc(bw));
  if (hi128.sle(maxS) && clampedLo.sle(sHi)) {
    return uRng.intersection(
        ConstantIntRanges::fromSigned(clampedLo.trunc(bw), sHi.trunc(bw)));
  }
  return uRng;
}

std::optional<ConstantIntRanges>
queryValueImpl(const IntegerRangeContext &ctx, Value value,
               const DataFlowSolver *solver, unsigned depth);

std::optional<ConstantIntRanges>
inferSteppedArgRange(unsigned bw, std::optional<int64_t> staticLb, Value dynLb,
                     std::optional<int64_t> staticUb, Value dynUb,
                     std::optional<int64_t> staticStep, Value dynStep,
                     const IntegerRangeContext &ctx,
                     const DataFlowSolver *solver, unsigned depth) {
  if (bw == 0 || bw > 64 || depth > 8)
    return std::nullopt;
  constexpr unsigned W = 128;

  auto boundOf = [&](std::optional<int64_t> cst,
                     Value dyn) -> std::optional<std::pair<APInt, APInt>> {
    if (cst) {
      APInt v(W, *cst, true);
      return std::make_pair(v, v);
    }
    if (!dyn)
      return std::nullopt;
    auto r = queryValueImpl(ctx, dyn, solver, depth + 1);
    if (!r || r->smin().getBitWidth() == 0)
      return std::nullopt;
    if (r->smin().isMinSignedValue() && r->smax().isMaxSignedValue() &&
        r->smin().getBitWidth() >= 32)
      return std::nullopt;
    return std::make_pair(r->smin().sext(W), r->smax().sext(W));
  };

  auto lbB = boundOf(staticLb, dynLb);
  auto ubB = boundOf(staticUb, dynUb);
  auto stB = boundOf(staticStep, dynStep);
  if (!lbB || !ubB || !stB)
    return std::nullopt;

  if (lbB->first.isNegative() || !stB->first.sgt(0))
    return std::nullopt;
  // Inside the body of `for iv = lb to ub step s` (with s > 0), `iv < ub`.
  if (ubB->second.sle(lbB->first))
    return std::nullopt;

  APInt ivMin = lbB->first;
  APInt ivMax = ubB->second - APInt(W, 1, true);
  if (lbB->first == lbB->second && stB->first == stB->second) {
    APInt span = ubB->second - APInt(W, 1, true) - lbB->first;
    APInt steps = span.sdiv(stB->first);
    ivMax = lbB->first + steps * stB->first;
  }
  if (ivMax.slt(ivMin))
    return std::nullopt;
  return makeNonNegRange(bw, ivMin, ivMax);
}

std::optional<ConstantIntRanges>
queryValueImpl(const IntegerRangeContext &ctx, Value value,
               const DataFlowSolver *solver, unsigned depth) {
  if (!ctx.valid() || !value || depth > 12)
    return std::nullopt;
  if (auto constOp = value.getDefiningOp<arith::ConstantOp>()) {
    if (auto intAttr = llvm::dyn_cast<IntegerAttr>(constOp.getValue())) {
      APInt v = intAttr.getValue();
      if (v.getBitWidth() == 0)
        return std::nullopt;
      return ConstantIntRanges::constant(v);
    }
    if (auto denseAttr =
            llvm::dyn_cast<DenseIntElementsAttr>(constOp.getValue())) {
      if (denseAttr.empty())
        return std::nullopt;
      auto vals = denseAttr.getValues<APInt>();
      auto it = vals.begin();
      APInt first = *it;
      if (first.getBitWidth() == 0)
        return std::nullopt;
      APInt smin = first, smax = first, umin = first, umax = first;
      for (++it; it != vals.end(); ++it) {
        const APInt &v = *it;
        if (v.slt(smin))
          smin = v;
        if (v.sgt(smax))
          smax = v;
        if (v.ult(umin))
          umin = v;
        if (v.ugt(umax))
          umax = v;
      }
      return ConstantIntRanges::fromSigned(smin, smax)
          .intersection(ConstantIntRanges::fromUnsigned(umin, umax));
    }
  }

  if (Operation *def = value.getDefiningOp()) {
    StringRef opName = def->getName().getStringRef();
    if (opName == "vector.step") {
      auto st = llvm::dyn_cast<ShapedType>(value.getType());
      unsigned bw = ConstantIntRanges::getStorageBitwidth(
          st ? st.getElementType() : Type{});
      if (st && st.getRank() == 1 && st.getDimSize(0) > 0 && bw > 0 &&
          bw <= 64) {
        return makeNonNegRange(bw, APInt(128, 0, true),
                               APInt(128, st.getDimSize(0) - 1, true));
      }
    }
    if ((opName == "vector.splat" || opName == "tensor.splat" ||
         opName == "vector.broadcast") &&
        def->getNumOperands() == 1 &&
        llvm::isa<IntegerType, IndexType>(def->getOperand(0).getType())) {
      return queryValueImpl(ctx, def->getOperand(0), solver, depth + 1);
    }
  }

  std::optional<ConstantIntRanges> latticeRange;
  if (solver) {
    const auto *lattice =
        solver->lookupState<dataflow::IntegerValueRangeLattice>(value);
    if (lattice && !lattice->getValue().isUninitialized())
      latticeRange = lattice->getValue().getValue();
  }

  if (auto barg = llvm::dyn_cast<BlockArgument>(value)) {
    unsigned bw = ConstantIntRanges::getStorageBitwidth(value.getType());
    Operation *parentOp =
        barg.getOwner() && barg.getOwner()->getParent()
            ? barg.getOwner()->getParent()->getParentOp()
            : nullptr;
    std::optional<ConstantIntRanges> steppedRange;
    if (auto idxReg = llvm::dyn_cast_or_null<IndexedRegionOpInterface>(parentOp)) {
      RegionIndexBounds rb;
      if (mlir::succeeded(idxReg.getRegionArgBounds(barg, rb))) {
        steppedRange = inferSteppedArgRange(
            bw, rb.staticLower, rb.dynamicLower, rb.staticUpperExclusive,
            rb.dynamicUpperExclusive, rb.staticStep, rb.dynamicStep, ctx,
            solver, depth + 1);
      }
    } else if (auto loop = llvm::dyn_cast_or_null<LoopLikeOpInterface>(parentOp)) {
      auto ivs = loop.getLoopInductionVars();
      auto lbs = loop.getLoopLowerBounds();
      auto ubs = loop.getLoopUpperBounds();
      auto steps = loop.getLoopSteps();
      if (ivs && lbs && ubs && steps && ivs->size() == lbs->size() &&
          ivs->size() == ubs->size() && ivs->size() == steps->size()) {
        for (size_t i = 0; i < ivs->size(); ++i) {
          if ((*ivs)[i] != barg)
            continue;
          auto extractTerm = [](OpFoldResult ofr, std::optional<int64_t> &cst,
                                Value &dyn) {
            if (Value v = ofr.dyn_cast<Value>())
              dyn = v;
            else if (auto ia =
                         llvm::dyn_cast_or_null<IntegerAttr>(ofr.dyn_cast<Attribute>()))
              cst = ia.getInt();
          };
          std::optional<int64_t> sLb, sUb, sSt;
          Value dLb, dUb, dSt;
          extractTerm((*lbs)[i], sLb, dLb);
          extractTerm((*ubs)[i], sUb, dUb);
          extractTerm((*steps)[i], sSt, dSt);
          steppedRange = inferSteppedArgRange(bw, sLb, dLb, sUb, dUb, sSt, dSt,
                                              ctx, solver, depth + 1);
          break;
        }
      }
    }
    if (steppedRange) {
      return latticeRange ? latticeRange->intersection(*steppedRange)
                          : steppedRange;
    }
  }

  return latticeRange;
}
} // namespace

IntegerRangeContext::IntegerRangeContext(Operation *root) : rootOp(root) {
  if (!root)
    return;
  DataFlowConfig cfg;
  cfg.setInterprocedural(false);
  solver = std::make_unique<DataFlowSolver>(cfg);
  solver->load<dataflow::DeadCodeAnalysis>();
  solver->load<dataflow::IntegerRangeAnalysis>();
  succeeded = mlir::succeeded(solver->initializeAndRun(root));
}

IntegerRangeContext::~IntegerRangeContext() = default;

std::optional<ConstantIntRanges>
IntegerRangeContext::query(Value value) const {
  return queryValueImpl(*this, value, solver.get(), 0);
}

namespace {
Type leafScalarType(Type ty) {
  if (auto shaped = llvm::dyn_cast<ShapedType>(ty))
    return shaped.getElementType();
  return ty;
}

bool isAddressScalarOrShapedInt(Type ty) {
  if (!ty || llvm::isa<MemRefType>(ty))
    return false;
  return llvm::isa<IntegerType, IndexType>(leafScalarType(ty));
}

bool fitsSignedWidth(const ConstantIntRanges &r, unsigned bits,
                     bool requireNonNegative = false, uint64_t scale = 1) {
  unsigned w = r.smin().getBitWidth();
  if (w == 0 || bits == 0 || bits > 64)
    return false;
  unsigned calcW = std::max(w, bits) + 65;
  APInt scaleAP(calcW, scale == 0 ? 1 : scale, /*isSigned=*/false);
  APInt lo = r.smin().sext(calcW) * scaleAP;
  APInt hi = r.smax().sext(calcW) * scaleAP;
  if (requireNonNegative && lo.isNegative())
    return false;
  APInt minT = APInt::getSignedMinValue(bits).sext(calcW);
  APInt maxT = APInt::getSignedMaxValue(bits).sext(calcW);
  return lo.sge(minT) && hi.sle(maxT);
}

bool fitsUnsignedWidth(const ConstantIntRanges &r, unsigned bits,
                       bool mustAlsoFitSignedPositive = false,
                       uint64_t scale = 1) {
  unsigned w = r.umax().getBitWidth();
  if (w == 0 || bits == 0 || bits > 64)
    return false;
  if (r.smin().isNegative())
    return false;
  unsigned calcW = std::max(w, bits) + 65;
  APInt scaleAP(calcW, scale == 0 ? 1 : scale, /*isSigned=*/false);
  APInt uhi = r.umax().zext(calcW) * scaleAP;
  APInt maxBound = mustAlsoFitSignedPositive
                       ? APInt::getSignedMaxValue(bits).zext(calcW)
                       : APInt::getMaxValue(bits).zext(calcW);
  return uhi.ule(maxBound);
}

bool rangeFitsOptions(const ConstantIntRanges &r,
                      const AddressProofOptions &opts, uint64_t scale) {
  if (opts.isSigned)
    return fitsSignedWidth(r, opts.targetBitwidth, opts.requireNonNegative,
                           scale);
  return fitsUnsignedWidth(r, opts.targetBitwidth,
                           /*mustAlsoFitSignedPositive=*/false, scale);
}

bool apIntervalFitsOptions(const APInt &lo, const APInt &hi,
                           const AddressProofOptions &opts,
                           bool checkNonNegative) {
  unsigned calcW = lo.getBitWidth();
  unsigned T = opts.targetBitwidth;
  if (opts.isSigned) {
    if (checkNonNegative && lo.isNegative())
      return false;
    APInt minT = APInt::getSignedMinValue(T).sext(calcW);
    APInt maxT = APInt::getSignedMaxValue(T).sext(calcW);
    return lo.sge(minT) && hi.sle(maxT);
  }
  if (lo.isNegative())
    return false;
  APInt maxU = APInt::getMaxValue(T).zext(calcW);
  return hi.ule(maxU);
}

bool isCastOrTruncOp(Operation *op) {
  return llvm::isa<arith::IndexCastOp, arith::IndexCastUIOp, arith::TruncIOp>(
      op);
}

bool isStandardMemoryIndexingOp(Operation *op) {
  if (!op)
    return false;
  if (llvm::isa<OffsetSizeAndStrideOpInterface>(op))
    return true;
  StringRef name = op->getName().getStringRef();
  return name == "memref.load" || name == "memref.store" ||
         name == "tensor.extract" || name == "tensor.insert" ||
         name == "vector.load" || name == "vector.store" ||
         name == "vector.maskedload" || name == "vector.maskedstore" ||
         name == "vector.transfer_read" || name == "vector.transfer_write";
}

AddressProof proveNarrowRootsImpl(Operation *scopeRoot,
                                  llvm::ArrayRef<Value> addressValues,
                                  const AddressProofOptions &options,
                                  const IntegerRangeContext *existingRanges) {
  AddressProof out;
  out.chosenBitwidth = options.fallbackBitwidth;
  if (!scopeRoot) {
    out.reason = "null scopeRoot";
    return out;
  }
  if (options.targetBitwidth == 0 || options.targetBitwidth > 64) {
    out.reason = "invalid targetBitwidth";
    return out;
  }

  std::unique_ptr<IntegerRangeContext> localSolver;
  if (!contextCoversScope(existingRanges, scopeRoot)) {
    localSolver = std::make_unique<IntegerRangeContext>(scopeRoot);
    existingRanges = localSolver.get();
  }
  if (!existingRanges->valid()) {
    out.reason = "IntegerRangeAnalysis failed to converge on scopeRoot";
    return out;
  }

  llvm::SmallVector<Value, 32> roots;
  llvm::DenseSet<Value> rootSet;
  if (!addressValues.empty()) {
    for (Value v : addressValues) {
      if (!v || !isAddressScalarOrShapedInt(v.getType())) {
        out.culpritValue = v;
        out.culpritOp = v ? v.getDefiningOp() : nullptr;
        out.reason = "explicit address value is null or not an integer/index "
                     "scalar or vector";
        return out;
      }
      Operation *parent =
          v.getParentRegion() ? v.getParentRegion()->getParentOp() : nullptr;
      if (!parent || (!scopeRoot->isAncestor(parent) && parent != scopeRoot)) {
        out.culpritValue = v;
        out.reason = "address value lies outside scopeRoot";
        return out;
      }
      if (rootSet.insert(v).second)
        roots.push_back(v);
    }
  } else {
    scopeRoot->walk([&](Operation *op) {
      bool memOrCast = isCastOrTruncOp(op) ||
                       llvm::isa<MemoryEffectOpInterface, AddressOpInterface>(op) ||
                       isStandardMemoryIndexingOp(op);
      if (!memOrCast)
        return;
      for (Value v : op->getOperands())
        if (isAddressScalarOrShapedInt(v.getType()) &&
            !leafScalarType(v.getType()).isInteger(1) &&
            rootSet.insert(v).second)
          roots.push_back(v);
    });
  }

  if (roots.empty()) {
    out.reason = "no address roots found to prove";
    return out;
  }

  llvm::SmallVector<Value, 64> worklist(roots.begin(), roots.end());
  llvm::DenseSet<Value> visited(roots.begin(), roots.end());
  auto enqueue = [&](Value v) {
    if (v && isAddressScalarOrShapedInt(v.getType()) &&
        !leafScalarType(v.getType()).isInteger(1) && visited.insert(v).second)
      worklist.push_back(v);
  };

  const unsigned T = options.targetBitwidth;

  while (!worklist.empty()) {
    Value cur = worklist.pop_back_val();
    uint64_t scale = rootSet.contains(cur) ? options.elementByteScale : 1;
    auto rng = existingRanges->query(cur);
    if (!rng) {
      out.culpritValue = cur;
      out.culpritOp = cur.getDefiningOp();
      out.reason = "uninitialized or unknown integer range";
      return out;
    }
    if (!rangeFitsOptions(*rng, options, scale)) {
      out.culpritValue = cur;
      out.culpritOp = cur.getDefiningOp();
      out.reason = "value range exceeds target narrow bitwidth";
      return out;
    }

    if (auto barg = llvm::dyn_cast<BlockArgument>(cur)) {
      Region *parentReg = barg.getOwner()->getParent();
      Operation *parentOp = parentReg ? parentReg->getParentOp() : nullptr;
      if (!parentOp || !scopeRoot->isAncestor(parentOp)) {
        if (parentOp == scopeRoot && !barg.getOwner()->isEntryBlock()) {
          out.culpritValue = cur;
          out.reason = "non-entry CFG block argument is not whitelisted";
          return out;
        }
        continue;
      }
      if (auto idxReg = llvm::dyn_cast<IndexedRegionOpInterface>(parentOp)) {
        RegionIndexBounds rb;
        if (barg.getOwner()->isEntryBlock() && parentReg->hasOneBlock() &&
            mlir::succeeded(idxReg.getRegionArgBounds(barg, rb))) {
          if (rb.dynamicLower)
            enqueue(rb.dynamicLower);
          if (rb.dynamicUpperExclusive)
            enqueue(rb.dynamicUpperExclusive);
          if (rb.dynamicStep)
            enqueue(rb.dynamicStep);
          continue;
        }
      }
      if (auto loop = llvm::dyn_cast<LoopLikeOpInterface>(parentOp)) {
        if (!barg.getOwner()->isEntryBlock() || !parentReg->hasOneBlock()) {
          out.culpritValue = cur;
          out.culpritOp = parentOp;
          out.reason = "multi-block loop block argument is not whitelisted";
          return out;
        }
        bool matched = false;
        if (auto ivs = loop.getLoopInductionVars()) {
          auto lbs = loop.getLoopLowerBounds();
          auto ubs = loop.getLoopUpperBounds();
          auto steps = loop.getLoopSteps();
          if (lbs && ubs && steps && ivs->size() == lbs->size() &&
              ivs->size() == ubs->size() && ivs->size() == steps->size()) {
            for (size_t i = 0; i < ivs->size(); ++i) {
              if ((*ivs)[i] != barg)
                continue;
              matched = true;
              if (Value lbV = (*lbs)[i].dyn_cast<Value>())
                enqueue(lbV);
              if (Value ubV = (*ubs)[i].dyn_cast<Value>())
                enqueue(ubV);
              if (Value stV = (*steps)[i].dyn_cast<Value>())
                enqueue(stV);
            }
          }
        }
        if (!matched) {
          auto iterArgs = loop.getRegionIterArgs();
          auto yielded = loop.getYieldedValuesMutable();
          for (auto [idx, ia] : llvm::enumerate(iterArgs)) {
            if (ia != barg)
              continue;
            matched = true;
            if (idx < loop.getInits().size())
              enqueue(loop.getInits()[idx]);
            if (yielded && idx < yielded->size())
              enqueue((*yielded)[idx].get());
          }
        }
        if (matched)
          continue;
      }
      out.culpritValue = cur;
      out.culpritOp = parentOp;
      out.reason = "unsupported BlockArgument in narrow-address whitelist";
      return out;
    }

    Operation *def = cur.getDefiningOp();
    if (!def || !scopeRoot->isAncestor(def))
      continue;

    if (llvm::isa<arith::ConstantOp>(def))
      continue;

    StringRef defName = def->getName().getStringRef();
    if (defName == "vector.step")
      continue;
    if ((defName == "vector.splat" || defName == "tensor.splat" ||
         defName == "vector.broadcast") &&
        def->getNumOperands() == 1 &&
        isAddressScalarOrShapedInt(def->getOperand(0).getType())) {
      enqueue(def->getOperand(0));
      continue;
    }

    if (llvm::isa<arith::SelectOp>(def)) {
      auto sel = llvm::cast<arith::SelectOp>(def);
      auto tR = existingRanges->query(sel.getTrueValue());
      auto fR = existingRanges->query(sel.getFalseValue());
      if (!tR || !fR || !rangeFitsOptions(*tR, options, 1) ||
          !rangeFitsOptions(*fR, options, 1)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "select branch exceeds target narrow bitwidth";
        return out;
      }
      enqueue(sel.getTrueValue());
      enqueue(sel.getFalseValue());
      continue;
    }

    if (llvm::isa<arith::ExtSIOp, arith::IndexCastOp, arith::TruncIOp>(def)) {
      Value src = def->getOperand(0);
      auto srcRng = existingRanges->query(src);
      unsigned dstW =
          ConstantIntRanges::getStorageBitwidth(leafScalarType(cur.getType()));
      unsigned effW = std::min(T, dstW);
      bool ok = srcRng && (options.isSigned
                               ? fitsSignedWidth(*srcRng, effW,
                                                 options.requireNonNegative, 1)
                               : fitsUnsignedWidth(*srcRng, effW, false, 1));
      if (!ok) {
        out.culpritOp = def;
        out.culpritValue = src;
        out.reason =
            "signed cast/trunci operand exceeds or wraps around narrow width";
        return out;
      }
      unsigned srcW =
          ConstantIntRanges::getStorageBitwidth(leafScalarType(src.getType()));
      if (llvm::isa<IndexType>(leafScalarType(src.getType())) || srcW >= T ||
          llvm::isa<arith::TruncIOp>(def))
        enqueue(src);
      continue;
    }

    if (llvm::isa<arith::ExtUIOp, arith::IndexCastUIOp>(def)) {
      Value src = def->getOperand(0);
      auto srcRng = existingRanges->query(src);
      unsigned dstW =
          ConstantIntRanges::getStorageBitwidth(leafScalarType(cur.getType()));
      unsigned effW = std::min(T, dstW);
      if (!srcRng ||
          !fitsUnsignedWidth(*srcRng, effW,
                             /*mustAlsoFitSignedPositive=*/options.isSigned,
                             1)) {
        out.culpritOp = def;
        out.culpritValue = src;
        out.reason =
            "unsigned cast operand may be negative or exceed narrow width";
        return out;
      }
      unsigned srcW =
          ConstantIntRanges::getStorageBitwidth(leafScalarType(src.getType()));
      if (llvm::isa<IndexType>(leafScalarType(src.getType())) || srcW >= T)
        enqueue(src);
      continue;
    }

    if (llvm::isa<arith::AddIOp, arith::SubIOp, arith::MulIOp>(def)) {
      Value lhs = def->getOperand(0), rhs = def->getOperand(1);
      auto lhsR = existingRanges->query(lhs), rhsR = existingRanges->query(rhs);
      if (!lhsR || !rhsR || !rangeFitsOptions(*lhsR, options, 1) ||
          !rangeFitsOptions(*rhsR, options, 1)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "arithmetic operand exceeds target narrow bitwidth";
        return out;
      }
      unsigned w = lhsR->smin().getBitWidth();
      unsigned extW = std::max(w, T) + 66;
      if (options.isSigned) {
        APInt lMin = lhsR->smin().sext(extW), lMax = lhsR->smax().sext(extW);
        APInt rMin = rhsR->smin().sext(extW), rMax = rhsR->smax().sext(extW);
        APInt wMin(extW, 0), wMax(extW, 0);
        if (llvm::isa<arith::AddIOp>(def)) {
          wMin = lMin + rMin;
          wMax = lMax + rMax;
        } else if (llvm::isa<arith::SubIOp>(def)) {
          wMin = lMin - rMax;
          wMax = lMax - rMin;
        } else {
          APInt p1 = lMin * rMin, p2 = lMin * rMax, p3 = lMax * rMin,
                p4 = lMax * rMax;
          wMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                      llvm::APIntOps::smin(p3, p4));
          wMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                      llvm::APIntOps::smax(p3, p4));
        }
        APInt minT = APInt::getSignedMinValue(T).sext(extW);
        APInt maxT = APInt::getSignedMaxValue(T).sext(extW);
        if (wMin.slt(minT) || wMax.sgt(maxT) ||
            (options.requireNonNegative && wMin.isNegative())) {
          out.culpritOp = def;
          out.culpritValue = cur;
          out.reason = "signed intermediate arithmetic overflows narrow width";
          return out;
        }
      } else {
        APInt lMin = lhsR->umin().zext(extW), lMax = lhsR->umax().zext(extW);
        APInt rMin = rhsR->umin().zext(extW), rMax = rhsR->umax().zext(extW);
        APInt wMax(extW, 0);
        if (llvm::isa<arith::AddIOp>(def)) {
          wMax = lMax + rMax;
        } else if (llvm::isa<arith::SubIOp>(def)) {
          if (lMin.ult(rMax)) {
            out.culpritOp = def;
            out.culpritValue = cur;
            out.reason = "unsigned subtraction may underflow in narrow width";
            return out;
          }
          wMax = lMax - rMin;
        } else {
          wMax = lMax * rMax;
        }
        if (wMax.ugt(APInt::getMaxValue(T).zext(extW))) {
          out.culpritOp = def;
          out.culpritValue = cur;
          out.reason =
              "unsigned intermediate arithmetic overflows narrow width";
          return out;
        }
      }
      enqueue(lhs);
      enqueue(rhs);
      continue;
    }

    if (llvm::isa<arith::DivSIOp, arith::RemSIOp, arith::MinSIOp,
                  arith::MaxSIOp>(def)) {
      Value lhs = def->getOperand(0), rhs = def->getOperand(1);
      auto lhsR = existingRanges->query(lhs), rhsR = existingRanges->query(rhs);
      bool reqNonNeg = !options.isSigned || options.requireNonNegative;
      if (!lhsR || !rhsR || !fitsSignedWidth(*lhsR, T, reqNonNeg, 1) ||
          !fitsSignedWidth(*rhsR, T, !options.isSigned, 1)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason =
            "signed div/rem/min/max operand exceeds signed narrow width";
        return out;
      }
      if (llvm::isa<arith::DivSIOp, arith::RemSIOp>(def)) {
        unsigned w = rhsR->smin().getBitWidth();
        unsigned extW = std::max(w, T) + 2;
        APInt rLo = rhsR->smin().sext(extW), rHi = rhsR->smax().sext(extW);
        if (rLo.sle(0) && rHi.sge(0)) {
          out.culpritOp = def;
          out.culpritValue = rhs;
          out.reason = "signed div/rem divisor may be zero";
          return out;
        }
        APInt minT = APInt::getSignedMinValue(T).sext(extW);
        APInt negOne = APInt::getAllOnes(extW);
        if (lhsR->smin().sext(extW).sle(minT) && rLo.sle(negOne) &&
            rHi.sge(negOne)) {
          out.culpritOp = def;
          out.culpritValue = cur;
          out.reason = "signed div/rem may overflow INT_MIN / -1 at narrow T";
          return out;
        }
      }
      enqueue(lhs);
      enqueue(rhs);
      continue;
    }

    if (llvm::isa<arith::DivUIOp, arith::RemUIOp, arith::MinUIOp,
                  arith::MaxUIOp>(def)) {
      Value lhs = def->getOperand(0), rhs = def->getOperand(1);
      auto lhsR = existingRanges->query(lhs), rhsR = existingRanges->query(rhs);
      if (!lhsR || !rhsR || !fitsUnsignedWidth(*lhsR, T, options.isSigned, 1) ||
          !fitsUnsignedWidth(*rhsR, T, options.isSigned, 1)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason =
            "unsigned divui/remui/minui/maxui operand exceeds unsigned narrow "
            "width (e.g. negative signed value)";
        return out;
      }
      if (llvm::isa<arith::DivUIOp, arith::RemUIOp>(def) &&
          rhsR->umin().isZero()) {
        out.culpritOp = def;
        out.culpritValue = rhs;
        out.reason = "unsigned divui/remui divisor may be zero";
        return out;
      }
      enqueue(lhs);
      enqueue(rhs);
      continue;
    }

    if (llvm::isa<arith::ShLIOp, arith::ShRSIOp, arith::ShRUIOp>(def)) {
      Value lhs = def->getOperand(0), rhs = def->getOperand(1);
      auto lhsR = existingRanges->query(lhs), rhsR = existingRanges->query(rhs);
      if (!lhsR || !rhsR) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "missing operand range on shift";
        return out;
      }
      if (rhsR->smin().isNegative() || rhsR->smax().uge(T) ||
          rhsR->umax().uge(T)) {
        out.culpritOp = def;
        out.culpritValue = rhs;
        out.reason = "shift amount may be negative or >= narrow bitwidth";
        return out;
      }
      if (llvm::isa<arith::ShRUIOp>(def)) {
        if (!fitsUnsignedWidth(*lhsR, T, options.isSigned, 1)) {
          out.culpritOp = def;
          out.culpritValue = lhs;
          out.reason =
              "unsigned right-shift input exceeds unsigned narrow bitwidth";
          return out;
        }
      } else if (llvm::isa<arith::ShRSIOp>(def)) {
        if (!fitsSignedWidth(*lhsR, T, !options.isSigned, 1)) {
          out.culpritOp = def;
          out.culpritValue = lhs;
          out.reason =
              "signed right-shift input exceeds signed narrow bitwidth";
          return out;
        }
      } else {
        unsigned w = lhsR->smin().getBitWidth();
        unsigned extW = std::max(w, T) + 66;
        unsigned shMin = static_cast<unsigned>(rhsR->umin().getZExtValue());
        unsigned shMax = static_cast<unsigned>(rhsR->umax().getZExtValue());
        if (options.isSigned) {
          if (!fitsSignedWidth(*lhsR, T, options.requireNonNegative, 1)) {
            out.culpritOp = def;
            out.culpritValue = lhs;
            out.reason = "shli input exceeds signed narrow bitwidth";
            return out;
          }
          APInt lMin = lhsR->smin().sext(extW), lMax = lhsR->smax().sext(extW);
          APInt p1 = lMin.shl(shMin), p2 = lMin.shl(shMax),
                p3 = lMax.shl(shMin), p4 = lMax.shl(shMax);
          APInt wMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                            llvm::APIntOps::smin(p3, p4));
          APInt wMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                            llvm::APIntOps::smax(p3, p4));
          if (wMin.slt(APInt::getSignedMinValue(T).sext(extW)) ||
              wMax.sgt(APInt::getSignedMaxValue(T).sext(extW)) ||
              (options.requireNonNegative && wMin.isNegative())) {
            out.culpritOp = def;
            out.culpritValue = cur;
            out.reason = "shli result overflows signed narrow bitwidth";
            return out;
          }
        } else {
          if (!fitsUnsignedWidth(*lhsR, T, false, 1)) {
            out.culpritOp = def;
            out.culpritValue = lhs;
            out.reason = "shli input exceeds unsigned narrow bitwidth";
            return out;
          }
          APInt uMax = lhsR->umax().zext(extW).shl(shMax);
          if (uMax.ugt(APInt::getMaxValue(T).zext(extW))) {
            out.culpritOp = def;
            out.culpritValue = cur;
            out.reason = "shli result overflows unsigned narrow bitwidth";
            return out;
          }
        }
      }
      enqueue(lhs);
      enqueue(rhs);
      continue;
    }

    if (llvm::isa<arith::AndIOp, arith::OrIOp, arith::XOrIOp>(def)) {
      Value lhs = def->getOperand(0), rhs = def->getOperand(1);
      auto lhsR = existingRanges->query(lhs), rhsR = existingRanges->query(rhs);
      if (!lhsR || !rhsR || !rangeFitsOptions(*lhsR, options, 1) ||
          !rangeFitsOptions(*rhsR, options, 1)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "bitwise operand exceeds target narrow bitwidth";
        return out;
      }
      enqueue(lhs);
      enqueue(rhs);
      continue;
    }

    if (llvm::isa<InferIntRangeInterface>(def)) {
      bool hasNonConstIntOperand = false;
      for (Value opnd : def->getOperands()) {
        if (isAddressScalarOrShapedInt(opnd.getType())) {
          if (!opnd.getDefiningOp<arith::ConstantOp>())
            hasNonConstIntOperand = true;
          else
            enqueue(opnd);
        }
      }
      if (!hasNonConstIntOperand)
        continue;
    }

    out.culpritOp = def;
    out.culpritValue = cur;
    out.reason = "operation not in narrow-address whitelist: " +
                 def->getName().getStringRef().str();
    return out;
  }

  out.proven = true;
  out.chosenBitwidth = options.targetBitwidth;
  return out;
}

AddressProof verifyLinearCombinationFits(
    Operation *op, Operation *scopeRoot, const LinearAddressDescriptor &desc,
    const AddressProofOptions &options, const IntegerRangeContext &ranges) {
  AddressProof out;
  out.chosenBitwidth = options.fallbackBitwidth;
  constexpr unsigned W = 192;

  size_t n = desc.coordinates.size();
  bool hasDynStrides = !desc.dynamicStrides.empty();
  bool hasStaticStrides = !desc.staticStrides.empty();
  if (n > 0 && ((hasDynStrides && desc.dynamicStrides.size() != n) ||
                (hasStaticStrides && desc.staticStrides.size() != n) ||
                (!hasDynStrides && !hasStaticStrides))) {
    out.culpritOp = op;
    out.reason = "linear address descriptor has mismatched coordinate/stride arity";
    return out;
  }

  llvm::SmallVector<Value, 16> roots;
  roots.append(desc.coordinates.begin(), desc.coordinates.end());
  if (hasDynStrides)
    roots.append(desc.dynamicStrides.begin(), desc.dynamicStrides.end());

  if (!roots.empty()) {
    AddressProofOptions unitOpts = options;
    unitOpts.requireNonNegative = false;
    unitOpts.elementByteScale = 1;
    auto rootProof = proveNarrowRootsImpl(scopeRoot, roots, unitOpts, &ranges);
    if (!rootProof.proven)
      return rootProof;
  }

  AddressProofOptions termOpts = options;
  termOpts.requireNonNegative = false;
  termOpts.elementByteScale = 1;

  APInt sumLo(W, desc.constantOffset, /*isSigned=*/true);
  APInt sumHi(W, desc.constantOffset, /*isSigned=*/true);
  if (!apIntervalFitsOptions(sumLo, sumHi, termOpts, false)) {
    out.culpritOp = op;
    out.reason = "constant address offset exceeds target narrow bitwidth";
    return out;
  }

  for (size_t i = 0; i < n; ++i) {
    Value cVal = desc.coordinates[i];
    auto rc = ranges.query(cVal);
    if (!rc) {
      out.culpritOp = op;
      out.culpritValue = cVal;
      out.reason = "missing coordinate integer range";
      return out;
    }
    APInt cLo = options.isSigned ? rc->smin().sext(W) : rc->umin().zext(W);
    APInt cHi = options.isSigned ? rc->smax().sext(W) : rc->umax().zext(W);

    APInt sLo(W, 0, true), sHi(W, 0, true);
    if (hasDynStrides) {
      auto rs = ranges.query(desc.dynamicStrides[i]);
      if (!rs) {
        out.culpritOp = op;
        out.culpritValue = desc.dynamicStrides[i];
        out.reason = "missing dynamic stride integer range";
        return out;
      }
      sLo = options.isSigned ? rs->smin().sext(W) : rs->umin().zext(W);
      sHi = options.isSigned ? rs->smax().sext(W) : rs->umax().zext(W);
    } else {
      if (desc.staticStrides[i] == ShapedType::kDynamic) {
        out.culpritOp = op;
        out.reason = "unresolved dynamic stride in container layout";
        return out;
      }
      sLo = APInt(W, desc.staticStrides[i], /*isSigned=*/true);
      sHi = sLo;
    }
    if (!apIntervalFitsOptions(sLo, sHi, termOpts, false)) {
      out.culpritOp = op;
      out.reason = "stride exceeds target narrow bitwidth";
      return out;
    }

    APInt p1 = cLo * sLo, p2 = cLo * sHi, p3 = cHi * sLo, p4 = cHi * sHi;
    APInt tLo = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                     llvm::APIntOps::smin(p3, p4));
    APInt tHi = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                     llvm::APIntOps::smax(p3, p4));
    if (!apIntervalFitsOptions(tLo, tHi, termOpts, false)) {
      out.culpritOp = op;
      out.culpritValue = cVal;
      out.reason = "coordinate * stride product overflows narrow bitwidth";
      return out;
    }

    sumLo += tLo;
    sumHi += tHi;
    if (!apIntervalFitsOptions(sumLo, sumHi, termOpts, false)) {
      out.culpritOp = op;
      out.culpritValue = cVal;
      out.reason = "linear address sum overflows narrow bitwidth";
      return out;
    }
  }

  APInt scaleAP(W, options.elementByteScale == 0 ? 1 : options.elementByteScale,
                /*isSigned=*/false);
  APInt finalLo = sumLo * scaleAP;
  APInt finalHi = sumHi * scaleAP;
  if (!apIntervalFitsOptions(finalLo, finalHi, options,
                             options.requireNonNegative)) {
    out.culpritOp = op;
    out.reason = "scaled linear address exceeds target narrow bitwidth";
    return out;
  }

  out.proven = true;
  out.chosenBitwidth = options.targetBitwidth;
  return out;
}
} // namespace

AddressProof compilerinfra::proveNarrowAddress(
    Value addressValue, const AddressProofOptions &options,
    const IntegerRangeContext *ranges) {
  AddressProof out;
  out.chosenBitwidth = options.fallbackBitwidth;
  if (!addressValue) {
    out.reason = "null addressValue";
    return out;
  }
  if (Operation *def = addressValue.getDefiningOp()) {
    if (llvm::isa<AddressOpInterface>(def))
      return proveNarrowAddress(def, options, ranges);
  }
  Operation *scopeRoot = findEnclosingProofScope(addressValue);
  if (!scopeRoot) {
    out.culpritValue = addressValue;
    out.reason = "unable to resolve enclosing scopeRoot from addressValue";
    return out;
  }
  return proveNarrowRootsImpl(scopeRoot, {addressValue}, options, ranges);
}

AddressProof compilerinfra::proveNarrowAddress(
    Operation *op, const AddressProofOptions &options,
    const IntegerRangeContext *ranges) {
  AddressProof out;
  out.chosenBitwidth = options.fallbackBitwidth;
  if (!op) {
    out.reason = "null operation";
    return out;
  }
  if (options.targetBitwidth == 0 || options.targetBitwidth > 64) {
    out.reason = "invalid targetBitwidth";
    return out;
  }

  Operation *scopeRoot = op->hasTrait<OpTrait::IsIsolatedFromAbove>()
                             ? op
                             : findEnclosingProofScope(op);
  if (!scopeRoot) {
    out.culpritOp = op;
    out.reason = "unable to resolve enclosing scopeRoot";
    return out;
  }

  std::unique_ptr<IntegerRangeContext> localSolver;
  if (!contextCoversScope(ranges, scopeRoot)) {
    localSolver = std::make_unique<IntegerRangeContext>(scopeRoot);
    ranges = localSolver.get();
  }
  if (!ranges->valid()) {
    out.reason = "IntegerRangeAnalysis failed to converge on scopeRoot";
    return out;
  }

  if (auto addrOp = llvm::dyn_cast<AddressOpInterface>(op)) {
    LinearAddressDescriptor desc;
    if (mlir::failed(addrOp.getLinearAddress(desc))) {
      out.culpritOp = op;
      out.reason = "AddressOpInterface::getLinearAddress failed";
      return out;
    }
    return verifyLinearCombinationFits(op, scopeRoot, desc, options, *ranges);
  }

  if (isStandardMemoryIndexingOp(op) || isCastOrTruncOp(op)) {
    llvm::SmallVector<Value, 8> idxOperands;
    Value memrefContainer;
    for (Value v : op->getOperands()) {
      if (!memrefContainer && llvm::isa<MemRefType>(v.getType()))
        memrefContainer = v;
      if (isAddressScalarOrShapedInt(v.getType()) &&
          !leafScalarType(v.getType()).isInteger(1))
        idxOperands.push_back(v);
    }
    if (auto mt = memrefContainer
                      ? llvm::dyn_cast<MemRefType>(memrefContainer.getType())
                      : MemRefType{};
        mt && !llvm::isa<OffsetSizeAndStrideOpInterface>(op) &&
        static_cast<size_t>(mt.getRank()) == idxOperands.size() &&
        mt.getRank() > 0) {
      int64_t memOff = 0;
      llvm::SmallVector<int64_t, 4> memStrides;
      if (mlir::succeeded(mt.getStridesAndOffset(memStrides, memOff)) &&
          memOff != ShapedType::kDynamic &&
          llvm::none_of(memStrides,
                        [](int64_t s) { return s == ShapedType::kDynamic; })) {
        LinearAddressDescriptor desc;
        desc.baseContainer = memrefContainer;
        desc.coordinates.assign(idxOperands.begin(), idxOperands.end());
        desc.staticStrides.assign(memStrides.begin(), memStrides.end());
        desc.constantOffset = memOff;
        return verifyLinearCombinationFits(op, scopeRoot, desc, options,
                                           *ranges);
      }
    }
    return proveNarrowRootsImpl(scopeRoot, idxOperands, options, ranges);
  }

  if (op->hasTrait<OpTrait::IsIsolatedFromAbove>())
    return proveNarrowRootsImpl(op, {}, options, ranges);

  out.culpritOp = op;
  out.reason = "operation does not implement AddressOpInterface or standard "
               "MLIR memory indexing: " +
               op->getName().getStringRef().str();
  return out;
}

AddressProof compilerinfra::proveNarrowAddress(
    Operation *scopeRoot, llvm::ArrayRef<Value> addressValues,
    const AddressProofOptions &options,
    const IntegerRangeContext *existingRanges) {
  return proveNarrowRootsImpl(scopeRoot, addressValues, options,
                              existingRanges);
}

namespace {
bool collectOpReadEffects(Operation *root,
                          llvm::SmallVectorImpl<Value> &locations,
                          llvm::DenseSet<Value> &seenLocs,
                          llvm::SmallVectorImpl<Operation *> &readOps,
                          llvm::SmallPtrSetImpl<Operation *> &seenReadOps,
                          bool &hasGlobalRead, std::string &err) {
  bool ok = true;
  root->walk([&](Operation *op) {
    auto mem = llvm::dyn_cast<MemoryEffectOpInterface>(op);
    if (!mem) {
      if (op->getNumRegions() > 0 &&
          op->hasTrait<OpTrait::HasRecursiveMemoryEffects>())
        return WalkResult::advance();
      err = "source operation lacks MemoryEffectOpInterface: " +
            op->getName().getStringRef().str();
      ok = false;
      return WalkResult::interrupt();
    }
    llvm::SmallVector<MemoryEffects::EffectInstance, 4> effects;
    mem.getEffects(effects);
    bool opReads = false;
    for (const auto &e : effects) {
      if (llvm::isa<MemoryEffects::Write, MemoryEffects::Allocate,
                    MemoryEffects::Free>(e.getEffect())) {
        err = "source slice operation has non-read memory side effect: " +
              op->getName().getStringRef().str();
        ok = false;
        return WalkResult::interrupt();
      }
      if (llvm::isa<MemoryEffects::Read>(e.getEffect())) {
        opReads = true;
        if (Value v = e.getValue()) {
          if (seenLocs.insert(v).second)
            locations.push_back(v);
        } else {
          hasGlobalRead = true;
        }
      }
    }
    if (opReads && seenReadOps.insert(op).second)
      readOps.push_back(op);
    return WalkResult::advance();
  });
  return ok;
}

bool collectTransitiveReads(Operation *firstOp, Operation *scopeOp,
                            llvm::SmallVectorImpl<Value> &locations,
                            llvm::SmallVectorImpl<Operation *> &readOps,
                            bool &hasGlobalRead, std::string &err) {
  llvm::DenseSet<Value> seenLocs(locations.begin(), locations.end());
  llvm::SmallPtrSet<Operation *, 16> visitedOps;
  llvm::SmallPtrSet<Operation *, 8> seenReadOps;
  llvm::DenseSet<Value> visitedVals;
  llvm::SmallVector<Operation *, 16> opWorklist;

  auto enqueueOp = [&](Operation *op) {
    if (op && (op == scopeOp || scopeOp->isAncestor(op)) &&
        visitedOps.insert(op).second)
      opWorklist.push_back(op);
  };

  std::function<bool(Value)> enqueueVal = [&](Value v) -> bool {
    if (!v || !visitedVals.insert(v).second)
      return true;
    if (Operation *def = v.getDefiningOp()) {
      if (!scopeOp->isAncestor(def) && def != scopeOp) {
        err = "value defined outside isolated scopeRoot";
        return false;
      }
      enqueueOp(def);
      return true;
    }
    auto barg = llvm::cast<BlockArgument>(v);
    Block *ownerBlk = barg.getOwner();
    Region *parentReg = ownerBlk ? ownerBlk->getParent() : nullptr;
    Operation *parentOp = parentReg ? parentReg->getParentOp() : nullptr;

    if (parentOp == scopeOp) {
      if (ownerBlk->isEntryBlock() &&
          scopeOp->hasTrait<OpTrait::IsIsolatedFromAbove>())
        return true;
      err = "non-entry CFG block argument may carry predecessor memory loads";
      return false;
    }

    if (auto idxReg = llvm::dyn_cast_or_null<IndexedRegionOpInterface>(parentOp)) {
      RegionIndexBounds rb;
      if (scopeOp->isAncestor(parentOp) && ownerBlk->isEntryBlock() &&
          parentReg->hasOneBlock() &&
          mlir::succeeded(idxReg.getRegionArgBounds(barg, rb))) {
        if (rb.dynamicLower && !enqueueVal(rb.dynamicLower))
          return false;
        if (rb.dynamicUpperExclusive && !enqueueVal(rb.dynamicUpperExclusive))
          return false;
        if (rb.dynamicStep && !enqueueVal(rb.dynamicStep))
          return false;
        return true;
      }
    }

    if (auto loop = llvm::dyn_cast_or_null<LoopLikeOpInterface>(parentOp)) {
      if (scopeOp->isAncestor(parentOp) && ownerBlk->isEntryBlock() &&
          parentReg->hasOneBlock()) {
        auto iterArgs = loop.getRegionIterArgs();
        auto yieldedVals = loop.getYieldedValuesMutable();
        for (auto [idx, iterArg] : llvm::enumerate(iterArgs)) {
          if (iterArg == barg) {
            if (idx >= loop.getInits().size() || !yieldedVals ||
                idx >= yieldedVals->size()) {
              err = "incomplete loop iter_arg init/yield values";
              return false;
            }
            return enqueueVal(loop.getInits()[idx]) &&
                   enqueueVal((*yieldedVals)[idx].get());
          }
        }
        if (auto ivs = loop.getLoopInductionVars()) {
          auto lbs = loop.getLoopLowerBounds();
          auto ubs = loop.getLoopUpperBounds();
          auto steps = loop.getLoopSteps();
          if (lbs && ubs && steps && ivs->size() == lbs->size() &&
              ivs->size() == ubs->size() && ivs->size() == steps->size()) {
            for (size_t i = 0; i < ivs->size(); ++i) {
              if ((*ivs)[i] != barg)
                continue;
              if (Value lbV = (*lbs)[i].dyn_cast<Value>())
                if (!enqueueVal(lbV))
                  return false;
              if (Value ubV = (*ubs)[i].dyn_cast<Value>())
                if (!enqueueVal(ubV))
                  return false;
              if (Value stV = (*steps)[i].dyn_cast<Value>())
                if (!enqueueVal(stV))
                  return false;
              return true;
            }
          }
        }
      }
    }

    err = "cannot trace memory provenance through non-whitelisted BlockArgument";
    return false;
  };

  enqueueOp(firstOp);
  while (!opWorklist.empty()) {
    Operation *cur = opWorklist.pop_back_val();
    if (!collectOpReadEffects(cur, locations, seenLocs, readOps, seenReadOps,
                              hasGlobalRead, err))
      return false;
    for (Value operand : cur->getOperands())
      if (!enqueueVal(operand))
        return false;
    bool nestedOk = true;
    cur->walk([&](Operation *nested) {
      for (Value operand : nested->getOperands()) {
        if (!enqueueVal(operand)) {
          nestedOk = false;
          return WalkResult::interrupt();
        }
      }
      return WalkResult::advance();
    });
    if (!nestedOk || !err.empty())
      return false;
  }
  return true;
}

bool checkInterferenceOnOp(Operation *op, llvm::ArrayRef<Value> locations,
                           bool hasGlobalRead, AliasAnalysis &aa,
                           ReplayProof &out) {
  bool ok = true;
  op->walk([&](Operation *leaf) {
    auto mem = llvm::dyn_cast<MemoryEffectOpInterface>(leaf);
    if (!mem) {
      if (leaf->getNumRegions() > 0 &&
          leaf->hasTrait<OpTrait::HasRecursiveMemoryEffects>())
        return WalkResult::advance();
      out.clobberOp = leaf;
      out.reason = "intervening operation lacks MemoryEffectOpInterface: " +
                   leaf->getName().getStringRef().str();
      ok = false;
      return WalkResult::interrupt();
    }
    llvm::SmallVector<MemoryEffects::EffectInstance, 4> effects;
    mem.getEffects(effects);
    bool anyMod = false;
    for (const auto &e : effects)
      if (llvm::isa<MemoryEffects::Write, MemoryEffects::Free>(e.getEffect()))
        anyMod = true;
    if (!anyMod)
      return WalkResult::advance();
    if (hasGlobalRead) {
      out.clobberOp = leaf;
      out.reason =
          "intervening write/free conflicts with unscoped memory read";
      ok = false;
      return WalkResult::interrupt();
    }
    for (Value loc : locations) {
      ModRefResult mr = aa.getModRef(leaf, loc);
      if (mr.isMod()) {
        out.clobberOp = leaf;
        out.aliasedLocation = loc;
        out.reason = "intervening operation may modify/alias replayed value: " +
                     leaf->getName().getStringRef().str();
        ok = false;
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  return ok;
}

Region *findCommonRegion(Operation *a, Operation *b) {
  llvm::SmallPtrSet<Region *, 8> ancestors;
  for (Region *r = a->getParentRegion(); r != nullptr;
       r = r->getParentOp() ? r->getParentOp()->getParentRegion() : nullptr)
    ancestors.insert(r);
  for (Region *r = b->getParentRegion(); r != nullptr;
       r = r->getParentOp() ? r->getParentOp()->getParentRegion() : nullptr)
    if (ancestors.contains(r))
      return r;
  return nullptr;
}

bool checkPathBetween(Operation *startOp, Operation *replayOp,
                      llvm::ArrayRef<Value> locations, bool hasGlobalRead,
                      AliasAnalysis &aa, ReplayProof &out) {
  if (startOp == replayOp)
    return true;
  Region *commonRegion = findCommonRegion(startOp, replayOp);
  if (!commonRegion || !commonRegion->getParentOp()) {
    out.reason = "read source and replayOp do not share an enclosing region";
    return false;
  }

  Operation *startTop = startOp;
  while (startTop->getParentRegion() != commonRegion)
    startTop = startTop->getParentOp();
  Operation *replayTop = replayOp;
  while (replayTop->getParentRegion() != commonRegion)
    replayTop = replayTop->getParentOp();

  if (startTop->getBlock() != replayTop->getBlock() ||
      !commonRegion->hasOneBlock()) {
    for (Block &blk : *commonRegion)
      for (Operation &op : blk)
        if (&op != startOp && &op != replayOp)
          if (!checkInterferenceOnOp(&op, locations, hasGlobalRead, aa, out))
            return false;
    return true;
  }

  if (startTop == replayTop || !startTop->isBeforeInBlock(replayTop)) {
    out.reason =
        "read operation does not precede replayOp in their common region";
    return false;
  }

  if (startOp != startTop) {
    for (Operation *enc = commonRegion->getParentOp(); enc != nullptr;
         enc = enc->getParentOp()) {
      if (llvm::isa<LoopLikeOpInterface, IndexedRegionOpInterface>(enc)) {
        if (!checkInterferenceOnOp(enc, locations, hasGlobalRead, aa, out))
          return false;
        break;
      }
    }
  }

  for (Operation *cur = startOp; cur != startTop; cur = cur->getParentOp()) {
    Operation *parent = cur->getParentOp();
    if (llvm::isa<LoopLikeOpInterface, IndexedRegionOpInterface>(parent) ||
        !cur->getParentRegion()->hasOneBlock()) {
      if (!checkInterferenceOnOp(parent, locations, hasGlobalRead, aa, out))
        return false;
      continue;
    }
    for (Operation *after = cur->getNextNode(); after != nullptr;
         after = after->getNextNode())
      if (!checkInterferenceOnOp(after, locations, hasGlobalRead, aa, out))
        return false;
  }

  for (Operation *mid = startTop->getNextNode(); mid != replayTop;
       mid = mid->getNextNode()) {
    if (!checkInterferenceOnOp(mid, locations, hasGlobalRead, aa, out))
      return false;
  }

  for (Operation *cur = replayOp; cur != replayTop; cur = cur->getParentOp()) {
    Operation *parent = cur->getParentOp();
    if (llvm::isa<LoopLikeOpInterface, IndexedRegionOpInterface>(parent) ||
        !cur->getParentRegion()->hasOneBlock()) {
      if (!checkInterferenceOnOp(parent, locations, hasGlobalRead, aa, out))
        return false;
      continue;
    }
    for (Operation *before = &cur->getBlock()->front(); before != cur;
         before = before->getNextNode())
      if (!checkInterferenceOnOp(before, locations, hasGlobalRead, aa, out))
        return false;
  }

  return true;
}
} // namespace

ReplayProof compilerinfra::proveValueReplay(
    Operation *firstOp, Operation *replayOp,
    llvm::ArrayRef<Value> readLocations, AliasAnalysis *aliasAnalysis) {
  ReplayProof out;
  if (!firstOp || !replayOp) {
    out.reason = "null firstOp or replayOp";
    return out;
  }
  if (firstOp == replayOp) {
    out.reason = "firstOp and replayOp must be distinct operations";
    return out;
  }

  Region *commonRegion = findCommonRegion(firstOp, replayOp);
  if (!commonRegion || !commonRegion->getParentOp()) {
    out.reason = "firstOp and replayOp do not share an enclosing region";
    return out;
  }
  Operation *scopeRoot = commonRegion->getParentOp();
  while (scopeRoot->getParentOp() &&
         !scopeRoot->hasTrait<OpTrait::IsIsolatedFromAbove>())
    scopeRoot = scopeRoot->getParentOp();

  std::unique_ptr<AliasAnalysis> localAA;
  if (!aliasAnalysis) {
    localAA = std::make_unique<AliasAnalysis>(scopeRoot);
    localAA->addAnalysisImplementation(LocalAliasAnalysis());
    aliasAnalysis = localAA.get();
  }

  llvm::SmallVector<Value, 4> locations;
  llvm::SmallVector<Operation *, 4> startOps;
  bool hasGlobalRead = false;

  if (!readLocations.empty()) {
    for (Value v : readLocations) {
      if (!v) {
        out.reason = "null Value in explicit readLocations";
        return out;
      }
      locations.push_back(v);
    }
    startOps.push_back(firstOp);
  } else {
    if (!collectTransitiveReads(firstOp, scopeRoot, locations, startOps,
                                hasGlobalRead, out.reason))
      return out;
    if (locations.empty() && !hasGlobalRead) {
      out.proven = true;
      return out;
    }
    if (llvm::find(startOps, firstOp) == startOps.end())
      startOps.push_back(firstOp);
  }

  for (Operation *srcOp : startOps) {
    if (!checkPathBetween(srcOp, replayOp, locations, hasGlobalRead,
                          *aliasAnalysis, out))
      return out;
  }

  out.proven = true;
  return out;
}

namespace {
struct SRange {
  APInt lo, hi;
};

std::optional<SRange> resolveSignedBounds(std::optional<int64_t> cst, Value dyn,
                                          const IntegerRangeContext *ranges) {
  constexpr unsigned W = 128;
  if (cst)
    return SRange{APInt(W, *cst, /*isSigned=*/true),
                  APInt(W, *cst, /*isSigned=*/true)};
  if (!dyn || !ranges)
    return std::nullopt;
  auto r = ranges->query(dyn);
  if (!r || r->smin().getBitWidth() == 0)
    return std::nullopt;
  unsigned bw = r->smin().getBitWidth();
  if (r->smin().isMinSignedValue() && r->smax().isMaxSignedValue() && bw >= 32)
    return std::nullopt;
  return SRange{r->smin().sext(W), r->smax().sext(W)};
}

bool opCannotWrapSigned(Operation *op, const IntegerRangeContext *ranges) {
  if (!ranges)
    return false;
  auto lhsR = ranges->query(op->getOperand(0));
  auto rhsR = ranges->query(op->getOperand(1));
  if (!lhsR || !rhsR)
    return false;
  unsigned bw =
      ConstantIntRanges::getStorageBitwidth(op->getResult(0).getType());
  if (bw == 0 || bw > 64)
    return false;
  unsigned extW = bw + 66;
  APInt lMin = lhsR->smin().sext(extW), lMax = lhsR->smax().sext(extW);
  APInt rMin = rhsR->smin().sext(extW), rMax = rhsR->smax().sext(extW);
  APInt wMin(extW, 0), wMax(extW, 0);
  if (llvm::isa<arith::AddIOp>(op)) {
    wMin = lMin + rMin;
    wMax = lMax + rMax;
  } else if (llvm::isa<arith::SubIOp>(op)) {
    wMin = lMin - rMax;
    wMax = lMax - rMin;
  } else if (llvm::isa<arith::MulIOp>(op)) {
    APInt p1 = lMin * rMin, p2 = lMin * rMax, p3 = lMax * rMin,
          p4 = lMax * rMax;
    wMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                llvm::APIntOps::smin(p3, p4));
    wMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                llvm::APIntOps::smax(p3, p4));
  } else if (llvm::isa<arith::ShLIOp>(op)) {
    if (rMin.isNegative() || rMax.uge(bw) || rhsR->umax().uge(bw))
      return false;
    unsigned s0 = static_cast<unsigned>(rMin.getZExtValue());
    unsigned s1 = static_cast<unsigned>(rMax.getZExtValue());
    APInt p1 = lMin.shl(s0), p2 = lMin.shl(s1), p3 = lMax.shl(s0),
          p4 = lMax.shl(s1);
    wMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                llvm::APIntOps::smin(p3, p4));
    wMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                llvm::APIntOps::smax(p3, p4));
  } else {
    return false;
  }
  APInt minT = APInt::getSignedMinValue(bw).sext(extW);
  APInt maxT = APInt::getSignedMaxValue(bw).sext(extW);
  return wMin.sge(minT) && wMax.sle(maxT);
}

bool isProvenMultipleOf(Value v, int64_t align,
                        const IntegerRangeContext *ranges, unsigned depth = 0) {
  if (!v || align <= 0 || depth > 16)
    return false;
  if (align == 1)
    return true;
  if (auto c = matchScalarConstInt(v))
    return (*c % align) == 0;
  if (!ranges)
    return false;

  if (auto barg = llvm::dyn_cast<BlockArgument>(v)) {
    auto vr = ranges->query(v);
    if (!vr || vr->smin().isNegative())
      return false;
    Operation *parent =
        barg.getOwner()->getParent() ? barg.getOwner()->getParentOp() : nullptr;
    if (auto idxReg = llvm::dyn_cast_or_null<IndexedRegionOpInterface>(parent)) {
      RegionIndexBounds rb;
      if (mlir::succeeded(idxReg.getRegionArgBounds(barg, rb))) {
        bool lbOk = false, stOk = false;
        if (rb.staticLower)
          lbOk = *rb.staticLower >= 0 && (*rb.staticLower % align) == 0;
        else if (rb.dynamicLower) {
          auto lbR = ranges->query(rb.dynamicLower);
          lbOk = lbR && !lbR->smin().isNegative() &&
                 isProvenMultipleOf(rb.dynamicLower, align, ranges, depth + 1);
        }
        if (rb.staticStep)
          stOk = *rb.staticStep > 0 && (*rb.staticStep % align) == 0;
        else if (rb.dynamicStep) {
          auto stR = ranges->query(rb.dynamicStep);
          stOk = stR && stR->smin().sgt(0) &&
                 isProvenMultipleOf(rb.dynamicStep, align, ranges, depth + 1);
        }
        return lbOk && stOk;
      }
    }
    if (auto loop = llvm::dyn_cast_or_null<LoopLikeOpInterface>(parent)) {
      auto ivs = loop.getLoopInductionVars();
      auto lbs = loop.getLoopLowerBounds();
      auto steps = loop.getLoopSteps();
      if (ivs && lbs && steps && ivs->size() == lbs->size() &&
          ivs->size() == steps->size()) {
        for (size_t i = 0; i < ivs->size(); ++i) {
          if ((*ivs)[i] != barg)
            continue;
          auto lbVal = (*lbs)[i].dyn_cast<Value>();
          auto lbAttr = (*lbs)[i].dyn_cast<Attribute>();
          auto stVal = (*steps)[i].dyn_cast<Value>();
          auto stAttr = (*steps)[i].dyn_cast<Attribute>();
          bool lbOk = false, stOk = false;
          if (lbVal) {
            auto lbR = ranges->query(lbVal);
            lbOk = lbR && !lbR->smin().isNegative() &&
                   isProvenMultipleOf(lbVal, align, ranges, depth + 1);
          } else if (auto ia = llvm::dyn_cast_or_null<IntegerAttr>(lbAttr)) {
            lbOk = ia.getInt() >= 0 && (ia.getInt() % align) == 0;
          }
          if (stVal) {
            auto stR = ranges->query(stVal);
            stOk = stR && stR->smin().sgt(0) &&
                   isProvenMultipleOf(stVal, align, ranges, depth + 1);
          } else if (auto ia = llvm::dyn_cast_or_null<IntegerAttr>(stAttr)) {
            stOk = ia.getInt() > 0 && (ia.getInt() % align) == 0;
          }
          return lbOk && stOk;
        }
      }
    }
    return false;
  }

  Operation *def = v.getDefiningOp();
  if (!def)
    return false;
  if (auto mul = llvm::dyn_cast<arith::MulIOp>(def)) {
    if (!opCannotWrapSigned(def, ranges))
      return false;
    return isProvenMultipleOf(mul.getLhs(), align, ranges, depth + 1) ||
           isProvenMultipleOf(mul.getRhs(), align, ranges, depth + 1);
  }
  if (auto shl = llvm::dyn_cast<arith::ShLIOp>(def)) {
    if (!opCannotWrapSigned(def, ranges))
      return false;
    if (auto sc = matchScalarConstInt(shl.getRhs());
        sc && *sc >= 0 && *sc < 62 && ((int64_t(1) << *sc) % align) == 0)
      return true;
    return false;
  }
  if (llvm::isa<arith::AddIOp, arith::SubIOp>(def)) {
    if (!opCannotWrapSigned(def, ranges))
      return false;
    return isProvenMultipleOf(def->getOperand(0), align, ranges, depth + 1) &&
           isProvenMultipleOf(def->getOperand(1), align, ranges, depth + 1);
  }
  if (auto sel = llvm::dyn_cast<arith::SelectOp>(def)) {
    return isProvenMultipleOf(sel.getTrueValue(), align, ranges, depth + 1) &&
           isProvenMultipleOf(sel.getFalseValue(), align, ranges, depth + 1);
  }
  if (llvm::isa<arith::ExtSIOp, arith::IndexCastOp>(def)) {
    Value src = def->getOperand(0);
    unsigned srcW = ConstantIntRanges::getStorageBitwidth(src.getType());
    unsigned dstW = ConstantIntRanges::getStorageBitwidth(v.getType());
    if (srcW == 0 || dstW == 0)
      return false;
    if (dstW < srcW) {
      auto sr = ranges->query(src);
      if (!sr || !fitsSignedWidth(*sr, dstW, false, 1))
        return false;
    }
    return isProvenMultipleOf(src, align, ranges, depth + 1);
  }
  if (llvm::isa<arith::ExtUIOp, arith::IndexCastUIOp>(def)) {
    Value src = def->getOperand(0);
    unsigned srcW = ConstantIntRanges::getStorageBitwidth(src.getType());
    unsigned dstW = ConstantIntRanges::getStorageBitwidth(v.getType());
    if (srcW == 0 || dstW == 0)
      return false;
    auto sr = ranges->query(src);
    if (!sr ||
        !fitsUnsignedWidth(*sr, std::min(srcW, dstW),
                           /*mustAlsoFitSignedPositive=*/true, 1))
      return false;
    return isProvenMultipleOf(src, align, ranges, depth + 1);
  }
  return false;
}

bool verifyOffsetAlignment(const TailDimension &d, int64_t align,
                           const IntegerRangeContext *ranges) {
  if (align <= 0)
    return false;
  if (d.staticOffset && !d.dynamicOffset)
    return *d.staticOffset >= 0 && (*d.staticOffset % align) == 0;
  if (d.dynamicOffset && !d.staticOffset) {
    if (!ranges)
      return false;
    auto r = ranges->query(d.dynamicOffset);
    if (!r || r->smin().isNegative())
      return false;
    return isProvenMultipleOf(d.dynamicOffset, align, ranges);
  }
  return false;
}

Value stripLosslessSignedCast(Value v) {
  while (v) {
    Operation *def = v.getDefiningOp();
    if (!def || !llvm::isa<arith::ExtSIOp, arith::IndexCastOp>(def))
      break;
    unsigned srcW =
        ConstantIntRanges::getStorageBitwidth(def->getOperand(0).getType());
    unsigned dstW = ConstantIntRanges::getStorageBitwidth(v.getType());
    if (srcW == 0 || dstW < srcW)
      break;
    v = def->getOperand(0);
  }
  return v;
}

Value matchSplatScalar(Value v, int64_t expectedLen) {
  auto st = llvm::dyn_cast<ShapedType>(v.getType());
  if (!st || st.getRank() != 1 || st.getDimSize(0) != expectedLen)
    return {};
  Operation *def = v.getDefiningOp();
  if (!def)
    return {};
  StringRef name = def->getName().getStringRef();
  if ((name == "vector.splat" || name == "tensor.splat" ||
       name == "vector.broadcast") &&
      def->getNumOperands() == 1 &&
      llvm::isa<IntegerType, IndexType>(def->getOperand(0).getType()))
    return def->getOperand(0);
  return {};
}

std::optional<int64_t> matchSplatConstInt(Value v, int64_t expectedLen) {
  if (expectedLen == 1 && llvm::isa<IntegerType, IndexType>(v.getType()))
    return matchScalarConstInt(v);
  if (Value s = matchSplatScalar(v, expectedLen))
    return matchScalarConstInt(s);
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    if (auto dense = llvm::dyn_cast<DenseIntElementsAttr>(c.getValue())) {
      auto st = llvm::dyn_cast<ShapedType>(v.getType());
      if (st && st.getRank() == 1 && st.getDimSize(0) == expectedLen &&
          dense.isSplat()) {
        APInt val = dense.getSplatValue<APInt>();
        if (val.getSignificantBits() <= 64)
          return val.getSExtValue();
      }
    }
  }
  return std::nullopt;
}

bool extractRampParameters(Value v, int64_t expectedLen, int64_t &baseOut,
                           int64_t &stepOut) {
  auto st = llvm::dyn_cast<ShapedType>(v.getType());
  if (!st || st.getRank() != 1 || st.getDimSize(0) != expectedLen ||
      expectedLen <= 0)
    return false;
  Operation *def = v.getDefiningOp();
  if (!def)
    return false;
  if (def->getName().getStringRef() == "vector.step") {
    baseOut = 0;
    stepOut = 1;
    return true;
  }
  if (auto c = llvm::dyn_cast<arith::ConstantOp>(def)) {
    if (auto dense = llvm::dyn_cast<DenseIntElementsAttr>(c.getValue())) {
      if (dense.getNumElements() != expectedLen)
        return false;
      auto vals = dense.getValues<APInt>();
      auto it = vals.begin();
      if ((*it).getSignificantBits() > 64)
        return false;
      int64_t first = (*it).getSExtValue();
      if (expectedLen == 1) {
        baseOut = first;
        stepOut = 1;
        return true;
      }
      ++it;
      if ((*it).getSignificantBits() > 64)
        return false;
      int64_t second = (*it).getSExtValue();
      if (second <= first)
        return false;
      int64_t step = second - first;
      int64_t idx = 0;
      for (const APInt &elem : vals) {
        if (elem.getSignificantBits() > 64 ||
            elem.getSExtValue() != first + idx * step)
          return false;
        ++idx;
      }
      baseOut = first;
      stepOut = step;
      return true;
    }
  }
  return false;
}

bool matchRampVector(Value v, int64_t tileSize, int64_t baseOffset,
                     int64_t step) {
  int64_t b = 0, s = 0;
  return extractRampParameters(v, tileSize, b, s) && b == baseOffset &&
         s == step;
}

bool matchLaneCoordinates(Value coordVal, const TailDimension &d) {
  if (d.tileSize == 1 && llvm::isa<IntegerType, IndexType>(coordVal.getType())) {
    if (d.dynamicOffset)
      return stripLosslessSignedCast(coordVal) ==
             stripLosslessSignedCast(d.dynamicOffset);
    if (d.staticOffset) {
      auto c = matchScalarConstInt(coordVal);
      return c && *c == *d.staticOffset;
    }
    return false;
  }
  if (d.staticOffset && !d.dynamicOffset &&
      matchRampVector(coordVal, d.tileSize, *d.staticOffset, d.step))
    return true;

  auto add = coordVal.getDefiningOp<arith::AddIOp>();
  if (!add)
    return false;
  for (int side : {0, 1}) {
    Value baseVec = add->getOperand(side);
    Value rampVec = add->getOperand(1 - side);
    if (!matchRampVector(rampVec, d.tileSize, 0, d.step))
      continue;
    if (d.dynamicOffset) {
      Value splatSrc = matchSplatScalar(baseVec, d.tileSize);
      if (splatSrc && stripLosslessSignedCast(splatSrc) ==
                          stripLosslessSignedCast(d.dynamicOffset))
        return true;
    } else if (d.staticOffset) {
      auto cst = matchSplatConstInt(baseVec, d.tileSize);
      if (cst && *cst == *d.staticOffset)
        return true;
    }
  }
  return false;
}

bool matchExtentBound(Value boundVal, const TailDimension &d) {
  if (d.tileSize == 1 && llvm::isa<IntegerType, IndexType>(boundVal.getType())) {
    if (d.dynamicExtent && stripLosslessSignedCast(boundVal) ==
                               stripLosslessSignedCast(d.dynamicExtent))
      return true;
    if (d.staticExtent) {
      auto c = matchScalarConstInt(boundVal);
      return c && *c >= 0 && *c <= *d.staticExtent;
    }
    return false;
  }
  if (d.dynamicExtent) {
    Value splatSrc = matchSplatScalar(boundVal, d.tileSize);
    if (splatSrc && stripLosslessSignedCast(splatSrc) ==
                        stripLosslessSignedCast(d.dynamicExtent))
      return true;
  }
  if (d.staticExtent) {
    auto cst = matchSplatConstInt(boundVal, d.tileSize);
    if (cst && *cst >= 0 && *cst <= *d.staticExtent)
      return true;
  }
  return false;
}

bool verifyMaskPredicate(Value pred, const TailDimension &d,
                         const APInt &maxCoord128,
                         const std::optional<SRange> &extB,
                         unsigned depth = 0) {
  constexpr unsigned W = 128;
  if (!pred || depth > 8)
    return false;
  if (d.tileSize == 1) {
    if (!pred.getType().isInteger(1))
      if (auto st = llvm::dyn_cast<ShapedType>(pred.getType());
          !st || st.getRank() != 1 || st.getDimSize(0) != 1 ||
          !st.getElementType().isInteger(1))
        return false;
  } else {
    auto st = llvm::dyn_cast<ShapedType>(pred.getType());
    if (!st || st.getRank() != 1 || st.getDimSize(0) != d.tileSize ||
        !st.getElementType().isInteger(1))
      return false;
  }

  if (auto andOp = pred.getDefiningOp<arith::AndIOp>()) {
    return verifyMaskPredicate(andOp.getLhs(), d, maxCoord128, extB,
                               depth + 1) ||
           verifyMaskPredicate(andOp.getRhs(), d, maxCoord128, extB, depth + 1);
  }

  auto cmp = pred.getDefiningOp<arith::CmpIOp>();
  if (!cmp)
    return false;

  Value coordVal, boundVal;
  bool isUnsigned = false;
  switch (cmp.getPredicate()) {
  case arith::CmpIPredicate::slt:
    coordVal = cmp.getLhs();
    boundVal = cmp.getRhs();
    break;
  case arith::CmpIPredicate::ult:
    coordVal = cmp.getLhs();
    boundVal = cmp.getRhs();
    isUnsigned = true;
    break;
  case arith::CmpIPredicate::sgt:
    coordVal = cmp.getRhs();
    boundVal = cmp.getLhs();
    break;
  case arith::CmpIPredicate::ugt:
    coordVal = cmp.getRhs();
    boundVal = cmp.getLhs();
    isUnsigned = true;
    break;
  default:
    return false;
  }

  unsigned coordBits =
      ConstantIntRanges::getStorageBitwidth(leafScalarType(coordVal.getType()));
  if (coordBits == 0 || coordBits > 64 || !extB || extB->lo.isNegative())
    return false;

  if (isUnsigned) {
    APInt maxU = APInt::getMaxValue(coordBits).zext(W);
    if (maxCoord128.ugt(maxU) || extB->hi.ugt(maxU))
      return false;
  } else {
    APInt maxS = APInt::getSignedMaxValue(coordBits).sext(W);
    if (maxCoord128.sgt(maxS) || extB->hi.sgt(maxS))
      return false;
  }

  return matchLaneCoordinates(coordVal, d) && matchExtentBound(boundVal, d);
}

TailProof proveTailDimensionsImpl(llvm::ArrayRef<TailDimension> dimensions,
                                  const IntegerRangeContext *ranges) {
  TailProof out;
  if (dimensions.empty()) {
    out.reason = "no tail dimensions supplied";
    return out;
  }
  out.dimensionKinds.reserve(dimensions.size());
  TailKind worst = TailKind::ExactInBounds;

  for (auto [idx, d] : llvm::enumerate(dimensions)) {
    if (d.tileSize <= 0 || d.step <= 0) {
      out.failingDimension = idx;
      out.reason =
          "dimension " + std::to_string(idx) + ": tileSize and step must be > 0";
      return out;
    }
    auto offB = resolveSignedBounds(d.staticOffset, d.dynamicOffset, ranges);
    auto extB = resolveSignedBounds(d.staticExtent, d.dynamicExtent, ranges);

    if (offB && offB->lo.isNegative()) {
      out.failingDimension = idx;
      out.reason = "dimension " + std::to_string(idx) +
                   ": access offset may be negative";
      return out;
    }

    TailKind kind = TailKind::Unproven;
    constexpr unsigned W = 128;
    APInt span = APInt(W, d.tileSize - 1, true) * APInt(W, d.step, true);

    if (offB && extB) {
      APInt maxCoord = offB->hi + span;
      if (maxCoord.slt(extB->lo)) {
        kind = TailKind::ExactInBounds;
      } else if (d.staticExtent && d.step == 1 &&
                 *d.staticExtent >= d.tileSize &&
                 (*d.staticExtent % d.tileSize == 0) &&
                 (!d.tileAlignment || *d.tileAlignment == d.tileSize) &&
                 verifyOffsetAlignment(d, d.tileSize, ranges) &&
                 offB->hi.slt(APInt(W, *d.staticExtent, true))) {
        kind = TailKind::ExactInBounds;
      }
    }

    if (kind == TailKind::Unproven && offB && d.paddedCapacity) {
      APInt maxCoord = offB->hi + span;
      if (maxCoord.slt(APInt(W, *d.paddedCapacity, true)))
        kind = TailKind::PaddedSafe;
    }

    if (kind == TailKind::Unproven && d.hasExplicitMask && offB &&
        !offB->lo.isNegative()) {
      APInt maxCoord = offB->hi + span;
      if (verifyMaskPredicate(d.maskPredicate, d, maxCoord, extB))
        kind = TailKind::MaskedSafe;
    }

    out.dimensionKinds.push_back(kind);
    if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(worst))
      worst = kind;
    if (kind == TailKind::Unproven && !out.failingDimension) {
      out.failingDimension = idx;
      out.reason = "dimension " + std::to_string(idx) +
                   ": unmasked or unverified tile may exceed logical extent";
    }
  }

  out.kind = worst;
  out.proven = (worst != TailKind::Unproven);
  out.fallback =
      out.proven ? TailFallback::None : TailFallback::RequireMaskOrScalarTail;
  return out;
}

void resolveContainerExtentFromIR(Value container, unsigned dim,
                                  std::optional<int64_t> &staticExtent,
                                  Value &dynamicExtent) {
  if (staticExtent || dynamicExtent || !container)
    return;
  auto st = llvm::dyn_cast<ShapedType>(container.getType());
  if (!st || dim >= static_cast<unsigned>(st.getRank()))
    return;
  if (!st.isDynamicDim(dim)) {
    staticExtent = st.getDimSize(dim);
    return;
  }
  if (Operation *def = container.getDefiningOp()) {
    StringRef n = def->getName().getStringRef();
    if (n == "memref.alloc" || n == "memref.alloca" || n == "tensor.empty") {
      unsigned dynIdx = 0;
      for (unsigned d = 0; d < dim; ++d)
        if (st.isDynamicDim(d))
          ++dynIdx;
      if (dynIdx < def->getNumOperands()) {
        dynamicExtent = def->getOperand(dynIdx);
        return;
      }
    }
  }
  for (Operation *user : container.getUsers()) {
    StringRef un = user->getName().getStringRef();
    if ((un == "memref.dim" || un == "tensor.dim") &&
        user->getNumOperands() == 2 && user->getOperand(0) == container) {
      if (auto c = matchScalarConstInt(user->getOperand(1));
          c && *c == static_cast<int64_t>(dim)) {
        dynamicExtent = user->getResult(0);
        return;
      }
    }
  }
}

bool convertAccessAxisToTailDim(const AccessAxis &ax, TailDimension &out,
                                const IntegerRangeContext *ranges,
                                std::string &err) {
  out = TailDimension{};
  out.staticExtent = ax.staticExtent;
  out.dynamicExtent = ax.dynamicExtent;
  resolveContainerExtentFromIR(ax.container, ax.containerDim, out.staticExtent,
                               out.dynamicExtent);
  out.paddedCapacity = ax.paddedCapacity;
  if (ax.maskPredicate) {
    out.hasExplicitMask = true;
    out.maskPredicate = ax.maskPredicate;
  }

  if (ax.entireAxisFromZero && !ax.coord) {
    out.staticOffset = 0;
    if (ax.tileSize > 0)
      out.tileSize = ax.tileSize;
    else if (out.staticExtent && *out.staticExtent > 0)
      out.tileSize = *out.staticExtent;
    else {
      err = "entireAxisFromZero axis has non-positive or unknown tileSize";
      return false;
    }
    out.step = ax.step;
    return true;
  }

  if (!ax.coord) {
    err = "AccessAxis requires a non-null IR coordinate Value";
    return false;
  }

  if (auto st = llvm::dyn_cast<ShapedType>(ax.coord.getType())) {
    if (st.getRank() != 1 || st.getDimSize(0) <= 0 ||
        !llvm::isa<IntegerType, IndexType>(st.getElementType())) {
      err = "shaped coordinate Value must be a static 1D integer/index vector "
            "or tensor";
      return false;
    }
    int64_t len = st.getDimSize(0);
    int64_t rampBase = 0, rampStep = 0;
    if (extractRampParameters(ax.coord, len, rampBase, rampStep)) {
      out.staticOffset = rampBase;
      out.tileSize = len;
      out.step = rampStep;
      return true;
    }
    if (Value splatSrc = matchSplatScalar(ax.coord, len)) {
      if (auto c = matchScalarConstInt(splatSrc))
        out.staticOffset = *c;
      else
        out.dynamicOffset = splatSrc;
      out.tileSize = 1;
      out.step = 1;
      return true;
    }
    if (auto add = ax.coord.getDefiningOp<arith::AddIOp>()) {
      for (int side : {0, 1}) {
        Value baseVec = add->getOperand(side);
        Value rampVec = add->getOperand(1 - side);
        if (!extractRampParameters(rampVec, len, rampBase, rampStep) ||
            rampBase != 0)
          continue;
        if (Value splatSrc = matchSplatScalar(baseVec, len)) {
          if (auto c = matchScalarConstInt(splatSrc))
            out.staticOffset = *c;
          else
            out.dynamicOffset = splatSrc;
          out.tileSize = len;
          out.step = rampStep;
          return true;
        }
        if (auto cst = matchSplatConstInt(baseVec, len)) {
          out.staticOffset = *cst;
          out.tileSize = len;
          out.step = rampStep;
          return true;
        }
      }
    }
    // General 1D vector coordinate fallback: sound ONLY for unmasked in-bounds
    // range checks (`rng.smin >= 0 && rng.smax < extent`).
    if (!ax.maskPredicate && ranges) {
      out.dynamicOffset = ax.coord;
      out.tileSize = 1;
      out.step = 1;
      return true;
    }
    err = "unable to decompose shaped lane coordinate into base + ramp";
    return false;
  }

  if (!llvm::isa<IntegerType, IndexType>(ax.coord.getType())) {
    err = "coordinate Value is neither scalar nor 1D shaped integer/index";
    return false;
  }
  if (auto c = matchScalarConstInt(ax.coord))
    out.staticOffset = *c;
  else
    out.dynamicOffset = ax.coord;
  out.tileSize = ax.tileSize;
  out.step = ax.step;
  return true;
}

bool extractStandardAccessAxes(Operation *op,
                               llvm::SmallVectorImpl<AccessAxis> &axes,
                               const IntegerRangeContext *ranges,
                               std::string &err) {
  if (auto oss = llvm::dyn_cast<OffsetSizeAndStrideOpInterface>(op)) {
    StringRef opName = op->getName().getStringRef();
    Value container;
    if (opName == "tensor.insert_slice" ||
        opName == "tensor.parallel_insert_slice") {
      if (op->getNumOperands() >= 2)
        container = op->getOperand(1);
    } else if (op->getNumOperands() >= 1) {
      container = op->getOperand(0);
    }
    auto st = container ? llvm::dyn_cast<ShapedType>(container.getType())
                        : ShapedType{};
    auto offsets = oss.getMixedOffsets();
    auto sizes = oss.getMixedSizes();
    auto strides = oss.getMixedStrides();
    if (!st || offsets.size() != static_cast<size_t>(st.getRank()) ||
        sizes.size() != offsets.size() || strides.size() != offsets.size()) {
      err = "OffsetSizeAndStrideOpInterface rank mismatch on container";
      return false;
    }
    for (size_t d = 0; d < offsets.size(); ++d) {
      AccessAxis ax;
      ax.container = container;
      ax.containerDim = static_cast<unsigned>(d);
      if (llvm::isa<Value>(offsets[d])) {
        ax.coord = llvm::cast<Value>(offsets[d]);
      } else if (auto ia = llvm::dyn_cast_or_null<IntegerAttr>(
                     llvm::isa<Attribute>(offsets[d])
                         ? llvm::cast<Attribute>(offsets[d])
                         : Attribute{})) {
        if (ia.getInt() == 0 && llvm::isa<Attribute>(sizes[d]) &&
            !st.isDynamicDim(d) &&
            llvm::cast<IntegerAttr>(llvm::cast<Attribute>(sizes[d])).getInt() ==
                st.getDimSize(d)) {
          ax.entireAxisFromZero = true;
        } else if (ia.getInt() == 0) {
          ax.entireAxisFromZero = true;
        } else {
          // Non-zero attribute offset on OffsetSizeAndStrideOpInterface:
          // materialize directly via TailDimension in caller below!
          err = "__use_oss_direct__";
          return false;
        }
      } else {
        err = "unsupported mixed offset on OffsetSizeAndStrideOpInterface";
        return false;
      }

      if (auto szAttr = llvm::dyn_cast_or_null<IntegerAttr>(
              llvm::isa<Attribute>(sizes[d]) ? llvm::cast<Attribute>(sizes[d])
                                             : Attribute{})) {
        ax.tileSize = szAttr.getInt();
      } else if (llvm::isa<Value>(sizes[d])) {
        Value szV = llvm::cast<Value>(sizes[d]);
        if (!ranges) {
          err = "dynamic slice size requires IntegerRangeContext";
          return false;
        }
        auto sr = ranges->query(szV);
        if (!sr || sr->smin().sle(0) || sr->smax().getSignificantBits() > 62) {
          err = "dynamic slice size is non-positive or unbounded";
          return false;
        }
        ax.tileSize = sr->smax().getSExtValue();
      }

      if (auto stAttr = llvm::dyn_cast_or_null<IntegerAttr>(
              llvm::isa<Attribute>(strides[d])
                  ? llvm::cast<Attribute>(strides[d])
                  : Attribute{})) {
        ax.step = stAttr.getInt();
      } else if (llvm::isa<Value>(strides[d])) {
        Value stV = llvm::cast<Value>(strides[d]);
        auto c = matchScalarConstInt(stV);
        if (!c || *c <= 0) {
          err = "dynamic non-constant slice stride cannot be proven";
          return false;
        }
        ax.step = *c;
      }
      axes.push_back(ax);
    }
    return true;
  }

  StringRef name = op->getName().getStringRef();
  if (name == "memref.load" || name == "tensor.extract") {
    if (op->getNumOperands() < 1)
      return false;
    Value base = op->getOperand(0);
    auto st = llvm::dyn_cast<ShapedType>(base.getType());
    auto indices = op->getOperands().drop_front(1);
    if (!st || indices.size() != static_cast<size_t>(st.getRank())) {
      err = "scalar load/extract rank mismatch";
      return false;
    }
    for (auto [d, idxV] : llvm::enumerate(indices)) {
      AccessAxis ax;
      ax.container = base;
      ax.containerDim = static_cast<unsigned>(d);
      ax.coord = idxV;
      ax.tileSize = 1;
      ax.step = 1;
      axes.push_back(ax);
    }
    return true;
  }

  if (name == "memref.store" || name == "tensor.insert") {
    if (op->getNumOperands() < 2)
      return false;
    Value base = op->getOperand(1);
    auto st = llvm::dyn_cast<ShapedType>(base.getType());
    auto indices = op->getOperands().drop_front(2);
    if (!st || indices.size() != static_cast<size_t>(st.getRank())) {
      err = "scalar store/insert rank mismatch";
      return false;
    }
    for (auto [d, idxV] : llvm::enumerate(indices)) {
      AccessAxis ax;
      ax.container = base;
      ax.containerDim = static_cast<unsigned>(d);
      ax.coord = idxV;
      ax.tileSize = 1;
      ax.step = 1;
      axes.push_back(ax);
    }
    return true;
  }

  if (name == "vector.load" || name == "vector.store" ||
      name == "vector.maskedload" || name == "vector.maskedstore") {
    bool isStore = (name == "vector.store");
    Value base = op->getOperand(isStore ? 1 : 0);
    auto st = llvm::dyn_cast<ShapedType>(base.getType());
    Type vecTyRaw = (name == "vector.load" || name == "vector.maskedload")
                        ? op->getResult(0).getType()
                        : (name == "vector.store"
                               ? op->getOperand(0).getType()
                               : op->getOperand(op->getNumOperands() - 1).getType());
    auto vt = llvm::dyn_cast<VectorType>(vecTyRaw);
    if (!st || !vt || vt.isScalable() || vt.getRank() > st.getRank()) {
      err = "unsupported or scalable vector load/store type";
      return false;
    }
    unsigned rank = static_cast<unsigned>(st.getRank());
    unsigned idxStart = isStore ? 2 : 1;
    if (op->getNumOperands() < idxStart + rank) {
      err = "vector load/store operand count mismatch";
      return false;
    }
    Value mask;
    if (name == "vector.maskedload" || name == "vector.maskedstore") {
      if (vt.getRank() != 1) {
        err = "multi-dimensional maskedload/maskedstore not modeled";
        return false;
      }
      mask = op->getOperand(idxStart + rank);
    }
    unsigned vecOffset = rank - static_cast<unsigned>(vt.getRank());
    for (unsigned d = 0; d < rank; ++d) {
      AccessAxis ax;
      ax.container = base;
      ax.containerDim = d;
      ax.coord = op->getOperand(idxStart + d);
      ax.tileSize = (d >= vecOffset) ? vt.getDimSize(d - vecOffset) : 1;
      ax.step = 1;
      if (mask && d == rank - 1)
        ax.maskPredicate = mask;
      axes.push_back(ax);
    }
    return true;
  }

  if (name == "vector.transfer_read" || name == "vector.transfer_write") {
    bool isWrite = (name == "vector.transfer_write");
    Value base = op->getOperand(isWrite ? 1 : 0);
    auto st = llvm::dyn_cast<ShapedType>(base.getType());
    auto vt = llvm::dyn_cast<VectorType>(
        isWrite ? op->getOperand(0).getType() : op->getResult(0).getType());
    auto permAttr = op->getAttrOfType<AffineMapAttr>("permutation_map");
    if (!st || !vt || vt.isScalable() || !permAttr) {
      err = "unsupported vector.transfer_read/write type or permutation_map";
      return false;
    }
    unsigned rank = static_cast<unsigned>(st.getRank());
    unsigned idxStart = isWrite ? 2 : 1;
    if (op->getNumOperands() < idxStart + rank) {
      err = "vector.transfer operand count mismatch";
      return false;
    }
    Value mask;
    unsigned maskPos = isWrite ? (2 + rank) : (2 + rank);
    if (auto segs = op->getAttrOfType<DenseI32ArrayAttr>("operandSegmentSizes")) {
      if (segs.size() >= 4 && segs[isWrite ? 3 : 3] == 1 &&
          maskPos < op->getNumOperands())
        mask = op->getOperand(maskPos);
    }
    llvm::SmallVector<int64_t, 4> axisTile(rank, 1);
    AffineMap pm = permAttr.getValue();
    for (unsigned r = 0; r < pm.getNumResults(); ++r) {
      AffineExpr expr = pm.getResult(r);
      if (auto dimExpr = llvm::dyn_cast<AffineDimExpr>(expr)) {
        unsigned d = dimExpr.getPosition();
        if (d >= rank) {
          err = "invalid permutation_map dim index";
          return false;
        }
        axisTile[d] = std::max<int64_t>(axisTile[d], vt.getDimSize(r));
      } else if (auto cstExpr = llvm::dyn_cast<AffineConstantExpr>(expr)) {
        if (cstExpr.getValue() != 0) {
          err = "unsupported non-zero constant in permutation_map";
          return false;
        }
      } else {
        err = "non-projected permutation_map in vector.transfer";
        return false;
      }
    }
    if (mask && vt.getRank() != 1) {
      err = "multi-dimensional masked vector.transfer not modeled";
      return false;
    }
    for (unsigned d = 0; d < rank; ++d) {
      AccessAxis ax;
      ax.container = base;
      ax.containerDim = d;
      ax.coord = op->getOperand(idxStart + d);
      ax.tileSize = axisTile[d];
      ax.step = 1;
      if (mask && pm.getNumResults() == 1 &&
          llvm::isa<AffineDimExpr>(pm.getResult(0)) &&
          llvm::cast<AffineDimExpr>(pm.getResult(0)).getPosition() == d)
        ax.maskPredicate = mask;
      axes.push_back(ax);
    }
    return true;
  }

  err = "operation does not implement TiledAccessOpInterface or standard MLIR "
        "memory/slice access: " +
        name.str();
  return false;
}

bool extractOssDirectTailDims(OffsetSizeAndStrideOpInterface oss,
                              llvm::SmallVectorImpl<TailDimension> &dims,
                              const IntegerRangeContext *ranges,
                              std::string &err) {
  Operation *op = oss.getOperation();
  StringRef opName = op->getName().getStringRef();
  Value container = (opName == "tensor.insert_slice" ||
                     opName == "tensor.parallel_insert_slice")
                        ? op->getOperand(1)
                        : op->getOperand(0);
  auto st = llvm::dyn_cast<ShapedType>(container.getType());
  auto offsets = oss.getMixedOffsets();
  auto sizes = oss.getMixedSizes();
  auto strides = oss.getMixedStrides();
  if (!st || offsets.size() != static_cast<size_t>(st.getRank())) {
    err = "OffsetSizeAndStrideOpInterface rank mismatch";
    return false;
  }
  for (size_t d = 0; d < offsets.size(); ++d) {
    TailDimension td;
    resolveContainerExtentFromIR(container, static_cast<unsigned>(d),
                                 td.staticExtent, td.dynamicExtent);
    if (llvm::isa<Value>(offsets[d])) {
      Value offV = llvm::cast<Value>(offsets[d]);
      if (auto c = matchScalarConstInt(offV))
        td.staticOffset = *c;
      else
        td.dynamicOffset = offV;
    } else if (auto ia = llvm::dyn_cast_or_null<IntegerAttr>(
                   llvm::isa<Attribute>(offsets[d])
                       ? llvm::cast<Attribute>(offsets[d])
                       : Attribute{})) {
      td.staticOffset = ia.getInt();
    } else {
      err = "unsupported offset on OffsetSizeAndStrideOpInterface";
      return false;
    }

    if (auto szAttr = llvm::dyn_cast_or_null<IntegerAttr>(
            llvm::isa<Attribute>(sizes[d]) ? llvm::cast<Attribute>(sizes[d])
                                           : Attribute{})) {
      td.tileSize = szAttr.getInt();
    } else if (llvm::isa<Value>(sizes[d])) {
      Value szV = llvm::cast<Value>(sizes[d]);
      if (!ranges) {
        err = "dynamic slice size requires IntegerRangeContext";
        return false;
      }
      auto sr = ranges->query(szV);
      if (!sr || sr->smin().sle(0) || sr->smax().getSignificantBits() > 62) {
        err = "dynamic slice size is non-positive or unbounded";
        return false;
      }
      td.tileSize = sr->smax().getSExtValue();
    }

    if (auto stAttr = llvm::dyn_cast_or_null<IntegerAttr>(
            llvm::isa<Attribute>(strides[d]) ? llvm::cast<Attribute>(strides[d])
                                             : Attribute{})) {
      td.step = stAttr.getInt();
    } else if (llvm::isa<Value>(strides[d])) {
      Value stV = llvm::cast<Value>(strides[d]);
      auto c = matchScalarConstInt(stV);
      if (!c || *c <= 0) {
        err = "dynamic non-constant slice stride cannot be proven";
        return false;
      }
      td.step = *c;
    }
    dims.push_back(td);
  }
  return true;
}
} // namespace

TailProof compilerinfra::proveTailAccess(Operation *accessOp,
                                         const IntegerRangeContext *ranges) {
  TailProof out;
  if (!accessOp) {
    out.reason = "null accessOp";
    return out;
  }
  Operation *scopeRoot = findEnclosingProofScope(accessOp);
  std::unique_ptr<IntegerRangeContext> localSolver;
  if (!contextCoversScope(ranges, scopeRoot)) {
    localSolver = std::make_unique<IntegerRangeContext>(scopeRoot);
    ranges = localSolver.get();
  }
  if (!ranges || !ranges->valid()) {
    out.reason = "IntegerRangeAnalysis failed to converge on enclosing scope";
    return out;
  }

  llvm::SmallVector<TailDimension, 4> dims;
  if (auto tiled = llvm::dyn_cast<TiledAccessOpInterface>(accessOp)) {
    llvm::SmallVector<AccessAxis, 4> axes;
    if (mlir::failed(tiled.getAccessedAxes(axes)) || axes.empty()) {
      out.reason = "TiledAccessOpInterface::getAccessedAxes failed or empty";
      return out;
    }
    for (const auto &ax : axes) {
      TailDimension td;
      if (!convertAccessAxisToTailDim(ax, td, ranges, out.reason))
        return out;
      dims.push_back(td);
    }
    return proveTailDimensionsImpl(dims, ranges);
  }

  llvm::SmallVector<AccessAxis, 4> stdAxes;
  std::string err;
  if (!extractStandardAccessAxes(accessOp, stdAxes, ranges, err)) {
    if (err == "__use_oss_direct__") {
      if (!extractOssDirectTailDims(
              llvm::cast<OffsetSizeAndStrideOpInterface>(accessOp), dims,
              ranges, out.reason))
        return out;
      return proveTailDimensionsImpl(dims, ranges);
    }
    out.reason = std::move(err);
    return out;
  }
  for (const auto &ax : stdAxes) {
    TailDimension td;
    if (!convertAccessAxisToTailDim(ax, td, ranges, out.reason))
      return out;
    dims.push_back(td);
  }
  return proveTailDimensionsImpl(dims, ranges);
}

TailProof compilerinfra::proveTailAccess(Value addressOrSliceValue,
                                         const IntegerRangeContext *ranges) {
  TailProof out;
  if (!addressOrSliceValue) {
    out.reason = "null addressOrSliceValue";
    return out;
  }
  Operation *def = addressOrSliceValue.getDefiningOp();
  if (!def ||
      !llvm::isa<TiledAccessOpInterface, OffsetSizeAndStrideOpInterface>(def)) {
    out.reason = "value defining op does not implement TiledAccessOpInterface "
                 "or OffsetSizeAndStrideOpInterface; pass the access Operation* "
                 "or (container, coordinates)";
    return out;
  }
  return proveTailAccess(def, ranges);
}

TailProof compilerinfra::proveTailAccess(Value container,
                                         llvm::ArrayRef<Value> coordinates,
                                         Value maskPredicate,
                                         const IntegerRangeContext *ranges) {
  TailProof out;
  if (!container || coordinates.empty()) {
    out.reason = "null container or empty coordinates";
    return out;
  }
  auto st = llvm::dyn_cast<ShapedType>(container.getType());
  if (!st || coordinates.size() != static_cast<size_t>(st.getRank())) {
    out.reason = "container is not a ShapedType or coordinate rank mismatches";
    return out;
  }

  Operation *scopeRoot = nullptr;
  for (Value c : coordinates) {
    if (c && c.getDefiningOp()) {
      scopeRoot = findEnclosingProofScope(c);
      break;
    }
  }
  if (!scopeRoot && maskPredicate && maskPredicate.getDefiningOp())
    scopeRoot = findEnclosingProofScope(maskPredicate);
  if (!scopeRoot)
    scopeRoot = findEnclosingProofScope(container);

  std::unique_ptr<IntegerRangeContext> localSolver;
  if (!contextCoversScope(ranges, scopeRoot)) {
    localSolver = std::make_unique<IntegerRangeContext>(scopeRoot);
    ranges = localSolver.get();
  }
  if (!ranges || !ranges->valid()) {
    out.reason = "IntegerRangeAnalysis failed to converge on enclosing scope";
    return out;
  }

  bool maskAssigned = false;
  llvm::SmallVector<TailDimension, 4> dims;
  for (unsigned d = 0; d < static_cast<unsigned>(st.getRank()); ++d) {
    AccessAxis ax;
    ax.container = container;
    ax.containerDim = d;
    ax.coord = coordinates[d];
    if (maskPredicate &&
        (st.getRank() == 1 || llvm::isa<ShapedType>(coordinates[d].getType()))) {
      ax.maskPredicate = maskPredicate;
      maskAssigned = true;
    }
    TailDimension td;
    if (!convertAccessAxisToTailDim(ax, td, ranges, out.reason))
      return out;
    dims.push_back(td);
  }
  if (maskPredicate && !maskAssigned) {
    out.reason = "maskPredicate could not be associated with a unique lane axis";
    return out;
  }
  return proveTailDimensionsImpl(dims, ranges);
}

TailProof compilerinfra::proveTailAccess(llvm::ArrayRef<TailDimension> dimensions,
                                         const IntegerRangeContext *ranges) {
  return proveTailDimensionsImpl(dimensions, ranges);
}
