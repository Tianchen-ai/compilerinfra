#include "compilerinfra/Proofs.h"
#include "mlir/Analysis/AliasAnalysis/LocalAliasAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/IntegerRangeAnalysis.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
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
  }
  const auto *lattice =
      solver->lookupState<dataflow::IntegerValueRangeLattice>(value);
  if (!lattice || lattice->getValue().isUninitialized())
    return std::nullopt;
  return lattice->getValue().getValue();
}

namespace {
bool isIntegerOrIndex(Type ty) {
  return llvm::isa<IntegerType, IndexType>(ty);
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
  // For unsigned narrowing, also ensure signed interpretation cannot be negative
  // unless unsigned bounds are non-maximal and within [0, umax(target)].
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
      if (!v || !isIntegerOrIndex(v.getType()))
        continue;
      Operation *parent = v.getParentRegion() ? v.getParentRegion()->getParentOp()
                                              : nullptr;
      if (parent && !scopeRoot->isAncestor(parent) && parent != scopeRoot) {
        out.culpritValue = v;
        out.reason = "address value lies outside scopeRoot";
        return out;
      }
      if (rootSet.insert(v).second)
        roots.push_back(v);
    }
  } else {
    // Default scan across ALL nested regions and blocks under scopeRoot:
    // collect index/integer operands of memory/cast/address ops.
    scopeRoot->walk([&](Operation *op) {
      bool memOrCast = isCastOrTruncOp(op) ||
                       llvm::isa<MemoryEffectOpInterface>(op);
      if (!memOrCast)
        return;
      for (Value v : op->getOperands())
        if (isIntegerOrIndex(v.getType()) && rootSet.insert(v).second)
          roots.push_back(v);
    });
  }

  if (roots.empty()) {
    out.proven = true;
    out.chosenBitwidth = options.targetBitwidth;
    return out;
  }

  // Backward slice all integer/index intermediates within scopeRoot across all
  // nested regions, checking both intermediate arithmetic and cast wraparound.
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

    // Guard against truncation / index_cast wraparound: the source operand
    // BEFORE truncation must also fit in the target width (and destination
    // width if narrower) so narrowing never hides modular wraparound.
    if (isCastOrTruncOp(def)) {
      Value src = def->getOperand(0);
      auto srcRng = existingRanges->query(src);
      unsigned dstW = ConstantIntRanges::getStorageBitwidth(cur.getType());
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

    // Check exact non-wrapping arithmetic on binary add/sub/mul/shl.
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
        if (rMin.isNegative() || rMax.uge(options.targetBitwidth)) {
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
      if (isIntegerOrIndex(operand.getType()) && visited.insert(operand).second)
        worklist.push_back(operand);
  }

  out.proven = true;
  out.chosenBitwidth = options.targetBitwidth;
  return out;
}

