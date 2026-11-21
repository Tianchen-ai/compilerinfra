#include "compilerinfra/Session.h"
#include "DecisionProgram.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace compilerinfra;
using llvm::createStringError;
using mlir::ModuleOp;

namespace {
struct ExecutableStage {
  const Stage &spec;
  std::unique_ptr<mlir::PassManager> passes;
};

// Preserve errors and attached notes for library callers. Warnings and remarks
// continue to the product's diagnostic handlers.
class Diagnostics {
public:
  Diagnostics(mlir::MLIRContext &context, bool forward)
      : handler(&context, [this, forward](mlir::Diagnostic &diagnostic) {
          if (diagnostic.getSeverity() != mlir::DiagnosticSeverity::Error)
            return mlir::failure();
          llvm::raw_string_ostream out(messages);
          out << diagnostic.getLocation() << ": " << diagnostic << '\n';
          for (const auto &note : diagnostic.getNotes())
            out << note.getLocation() << ": note: " << note << '\n';
          return mlir::success(!forward);
        }) {}

  llvm::Error finish(llvm::Error error) {
    if (messages.empty())
      return error;
    return llvm::joinErrors(std::move(error), createStringError(messages));
  }

private:
  std::string messages;
  mlir::ScopedDiagnosticHandler handler;
};

struct Bounds {
  size_t begin, end; // half-open stage interval
};
} // namespace

struct Session::Impl {
  Impl(std::shared_ptr<const Compiler::Definition> definition,
       const Target &target)
      : definition(std::move(definition)), target(target) {}

  std::shared_ptr<const Compiler::Definition> definition;
  const Target &target;
  std::unique_ptr<mlir::MLIRContext> ownedContext;
  mlir::MLIRContext *context;
  std::vector<ExecutableStage> stages;

  llvm::Expected<size_t> past(llvm::StringRef name) const {
    if (!name.contains('@'))
      return createStringError("expected node@boundary, got " + name);
    for (size_t i = 0; i < stages.size(); ++i)
      if (stages[i].spec.name == name)
        return i + 1;
    return createStringError("unknown stage '" + name + "' for target " +
                             target.name);
  }

  llvm::Expected<Bounds> bounds(const StageRange &range) const {
    Bounds result{0, stages.size()};
    if (range.after) {
      auto i = past(*range.after);
      if (!i)
        return i.takeError();
      result.begin = *i;
    }
    if (range.through) {
      auto i = past(*range.through);
      if (!i)
        return i.takeError();
      result.end = *i;
    }
    if (result.end < result.begin)
      return createStringError("stop stage precedes resume stage for target " +
                               target.name);
    return result;
  }

  llvm::Error checkBoundary(const ExecutableStage &stage,
                            ModuleOp module) const {
    if (mlir::failed(stage.spec.verifyOutput(module)))
      return createStringError(target.name +
                               ": invalid IR at output boundary '" +
                               stage.spec.name + "'");
    return llvm::Error::success();
  }

  llvm::Error run(ModuleOp module, Bounds bounds, Observer observe) {
    if (!module || module.getContext() != context)
      return createStringError(target.name +
                               ": module must belong to the session context");
    if (mlir::failed(mlir::verify(module)))
      return createStringError(target.name + ": invalid input module");
    if (bounds.begin) {
      if (auto error = checkBoundary(stages[bounds.begin - 1], module))
        return error;
      if (mlir::failed(detail::verifyDecisionCheckpoint(
              llvm::ArrayRef(target.stages).take_front(bounds.begin), module)))
        return createStringError("invalid decision state at checkpoint " +
                                 stages[bounds.begin - 1].spec.name);
    } else {
      if ((definition->frontend.verifyInput &&
           mlir::failed(definition->frontend.verifyInput(module))) ||
          (target.verifyInput && mlir::failed(target.verifyInput(module))))
        return createStringError(
            target.name +
            ": module does not satisfy the frontend input boundary");
      if (observe)
        if (auto error = observe("input", module))
          return error;
    }

    for (size_t i = bounds.begin; i < bounds.end; ++i) {
      ExecutableStage &stage = stages[i];
      if (mlir::failed(stage.passes->run(module)))
        return createStringError(target.name + ": lowering failed in stage '" +
                                 stage.spec.name + "'");
      if (auto error = checkBoundary(stage, module))
        return error;
      if (observe)
        if (auto error = observe(stage.spec.name, module))
          return error;
    }
    return llvm::Error::success();
  }
};

