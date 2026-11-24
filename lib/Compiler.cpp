#include "compilerinfra/Session.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

using namespace compilerinfra;
using llvm::createStringError;

llvm::Expected<Artifact> Compiler::compile(llvm::StringRef exit,
                                           llvm::MemoryBufferRef input) const {
  auto session = open(exit);
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
                                  bool enabledByDefault) {
  return PassGroup(false, std::move(name), std::move(build), enabledByDefault);
}

Compiler::Compiler(std::shared_ptr<const Definition> definition)
    : definition(std::move(definition)) {}

static llvm::Error validateExit(const Exit &exit) {
  if (!exit.printer.package || exit.printer.operations.empty())
    return createStringError("missing terminal printer for exit " + exit.name);

  llvm::StringSet<> stageNames;
  llvm::StringSet<> slotNames;
  for (const Stage &stage : exit.stages) {
    if (stage.name.empty() || !stageNames.insert(stage.name).second)
      return createStringError("empty or duplicate stage name '" + stage.name +
                               "' in exit " + exit.name);
    if (!stage.verifyOutput)
      return createStringError("missing output check for stage " + stage.name);
    for (const PassGroup &group : stage.groups) {
      if (!group.builder())
        return createStringError("missing pass builder in stage " + stage.name);
      if (!group.isRequired() &&
          (group.name().empty() || !slotNames.insert(group.name()).second))
        return createStringError("empty or duplicate optimization name '" +
                                 group.name() + "' in exit " + exit.name);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<Compiler> Compiler::create(Frontend frontend,
                                          std::vector<Exit> exits,
                                          llvm::json::Object description) {
  if (!frontend.import || exits.empty())
    return createStringError("compiler needs an importer and an exit");
  llvm::StringSet<> names;
  for (Exit &exit : exits) {
    if (!frontend.verifyInput && !exit.verifyInput)
      return createStringError("missing input model check for exit " +
                               exit.name);
    if (exit.name.empty() || !names.insert(exit.name).second)
      return createStringError("empty or duplicate exit name '" + exit.name +
                               "'");
    if (exit.error.empty()) {
      if (auto error = validateExit(exit)) {
        exit.error = llvm::toString(std::move(error));
        auto *status = description.getObject("exits")->getObject(exit.name);
        (*status)["valid"] = false;
        (*status)["error"] = exit.error;
        (*description.getObject("selected"))[exit.name] = llvm::json::Array{};
      }
    }
  }
  return Compiler(std::make_shared<Definition>(Definition{
      std::move(frontend), std::move(exits), std::move(description)}));
}

llvm::ArrayRef<Exit> Compiler::exits() const { return definition->exits; }

static llvm::Expected<const Exit *> selectExit(llvm::ArrayRef<Exit> exits,
                                               llvm::StringRef name) {
  for (const Exit &exit : exits)
    if (exit.name == name) {
      if (!exit.error.empty())
        return createStringError(exit.error);
      return &exit;
    }
  return createStringError("unknown compiler exit '" + name + "'");
}

llvm::Expected<std::unique_ptr<Session>>
Compiler::open(llvm::StringRef name, const SessionOptions &options) const {
  auto exit = selectExit(exits(), name);
  if (!exit)
    return exit.takeError();
  return Session::create(definition, **exit, nullptr, options);
}

llvm::Expected<std::unique_ptr<Session>>
Compiler::open(llvm::StringRef name, mlir::MLIRContext &context,
               const SessionOptions &options) const {
  auto exit = selectExit(exits(), name);
  if (!exit)
    return exit.takeError();
  return Session::create(definition, **exit, &context, options);
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
