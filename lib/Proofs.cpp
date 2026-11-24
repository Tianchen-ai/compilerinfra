#include "compilerinfra/Proofs.h"
#include "mlir/Analysis/AliasAnalysis/LocalAliasAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/IntegerRangeAnalysis.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace compilerinfra;
using namespace mlir;

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
  if (!succeeded || !value)
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
  const auto *lattice =
      solver->lookupState<dataflow::IntegerValueRangeLattice>(value);
  if (!lattice || lattice->getValue().isUninitialized())
    return std::nullopt;
  return lattice->getValue().getValue();
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

bool rangeFitsTarget(const ConstantIntRanges &r,
                     const AddressProofOptions &opts, uint64_t scale) {
  unsigned w = r.smin().getBitWidth();
  if (w == 0 || opts.targetBitwidth == 0 || opts.targetBitwidth > 64)
    return false;
  unsigned calcW = std::max(w, opts.targetBitwidth) + 65;
  APInt scaleAP(calcW, scale == 0 ? 1 : scale, /*isSigned=*/false);

  if (opts.isSigned) {
    APInt lo = r.smin().sext(calcW) * scaleAP;
    APInt hi = r.smax().sext(calcW) * scaleAP;
    APInt minT = APInt::getSignedMinValue(opts.targetBitwidth).sext(calcW);
    APInt maxT = APInt::getSignedMaxValue(opts.targetBitwidth).sext(calcW);
    if (opts.requireNonNegative && lo.isNegative())
      return false;
    return lo.sge(minT) && hi.sle(maxT);
  }
  if (r.smin().isNegative())
    return false;
  APInt uhi = r.umax().zext(calcW) * scaleAP;
  APInt maxU = APInt::getMaxValue(opts.targetBitwidth).zext(calcW);
  return uhi.ule(maxU);
}

bool isCastOrTruncOp(Operation *op) {
  return llvm::isa<arith::IndexCastOp, arith::IndexCastUIOp, arith::TruncIOp>(
      op);
}
} // namespace