Session::Session(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
Session::~Session() = default;
mlir::MLIRContext &Session::context() { return *impl->context; }

void Session::printPipeline(llvm::raw_ostream &output) const {
  for (const ExecutableStage &stage : impl->stages) {
    output << stage.spec.name << ": ";
    stage.passes->printAsTextualPipeline(output);
    output << '\n';
  }
}

llvm::Expected<std::unique_ptr<Session>>
Session::create(std::shared_ptr<const Compiler::Definition> definition,
                const Target &target, mlir::MLIRContext *external,
                const SessionOptions &options) {
  auto state = std::make_unique<Impl>(std::move(definition), target);
  const Frontend &frontend = state->definition->frontend;
  mlir::DialectRegistry dialects;
  if (frontend.registerDialects)
    frontend.registerDialects(dialects);
  if (target.registerDialects)
    target.registerDialects(dialects);
  if (external) {
    external->appendDialectRegistry(dialects);
    state->context = external;
  } else {
    state->ownedContext = std::make_unique<mlir::MLIRContext>(dialects);
    state->context = state->ownedContext.get();
  }
  for (const Stage &stage : target.stages) {
    auto passes = std::make_unique<mlir::PassManager>(
        state->context, ModuleOp::getOperationName());
    passes->enableVerifier();
    for (const PassGroup &group : stage.groups) {
      const BuildPasses *build = &group.builder();
      BuildPasses observed;
      if (group.decision() && options.observeDecision) {
        observed = group.decision()->builder(options.observeDecision);
        build = &observed;
      }
      if (group.defaultEnabled())
        if (auto error = (*build)(*passes))
          return createStringError(target.name + "/" + stage.name + " " +
                                   group.name() + ": " +
                                   llvm::toString(std::move(error)));
    }
    state->stages.push_back({stage, std::move(passes)});
  }
  return std::unique_ptr<Session>(new Session(std::move(state)));
}

llvm::Error Session::lower(ModuleOp module, const StageRange &range,
                           Observer observe) {
  auto bounds = impl->bounds(range);
  if (!bounds)
    return bounds.takeError();
  Diagnostics diagnostics(context(), !impl->ownedContext);
  return diagnostics.finish(impl->run(module, *bounds, observe));
}

llvm::Expected<Artifact> Session::compile(llvm::MemoryBufferRef input,
                                          const StageRange &range,
                                          IRFormat format, Observer observe) {
  auto bounds = impl->bounds(range);
  if (!bounds)
    return bounds.takeError();
  auto read = [&]() -> llvm::Expected<mlir::OwningOpRef<ModuleOp>> {
    Diagnostics diagnostics(context(), !impl->ownedContext);
    auto module = range.after
                      ? importMLIR(input, context())
                      : impl->definition->frontend.import(input, context());
    if (!module)
      return diagnostics.finish(module.takeError());
    if (auto error = diagnostics.finish(llvm::Error::success()))
      return error;
    return module;
  };
  auto module = read();
  if (!module)
    return module.takeError();
  return compile(std::move(*module), range, format, observe);
}

llvm::Expected<Artifact> Session::compile(mlir::OwningOpRef<ModuleOp> module,
                                          const StageRange &range,
                                          IRFormat format, Observer observe) {
  if (auto error = lower(module.get(), range, observe))
    return error;
  Diagnostics diagnostics(context(), !impl->ownedContext);
  Artifact bytes;
  llvm::raw_svector_ostream output(bytes);
  auto error = range.through ? writeIR(module.get(), format, output)
                             : impl->target.emit(module.get(), output);
  if (auto result = diagnostics.finish(std::move(error)))
    return result;
  return bytes;
}

llvm::Error Session::writeIR(ModuleOp module, IRFormat format,
                             llvm::raw_ostream &output) {
  if (format == IRFormat::Bytecode) {
    if (mlir::failed(mlir::writeBytecodeToFile(module, output)))
      return createStringError("failed to write MLIR bytecode");
  } else {
    module.print(output, mlir::OpPrintingFlags().enableDebugInfo());
    output << '\n';
  }
  return llvm::Error::success();
}
