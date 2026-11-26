#pragma once

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace compilerinfra {
struct TargetProfile;
struct ImplementationContract;

// Global/default numerical policy configured on the compiler or target.
// Semantics follow LLVM/MLIR directly: op-level arith::FastMathFlags plus
// function-scope llvm::DenormalMode, with no separate provenance ledger.
struct NumericalOptions {
  mlir::arith::FastMathFlags defaultFastMath = mlir::arith::FastMathFlags::none;
  llvm::DenormalMode defaultDenormal = llvm::DenormalMode::getIEEE();
  bool explicitFastMath = false, explicitDenormal = false;

  static llvm::Expected<NumericalOptions> parse(const llvm::json::Object &obj);
  llvm::json::Object toJSON() const;
};

// Query and propagate mature MLIR/LLVM numerical flags on IR operations.
mlir::arith::FastMathFlags effectiveFastMath(
    mlir::Operation *op,
    mlir::arith::FastMathFlags fallback = mlir::arith::FastMathFlags::none);
void setFastMath(mlir::Operation *op, mlir::arith::FastMathFlags flags);
mlir::arith::FastMathFlags intersectFastMath(
    llvm::ArrayRef<mlir::Operation *> sourceOps,
    mlir::arith::FastMathFlags fallback = mlir::arith::FastMathFlags::none);
void propagateNumericalFlags(
    llvm::ArrayRef<mlir::Operation *> fromOps, mlir::Operation *toOp,
    mlir::arith::FastMathFlags fallback = mlir::arith::FastMathFlags::none);

llvm::DenormalMode effectiveDenormalMode(
    mlir::Operation *op,
    llvm::DenormalMode fallback = llvm::DenormalMode::getIEEE());
void setDenormalMode(mlir::Operation *scopeOp, llvm::DenormalMode mode);

bool hasFastMath(mlir::Operation *op, mlir::arith::FastMathFlags required,
                 const NumericalOptions &defaults = {});
bool allowsInputSubnormalFlush(mlir::Operation *op,
                               const NumericalOptions &defaults = {});
bool allowsOutputSubnormalFlush(mlir::Operation *op,
                                const NumericalOptions &defaults = {});
bool allowsSubnormalFlush(mlir::Operation *op,
                          const NumericalOptions &defaults = {});

// MLIR OpInterface allowing dialect target operations to state shape, layout,
// accumulator and fastmath invariants once in IR definitions.
class ContractOpInterface;
namespace detail {
struct ContractOpInterfaceTraits {
  struct Concept {
    mlir::LogicalResult (*verifyShapeAndTypes)(const Concept *,
                                               mlir::Operation *);
    mlir::Type (*getAccumulatorType)(const Concept *, mlir::Operation *);
    mlir::LogicalResult (*verifyLayoutAndAddressSpace)(const Concept *,
                                                       mlir::Operation *);
    mlir::arith::FastMathFlags (*getRequiredFastMath)(const Concept *,
                                                      mlir::Operation *);
  };
  template <typename ConcreteOp> class Model : public Concept {
  public:
    using Interface = ContractOpInterface;
    Model()
        : Concept{verifyShapeAndTypesImpl, getAccumulatorTypeImpl,
                  verifyLayoutAndAddressSpaceImpl, getRequiredFastMathImpl} {}
    static mlir::LogicalResult verifyShapeAndTypesImpl(const Concept *,
                                                       mlir::Operation *op) {
      return mlir::cast<ConcreteOp>(op).verifyShapeAndTypes();
    }
    static mlir::Type getAccumulatorTypeImpl(const Concept *,
                                             mlir::Operation *op) {
      return mlir::cast<ConcreteOp>(op).getAccumulatorType();
    }
    static mlir::LogicalResult
    verifyLayoutAndAddressSpaceImpl(const Concept *, mlir::Operation *op) {
      return mlir::cast<ConcreteOp>(op).verifyLayoutAndAddressSpace();
    }
    static mlir::arith::FastMathFlags
    getRequiredFastMathImpl(const Concept *, mlir::Operation *op) {
      return mlir::cast<ConcreteOp>(op).getRequiredFastMath();
    }
  };
  template <typename ConcreteModel> class FallbackModel : public Concept {
  public:
    using Interface = ContractOpInterface;
    FallbackModel()
        : Concept{verifyShapeAndTypesImpl, getAccumulatorTypeImpl,
                  verifyLayoutAndAddressSpaceImpl, getRequiredFastMathImpl} {}
    static mlir::LogicalResult verifyShapeAndTypesImpl(const Concept *impl,
                                                       mlir::Operation *op) {
      return static_cast<const ConcreteModel *>(impl)->verifyShapeAndTypes(op);
    }
    static mlir::Type getAccumulatorTypeImpl(const Concept *impl,
                                             mlir::Operation *op) {
      return static_cast<const ConcreteModel *>(impl)->getAccumulatorType(op);
    }
    static mlir::LogicalResult
    verifyLayoutAndAddressSpaceImpl(const Concept *impl, mlir::Operation *op) {
      return static_cast<const ConcreteModel *>(impl)
          ->verifyLayoutAndAddressSpace(op);
    }
    static mlir::arith::FastMathFlags
    getRequiredFastMathImpl(const Concept *impl, mlir::Operation *op) {
      return static_cast<const ConcreteModel *>(impl)->getRequiredFastMath(op);
    }
  };
  template <typename ConcreteModel, typename ConcreteOp>
  class ExternalModel : public FallbackModel<ConcreteModel> {
  public:
    using ConcreteEntity = ConcreteOp;
    mlir::LogicalResult verifyShapeAndTypes(mlir::Operation *) const {
      return mlir::success();
    }
    mlir::Type getAccumulatorType(mlir::Operation *) const { return {}; }
    mlir::LogicalResult verifyLayoutAndAddressSpace(mlir::Operation *) const {
      return mlir::success();
    }
    mlir::arith::FastMathFlags getRequiredFastMath(mlir::Operation *) const {
      return mlir::arith::FastMathFlags::none;
    }
  };
};
} // namespace detail

class ContractOpInterface
    : public mlir::OpInterface<ContractOpInterface,
                               detail::ContractOpInterfaceTraits> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ContractOpInterface)
  using mlir::OpInterface<ContractOpInterface,
                          detail::ContractOpInterfaceTraits>::OpInterface;

  template <typename ConcreteOp>
  struct Trait : public mlir::OpInterface<
                     ContractOpInterface,
                     detail::ContractOpInterfaceTraits>::Trait<ConcreteOp> {
    mlir::LogicalResult verifyShapeAndTypes() { return mlir::success(); }
    mlir::Type getAccumulatorType() { return {}; }
    mlir::LogicalResult verifyLayoutAndAddressSpace() {
      return mlir::success();
    }
    mlir::arith::FastMathFlags getRequiredFastMath() {
      return mlir::arith::FastMathFlags::none;
    }
  };

  mlir::LogicalResult verifyShapeAndTypes() {
    return getImpl()->verifyShapeAndTypes(getImpl(), getOperation());
  }
  mlir::Type getAccumulatorType() {
    return getImpl()->getAccumulatorType(getImpl(), getOperation());
  }
  mlir::LogicalResult verifyLayoutAndAddressSpace() {
    return getImpl()->verifyLayoutAndAddressSpace(getImpl(), getOperation());
  }
  mlir::arith::FastMathFlags getRequiredFastMath() {
    return getImpl()->getRequiredFastMath(getImpl(), getOperation());
  }
};

