#pragma once

#include "mlir/Analysis/AliasAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/Value.h"
#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
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

  mlir::Operation *root() const { return rootOp; }
  bool valid() const { return succeeded; }
  std::optional<mlir::ConstantIntRanges> query(mlir::Value value) const;

private:
  mlir::Operation *rootOp = nullptr;
  std::unique_ptr<mlir::DataFlowSolver> solver;
  bool succeeded = false;
};

// ---------------------------------------------------------------------------
// Dialect-extensibility interfaces for IR-derived address & tail proofs (0.5.3)
// ---------------------------------------------------------------------------

// 1. Structured region / parallel grid / custom range-loop bounds for region
// BlockArguments (e.g. tile base or custom range-loop induction variables).
struct RegionIndexBounds {
  std::optional<int64_t> staticLower;
  mlir::Value dynamicLower;
  std::optional<int64_t> staticUpperExclusive;
  mlir::Value dynamicUpperExclusive;
  std::optional<int64_t> staticStep;
  mlir::Value dynamicStep;
};

class IndexedRegionOpInterface;
namespace detail {
struct IndexedRegionOpInterfaceTraits {
  struct Concept {
    mlir::LogicalResult (*getRegionArgBounds)(const Concept *,
                                              mlir::Operation *,
                                              mlir::BlockArgument,
                                              RegionIndexBounds &);
  };
  template <typename ConcreteOp> class Model : public Concept {
  public:
    using Interface = IndexedRegionOpInterface;
    Model() : Concept{getRegionArgBoundsImpl} {}
    static mlir::LogicalResult getRegionArgBoundsImpl(const Concept *,
                                                      mlir::Operation *op,
                                                      mlir::BlockArgument arg,
                                                      RegionIndexBounds &out) {
      return mlir::cast<ConcreteOp>(op).getRegionArgBounds(arg, out);
    }
  };
  template <typename ConcreteModel> class FallbackModel : public Concept {
  public:
    using Interface = IndexedRegionOpInterface;
    FallbackModel() : Concept{getRegionArgBoundsImpl} {}
    static mlir::LogicalResult getRegionArgBoundsImpl(const Concept *impl,
                                                      mlir::Operation *op,
                                                      mlir::BlockArgument arg,
                                                      RegionIndexBounds &out) {
      return static_cast<const ConcreteModel *>(impl)->getRegionArgBounds(
          op, arg, out);
    }
  };
  template <typename ConcreteModel, typename ConcreteOp>
  class ExternalModel : public FallbackModel<ConcreteModel> {
  public:
    using ConcreteEntity = ConcreteOp;
    mlir::LogicalResult getRegionArgBounds(mlir::Operation *,
                                           mlir::BlockArgument,
                                           RegionIndexBounds &) const {
      return mlir::failure();
    }
  };
};
} // namespace detail

class IndexedRegionOpInterface
    : public mlir::OpInterface<IndexedRegionOpInterface,
                               detail::IndexedRegionOpInterfaceTraits> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(IndexedRegionOpInterface)
  using mlir::OpInterface<IndexedRegionOpInterface,
                          detail::IndexedRegionOpInterfaceTraits>::OpInterface;

  template <typename ConcreteOp>
  struct Trait
      : public mlir::OpInterface<
            IndexedRegionOpInterface,
            detail::IndexedRegionOpInterfaceTraits>::Trait<ConcreteOp> {};

  mlir::LogicalResult getRegionArgBounds(mlir::BlockArgument arg,
                                         RegionIndexBounds &out) {
    return getImpl()->getRegionArgBounds(getImpl(), getOperation(), arg, out);
  }
};

// 2. Linear strided address calculation (`base + constantOffset + sum_i c_i * s_i`).
struct LinearAddressDescriptor {
  mlir::Value baseContainer;
  llvm::SmallVector<mlir::Value, 4> coordinates;
  llvm::SmallVector<mlir::Value, 4> dynamicStrides;
  llvm::SmallVector<int64_t, 4> staticStrides;
  int64_t constantOffset = 0;
};

