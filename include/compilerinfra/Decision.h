#ifndef COMPILERINFRA_DECISION_H
#define COMPILERINFRA_DECISION_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/AnalysisManager.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compilerinfra {
struct TargetProfile;
struct DecisionChoice {
  std::vector<std::string>
      strategies; // Ordered; last must decide or report unsupported.
  std::string realizer;
};
using DecisionChoices = std::map<std::string, DecisionChoice>;

struct TargetPreset {
  std::map<std::string, bool> optimizations;
  DecisionChoices decisions;
  std::optional<std::string> route;
  llvm::json::Object
      options; // Product-owned numerical options and target bindings.
};
using CompilerPreset = std::map<std::string, TargetPreset>;
llvm::Expected<CompilerPreset> parsePreset(const llvm::json::Value &);

struct DecisionFamily {
  std::string name;
  std::function<bool(mlir::Operation *)> applies;
  // read is defined on every operation, including rewritten IR. Return a null
  // attribute if the operation carries no outstanding decision.
  std::function<mlir::Attribute(mlir::Operation *)> read;
  std::function<llvm::Error(mlir::Operation *, mlir::Attribute)> write;
  std::function<llvm::Error(mlir::Operation *, mlir::Attribute,
                            const TargetProfile &)>
      verify;
};
struct Strategy {
  std::string name, family;
  // Null = no match. Error = illegal. A strategy only proposes; it must not
  // mutate IR.
  std::function<llvm::Expected<mlir::Attribute>(
      mlir::Operation *, const TargetProfile &, mlir::AnalysisManager &)>
      propose;
};
struct Realizer {
  std::string name, family;
  // Rewrite in postorder and consume the decision. Use the supplied rewriter
  // for erasure/replacement; preserve the top-level module.
  std::function<llvm::Error(mlir::Operation *, mlir::Attribute,
                            const TargetProfile &, mlir::RewriterBase &)>
      apply;
};
struct Decide {
  std::string family;
};
struct Materialize {
  std::string family;
};
struct DecisionEvent {
  std::string family, action, implementation, operation, location, value;
};
using DecisionObserver = std::function<void(const DecisionEvent &)>;

namespace detail {
struct DecisionProgram;
}
} // namespace compilerinfra
#endif