// Context-free scalar element type categories usable at static Unit::bind time.
enum class ScalarTypeKind : uint8_t {
  Any,
  AnyFloat,
  AnyInteger,
  F8E4M3FN,
  F8E5M2,
  F16,
  BF16,
  F32,
  F64,
  I1,
  I4,
  I8,
  UI8,
  I16,
  I32,
  I64,
  Index,
};
llvm::StringRef spelling(ScalarTypeKind kind);
bool matchScalarType(mlir::Type type, ScalarTypeKind kind);

enum class EffectMask : uint8_t {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Allocate = 1 << 2,
  Free = 1 << 3,
  ReadWrite = Read | Write,
  All = Read | Write | Allocate | Free,
};
inline constexpr EffectMask operator|(EffectMask a, EffectMask b) {
  return static_cast<EffectMask>(static_cast<uint8_t>(a) |
                                 static_cast<uint8_t>(b));
}
inline constexpr EffectMask operator&(EffectMask a, EffectMask b) {
  return static_cast<EffectMask>(static_cast<uint8_t>(a) &
                                 static_cast<uint8_t>(b));
}
inline constexpr EffectMask operator~(EffectMask a) {
  return static_cast<EffectMask>(~static_cast<uint8_t>(a) & 0x0Fu);
}
std::string spelling(EffectMask mask);
EffectMask observedMemoryEffects(mlir::Operation *op, bool *known = nullptr);

struct ShapeRankSpec {
  std::optional<unsigned> minOperands, maxOperands;
  std::optional<unsigned> minResults, maxResults;
  std::vector<std::optional<int64_t>> operandRanks, resultRanks;
  bool sameFirstOperandAndResultShape = false;
};

struct LayoutSpec {
  uint64_t minAlignmentBytes = 0;
  std::optional<uint64_t> requiredMemorySpace;
  bool requireContiguousMinorDim = false;
};

struct NumericalContract {
  mlir::arith::FastMathFlags requiredFastMath =
      mlir::arith::FastMathFlags::none;
  bool requireInputSubnormalFlush = false;
  bool requireOutputSubnormalFlush = false;
};

struct TypedContract {
  ShapeRankSpec shape;
  std::vector<ScalarTypeKind> operandTypes;
  std::vector<ScalarTypeKind> resultTypes;
  std::vector<ScalarTypeKind> accumulatorTypes;
  LayoutSpec layout;
  EffectMask effects = EffectMask::None;
  bool verifyEffectsInterface = true;
  NumericalContract numerical;
  std::function<llvm::Error(mlir::Operation *, const TargetProfile &)>
      precondition;
};

// Populate catalog string summaries from TypedContract when omitted.
void populateTypedContractSummaries(ImplementationContract &contract);

// Candidate-time check (returns structured llvm::Error for DecisionFamily /
// explain) and checkpoint/terminal-time check (emits MLIR diagnostics).
llvm::Error checkTypedContract(mlir::Operation *op,
                               const ImplementationContract &contract,
                               const TargetProfile &target);
mlir::LogicalResult verifyBoundContract(mlir::Operation *op,
                                        const ImplementationContract &contract,
                                        const TargetProfile &target);
mlir::LogicalResult verifyContractOp(mlir::Operation *op,
                                     const TargetProfile &target);

} // namespace compilerinfra
