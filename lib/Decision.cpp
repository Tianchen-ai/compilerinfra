#include "DecisionProgram.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
using namespace compilerinfra;
using namespace compilerinfra::detail;

llvm::Error DecisionProgram::validate() const {
  if (choice.strategies.empty() || !table->realizers.count(choice.realizer))
    return llvm::createStringError(
        "family " + table->family.name +
        " needs a strategy chain and a registered realizer");
  if (!choice.model.empty() && !table->models.count(choice.model))
    return llvm::createStringError("cost model is unavailable for " +
                                   table->family.name + ": " + choice.model);
  std::set<std::string> seen;
  for (const auto &name : choice.strategies)
    if (!table->strategies.count(name) || !seen.insert(name).second)
      return llvm::createStringError("unknown or repeated strategy " + name +
                                     " in family " + table->family.name);
  return llvm::Error::success();
}

namespace {
class DecisionPass final
    : public mlir::PassWrapper<DecisionPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DecisionPass)
  DecisionPass(DecisionProgram program, DecisionObserver observe, SelectionSink publish)
      : program(std::move(program)), observe(std::move(observe)), publish(std::move(publish)) {}
  llvm::StringRef getArgument() const final {
    return program.materialize ? "ci-materialize" : "ci-decide";
  }
  void runOnOperation() final {
    if (program.materialize) {
      if (mlir::failed(materialize()))
        signalPassFailure();
      return;
    }
    auto result = getOperation().walk([&](mlir::Operation *op) {
      return family().applies(op) && mlir::failed(select(op))
                 ? mlir::WalkResult::interrupt()
                 : mlir::WalkResult::advance();
    });
    if (result.wasInterrupted())
      signalPassFailure();
  }

private:
  const DecisionFamily &family() const { return program.table->family; }
  const TargetProfile &target() const { return *program.table->target; }
  mlir::LogicalResult fail(mlir::Location where, llvm::StringRef component,
                           llvm::Error e) const {
    return mlir::emitError(where) << family().name << "/" << component << ": "
                                  << llvm::toString(std::move(e));
  }

  mlir::LogicalResult select(mlir::Operation *op) {
    if (auto value = family().read(op)) {
      if (auto e = family().verify(op, value, target()))
        return fail(op->getLoc(), "input", std::move(e));
      report(op, "preserved", "input", value);
      return mlir::success();
    }
    auto rootAnalyses = getAnalysisManager();
    auto analyses = op == getOperation().getOperation() ? rootAnalyses
                                                        : rootAnalyses.nest(op);
    bool ranked = bool(family().verifyCandidate) ||
                  !program.choice.model.empty() ||
                  !program.choice.candidate.empty() ||
                  llvm::any_of(program.choice.strategies, [&](auto &id) {
                    return bool(program.table->strategies.at(id).enumerate);
                  });
    if (ranked) {
      CandidateObserver events;
      if (observe)
        events = [&](llvm::StringRef strategy, mlir::Attribute value,
                     llvm::json::Object detail) {
          auto event = makeEvent(op, "candidate", strategy, value);
          event->details = std::move(detail);
          observe(*event);
        };
      auto decision = rankCandidates(program, op, analyses, events);
      if (!decision)
        return fail(op->getLoc(), "selection", decision.takeError());
      if (auto err = family().write(op, decision->value))
        return fail(op->getLoc(), decision->strategy, std::move(err));
      if (family().read(op) != decision->value)
        return op->emitError("decision carrier lost selected candidate");
      rootAnalyses.invalidate({});
      if (publish) publish(family().name, decision->structure);
      report(op, "selected", decision->strategy, decision->value);
      return mlir::success();
    }
    for (const auto &name : program.choice.strategies) {
      auto value =
          program.table->strategies.at(name).propose(op, target(), analyses);
      if (!value)
        return fail(op->getLoc(), name, value.takeError());
      if (!*value) {
        report(op, "no_match", name, {});
        continue;
      }
      if (auto e = family().verify(op, *value, target()))
        return fail(op->getLoc(), name, std::move(e));
      if (auto e = family().write(op, *value))
        return fail(op->getLoc(), name, std::move(e));
      if (family().read(op) != *value)
        return op->emitError() << "carrier for " << family().name
                               << " did not retain decision from " << name;
      // Domain attributes may participate in the next operation's analysis.
      rootAnalyses.invalidate({});
      report(op, "selected", name, *value);
      return mlir::success();
    }
    return op->emitError() << "unsupported " << family().name
                           << ": all strategies declined";
  }

  mlir::LogicalResult materialize() {
    struct Worklist : mlir::RewriterBase::Listener {
      llvm::SmallVector<mlir::Operation *> order;
      llvm::SmallPtrSet<mlir::Operation *, 16> live;
      void notifyOperationErased(mlir::Operation *op) final { live.erase(op); }
    } work;
    const auto &name = program.choice.realizer;
    // Check every input before a realizer can rewrite multiple operations.
    auto admitted = getOperation().walk<mlir::WalkOrder::PostOrder>(
        [&](mlir::Operation *op) {
          if (!family().applies(op))
            return mlir::WalkResult::advance();
          auto value = family().read(op);
          if (!value) {
            op->emitError()
                << "missing decision " << family().name << " at " << name;
            return mlir::WalkResult::interrupt();
          }
          if (auto e = family().verify(op, value, target())) {
            (void)fail(op->getLoc(), name, std::move(e));
            return mlir::WalkResult::interrupt();
          }
          work.order.push_back(op);
          work.live.insert(op);
          return mlir::WalkResult::advance();
        });
    if (admitted.wasInterrupted())
      return mlir::failure();
    mlir::IRRewriter rewriter(&getContext(), &work);
    for (auto *op : work.order) {
      if (!work.live.contains(op))
        continue;
      auto where = op->getLoc();
      auto value = family().read(op);
      // A preceding region rewrite can consume a related operation's decision.
      if (!value) {
        if (family().applies(op))
          return op->emitError()
                 << "lost decision " << family().name << " at " << name;
        continue;
      }
      if (auto e = family().verify(op, value, target()))
        return fail(where, name, std::move(e));
      auto event = makeEvent(op, "materialized", name, value);
      rewriter.setInsertionPoint(op);
      if (auto e = program.table->realizers.at(name).apply(op, value, target(),
                                                           rewriter))
        return fail(where, name, std::move(e));
      if (event)
        observe(*event);
    }
    auto remaining = getOperation().walk([&](mlir::Operation *op) {
      if (!family().read(op))
        return mlir::WalkResult::advance();
      op->emitError() << "unconsumed decision " << family().name << " after "
                      << name;
      return mlir::WalkResult::interrupt();
    });
    return mlir::success(!remaining.wasInterrupted());
  }
  std::optional<DecisionEvent> makeEvent(mlir::Operation *op,
                                         llvm::StringRef action,
                                         llvm::StringRef implementation,
                                         mlir::Attribute value) const {
    if (!observe)
      return std::nullopt;
    DecisionEvent e{
        program.table->family.name,         action.str(), implementation.str(),
        op->getName().getStringRef().str(), {},           {}};
    llvm::raw_string_ostream loc(e.location);
    op->getLoc().print(loc);
    if (value) {
      llvm::raw_string_ostream out(e.value);
      value.print(out);
    }
    return e;
  }
  void report(mlir::Operation *op, llvm::StringRef action,
              llvm::StringRef implementation, mlir::Attribute value) {
    if (auto e = makeEvent(op, action, implementation, value))
      observe(*e);
  }
  DecisionProgram program;
  DecisionObserver observe;
  SelectionSink publish;
};
} // namespace

