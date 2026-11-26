#include "compilerinfra/Contract.h"
#include "compilerinfra/Assembly.h"
#include "mlir/IR/BuiltinTypes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace compilerinfra;
using llvm::createStringError;

namespace {
llvm::Expected<mlir::arith::FastMathFlags>
parseFastMathString(llvm::StringRef text) {
  text = text.trim();
  if (text.empty() || text == "none")
    return mlir::arith::FastMathFlags::none;
  mlir::arith::FastMathFlags acc = mlir::arith::FastMathFlags::none;
  llvm::SmallVector<llvm::StringRef, 4> parts;
  text.split(parts, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (auto tok : parts) {
    auto flag = mlir::arith::symbolizeFastMathFlags(tok.trim());
    if (!flag)
      return createStringError("invalid fastmath flag: " + tok.trim());
    acc = acc | *flag;
  }
  return acc;
}
} // namespace

llvm::Expected<NumericalOptions>
NumericalOptions::parse(const llvm::json::Object &obj) {
  NumericalOptions out;
  for (const auto &[k, v] : obj) {
    if (k == "fastmath") {
      auto s = v.getAsString();
      if (!s)
        return createStringError("numerical.fastmath must be a string");
      auto parsed = parseFastMathString(*s);
      if (!parsed)
        return parsed.takeError();
      out.defaultFastMath = *parsed;
      out.explicitFastMath = true;
    } else if (k == "denormal_mode") {
      auto s = v.getAsString();
      if (!s)
        return createStringError("numerical.denormal_mode must be a string");
      auto mode = llvm::parseDenormalFPAttribute(s->trim());
      if (!mode.isValid())
        return createStringError("invalid numerical.denormal_mode: " + *s);
      out.defaultDenormal = mode;
      out.explicitDenormal = true;
    } else {
      return createStringError("unknown numerical configuration field: " +
                               k.str());
    }
  }
  return out;
}

llvm::json::Object NumericalOptions::toJSON() const {
  return llvm::json::Object{
      {"fastmath", mlir::arith::stringifyFastMathFlags(defaultFastMath)},
      {"denormal_mode", defaultDenormal.str()},
  };
}

mlir::arith::FastMathFlags
compilerinfra::effectiveFastMath(mlir::Operation *op,
                                 mlir::arith::FastMathFlags fallback) {
  if (!op)
    return fallback;
  if (auto attr = op->getAttrOfType<mlir::arith::FastMathFlagsAttr>("fastmath"))
    return attr.getValue();
  if (auto sattr = op->getAttrOfType<mlir::StringAttr>("fastmath")) {
    if (auto parsed = parseFastMathString(sattr.getValue()))
      return *parsed;
  }
  if (auto iface = llvm::dyn_cast<mlir::arith::ArithFastMathInterface>(op)) {
    if (auto attr = iface.getFastMathFlagsAttr())
      return attr.getValue();
  }
  return fallback;
}

void compilerinfra::setFastMath(mlir::Operation *op,
                                mlir::arith::FastMathFlags flags) {
  if (!op)
    return;
  op->setAttr("fastmath",
              mlir::arith::FastMathFlagsAttr::get(op->getContext(), flags));
}

mlir::arith::FastMathFlags compilerinfra::intersectFastMath(
    llvm::ArrayRef<mlir::Operation *> sourceOps,
    mlir::arith::FastMathFlags fallback) {
  if (sourceOps.empty())
    return fallback;
  auto acc = mlir::arith::FastMathFlags::fast;
  for (auto *op : sourceOps)
    acc = acc & effectiveFastMath(op, fallback);
  return acc;
}

void compilerinfra::propagateNumericalFlags(
    llvm::ArrayRef<mlir::Operation *> fromOps, mlir::Operation *toOp,
    mlir::arith::FastMathFlags fallback) {
  if (!toOp || fromOps.empty())
    return;
  setFastMath(toOp, intersectFastMath(fromOps, fallback));
}

llvm::DenormalMode
compilerinfra::effectiveDenormalMode(mlir::Operation *op,
                                     llvm::DenormalMode fallback) {
  for (auto *cur = op; cur != nullptr; cur = cur->getParentOp()) {
    for (llvm::StringRef attrName : {"denormal-fp-math", "ci.denormal_mode"}) {
      if (auto s = cur->getAttrOfType<mlir::StringAttr>(attrName)) {
        auto parsed = llvm::parseDenormalFPAttribute(s.getValue());
        if (parsed.isValid())
          return parsed;
      }
    }
  }
  return fallback;
}

void compilerinfra::setDenormalMode(mlir::Operation *scopeOp,
                                    llvm::DenormalMode mode) {
  if (!scopeOp || !mode.isValid())
    return;
  scopeOp->setAttr("denormal-fp-math",
                   mlir::StringAttr::get(scopeOp->getContext(), mode.str()));
}

bool compilerinfra::hasFastMath(mlir::Operation *op,
                                mlir::arith::FastMathFlags required,
                                const NumericalOptions &defaults) {
  return mlir::arith::bitEnumContainsAll(
      effectiveFastMath(op, defaults.defaultFastMath), required);
}

bool compilerinfra::allowsInputSubnormalFlush(
    mlir::Operation *op, const NumericalOptions &defaults) {
  return effectiveDenormalMode(op, defaults.defaultDenormal).inputsAreZero();
}

bool compilerinfra::allowsOutputSubnormalFlush(
    mlir::Operation *op, const NumericalOptions &defaults) {
  return effectiveDenormalMode(op, defaults.defaultDenormal).outputsAreZero();
}

bool compilerinfra::allowsSubnormalFlush(mlir::Operation *op,
                                         const NumericalOptions &defaults) {
  auto mode = effectiveDenormalMode(op, defaults.defaultDenormal);
  return mode.inputsAreZero() && mode.outputsAreZero();
}

llvm::StringRef compilerinfra::spelling(ScalarTypeKind kind) {
  switch (kind) {
  case ScalarTypeKind::Any:
    return "any";
  case ScalarTypeKind::AnyFloat:
    return "float";
  case ScalarTypeKind::AnyInteger:
    return "integer";
  case ScalarTypeKind::F8E4M3FN:
    return "f8E4M3FN";
  case ScalarTypeKind::F8E5M2:
    return "f8E5M2";
  case ScalarTypeKind::F16:
    return "f16";
  case ScalarTypeKind::BF16:
    return "bf16";
  case ScalarTypeKind::F32:
    return "f32";
  case ScalarTypeKind::F64:
    return "f64";
  case ScalarTypeKind::I1:
    return "i1";
  case ScalarTypeKind::I4:
    return "i4";
  case ScalarTypeKind::I8:
    return "i8";
  case ScalarTypeKind::UI8:
    return "ui8";
  case ScalarTypeKind::I16:
    return "i16";
  case ScalarTypeKind::I32:
    return "i32";
  case ScalarTypeKind::I64:
    return "i64";
  case ScalarTypeKind::Index:
    return "index";
  }
  llvm_unreachable("invalid ScalarTypeKind");
}

static mlir::Type leafElementType(mlir::Type type) {
  if (auto shaped = llvm::dyn_cast<mlir::ShapedType>(type))
    return shaped.getElementType();
  return type;
}

bool compilerinfra::matchScalarType(mlir::Type type, ScalarTypeKind kind) {
  if (!type)
    return false;
  mlir::Type elem = leafElementType(type);
  switch (kind) {
  case ScalarTypeKind::Any:
    return true;
  case ScalarTypeKind::AnyFloat:
    return llvm::isa<mlir::FloatType>(elem);
  case ScalarTypeKind::AnyInteger:
    return llvm::isa<mlir::IntegerType, mlir::IndexType>(elem);
  case ScalarTypeKind::F8E4M3FN:
    return llvm::isa<mlir::Float8E4M3FNType>(elem);
  case ScalarTypeKind::F8E5M2:
    return llvm::isa<mlir::Float8E5M2Type>(elem);
  case ScalarTypeKind::F16:
    return elem.isF16();
  case ScalarTypeKind::BF16:
    return elem.isBF16();
  case ScalarTypeKind::F32:
    return elem.isF32();
  case ScalarTypeKind::F64:
    return elem.isF64();
  case ScalarTypeKind::I1:
    return elem.isInteger(1);
  case ScalarTypeKind::I4:
    return elem.isInteger(4);
  case ScalarTypeKind::I8:
    return elem.isInteger(8) && !elem.isUnsignedInteger();
  case ScalarTypeKind::UI8:
    return elem.isUnsignedInteger(8);
  case ScalarTypeKind::I16:
    return elem.isInteger(16);
  case ScalarTypeKind::I32:
    return elem.isInteger(32);
  case ScalarTypeKind::I64:
    return elem.isInteger(64);
  case ScalarTypeKind::Index:
    return elem.isIndex();
  }
  return false;
}

std::string compilerinfra::spelling(EffectMask mask) {
  if (mask == EffectMask::None)
    return "pure";
  llvm::SmallVector<llvm::StringRef, 4> names;
  if ((mask & EffectMask::Read) != EffectMask::None)
    names.push_back("read");
  if ((mask & EffectMask::Write) != EffectMask::None)
    names.push_back("write");
  if ((mask & EffectMask::Allocate) != EffectMask::None)
    names.push_back("allocate");
  if ((mask & EffectMask::Free) != EffectMask::None)
    names.push_back("free");
  return llvm::join(names, ",");
}

EffectMask compilerinfra::observedMemoryEffects(mlir::Operation *op,
                                                bool *known) {
  if (auto iface = llvm::dyn_cast<mlir::MemoryEffectOpInterface>(op)) {
    if (known)
      *known = true;
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance, 4> effects;
    iface.getEffects(effects);
    EffectMask out = EffectMask::None;
    for (const auto &eff : effects) {
      if (llvm::isa<mlir::MemoryEffects::Read>(eff.getEffect()))
        out = out | EffectMask::Read;
      else if (llvm::isa<mlir::MemoryEffects::Write>(eff.getEffect()))
        out = out | EffectMask::Write;
      else if (llvm::isa<mlir::MemoryEffects::Allocate>(eff.getEffect()))
        out = out | EffectMask::Allocate;
      else if (llvm::isa<mlir::MemoryEffects::Free>(eff.getEffect()))
        out = out | EffectMask::Free;
    }
    return out;
  }
  if (known)
    *known = false;
  return EffectMask::All;
}

static std::string joinTypes(llvm::ArrayRef<ScalarTypeKind> kinds) {
  if (kinds.empty())
    return "any";
  llvm::SmallVector<std::string, 4> parts;
  for (auto k : kinds)
    parts.push_back(spelling(k).str());
  return llvm::join(parts, "|");
}

void compilerinfra::populateTypedContractSummaries(
    ImplementationContract &contract) {
  if (!contract.typed)
    return;
  const auto &tc = *contract.typed;
  if (contract.operands.empty())
    contract.operands =
        "in(" + joinTypes(tc.operandTypes) + ")->out(" +
        joinTypes(tc.resultTypes) + ")";
  if (contract.layout.empty()) {
    llvm::SmallVector<std::string, 3> lparts;
    if (tc.layout.requireContiguousMinorDim)
      lparts.push_back("contiguous_minor");
    if (tc.layout.minAlignmentBytes > 0)
      lparts.push_back("align>=" + std::to_string(tc.layout.minAlignmentBytes));
    if (tc.layout.requiredMemorySpace)
      lparts.push_back("space=" +
                       std::to_string(*tc.layout.requiredMemorySpace));
    contract.layout = lparts.empty() ? "any" : llvm::join(lparts, ",");
  }
  if (contract.accumulator.empty())
    contract.accumulator =
        tc.accumulatorTypes.empty() ? "none" : joinTypes(tc.accumulatorTypes);
  if (contract.effects.empty())
    contract.effects = spelling(tc.effects);
  if (contract.numerical.empty()) {
    std::string num =
        "fastmath=" +
        mlir::arith::stringifyFastMathFlags(tc.numerical.requiredFastMath);
    if (tc.numerical.requireInputSubnormalFlush ||
        tc.numerical.requireOutputSubnormalFlush)
      num += ",ftz";
    contract.numerical = std::move(num);
  }
}

static bool matchesAnyKind(mlir::Type type,
                           llvm::ArrayRef<ScalarTypeKind> allowed) {
  if (allowed.empty())
    return true;
  for (auto k : allowed)
    if (matchScalarType(type, k))
      return true;
  return false;
}

llvm::Error compilerinfra::checkTypedContract(
    mlir::Operation *op, const ImplementationContract &contract,
    const TargetProfile &target) {
  if (auto iface = llvm::dyn_cast<ContractOpInterface>(op)) {
    if (mlir::failed(iface.verifyShapeAndTypes()))
      return createStringError(contract.name +
                               ": ContractOpInterface shape/type check failed");
    if (mlir::failed(iface.verifyLayoutAndAddressSpace()))
      return createStringError(
          contract.name +
          ": ContractOpInterface layout/address-space check failed");
    auto reqFm = iface.getRequiredFastMath();
    if (!hasFastMath(op, reqFm, target.numerical))
      return createStringError(
          contract.name + ": operation requires fastmath " +
          mlir::arith::stringifyFastMathFlags(reqFm));
  }
  if (!contract.typed)
    return llvm::Error::success();
  const auto &tc = *contract.typed;

  // 1. Numerical contract (fastmath flags + function denormal mode).
  if (!hasFastMath(op, tc.numerical.requiredFastMath, target.numerical)) {
    auto actual = effectiveFastMath(op, target.numerical.defaultFastMath);
    auto missing = tc.numerical.requiredFastMath & ~actual;
    return createStringError(
        contract.name + ": missing required fastmath flags (" +
        mlir::arith::stringifyFastMathFlags(missing) + ")");
  }
  if (tc.numerical.requireInputSubnormalFlush &&
      !allowsInputSubnormalFlush(op, target.numerical))
    return createStringError(
        contract.name +
        ": requires input denormal flush permission (denormal-fp-math)");
  if (tc.numerical.requireOutputSubnormalFlush &&
      !allowsOutputSubnormalFlush(op, target.numerical))
    return createStringError(
        contract.name +
        ": requires output denormal flush permission (denormal-fp-math)");

  // 2. Shape, arity, and element/accumulator types.
  unsigned nIn = op->getNumOperands(), nOut = op->getNumResults();
  if ((tc.shape.minOperands && nIn < *tc.shape.minOperands) ||
      (tc.shape.maxOperands && nIn > *tc.shape.maxOperands))
    return createStringError(contract.name + ": operand count mismatch");
  if ((tc.shape.minResults && nOut < *tc.shape.minResults) ||
      (tc.shape.maxResults && nOut > *tc.shape.maxResults))
    return createStringError(contract.name + ": result count mismatch");

  for (size_t i = 0; i < tc.shape.operandRanks.size() && i < nIn; ++i) {
    if (!tc.shape.operandRanks[i])
      continue;
    auto st = llvm::dyn_cast<mlir::ShapedType>(op->getOperand(i).getType());
    if (!st || !st.hasRank() || st.getRank() != *tc.shape.operandRanks[i])
      return createStringError(contract.name + ": operand " +
                               std::to_string(i) + " rank mismatch");
  }
  for (size_t i = 0; i < tc.shape.resultRanks.size() && i < nOut; ++i) {
    if (!tc.shape.resultRanks[i])
      continue;
    auto st = llvm::dyn_cast<mlir::ShapedType>(op->getResult(i).getType());
    if (!st || !st.hasRank() || st.getRank() != *tc.shape.resultRanks[i])
      return createStringError(contract.name + ": result " +
                               std::to_string(i) + " rank mismatch");
  }
  if (tc.shape.sameFirstOperandAndResultShape && nIn > 0 && nOut > 0) {
    auto inSt = llvm::dyn_cast<mlir::ShapedType>(op->getOperand(0).getType());
    auto outSt = llvm::dyn_cast<mlir::ShapedType>(op->getResult(0).getType());
    if (!inSt || !outSt || !inSt.hasRank() || !outSt.hasRank() ||
        inSt.getShape() != outSt.getShape())
      return createStringError(contract.name +
                               ": operand/result shape mismatch");
  }
  if (!tc.operandTypes.empty()) {
    for (unsigned i = 0; i < nIn; ++i)
      if (!matchesAnyKind(op->getOperand(i).getType(), tc.operandTypes))
        return createStringError(contract.name + ": unsupported operand " +
                                 std::to_string(i) + " element type");
  }
  if (!tc.resultTypes.empty()) {
    for (unsigned i = 0; i < nOut; ++i)
      if (!matchesAnyKind(op->getResult(i).getType(), tc.resultTypes))
        return createStringError(contract.name + ": unsupported result " +
                                 std::to_string(i) + " element type");
  }
  if (!tc.accumulatorTypes.empty()) {
    mlir::Type accType;
    if (auto iface = llvm::dyn_cast<ContractOpInterface>(op))
      accType = iface.getAccumulatorType();
    if (!accType)
      if (auto ta = op->getAttrOfType<mlir::TypeAttr>("accumulator_type"))
        accType = ta.getValue();
    if (!accType)
      return createStringError(contract.name +
                               ": missing required accumulator type");
    if (!matchesAnyKind(accType, tc.accumulatorTypes))
      return createStringError(contract.name +
                               ": unsupported accumulator type");
  }

  // 3. Layout & address space checks.
  if (tc.layout.minAlignmentBytes > 0) {
    auto alignAttr = op->getAttrOfType<mlir::IntegerAttr>("alignment");
    if (!alignAttr || alignAttr.getValue().isNegative() ||
        alignAttr.getValue().getZExtValue() < tc.layout.minAlignmentBytes)
      return createStringError(
          contract.name +
          ": missing or insufficient alignment attribute (requires >= " +
          std::to_string(tc.layout.minAlignmentBytes) + " bytes)");
  }
  for (mlir::Type ty : op->getOperandTypes()) {
    auto mr = llvm::dyn_cast<mlir::MemRefType>(ty);
    if (!mr)
      continue;
    if (tc.layout.requiredMemorySpace &&
        mr.getMemorySpaceAsInt() != *tc.layout.requiredMemorySpace)
      return createStringError(contract.name + ": operand memory space != " +
                               std::to_string(*tc.layout.requiredMemorySpace));
    if (tc.layout.requireContiguousMinorDim && mr.getRank() > 0) {
      int64_t offset = 0;
      llvm::SmallVector<int64_t, 4> strides;
      if (mlir::failed(mr.getStridesAndOffset(strides, offset)) ||
          strides.back() != 1)
        return createStringError(
            contract.name + ": requires unit minor stride on memref operand");
    }
  }

  // 4. Side-effect verification via MemoryEffectOpInterface.
  if (tc.verifyEffectsInterface) {
    bool known = false;
    EffectMask actual = observedMemoryEffects(op, &known);
    if (!known)
      return createStringError(
          contract.name + ": operation " + op->getName().getStringRef().str() +
          " must implement MemoryEffectOpInterface for typed contract check");
    if ((actual & ~tc.effects) != EffectMask::None)
      return createStringError(contract.name + ": operation has side effects (" +
                               spelling(actual) + ") beyond contract (" +
                               spelling(tc.effects) + ")");
  }

  // 5. Optional domain precondition.
  if (tc.precondition)
    if (auto err = tc.precondition(op, target))
      return err;

  return llvm::Error::success();
}

mlir::LogicalResult compilerinfra::verifyBoundContract(
    mlir::Operation *op, const ImplementationContract &contract,
    const TargetProfile &target) {
  if (auto err = checkTypedContract(op, contract, target)) {
    op->emitError() << llvm::toString(std::move(err));
    return mlir::failure();
  }
  if (contract.verify && mlir::failed(contract.verify(op)))
    return mlir::failure();
  return mlir::success();
}

mlir::LogicalResult
compilerinfra::verifyContractOp(mlir::Operation *op,
                                const TargetProfile &target) {
  auto iface = llvm::dyn_cast<ContractOpInterface>(op);
  if (!iface)
    return mlir::success();
  if (mlir::failed(iface.verifyShapeAndTypes()) ||
      mlir::failed(iface.verifyLayoutAndAddressSpace()))
    return mlir::failure();
  auto reqFm = iface.getRequiredFastMath();
  if (!hasFastMath(op, reqFm, target.numerical))
    return op->emitError("operation missing required fastmath ")
           << mlir::arith::stringifyFastMathFlags(reqFm);
  return mlir::success();
}
