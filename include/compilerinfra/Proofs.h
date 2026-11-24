#pragma once

#include "mlir/Analysis/AliasAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compilerinfra {

// Reusable integer range solver wrapper over an operation and all its nested
// regions. Construct fresh after IR mutations.
class IntegerRangeContext {
public:
  explicit IntegerRangeContext(mlir::Operation *root);
  ~IntegerRangeContext();

  bool valid() const { return succeeded; }
  std::optional<mlir::ConstantIntRanges> query(mlir::Value value) const;

private:
  mlir::Operation *rootOp = nullptr;
  std::unique_ptr<mlir::DataFlowSolver> solver;
  bool succeeded = false;
};

// 1. Narrow-Address Proof (IntegerRangeAnalysis across all nested regions).
struct AddressProofOptions {
  unsigned targetBitwidth = 32;
  unsigned fallbackBitwidth = 64;
  bool isSigned = true;
  bool requireNonNegative = false;
  uint64_t elementByteScale = 1;
};

struct AddressProof {
  bool proven = false;
  unsigned chosenBitwidth = 64;
  mlir::Operation *culpritOp = nullptr;
  mlir::Value culpritValue;
  std::string reason;
};

AddressProof proveNarrowAddress(
    mlir::Operation *scopeRoot, llvm::ArrayRef<mlir::Value> addressValues = {},
    const AddressProofOptions &options = {},
    const IntegerRangeContext *existingRanges = nullptr);

// 2. Value-Replay Proof (MemoryEffects + AliasAnalysis/ModRef across paths and
// enclosing loop backedges).
struct ReplayProof {
  bool proven = false;
  mlir::Operation *clobberOp = nullptr;
  mlir::Value aliasedLocation;
  std::string reason;
};

ReplayProof proveValueReplay(mlir::Operation *firstOp,
                             mlir::Operation *replayOp,
                             llvm::ArrayRef<mlir::Value> readLocations = {},
                             mlir::AliasAnalysis *aliasAnalysis = nullptr);

// 3. Tail-Access Proof (per-dimension extent, tile, stride and mask check).
enum class TailKind : uint8_t {
  ExactInBounds,
  PaddedSafe,
  MaskedSafe,
  Unproven,
};

enum class TailFallback : uint8_t {
  None,
  RequireMaskOrScalarTail,
};

struct TailDimension {
  std::optional<int64_t> staticExtent;
  mlir::Value dynamicExtent;
  std::optional<int64_t> staticOffset;
  mlir::Value dynamicOffset;
  int64_t tileSize = 1;
  int64_t step = 1;
  std::optional<int64_t> tileAlignment; // Verified against IR; ignored if unproven
  bool hasExplicitMask = false;
  mlir::Value maskPredicate; // Required i1 / vector<tileSize x i1> predicate for MaskedSafe
  std::optional<int64_t> paddedCapacity;
};

struct TailProof {
  bool proven = false;
  TailKind kind = TailKind::Unproven;
  TailFallback fallback = TailFallback::RequireMaskOrScalarTail;
  std::vector<TailKind> dimensionKinds;
  std::optional<unsigned> failingDimension;
  std::string reason;
};

TailProof proveTailAccess(llvm::ArrayRef<TailDimension> dimensions,
                          const IntegerRangeContext *ranges = nullptr);

} // namespace compilerinfra
