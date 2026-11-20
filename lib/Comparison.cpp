#include "compilerinfra/Comparison.h"

using namespace compilerinfra;

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
