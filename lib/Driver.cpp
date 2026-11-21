#include "compilerinfra/Driver.h"
#include "compilerinfra/Comparison.h"
#include "compilerinfra/Session.h"
#include "mlir/IR/AsmState.h"
#include "mlir/Pass/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace compilerinfra;
using llvm::createStringError;
using llvm::StringRef;

namespace {
namespace cl = llvm::cl;

struct DriverOptions {
  cl::list<std::string> presets{
      "preset",
      cl::desc("Derive using a product preset JSON file (repeatable)")};
  cl::list<std::string> against{
      "against-preset",
      cl::desc("Compare this compiler with its derived preset (repeatable)")};
  cl::opt<bool> layout{"layout", cl::desc("Show linked source layers, units and dependencies")};
  cl::opt<bool> explain{
      "explain",
      cl::desc("Report observed IR decisions for this compilation as JSON")};
  cl::OptionCategory category{"CompilerInfra"};
  cl::opt<bool> compareIR{
      "compare-ir",
      cl::desc(
          "Compare default and edited IR at --through from one common input"),
      cl::cat(category)};
  cl::opt<std::string> target{"target", cl::desc("Select product target"),
                              cl::cat(category)};
  cl::opt<std::string> input{cl::Positional, cl::init("-"), cl::desc("[input]"),
                             cl::cat(category)};
  cl::opt<std::string> output{"o", cl::init("-"),
                              cl::desc("Write artifact to file"),
                              cl::cat(category)};
  cl::opt<bool> list{"list", cl::desc("List declarations as JSON"),
                     cl::cat(category)};
  cl::opt<bool> components{
      "components",
      cl::desc("List source components and extension points as JSON"),
      cl::cat(category)};
  cl::opt<std::string> impact{
      "impact", cl::desc("Describe affected targets for KIND:NAME as JSON"),
      cl::cat(category)};
  cl::opt<std::string> plan{"plan",
                            cl::desc("Describe the linked route and "
                                     "optimization slots for TARGET as JSON"),
                            cl::cat(category)};
  cl::opt<bool> pipeline{"pipeline", cl::desc("Print the assembled pipeline"),
                         cl::cat(category)};
  cl::opt<std::string> after{"after", cl::desc("Resume after boundary"),
                             cl::cat(category)};
  cl::opt<std::string> through{"through", cl::desc("Stop at boundary"),
                               cl::cat(category)};
  cl::opt<std::string> format{"format", cl::init("asm"),
                              cl::desc("Checkpoint encoding: asm or bytecode"),
                              cl::cat(category)};
  cl::list<std::string> enable{"enable", cl::desc("Enable optimization"),
                               cl::cat(category)};
  cl::list<std::string> disable{"disable", cl::desc("Disable optimization"),
                                cl::cat(category)};
  cl::list<std::string> replace{
      "replace", cl::desc("Replace slot: NAME=MLIR-PASS-PIPELINE"),
      cl::cat(category)};
};

llvm::Error flushStdout() {
  auto &os = llvm::outs();
  os.flush();
  auto ec = os.error();
  os.clear_error();
  return llvm::errorCodeToError(ec);
}

void printCatalog(const Compiler &compiler) {
  llvm::json::OStream json(llvm::outs(), 2);
  json.array([&] {
    for (const Target &target : compiler.targets())
      json.object([&] {
        json.attribute("target", target.name);
        json.attributeArray("stages", [&] {
          for (const Stage &stage : target.stages)
            json.object([&] {
              json.attribute("name", stage.name);
              json.attributeArray("optimizations", [&] {
                for (const PassGroup &group : stage.groups)
                  if (!group.isRequired())
                    json.object([&] {
                      json.attribute("name", group.name());
                      json.attribute("default", group.defaultEnabled());
                    });
              });
            });
        });
      });
  });
  llvm::outs() << '\n';
}

llvm::Expected<SessionOptions> sessionOptions(const DriverOptions &cmd) {
  SessionOptions settings;
  auto add = [&](StringRef name, Optimization op) -> llvm::Error {
    if (!settings.optimizations.emplace(name.str(), std::move(op)).second)
      return createStringError("duplicate optimization override: " + name);
    return llvm::Error::success();
  };
  for (const auto &name : cmd.enable)
    if (auto error = add(name, {true, {}}))
      return error;
  for (const auto &name : cmd.disable)
    if (auto error = add(name, {false, {}}))
      return error;
  for (const auto &spec : cmd.replace) {
    auto [name, pipeline] = StringRef(spec).split('=');
    if (name.empty() || pipeline.empty())
      return createStringError("--replace requires NAME=MLIR-PASS-PIPELINE");
    if (pipeline.consume_front("@")) {
      if (pipeline.empty())
        return createStringError("expected a contribution name after @");
      if (auto error = add(name, {true, {}, pipeline.str()}))
        return error;
      continue;
    }
    auto build = [text =
                      pipeline.str()](mlir::OpPassManager &pm) -> llvm::Error {
      std::string detail;
      llvm::raw_string_ostream diagnostic(detail);
      if (mlir::failed(mlir::parsePassPipeline(text, pm, diagnostic)))
        return createStringError(detail);
      return llvm::Error::success();
    };
    if (auto error = add(name, {true, std::move(build)}))
      return error;
  }
  settings.configureStage = [](StringRef name,
                               mlir::PassManager &pm) -> llvm::Error {
    if (mlir::failed(mlir::applyPassManagerCLOptions(pm)))
      return createStringError("invalid MLIR options for stage " + name);
    return llvm::Error::success();
  };
  return settings;
}

llvm::Error writeOutput(StringRef path, llvm::ArrayRef<char> bytes) {
  if (path == "-") {
    if (auto ec = llvm::sys::ChangeStdoutToBinary())
      return llvm::errorCodeToError(ec);
    auto &os = llvm::outs();
    os.write(bytes.data(), bytes.size());
    return flushStdout();
  }
  auto temp = llvm::sys::fs::TempFile::create(path + ".tmp-%%%%%%");
  if (!temp)
    return temp.takeError();
  llvm::raw_fd_ostream os(temp->FD, false);
  os.write(bytes.data(), bytes.size());
  os.flush();
  if (auto ec = os.error()) {
    os.clear_error();
    return llvm::joinErrors(llvm::errorCodeToError(ec), temp->discard());
  }
  return temp->keep(path);
}

llvm::Expected<Compiler> deriveFile(const Compiler &compiler,
                                    llvm::StringRef path) {
  auto input = llvm::MemoryBuffer::getFile(path);
  if (!input)
    return createStringError(input.getError(), "cannot read preset " + path);
  auto value = llvm::json::parse((*input)->getBuffer());
  if (!value)
    return value.takeError();
  auto preset = parsePreset(*value);
  if (!preset)
    return preset.takeError();
  return compiler.derive(*preset);
}
llvm::Error run(const DriverOptions &cmd, const Compiler &compiler,
                const Compiler &root) {
  unsigned queries = unsigned(cmd.layout) + unsigned(cmd.list) + unsigned(cmd.components) +
                     cmd.impact.getNumOccurrences() +
                     cmd.plan.getNumOccurrences();
  if (queries) {
    if (queries != 1 || cmd.target.getNumOccurrences() || cmd.pipeline ||
        cmd.explain || !cmd.against.empty() || cmd.compareIR ||
        cmd.after.getNumOccurrences() || cmd.through.getNumOccurrences() ||
        cmd.input.getNumOccurrences() || cmd.output.getNumOccurrences() ||
        cmd.format.getNumOccurrences() || !cmd.enable.empty() ||
        !cmd.disable.empty() || !cmd.replace.empty())
      return createStringError(
          "use a single declaration query without execution options");
    if (cmd.layout) {
      llvm::outs() << llvm::formatv("{0:2}\n", compiler.layout());
    } else if (cmd.list) {
      printCatalog(compiler);
    } else if (cmd.components) {
      llvm::outs() << llvm::formatv("{0:2}\n", compiler.components());
    } else {
      auto value = cmd.plan.getNumOccurrences()
                       ? compiler.plan(cmd.plan.getValue())
                       : compiler.impact(cmd.impact.getValue());
      if (!value)
        return value.takeError();
      llvm::outs() << llvm::formatv("{0:2}\n", *value);
    }
    return flushStdout();
  }
  if (cmd.target.empty())
    return createStringError("select a target with --target NAME");
  StageRange range;
  if (cmd.after.getNumOccurrences())
    range.after = cmd.after.getValue();
  if (cmd.through.getNumOccurrences())
    range.through = cmd.through.getValue();
  if (cmd.format.getNumOccurrences() &&
      (!range.through || (cmd.format != "asm" && cmd.format != "bytecode")))
    return createStringError(
        "--format requires --through and either asm or bytecode");
  if (cmd.pipeline &&
      (range.after || range.through || cmd.output.getNumOccurrences() ||
       cmd.input.getNumOccurrences()))
    return createStringError("--pipeline does not read or write artifacts");
  auto settings = sessionOptions(cmd);
  if (!settings)
    return settings.takeError();
  llvm::json::Array decisions;
  if (cmd.explain) {
    if (cmd.compareIR || cmd.pipeline || cmd.format.getNumOccurrences())
      return createStringError("--explain takes a compile request");
    settings->observeDecision = [&](const DecisionEvent &e) {
      decisions.push_back(
          llvm::json::Object{{"family", e.family},
                             {"action", e.action},
                             {"implementation", e.implementation},
                             {"operation", e.operation},
                             {"location", e.location},
                             {"value", e.value}});
    };
  }
  if (!cmd.against.empty() &&
      (!cmd.compareIR || !settings->optimizations.empty()))
    return createStringError(
        "--against-preset requires --compare-ir without slot overrides");
  if (cmd.compareIR) {
    if (cmd.pipeline || cmd.format.getNumOccurrences() || !range.through ||
        (settings->optimizations.empty() && cmd.against.empty()))
      return createStringError("--compare-ir requires --through and an "
                               "optimization edit; output is JSON");
    auto input = llvm::MemoryBuffer::getFileOrSTDIN(cmd.input.getValue());
    if (!input)
      return createStringError(input.getError(),
                               "cannot read " + cmd.input.getValue());
    auto pair = [&]() -> llvm::Expected<IRComparison> {
      if (cmd.against.empty())
        return compilerinfra::compareIR(compiler, cmd.target.getValue(),
                                        (*input)->getMemBufferRef(), range,
                                        *settings);
      Compiler other = root;
      for (const auto &path : cmd.against) {
        auto c = deriveFile(other, path);
        if (!c)
          return c.takeError();
        other = std::move(*c);
      }
      return compareCompilers(compiler, other, cmd.target.getValue(),
                              (*input)->getMemBufferRef(), range);
    }();
    if (!pair)
      return pair.takeError();
    llvm::json::Object data{
        {"shared_through", pair->sharedThrough},
        {"through", *range.through},
        {"baseline", StringRef(pair->baseline.data(), pair->baseline.size())},
        {"candidate",
         StringRef(pair->candidate.data(), pair->candidate.size())}};
    std::string bytes =
        llvm::formatv("{0}\n", llvm::json::Value(std::move(data))).str();
    return writeOutput(cmd.output.getValue(),
                       llvm::ArrayRef<char>(bytes.data(), bytes.size()));
  }
  auto session = compiler.open(cmd.target.getValue(), *settings);
  if (!session)
    return session.takeError();
  if (cmd.pipeline) {
    (*session)->printPipeline(llvm::outs());
    return flushStdout();
  }
  if (auto error = (*session)->checkOptimizationRange(range))
    return error;
  auto input = llvm::MemoryBuffer::getFileOrSTDIN(cmd.input.getValue());
  if (!input)
    return createStringError(input.getError(),
                             "cannot read " + cmd.input.getValue());
  auto format = cmd.format == "asm" ? IRFormat::Assembly : IRFormat::Bytecode;
  auto result = (*session)->compile((*input)->getMemBufferRef(), range, format);
  if (!result)
    return result.takeError();
  if (cmd.explain) {
    std::string json =
        llvm::formatv("{0}\n", llvm::json::Value(llvm::json::Object{
                                   {"target", cmd.target.getValue()},
                                   {"decisions", std::move(decisions)},
                                   {"artifact_bytes", result->size()}}))
            .str();
    return writeOutput(cmd.output.getValue(),
                       llvm::ArrayRef<char>(json.data(), json.size()));
  }
  return writeOutput(cmd.output.getValue(), *result);
}
} // namespace

int compilerinfra::driverMain(
    int argc, const char *const *argv,
    llvm::function_ref<llvm::Expected<Compiler>()> makeCompiler) {
  DriverOptions cmd;
  mlir::registerPassManagerCLOptions();
  mlir::registerMLIRContextCLOptions();
  mlir::registerAsmPrinterCLOptions();
  if (!llvm::cl::ParseCommandLineOptions(argc, argv, "Product compiler\n",
                                         &llvm::errs()))
    return 1;
  auto compiler = makeCompiler();
  auto root = compiler ? std::optional<Compiler>(*compiler) : std::nullopt;
  for (const auto &file : cmd.presets) {
    if (!compiler)
      break;
    compiler = deriveFile(*compiler, file);
  }
  auto error = compiler ? run(cmd, *compiler, *root) : compiler.takeError();
  if (error) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs(),
                                llvm::Twine(argv[0]) + ": ");
    return 1;
  }
  return 0;
}

int compilerinfra::driverMain(int argc, const char *const *argv,
                              const Compiler &compiler) {
  return driverMain(argc, argv,
                    [&]() -> llvm::Expected<Compiler> { return compiler; });
}