AddressProof compilerinfra::proveNarrowAddress(
    Operation *scopeRoot, llvm::ArrayRef<Value> addressValues,
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
  if (!existingRanges) {
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
        out.reason =
            "explicit address value is null or not an integer/index scalar or vector";
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
    // Default scan across ALL nested regions and blocks under scopeRoot:
    // collect scalar or vector/tensor integer/index operands of memory/cast ops.
    scopeRoot->walk([&](Operation *op) {
      bool memOrCast =
          isCastOrTruncOp(op) || llvm::isa<MemoryEffectOpInterface>(op);
      if (!memOrCast)
        return;
      for (Value v : op->getOperands())
        if (isAddressScalarOrShapedInt(v.getType()) &&
            rootSet.insert(v).second)
          roots.push_back(v);
    });
  }

  // Conservative rule: empty root set means nothing was identified to prove.
  if (roots.empty()) {
    out.reason = "no address roots found to prove";
    return out;
  }

  llvm::SmallVector<Value, 64> worklist(roots.begin(), roots.end());
  llvm::DenseSet<Value> visited(roots.begin(), roots.end());

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
    if (!rangeFitsTarget(*rng, options, scale)) {
      out.culpritValue = cur;
      out.culpritOp = cur.getDefiningOp();
      out.reason = "value range exceeds target narrow bitwidth";
      return out;
    }

    Operation *def = cur.getDefiningOp();
    if (!def || !scopeRoot->isAncestor(def))
      continue;

    if (isCastOrTruncOp(def)) {
      Value src = def->getOperand(0);
      auto srcRng = existingRanges->query(src);
      unsigned dstW =
          ConstantIntRanges::getStorageBitwidth(leafScalarType(cur.getType()));
      AddressProofOptions castCheck = options;
      castCheck.targetBitwidth = std::min(options.targetBitwidth, dstW);
      castCheck.requireNonNegative =
          llvm::isa<arith::IndexCastUIOp>(def) || options.requireNonNegative;
      if (!srcRng || !rangeFitsTarget(*srcRng, castCheck, 1)) {
        out.culpritOp = def;
        out.culpritValue = src;
        out.reason = "index_cast/trunci operand may wrap around narrow width";
        return out;
      }
    }

    if (llvm::isa<arith::ShRSIOp, arith::ShRUIOp>(def)) {
      auto lhsR = existingRanges->query(def->getOperand(0));
      auto rhsR = existingRanges->query(def->getOperand(1));
      if (!lhsR || !rhsR) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "missing operand range on right shift";
        return out;
      }
      if (rhsR->smin().isNegative() ||
          rhsR->smax().uge(options.targetBitwidth) ||
          rhsR->umax().uge(options.targetBitwidth)) {
        out.culpritOp = def;
        out.culpritValue = def->getOperand(1);
        out.reason = "right-shift amount may be negative or >= narrow bitwidth";
        return out;
      }
      if (llvm::isa<arith::ShRUIOp>(def)) {
        AddressProofOptions uOpts = options;
        uOpts.isSigned = false;
        uOpts.requireNonNegative = true;
        if (!rangeFitsTarget(*lhsR, uOpts, 1)) {
          out.culpritOp = def;
          out.culpritValue = def->getOperand(0);
          out.reason =
              "unsigned right-shift input exceeds unsigned narrow bitwidth";
          return out;
        }
      } else {
        AddressProofOptions sOpts = options;
        sOpts.isSigned = true;
        if (!rangeFitsTarget(*lhsR, sOpts, 1)) {
          out.culpritOp = def;
          out.culpritValue = def->getOperand(0);
          out.reason =
              "signed right-shift input exceeds signed narrow bitwidth";
          return out;
        }
      }
    }

    if (llvm::isa<arith::AddIOp, arith::SubIOp, arith::MulIOp, arith::ShLIOp>(
            def)) {
      auto lhsR = existingRanges->query(def->getOperand(0));
      auto rhsR = existingRanges->query(def->getOperand(1));
      if (!lhsR || !rhsR) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "missing operand range on address arithmetic";
        return out;
      }
      unsigned w = lhsR->smin().getBitWidth();
      unsigned extW = std::max(w, options.targetBitwidth) + 66;
      APInt lMin = lhsR->smin().sext(extW), lMax = lhsR->smax().sext(extW);
      APInt rMin = rhsR->smin().sext(extW), rMax = rhsR->smax().sext(extW);
      APInt wideMin(extW, 0), wideMax(extW, 0);
      if (llvm::isa<arith::AddIOp>(def)) {
        wideMin = lMin + rMin;
        wideMax = lMax + rMax;
      } else if (llvm::isa<arith::SubIOp>(def)) {
        wideMin = lMin - rMax;
        wideMax = lMax - rMin;
      } else if (llvm::isa<arith::MulIOp>(def)) {
        APInt p1 = lMin * rMin, p2 = lMin * rMax, p3 = lMax * rMin,
              p4 = lMax * rMax;
        wideMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                       llvm::APIntOps::smin(p3, p4));
        wideMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                       llvm::APIntOps::smax(p3, p4));
      } else {
        if (rMin.isNegative() || rMax.uge(options.targetBitwidth) ||
            rhsR->umax().uge(options.targetBitwidth)) {
          out.culpritOp = def;
          out.culpritValue = cur;
          out.reason = "shift amount may overflow narrow width";
          return out;
        }
        unsigned shMin = static_cast<unsigned>(rMin.getZExtValue());
        unsigned shMax = static_cast<unsigned>(rMax.getZExtValue());
        APInt p1 = lMin.shl(shMin), p2 = lMin.shl(shMax),
              p3 = lMax.shl(shMin), p4 = lMax.shl(shMax);
        wideMin = llvm::APIntOps::smin(llvm::APIntOps::smin(p1, p2),
                                       llvm::APIntOps::smin(p3, p4));
        wideMax = llvm::APIntOps::smax(llvm::APIntOps::smax(p1, p2),
                                       llvm::APIntOps::smax(p3, p4));
      }
      APInt minT =
          options.isSigned
              ? APInt::getSignedMinValue(options.targetBitwidth).sext(extW)
              : APInt(extW, 0);
      APInt maxT =
          options.isSigned
              ? APInt::getSignedMaxValue(options.targetBitwidth).sext(extW)
              : APInt::getMaxValue(options.targetBitwidth).zext(extW);
      if (wideMin.slt(minT) || wideMax.sgt(maxT)) {
        out.culpritOp = def;
        out.culpritValue = cur;
        out.reason = "intermediate arithmetic overflows narrow bitwidth";
        return out;
      }
    }

    for (Value operand : def->getOperands())
      if (isAddressScalarOrShapedInt(operand.getType()) &&
          visited.insert(operand).second)
        worklist.push_back(operand);
  }

  out.proven = true;
  out.chosenBitwidth = options.targetBitwidth;
  return out;
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

