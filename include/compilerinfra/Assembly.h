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

struct Boundary {
  std::string name; // local to this node
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
struct Exit {
  ModelPasses legalize;
  Emit emit;
  std::function<llvm::Expected<TargetProfile>(const llvm::json::Object &)>
      capabilities;
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
  std::optional<Located<Exit>> exit;
  std::string qualify(llvm::StringRef local) const {
    return id + "::" + local.str();
  }
};

class Assembly {
public:
  void bind(llvm::StringRef unit, void (*registerUnit)(Unit &));
  llvm::Expected<Compiler> link(const llvm::json::Object &manifest,
                                const llvm::json::Object &configuration) const;

private:
  std::map<std::string, Unit> units;
};
} // namespace compilerinfra
#endif