class AddressOpInterface;
namespace detail {
struct AddressOpInterfaceTraits {
  struct Concept {
    mlir::LogicalResult (*getLinearAddress)(const Concept *, mlir::Operation *,
                                            LinearAddressDescriptor &);
  };
  template <typename ConcreteOp> class Model : public Concept {
  public:
    using Interface = AddressOpInterface;
    Model() : Concept{getLinearAddressImpl} {}
    static mlir::LogicalResult
    getLinearAddressImpl(const Concept *, mlir::Operation *op,
                         LinearAddressDescriptor &out) {
      return mlir::cast<ConcreteOp>(op).getLinearAddress(out);
    }
  };
  template <typename ConcreteModel> class FallbackModel : public Concept {
  public:
    using Interface = AddressOpInterface;
    FallbackModel() : Concept{getLinearAddressImpl} {}
    static mlir::LogicalResult
    getLinearAddressImpl(const Concept *impl, mlir::Operation *op,
                         LinearAddressDescriptor &out) {
      return static_cast<const ConcreteModel *>(impl)->getLinearAddress(op,
                                                                        out);
    }
  };
  template <typename ConcreteModel, typename ConcreteOp>
  class ExternalModel : public FallbackModel<ConcreteModel> {
  public:
    using ConcreteEntity = ConcreteOp;
    mlir::LogicalResult getLinearAddress(mlir::Operation *,
                                         LinearAddressDescriptor &) const {
      return mlir::failure();
    }
  };
};
} // namespace detail

class AddressOpInterface
    : public mlir::OpInterface<AddressOpInterface,
                               detail::AddressOpInterfaceTraits> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AddressOpInterface)
  using mlir::OpInterface<AddressOpInterface,
                          detail::AddressOpInterfaceTraits>::OpInterface;

  template <typename ConcreteOp>
  struct Trait : public mlir::OpInterface<
                     AddressOpInterface,
                     detail::AddressOpInterfaceTraits>::Trait<ConcreteOp> {};

  mlir::LogicalResult getLinearAddress(LinearAddressDescriptor &out) {
    return getImpl()->getLinearAddress(getImpl(), getOperation(), out);
  }
};

// 3. Tiled container access descriptor for custom slice / load / store / DMA ops.
// Notice: there is NO arbitrary static integer offset field and NO caller
// alignment hint; every coordinate must be an actual IR SSA Value (`coord`),
// or `entireAxisFromZero = true` when an op unconditionally covers [0, extent)
// from 0. Alignment, bounds, and vector ramp spans are derived strictly from IR.
struct AccessAxis {
  mlir::Value container;
  unsigned containerDim = 0;
  std::optional<int64_t> staticExtent;
  mlir::Value dynamicExtent;

  mlir::Value coord;
  bool entireAxisFromZero = false;

  int64_t tileSize = 1;
  int64_t step = 1;

  mlir::Value maskPredicate;
  std::optional<int64_t> paddedCapacity;
};

class TiledAccessOpInterface;
namespace detail {
struct TiledAccessOpInterfaceTraits {
  struct Concept {
    mlir::LogicalResult (*getAccessedAxes)(const Concept *, mlir::Operation *,
                                           llvm::SmallVectorImpl<AccessAxis> &);
  };
  template <typename ConcreteOp> class Model : public Concept {
  public:
    using Interface = TiledAccessOpInterface;
    Model() : Concept{getAccessedAxesImpl} {}
    static mlir::LogicalResult
    getAccessedAxesImpl(const Concept *, mlir::Operation *op,
                        llvm::SmallVectorImpl<AccessAxis> &out) {
      return mlir::cast<ConcreteOp>(op).getAccessedAxes(out);
    }
  };
  template <typename ConcreteModel> class FallbackModel : public Concept {
  public:
    using Interface = TiledAccessOpInterface;
    FallbackModel() : Concept{getAccessedAxesImpl} {}
    static mlir::LogicalResult
    getAccessedAxesImpl(const Concept *impl, mlir::Operation *op,
                        llvm::SmallVectorImpl<AccessAxis> &out) {
      return static_cast<const ConcreteModel *>(impl)->getAccessedAxes(op, out);
    }
  };
  template <typename ConcreteModel, typename ConcreteOp>
  class ExternalModel : public FallbackModel<ConcreteModel> {
  public:
    using ConcreteEntity = ConcreteOp;
    mlir::LogicalResult
    getAccessedAxes(mlir::Operation *,
                    llvm::SmallVectorImpl<AccessAxis> &) const {
      return mlir::failure();
    }
  };
};
} // namespace detail

