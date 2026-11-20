#pragma once
#include "compilerinfra/Assembly.h"
namespace compilerinfra::detail {
struct DecisionTable {
  DecisionFamily family;
  std::map<std::string, Strategy> strategies;
  std::map<std::string, Realizer> realizers;
  std::shared_ptr<const TargetProfile> target;
};
struct DecisionProgram {
  std::shared_ptr<const DecisionTable> table;
  DecisionChoice choice;
  bool materialize;
  llvm::Error validate() const;
  BuildPasses builder(DecisionObserver observe = {}) const;
};
mlir::LogicalResult verifyDecisionCheckpoint(llvm::ArrayRef<Stage> prefix,
                                             mlir::ModuleOp module);
} // namespace compilerinfra::detail
