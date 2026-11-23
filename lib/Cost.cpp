#include "DecisionProgram.h"
#include "llvm/ADT/ScopeExit.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

using namespace compilerinfra;
using llvm::createStringError;
using llvm::json::Array;
using llvm::json::Object;

llvm::Error MachineFacts::query(std::string name, MachineFact fact) {
  return insert(std::move(name), std::move(fact), "queried");
}
llvm::Error MachineFacts::insert(std::string name, MachineFact fact, llvm::StringRef kind) {
  if(fact.source.get("kind"))return createStringError(name+": source.kind is assigned by the fact entry point");
  if (name.empty() || fact.unit.empty() || fact.version.empty() ||
      fact.source.empty() ||
      (!fact.value.getAsNumber() && !fact.value.getAsString() &&
       !fact.value.getAsBoolean()))
    return createStringError("invalid machine fact " + name);
  fact.source["kind"] = kind.str();
  if (!values.try_emplace(name, std::move(fact)).second)
    return createStringError("duplicate machine fact " + name);
  return llvm::Error::success();
}
llvm::Error MachineFacts::loadCalibration(const Object &doc) {
  auto version = doc.getString("version");
  auto *facts = doc.getObject("facts");
  if (!version || version->empty() || !facts || facts->empty())
    return createStringError("calibration requires a version and facts");
  MachineFacts updated = *this;
  for (auto &[name, v] : *facts) {
    auto *row = v.getAsObject();
    auto *source = row ? row->getObject("source") : nullptr;
    if (!source || !row->getString("unit") || !row->getNumber("value"))
      return createStringError("invalid calibrated quantity " + name.str());
    for (auto key :
         {"device", "isa", "toolchain", "conditions", "benchmark", "error"})
      if (!source->getString(key) || source->getString(key)->empty())
        return createStringError(name.str() +
                                 ": missing calibration provenance " + key);
    Object origin = *source;

    if (auto err = updated.insert(
            name.str(), {*row->get("value"), row->getString("unit")->str(),
                         version->str(), std::move(origin)}, "calibrated"))
      return err;
  }
  *this = std::move(updated);
  return llvm::Error::success();
}
const MachineFact *MachineFacts::find(llvm::StringRef name) const {
  auto it = values.find(name.str());
  return it == values.end() ? nullptr : &it->second;
}
const MachineFact *CostContext::fact(llvm::StringRef name,
                                     llvm::StringRef unit) {
  auto *f = target.services.facts.find(name);
  if (f && f->unit != unit) {
    mismatches.push_back(Object{{"fact", name.str()}, {"expected", unit.str()}, {"actual", f->unit}});
    return nullptr;
  }
  if (!f) {
    if (!llvm::is_contained(missing, name.str()))
      missing.push_back(name.str());
    return nullptr;
  }
  used[name.str()] = f->version;
  return f;
}
llvm::Expected<CostEstimate> CostContext::local(llvm::StringRef name) {
  if (inLocal)
    return createStringError("recursive exit-local cost estimate");
  inLocal = true;
  auto restore = llvm::make_scope_exit([&] { inLocal = false; });
  return callback(name, *this);
}