class TiledAccessOpInterface
    : public mlir::OpInterface<TiledAccessOpInterface,
                               detail::TiledAccessOpInterfaceTraits> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TiledAccessOpInterface)
  using mlir::OpInterface<TiledAccessOpInterface,
                          detail::TiledAccessOpInterfaceTraits>::OpInterface;

  template <typename ConcreteOp>
  struct Trait : public mlir::OpInterface<
                     TiledAccessOpInterface,
                     detail::TiledAccessOpInterfaceTraits>::Trait<ConcreteOp> {
  };

  mlir::LogicalResult getAccessedAxes(llvm::SmallVectorImpl<AccessAxis> &out) {
    return getImpl()->getAccessedAxes(getImpl(), getOperation(), out);
  }
};

// ---------------------------------------------------------------------------
// 1. Narrow-Address Proof (IR-derived scope, coordinates, and stride products)
// ---------------------------------------------------------------------------
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

// Prove narrow-address safety of an AddressOpInterface op, a standard MLIR
// memory/slice/transfer op, or all memory indexing within an IsolatedFromAbove
// container op. Enclosing scopeRoot is derived automatically from IR.
AddressProof proveNarrowAddress(
    mlir::Operation *op, const AddressProofOptions &options = {},
    const IntegerRangeContext *ranges = nullptr);

// Prove narrow-address safety of a scalar or shaped coordinate/address SSA
// value and its full transitive use-def / loop-IV closure inside its enclosing
// IsolatedFromAbove scope.
AddressProof proveNarrowAddress(
    mlir::Value addressValue, const AddressProofOptions &options = {},
    const IntegerRangeContext *ranges = nullptr);

[[deprecated("pass the access/address Operation* or address Value directly; "
             "scopeRoot and operands are derived from IR in 0.5.3+")]]
AddressProof proveNarrowAddress(
    mlir::Operation *scopeRoot, llvm::ArrayRef<mlir::Value> addressValues,
    const AddressProofOptions &options = {},
    const IntegerRangeContext *existingRanges = nullptr);

// ---------------------------------------------------------------------------
// 2. Value-Replay Proof (MemoryEffects + AliasAnalysis/ModRef across paths and
// enclosing loop backedges).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// 3. Tail-Access Proof (IR-derived offsets, extents, tile spans, and masks)
// ---------------------------------------------------------------------------
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

// Prove tail safety directly from a memory, subview/slice, vector-transfer, or
// TiledAccessOpInterface operation in IR.
TailProof proveTailAccess(mlir::Operation *accessOp,
                          const IntegerRangeContext *ranges = nullptr);

// Prove tail safety directly from a sliced view / address SSA Value (defined by
// OffsetSizeAndStrideOpInterface or TiledAccessOpInterface).
TailProof proveTailAccess(mlir::Value addressOrSliceValue,
                          const IntegerRangeContext *ranges = nullptr);

// Prove tail safety of scalar or 1D-shaped coordinate SSA Values indexing
// `container` (`ShapedType`), with optional `maskPredicate` Value.
TailProof proveTailAccess(mlir::Value container,
                          llvm::ArrayRef<mlir::Value> coordinates,
                          mlir::Value maskPredicate = {},
                          const IntegerRangeContext *ranges = nullptr);

[[deprecated("pass the access Operation* or address/slice Value directly so "
             "offsets, extents, and steps are derived from IR in 0.5.3+")]]
TailProof proveTailAccess(llvm::ArrayRef<TailDimension> dimensions,
                          const IntegerRangeContext *ranges = nullptr);

} // namespace compilerinfra
