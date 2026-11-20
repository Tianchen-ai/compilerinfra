#ifndef COMPILERINFRA_COMPILER_H
#define COMPILERINFRA_COMPILER_H

#include "compilerinfra/Decision.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace compilerinfra {
namespace detail {
struct ComponentIndex;
}

using RegisterDialects = std::function<void(mlir::DialectRegistry &)>;
using Import = std::function<llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>(
    llvm::MemoryBufferRef, mlir::MLIRContext &)>;
using Emit =
    std::function<llvm::Error(mlir::ModuleOp, llvm::raw_pwrite_stream &)>;
using BuildPasses = std::function<llvm::Error(mlir::OpPassManager &)>;
using BoundaryCheck = std::function<mlir::LogicalResult(mlir::ModuleOp)>;

struct Frontend {
  RegisterDialects registerDialects;
  Import import;
  // Canonical input admitted by this product, including in-memory JIT calls.
  BoundaryCheck verifyInput;
};

// Required groups always run. Named optimization groups are the only editable
// slots. Builders add native MLIR passes in their actual execution order.
class PassGroup {
public:
  static PassGroup required(BuildPasses build);
  static PassGroup optimization(std::string name, BuildPasses build,
                                bool enabledByDefault = true,
                                std::string point = {});

  bool isRequired() const { return requiredGroup; }
  llvm::StringRef name() const { return slot; }
  bool defaultEnabled() const { return enabled; }
  const BuildPasses &builder() const { return build; }
  llvm::StringRef extensionPoint() const { return point; }
  const detail::DecisionProgram *decision() const { return program.get(); }

private:
  friend class AssemblyLinker;
  friend class Compiler;
  static PassGroup forDecision(detail::DecisionProgram program);
  std::shared_ptr<const detail::DecisionProgram> program;
  PassGroup(bool requiredGroup, std::string slot, BuildPasses build,
            bool enabled);
  bool requiredGroup;
  std::string slot;
  BuildPasses build;
  bool enabled;
  std::string point;
};

struct Stage {
  std::string name;
  std::vector<PassGroup> groups;
  // Checks the representation and target invariants both on exit and on resume.
  BoundaryCheck verifyOutput;
  std::shared_ptr<const char>
      identity; // Process-local sharing across immutable derivations.
};

struct Target {
  std::string name;
  RegisterDialects registerDialects;
  std::vector<Stage> stages;
  Emit emit;
  BoundaryCheck verifyInput;
};

struct Optimization {
  bool enabled = true;
  BuildPasses replacement;
  // Select this eligible sibling instead, at its own declared ordering
  // position.
  std::string useContribution;
};
using Optimizations = std::map<std::string, Optimization>;

struct SessionOptions {
  Optimizations optimizations;
  DecisionObserver observeDecision;
  // Install native MLIR instrumentation/options at session construction.
  std::function<llvm::Error(llvm::StringRef, mlir::PassManager &)>
      configureStage;
};

class Session;
class Assembly;
using Artifact = llvm::SmallVector<char, 0>;

// Construct once per product configuration. Callbacks own their captures.
class Compiler {
public:
  static llvm::Expected<Compiler> create(Frontend frontend,
                                         std::vector<Target> targets);
  llvm::ArrayRef<Target> targets() const;
  llvm::Expected<Compiler> derive(const CompilerPreset &changes) const;
  llvm::Expected<Artifact> compile(llvm::StringRef target,
                                   llvm::MemoryBufferRef input) const;
  bool sharesInputWith(const Compiler &other) const {
    return definition->origin == other.definition->origin;
  }
  // Static queries use the very same routes and optional slots as open().
  llvm::json::Value components() const;
  llvm::Expected<llvm::json::Value> impact(llvm::StringRef component) const;
  llvm::Expected<llvm::json::Value> plan(llvm::StringRef target) const;
  // Assembly-linked products name the model carried by each boundary.
  llvm::Expected<llvm::StringRef> boundaryModel(llvm::StringRef target,
                                                llvm::StringRef boundary) const;

  llvm::Expected<std::unique_ptr<Session>>
  open(llvm::StringRef target, const SessionOptions &options = {}) const;
  // Embed in a product that already owns an MLIR context. The context must
  // outlive this session and be available for its exclusive use during calls.
  llvm::Expected<std::unique_ptr<Session>>
  open(llvm::StringRef target, mlir::MLIRContext &context,
       const SessionOptions &options = {}) const;

private:
  friend class Session;
  friend class Assembly;
  friend class AssemblyLinker;
  static llvm::Expected<Compiler>
  create(Frontend frontend, std::vector<Target> targets,
         std::shared_ptr<const detail::ComponentIndex> index);
  struct Definition {
    Frontend frontend;
    std::vector<Target> targets;
    std::shared_ptr<const detail::ComponentIndex> index;
    std::shared_ptr<const Assembly> assembly;
    std::string inputModel;
    CompilerPreset preset;
    std::shared_ptr<const char> origin = std::make_shared<const char>(0);
  };
  explicit Compiler(std::shared_ptr<const Definition> definition);
  std::shared_ptr<const Definition> definition;
};

// Import assembly or MLIR bytecode using the registered dialects.
llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
importMLIR(llvm::MemoryBufferRef input, mlir::MLIRContext &context);

} // namespace compilerinfra

#endif
