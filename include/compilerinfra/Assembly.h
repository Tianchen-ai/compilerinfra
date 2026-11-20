#ifndef COMPILERINFRA_ASSEMBLY_H
#define COMPILERINFRA_ASSEMBLY_H

#include "compilerinfra/Compiler.h"
#include "mlir/Support/TypeID.h"
#include "llvm/ADT/DenseMap.h"
#include <set>
#include <variant>

namespace compilerinfra {

// Prefer checkout-relative paths in distributable SDKs.
struct SourceRef {
  std::string file;
  unsigned line;
  std::string component;
};
#ifndef COMPILERINFRA_COMPONENT_NAME
#define COMPILERINFRA_COMPONENT_NAME ""
#endif
#define COMPILERINFRA_SOURCE                                                   \
  ::compilerinfra::SourceRef {                                                 \
    __FILE__, __LINE__, COMPILERINFRA_COMPONENT_NAME                           \
  }

// Providers supply typed product interfaces used by shared lowering code.
// MLIR TypeID keeps interface identity independent of the provider name.
class TargetServices {
public:
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
};
using ModelCheck =
    std::function<mlir::LogicalResult(mlir::ModuleOp, const TargetProfile &)>;
using ModelPasses =
    std::function<llvm::Error(mlir::OpPassManager &, const TargetProfile &)>;

struct Model {
  std::string name;
  ModelCheck verify;
  RegisterDialects registerDialects;
};

// A lowering owns program order. It can expose a point exactly where
// independently contributed optimizations may operate on its representation.
struct ExtensionPoint {
  std::string name;
};
using LoweringStep =
    std::variant<ModelPasses, ExtensionPoint, Decide, Materialize>;
struct Lowering {
  std::string name;
  std::string from;
  std::string to;
  std::vector<LoweringStep> steps;
};

struct UseRoute {
  std::string name;
};
struct UseLowering {
  std::string name;
};
struct Route {
  std::string name;
  std::vector<std::variant<UseRoute, UseLowering>> steps;
};

struct Contribution {
  std::string name;
  std::string lowering;
  std::string point;
  ModelPasses build;
  // Named capabilities of the product's selected device/toolchain.
  std::vector<std::string> requiresFeatures;
  // Local ordering only, not a semantic dependency on an enabled sibling.
  std::vector<std::string> after;
  bool enabledByDefault = true;
  // Precedence relative to other independently distributed components.
  std::vector<std::string> afterIfPresent;
};

struct Provider {
  TargetProfile profile;
  std::string route;
  std::string accepts; // Model required by the emitter.
  RegisterDialects registerDialects;
  Emit emit;
  DecisionChoices decisions;
  std::function<llvm::Expected<TargetProfile>(const TargetProfile &,
                                              const llvm::json::Object &)>
      configurePreset;
};

// Registration order is immaterial. Linking is pure declaration work:
// no device discovery, pass construction, compilation, or artifact I/O.
class Assembly {
public:
  void add(Model model, SourceRef source);
  void add(Lowering lowering, SourceRef source);
  void add(Route route, SourceRef source);
  void add(Contribution contribution, SourceRef source);
  void add(Provider provider, SourceRef source);
  void add(DecisionFamily value, SourceRef source);
  void add(Strategy value, SourceRef source);
  void add(Realizer value, SourceRef source);

  llvm::Expected<Compiler> link(Import importer,
                                llvm::StringRef inputModel) const;
  llvm::Expected<Compiler> link(Import importer, llvm::StringRef inputModel,
                                const CompilerPreset &preset) const;

private:
  template <typename T> struct Located {
    T value;
    SourceRef source;
  };
  std::vector<Located<Model>> models;
  std::vector<Located<Lowering>> lowerings;
  std::vector<Located<Route>> routes;
  std::vector<Located<Contribution>> contributions;
  std::vector<Located<Provider>> providers;
  std::vector<Located<DecisionFamily>> families;
  std::vector<Located<Strategy>> strategies;
  std::vector<Located<Realizer>> realizers;
  friend class AssemblyLinker;
  friend class Compiler;
};

} // namespace compilerinfra
#endif
