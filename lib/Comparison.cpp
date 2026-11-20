#include "compilerinfra/Comparison.h"

using namespace compilerinfra;

llvm::Expected<IRComparison> compilerinfra::compareCompilers(
    const Compiler &a, const Compiler &b, llvm::StringRef target,
    llvm::MemoryBufferRef input, const StageRange &range) {
  if (!range.through)
    return llvm::createStringError("comparison needs an output boundary");
  auto firstModel = a.boundaryModel(target, *range.through);
  if (!firstModel)
    return firstModel.takeError();
  auto secondModel = b.boundaryModel(target, *range.through);
  if (!secondModel)
    return secondModel.takeError();
  if (*firstModel != *secondModel)
    return llvm::createStringError("different models at boundary " +
                                   *range.through + ": " + *firstModel +
                                   " and " + *secondModel);
  auto left = a.open(target);
  if (!left)
    return left.takeError();
  auto right = b.open(target);
  if (!right)
    return right.takeError();
  if (auto e = (*left)->checkOptimizationRange(range))
    return std::move(e);
  if (auto e = (*right)->checkOptimizationRange(range))
    return std::move(e);
  auto lookup = [&](const Compiler &c) -> const Target & {
    return *llvm::find_if(c.targets(),
                          [&](const Target &t) { return t.name == target; });
  };
  StageRange suffix = range;
  Artifact checkpoint;
  std::string common;
  if (a.sharesInputWith(b)) {
    common = "input";
    bool includesStart = !range.after || *range.after == "input";
    for (auto pair : llvm::zip(lookup(a).stages, lookup(b).stages)) {
      auto &[s, t] = pair;
      if (common == *range.through || s.identity != t.identity)
        break;
      common = s.name;
      if (range.after == common)
        includesStart = true;
    }
    if (!includesStart)
      return llvm::createStringError(
          "checkpoint follows a changed stage; supply the original input");
    auto bytes =
        (*left)->compile(input, {range.after, common}, IRFormat::Bytecode);
    if (!bytes)
      return bytes.takeError();
    checkpoint = std::move(*bytes);
    input = llvm::MemoryBufferRef(
        llvm::StringRef(checkpoint.data(), checkpoint.size()), common);
    suffix.after = common;
  } else if (range.after)
    return llvm::createStringError(
        "independent definitions need the original input");
  auto x = (*left)->compile(input, suffix, IRFormat::Assembly);
  if (!x)
    return x.takeError();
  auto y = (*right)->compile(input, suffix, IRFormat::Assembly);
  if (!y)
    return y.takeError();
  return IRComparison{std::move(common), std::move(*x), std::move(*y)};
}

llvm::Expected<IRComparison>
compilerinfra::compareIR(const Compiler &compiler, llvm::StringRef target,
                         llvm::MemoryBufferRef input, const StageRange &range,
                         const SessionOptions &candidate) {
  if (!range.through || candidate.optimizations.empty())
    return llvm::createStringError("IR comparison requires a stop boundary and "
                                   "an explicit optimization edit");
  auto edited = compiler.open(target, candidate);
  if (!edited)
    return edited.takeError();
  if (auto error = (*edited)->checkOptimizationRange(range))
    return error;

  SessionOptions defaults;
  defaults.configureStage = candidate.configureStage;
  auto original = compiler.open(target, defaults);
  if (!original)
    return original.takeError();

  // checkOptimizationRange already resolved the names. A named substitution
  // can only affect a sibling in the same stage as the requested edit.
  const Target &selected = *llvm::find_if(
      compiler.targets(), [&](const Target &t) { return t.name == target; });
  std::string boundary = "input";
  for (const Stage &stage : selected.stages) {
    if (llvm::any_of(stage.groups, [&](const PassGroup &group) {
          return !group.isRequired() &&
                 candidate.optimizations.count(group.name().str());
        }))
      break;
    boundary = stage.name;
  }
  auto common =
      (*original)->compile(input, {range.after, boundary}, IRFormat::Bytecode);
  if (!common)
    return common.takeError();
  llvm::MemoryBufferRef buffer(llvm::StringRef(common->data(), common->size()),
                               boundary);
  StageRange suffix{boundary, range.through};
  auto baseline = (*original)->compile(buffer, suffix, IRFormat::Assembly);
  if (!baseline)
    return baseline.takeError();
  auto changed = (*edited)->compile(buffer, suffix, IRFormat::Assembly);
  if (!changed)
    return changed.takeError();
  return IRComparison{std::move(boundary), std::move(*baseline),
                      std::move(*changed)};
}