namespace {
Operation *liftToBlock(Operation *op, Block *block) {
  while (op && op->getBlock() != block)
    op = op->getParentOp();
  return op;
}

bool collectReadLocations(Operation *root,
                          llvm::SmallVectorImpl<Value> &locations,
                          bool &hasGlobalRead, std::string &err) {
  llvm::DenseSet<Value> seen(locations.begin(), locations.end());
  bool ok = true;
  root->walk([&](Operation *op) {
    if (op->getNumRegions() > 0 && !llvm::isa<MemoryEffectOpInterface>(op))
      return WalkResult::advance();
    auto mem = llvm::dyn_cast<MemoryEffectOpInterface>(op);
    if (!mem) {
      if (!op->hasTrait<OpTrait::IsIsolatedFromAbove>()) {
        err = "source operation lacks MemoryEffectOpInterface: " +
              op->getName().getStringRef().str();
        ok = false;
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    }
    llvm::SmallVector<MemoryEffects::EffectInstance, 4> effects;
    mem.getEffects(effects);
    for (const auto &e : effects) {
      if (llvm::isa<MemoryEffects::Read>(e.getEffect())) {
        if (Value v = e.getValue()) {
          if (seen.insert(v).second)
            locations.push_back(v);
        } else {
          hasGlobalRead = true;
        }
      }
    }
    return WalkResult::advance();
  });
  return ok;
}

bool checkInterferenceOnOp(Operation *op, llvm::ArrayRef<Value> locations,
                           bool hasGlobalRead, AliasAnalysis &aa,
                           ReplayProof &out) {
  bool ok = true;
  op->walk([&](Operation *leaf) {
    auto mem = llvm::dyn_cast<MemoryEffectOpInterface>(leaf);
    if (!mem) {
      if (leaf->getNumRegions() > 0)
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

  // Find the deepest common enclosing Region.
  llvm::SmallPtrSet<Region *, 8> firstAncestors;
  for (Region *r = firstOp->getParentRegion(); r != nullptr;
       r = r->getParentOp() ? r->getParentOp()->getParentRegion() : nullptr)
    firstAncestors.insert(r);
  Region *commonRegion = nullptr;
  for (Region *r = replayOp->getParentRegion(); r != nullptr;
       r = r->getParentOp() ? r->getParentOp()->getParentRegion() : nullptr) {
    if (firstAncestors.contains(r)) {
      commonRegion = r;
      break;
    }
  }
  if (!commonRegion || !commonRegion->getParentOp()) {
    out.reason = "firstOp and replayOp do not share an enclosing region";
    return out;
  }

  std::unique_ptr<AliasAnalysis> localAA;
  if (!aliasAnalysis) {
    localAA = std::make_unique<AliasAnalysis>(commonRegion->getParentOp());
    localAA->addAnalysisImplementation(LocalAliasAnalysis());
    aliasAnalysis = localAA.get();
  }

  llvm::SmallVector<Value, 4> locations(readLocations.begin(),
                                        readLocations.end());
  bool hasGlobalRead = false;
  if (locations.empty()) {
    if (!collectReadLocations(firstOp, locations, hasGlobalRead, out.reason))
      return out;
  }
  if (locations.empty() && !hasGlobalRead) {
    // Pure value with no memory reads is trivially replayable.
    out.proven = true;
    return out;
  }

  // Lift both operations to direct children of blocks in `commonRegion`.
  Operation *firstTop = firstOp;
  while (firstTop->getParentRegion() != commonRegion)
    firstTop = firstTop->getParentOp();
  Operation *replayTop = replayOp;
  while (replayTop->getParentRegion() != commonRegion)
    replayTop = replayTop->getParentOp();

  if (firstTop->getBlock() != replayTop->getBlock()) {
    // Multi-block CFG: conservatively inspect every block in commonRegion.
    for (Block &blk : *commonRegion)
      for (Operation &op : blk)
        if (&op != firstOp && &op != replayOp)
          if (!checkInterferenceOnOp(&op, locations, hasGlobalRead,
                                     *aliasAnalysis, out))
            return out;
    out.proven = true;
    return out;
  }

  if (firstTop == replayTop || !firstTop->isBeforeInBlock(replayTop)) {
    out.reason = "firstOp does not precede replayOp in their common region";
    return out;
  }

  // 1. Check trailing ops after `firstOp` inside `firstTop` (when nested).
  for (Operation *cur = firstOp; cur != firstTop; cur = cur->getParentOp()) {
    for (Operation *after = cur->getNextNode(); after != nullptr;
         after = after->getNextNode())
      if (!checkInterferenceOnOp(after, locations, hasGlobalRead,
                                 *aliasAnalysis, out))
        return out;
    // If cur's parent is a loop, earlier ops in that loop can also execute
    // after `firstOp` on subsequent iterations before exiting `firstTop`!
    if (llvm::isa<LoopLikeOpInterface>(cur->getParentOp())) {
      for (Operation *before = &cur->getBlock()->front(); before != cur;
           before = before->getNextNode())
        if (!checkInterferenceOnOp(before, locations, hasGlobalRead,
                                   *aliasAnalysis, out))
          return out;
    }
  }

  // 2. Check every sibling operation strictly between `firstTop` and
  // `replayTop` in their shared block (including all nested loop/if regions).
  for (Operation *mid = firstTop->getNextNode(); mid != replayTop;
       mid = mid->getNextNode()) {
    if (!checkInterferenceOnOp(mid, locations, hasGlobalRead, *aliasAnalysis,
                               out))
      return out;
  }

  // 3. Check ops inside `replayTop` that execute before `replayOp` — AND if
  // `replayOp` is nested inside any loop within `replayTop` (or if
  // `commonRegion->getParentOp()` itself is a loop!), check loop-carried
  // backedge writes after `replayOp` too!
  for (Operation *cur = replayOp; cur != replayTop; cur = cur->getParentOp()) {
    for (Operation *before = &cur->getBlock()->front(); before != cur;
         before = before->getNextNode())
      if (!checkInterferenceOnOp(before, locations, hasGlobalRead,
                                 *aliasAnalysis, out))
        return out;
    if (llvm::isa<LoopLikeOpInterface>(cur->getParentOp())) {
      for (Operation *after = cur->getNextNode(); after != nullptr;
           after = after->getNextNode())
        if (!checkInterferenceOnOp(after, locations, hasGlobalRead,
                                   *aliasAnalysis, out))
          return out;
    }
  }

  // 4. Also check if commonRegion itself belongs to an outer loop whose tail
  // after `replayTop` could clobber `locations` before the next outer iteration
  // if `firstTop` lies outside that loop (already handled because commonRegion
  // is the LCA, so both are inside the outer loop; if firstOp's cached buffer
  // is refreshed each iteration of commonRegion, trailing writes after
  // replayTop are overwritten by firstTop on the next iteration — safe!).

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
  // Reject uninformative maximal signed range [INT_MIN, INT_MAX].
  unsigned bw = r->smin().getBitWidth();
  if (r->smin().isMinSignedValue() && r->smax().isMaxSignedValue() && bw >= 32)
    return std::nullopt;
  return SRange{r->smin().sext(W), r->smax().sext(W)};
}
} // namespace

TailProof
compilerinfra::proveTailAccess(llvm::ArrayRef<TailDimension> dimensions,
                               const IntegerRangeContext *ranges) {
  TailProof out;
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
                 offB->hi.slt(APInt(W, *d.staticExtent, true))) {
        kind = TailKind::ExactInBounds;
      }
    }

    if (kind == TailKind::Unproven && offB && d.paddedCapacity) {
      APInt maxCoord = offB->hi + span;
      if (maxCoord.slt(APInt(W, *d.paddedCapacity, true)))
        kind = TailKind::PaddedSafe;
    }

    if (kind == TailKind::Unproven && d.hasExplicitMask &&
        (!offB || !offB->lo.isNegative())) {
      kind = TailKind::MaskedSafe;
    }

    out.dimensionKinds.push_back(kind);
    if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(worst))
      worst = kind;
    if (kind == TailKind::Unproven && !out.failingDimension) {
      out.failingDimension = idx;
      out.reason = "dimension " + std::to_string(idx) +
                   ": unmasked tile may exceed logical extent";
    }
  }

  out.kind = worst;
  out.proven = (worst != TailKind::Unproven);
  out.fallback =
      out.proven ? TailFallback::None : TailFallback::RequireMaskOrScalarTail;
  return out;
}
