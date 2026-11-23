#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"
#include <functional>
#include <map>

namespace compilerinfra {
struct TargetProfile;

// The visitor owns recursion. A rule spells only its own syntax at each point.
enum class PrintPoint {
  Enter,
  BeginRegion,
  BeginBlock,
  EndBlock,
  EndRegion,
  Leave
};
class PrintContext {
public:
  std::string name(mlir::Value value);
  std::string label(mlir::Block *block);
  llvm::Expected<llvm::StringRef> source(llvm::StringRef implementation) const;
  void line(const llvm::Twine &text);
  unsigned indent = 0;
  unsigned region = 0, block = 0;
  std::map<std::string, std::string> files;
  std::string file;

private:
  friend struct TerminalPrinter;
  const std::map<std::string, std::string> *sources = nullptr;
  llvm::DenseMap<mlir::Value, unsigned> names;
  llvm::DenseMap<mlir::Block *, unsigned> labels;
};
using PrintRule =
    std::function<llvm::Error(mlir::Operation &, PrintPoint, PrintContext &)>;
struct TerminalPrinter {
  std::map<std::string, std::string> runtimeSources;
  std::map<std::string, PrintRule> operations;
  std::function<llvm::Error(mlir::DictionaryAttr,
                            const std::map<std::string, std::string> &,
                            llvm::raw_pwrite_stream &)>
      package;
  mlir::LogicalResult verify(mlir::Operation *, const TargetProfile &) const;
  llvm::Error emit(mlir::Operation *, llvm::raw_pwrite_stream &) const;
};

enum class Reuse { Native, Vendor, Library };
llvm::StringRef spelling(Reuse level);
enum class ImplementationSource { Builtin, Runtime, External };
struct ImplementationContract {
  std::string name, operation, symbol, resource;
  ImplementationSource source = ImplementationSource::Builtin;
  Reuse minimum = Reuse::Native;
  std::string operands, layout, accumulator, effects, numerical, estimator;
  std::vector<std::string> costParameters, dependencies;
  std::function<llvm::Error(const TargetProfile &)> available;
  std::function<mlir::LogicalResult(mlir::Operation *)> verify;
};
} // namespace compilerinfra