// Collect transitive backward SSA slice of `firstOp` (within `scopeOp`) so
// loads feeding pure arithmetic ops are always included in the read set.
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
  auto enqueueVal = [&](Value v) {
    if (!v || !visitedVals.insert(v).second)
      return true;
    if (Operation *def = v.getDefiningOp()) {
      enqueueOp(def);
      return true;
    }
    auto barg = llvm::cast<BlockArgument>(v);
    Region *parentReg = barg.getOwner()->getParent();
    Operation *parentOp = parentReg ? parentReg->getParentOp() : nullptr;
    if (!parentOp || parentOp == scopeOp || !scopeOp->isAncestor(parentOp))
      return true; // Outer function/entry region parameter is immutable SSA.
    if (auto loop = llvm::dyn_cast<LoopLikeOpInterface>(parentOp)) {
      auto iterArgs = loop.getRegionIterArgs();
      for (auto [idx, iterArg] : llvm::enumerate(iterArgs)) {
        if (iterArg == barg) {
          if (idx < loop.getInits().size())
            enqueueOp(loop.getInits()[idx].getDefiningOp());
          if (auto yieldedVals = loop.getYieldedValuesMutable())
            for (OpOperand &yielded : *yieldedVals)
              enqueueOp(yielded.get().getDefiningOp());
          return true;
        }
      }
      return true;
    }
    err = "cannot trace memory provenance through unstructured block argument";
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
    cur->walk([&](Operation *nested) {
      for (Value operand : nested->getOperands())
        if (!visitedVals.contains(operand))
          (void)enqueueVal(operand);
    });
    if (!err.empty())
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

  // If `startOp` is conditionally nested inside `startTop` and `commonRegion`
  // itself lies inside an outer loop, another iteration could skip `startOp`
  // or execute sibling branches / trailing ops in that outer loop.
  if (startOp != startTop) {
    for (Operation *enc = commonRegion->getParentOp(); enc != nullptr;
         enc = enc->getParentOp()) {
      if (llvm::isa<LoopLikeOpInterface>(enc)) {
        if (!checkInterferenceOnOp(enc, locations, hasGlobalRead, aa, out))
          return false;
        break;
      }
    }
  }

  // 1. Climb from `startOp` up to `startTop`. If any enclosing ancestor is a
  // loop (or multi-region non-if), check the ENTIRE loop operation so writes in
  // sibling conditional branches (`else` when `startOp` is in `then`) on
  // subsequent iterations are always scanned.
  for (Operation *cur = startOp; cur != startTop; cur = cur->getParentOp()) {
    Operation *parent = cur->getParentOp();
    if (llvm::isa<LoopLikeOpInterface>(parent) ||
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

  // 2. Check all operations strictly between `startTop` and `replayTop`.
  for (Operation *mid = startTop->getNextNode(); mid != replayTop;
       mid = mid->getNextNode()) {
    if (!checkInterferenceOnOp(mid, locations, hasGlobalRead, aa, out))
      return false;
  }

  // 3. Climb from `replayOp` up to `replayTop`. If any enclosing ancestor is a
  // loop, check the ENTIRE loop operation so writes in sibling conditional
  // branches (`else` on iteration 0 before `replayOp` in `then` on iteration 1)
  // and loop backedges are all scanned.
  for (Operation *cur = replayOp; cur != replayTop; cur = cur->getParentOp()) {
    Operation *parent = cur->getParentOp();
    if (llvm::isa<LoopLikeOpInterface>(parent) ||
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

std::optional<int64_t> matchScalarConstInt(Value v) {
  if (!v)
    return std::nullopt;
  if (auto c = v.getDefiningOp<arith::ConstantOp>())
    if (auto ia = llvm::dyn_cast<IntegerAttr>(c.getValue()))
      if (ia.getValue().getSignificantBits() <= 64)
        return ia.getValue().getSExtValue();
  return std::nullopt;
}

bool opCannotWrapSigned(Operation *op, const IntegerRangeContext *ranges) {
  if (!ranges)
    return false;
  auto lhsR = ranges->query(op->getOperand(0));
  auto rhsR = ranges->query(op->getOperand(1));
  if (!lhsR || !rhsR)
    return false;
  unsigned bw = ConstantIntRanges::getStorageBitwidth(op->getResult(0).getType());
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
    if (rMin.isNegative() || rMax.uge(bw))
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
  if (auto barg = llvm::dyn_cast<BlockArgument>(v)) {
    Operation *parent =
        barg.getOwner()->getParent() ? barg.getOwner()->getParentOp() : nullptr;
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
          bool lbOk = lbVal ? isProvenMultipleOf(lbVal, align, ranges, depth + 1)
                            : (llvm::isa_and_nonnull<IntegerAttr>(lbAttr) &&
                               (llvm::cast<IntegerAttr>(lbAttr).getInt() %
                                align) == 0);
          bool stOk = stVal ? isProvenMultipleOf(stVal, align, ranges, depth + 1)
                            : (llvm::isa_and_nonnull<IntegerAttr>(stAttr) &&
                               (llvm::cast<IntegerAttr>(stAttr).getInt() %
                                align) == 0);
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
  if (llvm::isa<arith::ExtSIOp, arith::ExtUIOp, arith::IndexCastOp,
                arith::IndexCastUIOp>(def)) {
    Value src = def->getOperand(0);
    unsigned srcW = ConstantIntRanges::getStorageBitwidth(src.getType());
    unsigned dstW = ConstantIntRanges::getStorageBitwidth(v.getType());
    if (srcW == 0 || dstW == 0)
      return false;
    if (dstW < srcW) {
      if (!ranges)
        return false;
      auto sr = ranges->query(src);
      AddressProofOptions chk;
      chk.targetBitwidth = dstW;
      chk.isSigned = !llvm::isa<arith::IndexCastUIOp>(def);
      chk.requireNonNegative = llvm::isa<arith::IndexCastUIOp>(def);
      if (!sr || !rangeFitsTarget(*sr, chk, 1))
        return false;
    }
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
  if (d.dynamicOffset && !d.staticOffset)
    return isProvenMultipleOf(d.dynamicOffset, align, ranges);
  return false;
}

Value stripLosslessIntCast(Value v) {
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

bool matchRampVector(Value v, int64_t tileSize, int64_t baseOffset,
                     int64_t step) {
  auto st = llvm::dyn_cast<ShapedType>(v.getType());
  if (!st || st.getRank() != 1 || st.getDimSize(0) != tileSize)
    return false;
  Operation *def = v.getDefiningOp();
  if (!def)
    return false;
  if (baseOffset == 0 && step == 1 &&
      def->getName().getStringRef() == "vector.step")
    return true;
  if (auto c = llvm::dyn_cast<arith::ConstantOp>(def)) {
    if (auto dense = llvm::dyn_cast<DenseIntElementsAttr>(c.getValue())) {
      if (dense.getNumElements() != tileSize)
        return false;
      int64_t idx = 0;
      for (const APInt &elem : dense.getValues<APInt>()) {
        if (elem.getSignificantBits() > 64 ||
            elem.getSExtValue() != baseOffset + idx * step)
          return false;
        ++idx;
      }
      return true;
    }
  }
  return false;
}

bool matchLaneCoordinates(Value coordVal, const TailDimension &d) {
  if (d.tileSize == 1 && llvm::isa<IntegerType, IndexType>(coordVal.getType())) {
    if (d.dynamicOffset)
      return stripLosslessIntCast(coordVal) ==
             stripLosslessIntCast(d.dynamicOffset);
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
      if (splatSrc && stripLosslessIntCast(splatSrc) ==
                          stripLosslessIntCast(d.dynamicOffset))
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
    if (d.dynamicExtent && stripLosslessIntCast(boundVal) ==
                               stripLosslessIntCast(d.dynamicExtent))
      return true;
    if (d.staticExtent) {
      auto c = matchScalarConstInt(boundVal);
      return c && *c >= 0 && *c <= *d.staticExtent;
    }
    return false;
  }
  if (d.dynamicExtent) {
    Value splatSrc = matchSplatScalar(boundVal, d.tileSize);
    if (splatSrc && stripLosslessIntCast(splatSrc) ==
                        stripLosslessIntCast(d.dynamicExtent))
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
                         const std::optional<SRange> &extB,
                         unsigned depth = 0) {
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
    return verifyMaskPredicate(andOp.getLhs(), d, extB, depth + 1) ||
           verifyMaskPredicate(andOp.getRhs(), d, extB, depth + 1);
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

  if (isUnsigned && (!extB || extB->lo.isNegative()))
    return false;

  return matchLaneCoordinates(coordVal, d) && matchExtentBound(boundVal, d);
}
} // namespace

TailProof
compilerinfra::proveTailAccess(llvm::ArrayRef<TailDimension> dimensions,
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
                 (*d.staticExtent % d.tileSize == 0) && d.tileAlignment &&
                 *d.tileAlignment == d.tileSize &&
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

    // MaskedSafe requires BOTH a proven non-negative offset bound (with no
    // i64 lane-coordinate overflow) AND a verified predicate covering all
    // out-of-bounds lanes.
    if (kind == TailKind::Unproven && d.hasExplicitMask && offB &&
        !offB->lo.isNegative()) {
      APInt maxCoord = offB->hi + span;
      APInt maxI64 = APInt::getSignedMaxValue(64).sext(W);
      if (maxCoord.sle(maxI64) &&
          verifyMaskPredicate(d.maskPredicate, d, extB)) {
        kind = TailKind::MaskedSafe;
      }
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
