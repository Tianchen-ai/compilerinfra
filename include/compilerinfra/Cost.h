#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace compilerinfra {
struct TargetProfile;

// value is the complete domain-owned decision, including every admissible row.
struct StructuralCandidate {
  std::string id, topology;
  mlir::Attribute value;
  std::vector<std::string> preconditions;
};
struct Quantity {
  double value;
  std::string unit;
};
struct MachineFact {
  llvm::json::Value value;
  std::string unit, version;
  llvm::json::Object source;
};
class MachineFacts {
public:
  llvm::Error query(std::string name, MachineFact fact);
  llvm::Error loadCalibration(const llvm::json::Object &document);
  const MachineFact *find(llvm::StringRef name) const;

private:
  std::map<std::string, MachineFact> values;
};
struct ProvenInterval {
  std::optional<double> lowerNs, upperNs;
  std::string proof;
};
struct RowPrediction {
  std::map<std::string, Quantity> terms;
  std::optional<double> timeNs;
};
struct CostEstimate {
  std::map<std::string, Quantity> terms;
  std::optional<double> timeNs;
  std::string bottleneck, scope;
  std::vector<std::string> missing;
  ProvenInterval bounds;
  // Optional estimates for native rows, keyed by the stable row IDs in value.
  std::map<std::string, RowPrediction> rows;
};
class CostContext {
public:
  const TargetProfile &target;
  // A unit mismatch is a missing fact. Never substitute a guessed value.
  const MachineFact *fact(llvm::StringRef name, llvm::StringRef unit);
  llvm::Expected<CostEstimate> local(llvm::StringRef implementation);
  const std::map<std::string, std::string> &versions() const { return used; }
  const std::vector<std::string> &missingFacts() const { return missing; }
  using Local = std::function<llvm::Expected<CostEstimate>(llvm::StringRef,
                                                           CostContext &)>;
  CostContext(const TargetProfile &p, Local callback)
      : target(p), callback(std::move(callback)) {}

private:
  Local callback;
  std::map<std::string, std::string> used;
  std::vector<std::string> missing;
  bool inLocal = false;
};
struct CostModel {
  std::string name, family, version;
  std::function<llvm::Expected<CostEstimate>(
      mlir::Operation *, const StructuralCandidate &, CostContext &)>
      estimate;
};
} // namespace compilerinfra
