#ifndef COMPILERINFRA_ASSEMBLY_H
#define COMPILERINFRA_ASSEMBLY_H

#include "compilerinfra/Compiler.h"
#include "compilerinfra/Terminal.h"
#include "mlir/Support/TypeID.h"
#include "llvm/ADT/DenseMap.h"
#include <set>
#include <variant>

namespace compilerinfra {

// Prefer checkout-relative paths in distributable SDKs.
struct SourceRef {
  std::string file;
  unsigned line;
};
#define COMPILERINFRA_SOURCE                                                   \
  ::compilerinfra::SourceRef { __FILE__, __LINE__ }

// Exit implementations bind typed hardware and toolchain services.
// MLIR TypeID keeps interface identity independent of the exit name.
class TargetServices {
public:
  MachineFacts facts;
  // The exit selects the calibrated device/toolchain profile during discovery.
  std::string calibrationProfile;
  template <typename Interface>
  llvm::Error provide(std::shared_ptr<const Interface> implementation) {
    if (!implementation)
      return llvm::createStringError("missing target interface implementation");
    if (!interfaces
             .try_emplace(mlir::TypeID::get<Interface>(),
                          std::move(implementation))
             .second)
      return llvm::createStringError("target interface already bound");
    return llvm::Error::success();
  }
  template <typename Interface> const Interface *get() const {
    auto it = interfaces.find(mlir::TypeID::get<Interface>());
    return it == interfaces.end()
               ? nullptr
               : static_cast<const Interface *>(it->second.get());
  }

private:
  llvm::DenseMap<mlir::TypeID, std::shared_ptr<const void>> interfaces;
};

struct TargetProfile {
  std::string name;
  std::set<std::string> features;
  TargetServices services;
  Reuse reuse = Reuse::Vendor;
  std::map<std::string, ImplementationContract> implementations;
  std::set<std::string> implementationOps;
  std::map<std::string, std::function<llvm::json::Object()>> dependencies;
  llvm::Error checkImplementation(llvm::StringRef name) const;
};
using ModelCheck =
    std::function<mlir::LogicalResult(mlir::ModuleOp, const TargetProfile &)>;
using ModelPasses =
    std::function<llvm::Error(mlir::OpPassManager &, const TargetProfile &)>;

struct Boundary {
  std::string name; // local to the registering node or exit
  ModelPasses lower;
  ModelCheck verify;
};
struct Contribution {
  std::string name, at;
  ModelPasses build;
  std::vector<std::string> after, afterIfPresent;
  bool enabled = true;
};
struct FamilyBinding {
  DecisionFamily family;
  std::string decideAt, materializeAt;
};
struct ExitImplementation {
  using Capabilities = std::function<llvm::Expected<TargetProfile>(const llvm::json::Object &)>;
  ExitImplementation() = default;
  [[deprecated("register a TerminalPrinter to emit verified operations")]]
  ExitImplementation(Emit legacy, Capabilities caps)
      : emit(std::move(legacy)), capabilities(std::move(caps)) {}
  Emit emit;
  Capabilities capabilities;
  std::optional<TerminalPrinter> terminal;
};
template <typename T> struct Located {
  T value;
  SourceRef source;
};

// The generated registration function receives exactly its own unit.
class Unit {
public:
  explicit Unit(std::string id) : id(std::move(id)) {}
  const std::string id;
  RegisterDialects dialects;
  BoundaryCheck input;
  std::vector<Located<Boundary>> boundaries;
  std::vector<Located<Contribution>> contributions;
  std::vector<Located<FamilyBinding>> families;
  std::vector<Located<Strategy>> strategies;
  std::vector<Located<Realizer>> realizers;
  std::vector<Located<CostModel>> costModels;
  std::vector<Located<ImplementationContract>> implementations;
  std::map<std::string, std::function<llvm::json::Object()>> dependencyProbes;
  std::optional<Located<ExitImplementation>> exit;
  std::string qualify(llvm::StringRef local) const {
    return id + "::" + local.str();
  }
};

class InstalledProduct;
class Assembly {
public:
  void bind(llvm::StringRef unit, void (*registerUnit)(Unit &));

private:
  friend class InstalledProduct;
  llvm::Expected<Compiler> link(const llvm::json::Object &manifest,
                                const llvm::json::Object &configuration) const;

  std::map<std::string, Unit> units;
};
} // namespace compilerinfra
#endif