BuildPasses DecisionProgram::builder(DecisionObserver observe, SelectionSink publish) const {
  return [program = *this, observe = std::move(observe), publish = std::move(publish)](
             mlir::OpPassManager &pm) -> llvm::Error {
    pm.addPass(std::make_unique<DecisionPass>(program, observe, publish));
    return llvm::Error::success();
  };
}

mlir::LogicalResult
compilerinfra::detail::verifyDecisionCheckpoint(llvm::ArrayRef<Stage> prefix,
                                                mlir::ModuleOp module) {
  std::map<std::string, const DecisionProgram *> active;
  for (const auto &stage : prefix)
    for (const auto &group : stage.groups)
      if (auto *program = group.decision())
        active[program->table->family.name] = program;
  if (active.empty())
    return mlir::success();
  auto result =
      module.walk([&](mlir::Operation *op) {
        for (const auto &[name, program] : active) {
          const auto &family = program->table->family;
          auto value = family.read(op);
          if (program->materialize) {
            if (!value)
              continue;
            op->emitError()
                << "checkpoint retains materialized decision " << name;
          } else if (value && family.applies(op)) {
            auto error = family.verify(op, value, *program->table->target);
            if (!error)
              continue;
            op->emitError() << name << ": " << llvm::toString(std::move(error));
          } else if (!value && !family.applies(op)) {
            continue;
          } else {
            op->emitError()
                << "checkpoint has inconsistent carrier for pending decision "
                << name;
          }
          return mlir::WalkResult::interrupt();
        }
        return mlir::WalkResult::advance();
      });
  return mlir::success(!result.wasInterrupted());
}

PassGroup PassGroup::forDecision(DecisionProgram program) {
  auto group = required(program.builder());
  group.program = std::make_shared<const DecisionProgram>(std::move(program));
  return group;
}
