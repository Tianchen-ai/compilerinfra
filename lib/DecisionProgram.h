#pragma once
#include "compilerinfra/Assembly.h"
namespace compilerinfra::detail {
struct DecisionTable {
  DecisionFamily family;
  std::map<std::string, CostModel> models, localModels;
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
struct RankedDecision {
  std::string strategy;
  mlir::Attribute value;
};
using CandidateObserver =
    std::function<void(llvm::StringRef, mlir::Attribute, llvm::json::Object)>;
llvm::Expected<RankedDecision> rankCandidates(const DecisionProgram &,
                                              mlir::Operation *,
                                              mlir::AnalysisManager &,
                                              CandidateObserver);
mlir::LogicalResult verifyDecisionCheckpoint(llvm::ArrayRef<Stage> prefix,
                                             mlir::ModuleOp module);
} // namespace compilerinfra::detail