namespace {
struct Row {
  StructuralCandidate proposal;
  std::string strategy, reason;
  CostEstimate estimate;
  Array mismatches;
  std::map<std::string, std::string> versions;
  std::map<std::string, std::string> localVersions;
  bool eliminated = false, retained = false, uncertain = false,
       estimated = false;
};
llvm::Error check(const CostEstimate &e) {
  auto ok = [](std::optional<double> x) {
    return !x || (std::isfinite(*x) && *x >= 0);
  };
  auto valid=[&](const std::optional<TimeBound> &b) {
    return !b || (ok(b->ns) && !b->scope.empty() && !b->comparison.empty() && !b->proof.empty());
  };
  const auto &lo=e.bounds.lower,&hi=e.bounds.upper;
  if (e.scope.empty() || !ok(e.timeNs) || !valid(lo) || !valid(hi) ||
      (lo && hi && lo->scope==hi->scope && lo->comparison==hi->comparison && lo->ns>hi->ns))
    return createStringError("invalid time, scope or proven bounds in cost estimate");
  for (auto &[name, q] : e.terms)
    if (name.empty() || q.unit.empty() ||
        (!std::isfinite(q.value) || q.value < 0))
      return createStringError("invalid cost term " + name);
  for (auto &[id, row] : e.rows) {
    if (id.empty() || !ok(row.timeNs))
      return createStringError("invalid predicted row or time");
    for (auto &[name, q] : row.terms)
      if (name.empty() || q.unit.empty() || !std::isfinite(q.value) ||
          q.value < 0)
        return createStringError("invalid predicted row term " + name);
  }
  return llvm::Error::success();
}
Array strings(const std::vector<std::string> &values) {
  Array arr;
  for (auto &s : values)
    arr.push_back(s);
  return arr;
}
Object describe(const Row &r, const CostModel *model, bool selected) {
  Object terms, versions, estimators;
  for (auto &[name, version] : r.localVersions)
    estimators[name] = version;
  for (auto &[k, v] : r.estimate.terms)
    terms[k] = Object{{"value", v.value}, {"unit", v.unit}};
  for (auto &[k, v] : r.versions)
    versions[k] = v;
  Object out{{"candidate", r.proposal.id},
             {"topology", r.proposal.topology},
             {"requirements", strings(r.proposal.preconditions)},
             {"terms", std::move(terms)},
             {"scope", r.estimate.scope},
             {"bottleneck", r.estimate.bottleneck},
             {"missing", strings(r.estimate.missing)}, {"unit_mismatches", Array(r.mismatches)},
             {"machine_versions", std::move(versions)},
             {"estimator_versions", std::move(estimators)},
             {"predicted_ns", nullptr},
             {"uncertain", r.uncertain},
             {"retained", r.retained},
             {"estimated", r.estimated},
             {"eliminated", r.eliminated},
             {"selected", selected},
             {"reason", r.reason}};
  Object predictions;
  for (auto &[id, row] : r.estimate.rows) {
    Object terms;
    for (auto &[name, q] : row.terms)
      terms[name] = Object{{"value", q.value}, {"unit", q.unit}};
    Object prediction{{"terms", std::move(terms)}, {"predicted_ns", nullptr}};
    if (row.timeNs)
      prediction["predicted_ns"] = *row.timeNs;
    predictions[id] = std::move(prediction);
  }
  out["rows"] = std::move(predictions);
  if (model) {
    out["model"] = model->name;
    out["model_version"] = model->version;
  }
  if (r.estimate.timeNs)
    out["predicted_ns"] = *r.estimate.timeNs;
  auto describeBound=[](const TimeBound &b){return Object{{"ns",b.ns},{"scope",b.scope},{"comparison",b.comparison},{"proof",b.proof}};};
  if(r.estimate.bounds.lower)out["lower_bound"]=describeBound(*r.estimate.bounds.lower);
  if(r.estimate.bounds.upper)out["upper_bound"]=describeBound(*r.estimate.bounds.upper);
  return out;
}
} // namespace
llvm::Expected<compilerinfra::detail::RankedDecision>
compilerinfra::detail::rankCandidates(const DecisionProgram &p,
                                      mlir::Operation *op,
                                      mlir::AnalysisManager &analyses,
                                      CandidateObserver observe) {
  auto &table = *p.table;
  auto &choice = p.choice;
  const CostModel *model =
      choice.model.empty() ? nullptr : &table.models.at(choice.model);
  std::vector<Row> rows;
  std::set<std::string> ids;
  for (auto &name : choice.strategies) {
    const auto &strategy = table.strategies.at(name);
    std::vector<StructuralCandidate> batch;
    if (strategy.enumerate) {
      auto set = strategy.enumerate(op, *table.target, analyses);
      if (!set)
        return set.takeError();
      batch = std::move(*set);
    } else {
      auto value = strategy.propose(op, *table.target, analyses);
      if (!value)
        return value.takeError();
      if (*value)
        batch.push_back({name, name, *value, {}});
    }
    for (auto &candidate : batch) {
      if (candidate.id.empty() || candidate.topology.empty() ||
          !candidate.value || !ids.insert(candidate.id).second)
        return createStringError(
            name + ": incomplete or duplicate structure candidate " +
            candidate.id);
      Row r{std::move(candidate), name};
      auto legality = table.family.verify(op, r.proposal.value, *table.target);
      for(auto &impl:r.proposal.implementations)
        if(!legality)legality=table.target->checkImplementation(impl);
      if (!legality && table.family.verifyCandidate)
        legality = table.family.verifyCandidate(op, r.proposal, *table.target);
      if (legality) {
        r.reason = "illegal: " + llvm::toString(std::move(legality));
        r.eliminated = true;
      } else if (model) {
        CostContext context(
            *table.target,
            [&](llvm::StringRef key,
                CostContext &ctx) -> llvm::Expected<CostEstimate> {
              auto impl = table.localModels.find(key.str());
              if (impl == table.localModels.end())
                return createStringError("unregistered exit-local estimator " +
                                         key);
              auto estimate = impl->second.estimate(op, r.proposal, ctx);
              if (!estimate)
                return estimate.takeError();
              if (auto err = check(*estimate))
                return std::move(err);
              for(auto b:{estimate->bounds.lower,estimate->bounds.upper})
                if(b && b->scope=="full_call")return createStringError("local estimator cannot certify a full-call bound");
              r.localVersions[impl->first] = impl->second.version;
              return estimate;
            });
        auto e = model->estimate(op, r.proposal, context);
        if (!e)
          return e.takeError();
        if (auto err = check(*e))
          return std::move(err);
        r.versions.insert(context.versions().begin(), context.versions().end());
        r.mismatches=context.unitMismatches();
        r.estimated = true;
        r.estimate = std::move(*e);
        for (auto &fact : context.missingFacts())
          if (!llvm::is_contained(r.estimate.missing, fact))
            r.estimate.missing.push_back(fact);
      }
      rows.push_back(std::move(r));
    }
    if (!model && choice.candidate.empty() &&
        llvm::any_of(rows, [](const Row &r) { return !r.eliminated; }))
      break;
  }
  std::map<std::string,std::pair<double,std::string>> uppers;
  for(auto &r:rows){
    auto &upper=r.estimate.bounds.upper;
    if(r.eliminated || !upper || upper->scope!="full_call")continue;
    auto [it,inserted]=uppers.try_emplace(upper->comparison,upper->ns,r.proposal.id);
    if(!inserted && upper->ns<it->second.first)it->second={upper->ns,r.proposal.id};
  }
  std::vector<size_t> order;
  std::optional<size_t> baseline;
  for (size_t i = 0; i < rows.size(); ++i) {
    auto &r = rows[i];
    if (r.eliminated)
      continue;
    if (!baseline)
      baseline = i;
    auto &lower=r.estimate.bounds.lower;
    auto bound=lower?uppers.find(lower->comparison):uppers.end();
    if (lower && lower->scope=="full_call" && bound!=uppers.end() && lower->ns>bound->second.first) {
      r.eliminated = true;
      r.reason = "proved slower than " + bound->second.second;
      continue;
    }
    order.push_back(i);
  }
  if (model)
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return rows[a].estimate.timeNs.value_or(INFINITY) <
             rows[b].estimate.timeNs.value_or(INFINITY);
    });
  std::set<std::string> topologies;
  size_t selected = rows.size();
  for (auto i : order) {
    auto &r = rows[i];
    auto best = rows[order.front()].estimate.timeNs;
    bool near = best && r.estimate.timeNs &&
                *r.estimate.timeNs <= *best * (1 + choice.uncertainty);
    bool tiedLeader =
        i == order.front() && order.size() > 1 && best &&
        rows[order[1]].estimate.timeNs &&
        *rows[order[1]].estimate.timeNs <= *best * (1 + choice.uncertainty);
    r.uncertain = model && (!r.mismatches.empty() || !r.estimate.missing.empty() || !r.estimate.timeNs ||
                            tiedLeader || (near && i != order.front()));
    bool first = topologies.insert(r.proposal.topology).second;
    r.retained = i == order.front() || baseline == i || first || r.uncertain;
    r.reason = i == order.front() ? (model ? "ranked first" : "ordered default")
               : baseline == i    ? "baseline"
               : first            ? "topology representative"
               : r.uncertain      ? "uncertain"
                                  : "ranked lower";
    if ((choice.candidate.empty() && i == order.front()) ||
        r.proposal.id == choice.candidate) {
      selected = i;
      r.retained = true;
      if (!choice.candidate.empty())
        r.reason = "configuration override";
    }
  }
  if (observe)
    for (size_t i = 0; i < rows.size(); ++i)
      {
        Object details=describe(rows[i], model, selected == i);
        details["reuse"]=spelling(table.target->reuse).str();
        details["implementations"]=strings(rows[i].proposal.implementations);
        Object contracts;
        for(const auto &key:rows[i].proposal.implementations){
          auto it=table.target->implementations.find(key);
          if(it==table.target->implementations.end())continue;
          const auto &impl=it->second;
          contracts[key]=Object{{"symbol",impl.symbol},{"minimum_reuse",spelling(impl.minimum).str()},
            {"source",impl.source==ImplementationSource::Builtin?"builtin":impl.source==ImplementationSource::Runtime?"runtime":"external"},
            {"estimator",impl.estimator},{"parameters",strings(impl.costParameters)},{"dependencies",strings(impl.dependencies)}};
        }
        details["contracts"]=std::move(contracts);
        observe(rows[i].strategy, rows[i].proposal.value,std::move(details));
      }
  if (selected == rows.size()) {
    std::string why = choice.candidate.empty()
                          ? "all proposals declined or eliminated"
                          : "unavailable candidate " + choice.candidate;
    for (auto &r : rows)
      if (r.eliminated &&
          (choice.candidate.empty() || choice.candidate == r.proposal.id))
        why += "; " + r.proposal.id + ": " + r.reason;
    return createStringError(table.family.name + ": " + why);
  }
  return RankedDecision{rows[selected].strategy, rows[selected].proposal.value,
                        rows[selected].proposal.id};
}
