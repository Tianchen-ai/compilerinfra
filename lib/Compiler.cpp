#include "compilerinfra/Session.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

using namespace compilerinfra;
using llvm::createStringError;

llvm::Expected<Artifact> Compiler::compile(llvm::StringRef target,
                                           llvm::MemoryBufferRef input) const {
  auto session = open(target);
  if (!session)
    return session.takeError();
  return (*session)->compile(input);
}

PassGroup::PassGroup(bool requiredGroup, std::string slot, BuildPasses build,
                     bool enabled)
    : requiredGroup(requiredGroup), slot(std::move(slot)),
      build(std::move(build)), enabled(enabled) {}

PassGroup PassGroup::required(BuildPasses build) {
  return PassGroup(true, {}, std::move(build), true);
}

PassGroup PassGroup::optimization(std::string name, BuildPasses build,
                                  bool enabledByDefault, std::string point) {
  PassGroup group(false, std::move(name), std::move(build), enabledByDefault);
  group.point = std::move(point);
  return group;
}

Compiler::Compiler(std::shared_ptr<const Definition> definition)
    : definition(std::move(definition)) {}

static llvm::Error validateTarget(const Target &target) {
  if (!target.emit)
    return createStringError("missing artifact emitter for target " +
                             target.name);

  llvm::StringSet<> stageNames;
  stageNames.insert(
      "input"); // Boundary immediately after the product importer.
  llvm::StringSet<> slotNames;
  for (const Stage &stage : target.stages) {
    if (stage.name.empty() || !stageNames.insert(stage.name).second)
      return createStringError("empty or duplicate stage name '" + stage.name +
                               "' in target " + target.name);
    if (!stage.verifyOutput)
      return createStringError("missing output check for stage " + stage.name);
    for (const PassGroup &group : stage.groups) {
      if (!group.builder())
        return createStringError("missing pass builder in stage " + stage.name);
      if (!group.isRequired() &&
          (group.name().empty() || !slotNames.insert(group.name()).second))
        return createStringError("empty or duplicate optimization name '" +
                                 group.name() + "' in target " + target.name);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<Compiler>
Compiler::create(Frontend frontend, std::vector<Target> targets,
                 llvm::json::Object description) {
  if (!frontend.import || targets.empty())
    return createStringError("compiler needs an importer and a target");
  llvm::StringSet<> names;
  for (Target &target : targets) {
    for (auto &stage : target.stages)
      stage.identity = std::make_shared<const char>(0);
    if (!frontend.verifyInput && !target.verifyInput)
      return createStringError("missing input model check for target " +
                               target.name);
    if (target.name.empty() || !names.insert(target.name).second)
      return createStringError("empty or duplicate target name '" +
                               target.name + "'");
    if (auto error = validateTarget(target))
      return error;
  }
  return Compiler(std::make_shared<Definition>(
      Definition{std::move(frontend), std::move(targets), std::move(description)}));
}

llvm::ArrayRef<Target> Compiler::targets() const { return definition->targets; }

static llvm::Expected<const Target *>
selectTarget(llvm::ArrayRef<Target> targets, llvm::StringRef name) {
  for (const Target &target : targets)
    if (target.name == name)
      return &target;
  return createStringError("unknown compiler target '" + name + "'");
}

llvm::Expected<std::unique_ptr<Session>>
Compiler::open(llvm::StringRef name, const SessionOptions &options) const {
  auto target = selectTarget(targets(), name);
  if (!target)
    return target.takeError();
  return Session::create(definition, **target, nullptr, options);
}

llvm::Expected<std::unique_ptr<Session>>
Compiler::open(llvm::StringRef name, mlir::MLIRContext &context,
               const SessionOptions &options) const {
  auto target = selectTarget(targets(), name);
  if (!target)
    return target.takeError();
  return Session::create(definition, **target, &context, options);
}

llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
compilerinfra::importMLIR(llvm::MemoryBufferRef input,
                          mlir::MLIRContext &context) {
  // Bytecode resources may retain the source manager beyond the parser call.
  auto sources = std::make_shared<llvm::SourceMgr>();
  sources->AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy(input.getBuffer(),
                                           input.getBufferIdentifier()),
      llvm::SMLoc());
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(sources, &context);
  if (!module)
    return createStringError("unable to parse MLIR from " +
                             input.getBufferIdentifier());
  return std::move(module);
}
