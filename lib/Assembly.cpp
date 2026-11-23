#include "DecisionProgram.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/Path.h"
#include <cmath>
#include <set>

using namespace compilerinfra;
using llvm::createStringError;
using llvm::json::Array;
using llvm::json::Object;

void Assembly::bind(llvm::StringRef id, void (*registerUnit)(Unit &)) {
  auto entry = units.try_emplace(id.str(), id.str());
  registerUnit(entry.first->second);
}

namespace {
std::string field(const Object &obj, llvm::StringRef key) {
  return obj.getString(key)->str();
}

struct Components {
  std::map<std::string,std::map<std::string,ImplementationContract>> contracts;
  std::map<std::string,std::function<Object()>> probes;
  std::map<std::string, const Located<Boundary> *> boundaries;
  std::map<std::string, const Located<ExitImplementation> *> exits;
  std::map<std::string, Located<Contribution>> contributions;
  std::map<std::string, Located<FamilyBinding>> families;
  std::map<std::string, Located<Strategy>> strategies;
  std::map<std::string, Located<Realizer>> realizers;
  std::map<std::string, Located<CostModel>> costModels;
  Object catalog;
  llvm::Error record(const std::string &name, const std::string &unit,
                     const std::string &at, llvm::StringRef kind,
                     const SourceRef &source) {
    if (llvm::sys::path::is_absolute(source.file) ||
        llvm::sys::path::is_absolute(source.file,
                                     llvm::sys::path::Style::windows_backslash))
      return createStringError(
          unit +
          ": registration needs a source-relative or @generated location");
    if (catalog.get(name))
      return createStringError("duplicate registration " + name + " in " +
                               unit);
    catalog[name] = Object{{"unit", unit},
                           {"at", at},
                           {"kind", kind},
                           {"file", source.file},
                           {"line", source.line}};
    return llvm::Error::success();
  }
};

llvm::Error collect(const Object &manifest, const Unit &u, Components &c) {
  const Object &units = *manifest.getObject("units");
  auto owner = field(*units.getObject(u.id), "owner");
  auto node = llvm::StringRef(owner).rsplit('/').second.str();
  bool exitUnit = llvm::StringRef(owner).starts_with("exits/");
  auto *n = manifest.getObject(exitUnit ? "exits" : "nodes")->getObject(node);
  auto local = [&](llvm::StringRef at) {
    if (!n || owner != field(*units.getObject(field(*n, "unit")), "owner"))
      return false;
    for (auto &b : *n->getArray("boundaries"))
      if (at == node + "@" + b.getAsString()->str())
        return true;
    return false;
  };
  for (auto &b : u.boundaries) {
    auto at = node + "@" + b.value.name;
    if (!local(at) || !b.value.lower || !b.value.verify)
      return createStringError(u.id + ": invalid boundary implementation " +
                               at);
    if (auto e = c.record(at, u.id, at, "boundary", b.source))
      return e;
    c.boundaries[at] = &b;
  }
  if (u.exit) {
    if (owner != "exits/" + node || u.id != field(*n, "unit") ||
        bool(u.exit->value.emit)==bool(u.exit->value.terminal))
      return createStringError(u.id + ": invalid exit implementation");
    auto at = field(*manifest.getObject("exits")->getObject(node), "at");
    if (auto e = c.record(owner, u.id, at, "exit", u.exit->source))
      return e;
    c.exits[node] = &*u.exit;
  }
  for (auto &[localName, probe]:u.dependencyProbes) {
    if(!exitUnit || !probe || !c.probes.emplace(u.qualify(localName), probe).second)
      return createStringError(u.id+": invalid dependency probe "+localName);
  }
  for (auto &[impl,where]:u.implementations) {
    if(!exitUnit || impl.name.empty() || impl.operation.empty() || impl.symbol.empty() ||
       impl.operands.empty() || impl.layout.empty() || impl.accumulator.empty() ||
       impl.effects.empty() || impl.numerical.empty() || impl.estimator.empty() ||
       !impl.verify || !impl.available)
      return createStringError(u.id+": incomplete implementation "+impl.name);
    auto id=u.qualify(impl.name);
    if(auto e=c.record(id,u.id,node+"@"+n->getArray("boundaries")->back().getAsString()->str(),"implementation",where))return e;
    auto &item=*c.catalog.getObject(id);
    item["operation"]=impl.operation; item["symbol"]=impl.symbol;
    item["source"]=impl.source==ImplementationSource::Builtin?"builtin":impl.source==ImplementationSource::Runtime?"runtime":"external";
    item["reuse"]=spelling(impl.minimum).str(); item["operands"]=impl.operands;
    item["layout"]=impl.layout; item["accumulator"]=impl.accumulator;item["effects"]=impl.effects;
    item["numerical"]=impl.numerical;item["estimator"]=impl.estimator;
    item["cost_parameters"]=Array(impl.costParameters);item["dependencies"]=Array(impl.dependencies);
    c.contracts[node].emplace(id,impl);
  }
  for (auto &[v, s] : u.contributions) {
    auto id = u.qualify(v.name);
    if (!local(v.at) || !v.build)
      return createStringError(id + ": invalid attachment " + v.at);
    if (auto e = c.record(id, u.id, v.at, "contribution", s))
      return e;
    c.contributions.emplace(id, Located<Contribution>{v, s});
  }
  for (auto &[v, s] : u.families) {
    auto id = u.qualify(v.family.name);
    if (!local(v.decideAt) || !local(v.materializeAt) || !v.family.applies ||
        !v.family.read || !v.family.write || !v.family.verify)
      return createStringError(id + ": invalid decision family");
    if (auto e = c.record(id, u.id, v.decideAt, "family", s))
      return e;
    (*c.catalog.getObject(id))["materialize_at"] = v.materializeAt;
    auto copy = v;
    copy.family.name = id;
    c.families.emplace(id, Located<FamilyBinding>{copy, s});
  }
  for (auto &[v, s] : u.strategies) {
    auto id = u.qualify(v.name);
    if (bool(v.propose) == bool(v.enumerate))
      return createStringError(id + ": declare exactly one proposal interface");
    if (auto e = c.record(id, u.id, "", "strategy", s))
      return e;
    c.strategies.emplace(id, Located<Strategy>{v, s});
  }
  for (auto &[v, s] : u.costModels) {
    auto prefix=field(*units.getObject(u.id),"directory")+"/cost/";
    llvm::SmallString<128> path(s.file);
    llvm::sys::path::remove_dots(path,true);
    if(!llvm::StringRef(path).starts_with(prefix))return createStringError(u.id+": cost model must register from "+prefix+"; got "+s.file);
    auto id = u.qualify(v.name);
    if (!v.estimate || v.version.empty())
      return createStringError(id + ": missing estimator or version");
    auto owner = field(*manifest.getObject("units")->getObject(u.id), "owner");
    bool localEstimate = llvm::StringRef(owner).starts_with("exits/");
    if (!localEstimate && !llvm::StringRef(owner).starts_with("levels/"))
      return createStringError(id + ": estimators belong to nodes and exits");
    if (auto err = c.record(
            id, u.id, "", localEstimate ? "local_estimator" : "cost_model", s))
      return err;
    (*c.catalog.getObject(id))["version"] = v.version;
    auto copy = v;
    copy.name = id;
    c.costModels.emplace(id, Located<CostModel>{std::move(copy), s});
  }
  for (auto &[v, s] : u.realizers) {
    auto id = u.qualify(v.name);
    if (!v.apply)
      return createStringError(id + ": missing realizer implementation");
    if (auto e = c.record(id, u.id, "", "realizer", s))
      return e;
    c.realizers.emplace(id, Located<Realizer>{v, s});
  }
  return llvm::Error::success();
}

Object settings(const Object &config, llvm::StringRef scope,
                llvm::StringRef name) {
  if (auto *group = config.getObject(scope))
    if (auto *entry = group->getObject(name))
      return *entry;
  return {};
}
Object mergedField(const Object &base, const Object &over,
                   llvm::StringRef name) {
  Object result;
  for (auto *scope : {&base, &over})
    if (auto *items = scope->getObject(name))
      for (auto &[key, val] : *items)
        result[key] = val;
  return result;
}

llvm::Error orderContributions(const Components &c, const std::string &at,
                               std::vector<std::string> &ordered) {
  std::set<std::string> todo;
  for (auto &[id, reg] : c.contributions)
    if (reg.value.at == at)
      todo.insert(id);
  while (!todo.empty()) {
    auto ready = todo.end();
    for (auto it = todo.begin(); it != todo.end(); ++it) {
      const auto &v = c.contributions.at(*it).value;
      bool wait = false;
      for (auto *deps : {&v.after, &v.afterIfPresent})
        for (auto &dep : *deps) {
          auto other = c.contributions.find(dep);
          if (other == c.contributions.end() && deps == &v.afterIfPresent)
            continue;
          if (other == c.contributions.end() || other->second.value.at != at)
            return createStringError(*it + ": invalid ordering dependency " +
                                     dep);
          wait |= todo.count(dep) != 0;
        }
      if (!wait) {
        ready = it;
        break;
      }
    }
    if (ready == todo.end())
      return createStringError("contribution order cycle at " + at);
    ordered.push_back(*ready);
    todo.erase(ready);
  }
  return llvm::Error::success();
}

llvm::Expected<DecisionChoice> choiceFor(const Object &settings,
                                         const std::string &id) {
  auto *obj = settings.getObject(id);
  if (!obj || !obj->getArray("strategies") || !obj->getString("realizer"))
    return createStringError(
        "configuration must select strategies and a realizer for " + id);
  DecisionChoice result;
  for (auto key : {"model", "candidate"})
    if (obj->get(key) && !obj->getString(key))
      return createStringError(id + ": expected a component name for " + key);
  result.model = obj->getString("model").value_or("").str();
  result.candidate = obj->getString("candidate").value_or("").str();
  if (obj->get("uncertainty")) {
    auto v = obj->getNumber("uncertainty");
    if (!v || !std::isfinite(*v) || *v < 0)
      return createStringError(id + ": invalid relative uncertainty threshold");
    result.uncertainty = *v;
  }
  result.realizer = field(*obj, "realizer");
  for (auto &v : *obj->getArray("strategies")) {
    if (!v.getAsString())
      return createStringError("expected a strategy name for " + id);
    result.strategies.push_back(v.getAsString()->str());
  }
  return result;
}

llvm::Error checkConfiguration(const Object &config, const Object &manifest,
                               const Components &c) {
  for (auto &[group, entries] : config) {
    if(group=="reuse"){
      auto v=entries.getAsString();
      if(v!="native" && v!="vendor" && v!="library")return createStringError("reuse must be native, vendor or library");
      continue;
    }
    if ((group != "nodes" && group != "exits") || !entries.getAsObject())
      return createStringError("unknown configuration section " + group.str());
    for (auto &[key, value] : *entries.getAsObject()) {
      auto *declaration = manifest.getObject(group)->getObject(key);
      if (!declaration || !value.getAsObject())
        return createStringError("invalid scope " + key.str());
      for (auto &[kind, items] : *value.getAsObject()) {
        if (!items.getAsObject() ||
            (kind != "optimizations" && kind != "decisions" &&
             (kind != "options" || group != "exits")))
          return createStringError("invalid configuration field " + key.str() +
                                   "/" + kind.str());
        if (kind == "options")
          continue;
        for (auto &[id, item] : *items.getAsObject()) {
          auto *component = c.catalog.getObject(id);
          if (!component ||
              field(*component, "kind") !=
                  (kind == "decisions" ? "family" : "contribution") ||
              (kind == "optimizations" && !item.getAsBoolean()))
            return createStringError("invalid configuration for " + id.str());
          auto at = field(*component, "at");
          bool inherited = group == "nodes" && kind == "decisions" &&
                           !llvm::StringRef(at).starts_with(key.str() + "@");
          if (inherited) {
            auto *choice = item.getAsObject();
            auto source = llvm::StringRef(at).split('@').first;
            auto attachment = declaration->getString("at").value();
            auto onPath = [&](llvm::StringRef boundary) {
              return llvm::any_of(
                  *declaration->getArray("path"),
                  [&](const auto &v) { return v.getAsString() == boundary; });
            };
            if (attachment.split('@').first == source && onPath(at) &&
                !onPath(field(*component, "materialize_at")) && choice &&
                choice->size() == 1 && choice->getString("realizer"))
              continue;
          }
          if (group == "nodes"
                  ? !llvm::StringRef(at).starts_with(key.str() + "@")
                  : !llvm::any_of(
                        *declaration->getArray("path"),
                        [&](const auto &v) { return v.getAsString() == at; }))
            return createStringError(id.str() + " is outside " + key.str());
        }
      }
    }
  }
  return llvm::Error::success();
}

using FamilyBindings = std::map<std::string, std::string>;
// One attachment contract for both child representations and exits. Compute it
// independently of exit traversal so unused nodes have meaningful status too.
llvm::Expected<FamilyBindings> bindAttachment(const Components &c,
                                              const Object &units,
                                              const Object &branch,
                                              const Object &options) {
  FamilyBindings result;
  auto attachment = field(branch, "at");
  if (attachment == "entry")
    return result;
  auto owner = field(*units.getObject(field(branch, "unit")), "owner");
  auto path = branch.getArray("path");
  auto includes = [&](const std::string &b) {
    return llvm::any_of(*path,
                        [&](const auto &v) { return v.getAsString() == b; });
  };
  for (const auto &[id, f] : c.families) {
    if (llvm::StringRef(f.value.decideAt).split('@').first !=
            llvm::StringRef(attachment).split('@').first ||
        !includes(f.value.decideAt) || includes(f.value.materializeAt))
      continue;
    std::vector<std::string> choices;
    for (const auto &[rid, r] : c.realizers) {
      if (r.value.family == id &&
          field(*units.getObject(field(*c.catalog.getObject(rid), "unit")),
                "owner") == owner)
        choices.push_back(rid);
    }
    auto *decisions = options.getObject("decisions");
    auto *explicitChoice = decisions ? decisions->getObject(id) : nullptr;
    if (explicitChoice && explicitChoice->getString("realizer") &&
        llvm::is_contained(choices, field(*explicitChoice, "realizer")))
      result[id] = field(*explicitChoice, "realizer");
    else if (!explicitChoice && choices.size() == 1)
      result[id] = choices.front();
    else
      return createStringError(owner + " at " + attachment +
                               (llvm::StringRef(owner).starts_with("exits/")
                                    ? ": needs an exit-private realizer for "
                                    : ": needs a node-private realizer for ") +
                               id);
  }
  return result;
}
} // namespace

llvm::Expected<Compiler> Assembly::link(const Object &manifest,
                                        const Object &config) const {
  Components components;
  std::map<std::string, std::string> invalidExits;
  for (auto &[id, unit] : units) {
    if (auto error = collect(manifest, unit, components)) {
      auto owner = field(*manifest.getObject("units")->getObject(id), "owner");
      if (!llvm::StringRef(owner).starts_with("exits/"))
        return std::move(error);
      invalidExits.emplace(owner.substr(6), llvm::toString(std::move(error)));
    }
  }
  auto &c = components;
  for (auto &[node, value] : *manifest.getObject("nodes"))
    for (auto &b : *value.getAsObject()->getArray("boundaries")) {
      auto at = node.str() + "@" + b.getAsString()->str();
      if (!c.boundaries.count(at))
        return createStringError("missing implementation of " + at);
    }
  for (auto &[id, binding] : c.families) {
    auto end = c.boundaries.find(binding.value.materializeAt);
    if (end == c.boundaries.end()) {
      auto unit = field(*c.catalog.getObject(id), "unit");
      auto owner =
          field(*manifest.getObject("units")->getObject(unit), "owner");
      auto message = id + ": unknown materialization boundary " +
                     binding.value.materializeAt;
      if (!llvm::StringRef(owner).starts_with("exits/"))
        return createStringError(message);
      invalidExits.emplace(owner.substr(6), message);
    }
  }
  auto attach = [&](const auto &entries) -> llvm::Error {
    for (auto &[id, v] : entries) {
      auto f = c.families.find(v.value.family);
      auto &unitMap = *manifest.getObject("units");
      auto *itemUnit =
          unitMap.getObject(field(*c.catalog.getObject(id), "unit"));
      auto owner = field(*itemUnit, "owner");
      if (f == c.families.end()) {
        auto message = id + ": unknown family " + v.value.family;
        if (!llvm::StringRef(owner).starts_with("exits/"))
          return createStringError(message);
        invalidExits.emplace(owner.substr(6), message);
        continue;
      }
      auto *familyUnit = unitMap.getObject(
          field(*c.catalog.getObject(v.value.family), "unit"));
      if (field(*itemUnit, "owner") != field(*familyUnit, "owner") &&
          !llvm::any_of(*itemUnit->getArray("allowed"), [&](const auto &key) {
            return field(*unitMap.getObject(*key.getAsString()), "owner") ==
                   field(*familyUnit, "owner");
          })) {
        auto message =
            id + ": family is outside unit ancestry: " + v.value.family;
        if (!llvm::StringRef(owner).starts_with("exits/"))
          return createStringError(message);
        invalidExits.emplace(owner.substr(6), message);
        continue;
      }
      (*c.catalog.getObject(id))["at"] = f->second.value.decideAt;
    }
    return llvm::Error::success();
  };
  if (auto e = attach(c.costModels))
    return std::move(e);
  for (auto &[id, model] : c.costModels) {
    auto *entry = c.catalog.getObject(id);
    auto owner = [&](const Object &item) {
      return field(*manifest.getObject("units")->getObject(field(item, "unit")),
                   "owner");
    };
    if (field(*entry, "kind") == "cost_model" &&
        (!c.families.count(model.value.family) ||
         owner(*entry) != owner(*c.catalog.getObject(model.value.family))))
      return createStringError(
          id + ": structural model must belong to its decision node");
  }
  for(const auto &[id,s]:c.strategies) {
    auto f=c.families.find(s.value.family);
    if(s.value.enumerate && f!=c.families.end() && !f->second.value.family.verifyCandidate)
      return createStringError(id+": enumerating strategies require verifyCandidate on "+s.value.family);
  }
  if (auto e = attach(c.strategies))
    return std::move(e);
  if (auto e = attach(c.realizers))
    return std::move(e);
  Object sharedConfig = config;
  sharedConfig.erase("exits");
  if (auto e = checkConfiguration(sharedConfig, manifest, c))
    return std::move(e);
  if (auto *overrides = config.get("exits")) {
    if (!overrides->getAsObject())
      return createStringError("expected exit configuration map");
    for (auto &[name, settings] : *overrides->getAsObject()) {
      if (!manifest.getObject("exits")->get(name))
        return createStringError("unknown exit " + name.str());
      if (auto err = checkConfiguration(
              Object{{"exits", Object{{name, settings}}}}, manifest, c))
        invalidExits.emplace(name.str(), llvm::toString(std::move(err)));
    }
  }
  Frontend front;
  front.import = importMLIR;
  front.verifyInput = [](mlir::ModuleOp op) { return mlir::verify(op); };
  auto frontId =
      "frontend/" +
      field(*manifest.getObject("product")->getObject("frontend"), "language");
  front.registerDialects = units.at(frontId).dialects;
  if (units.at(frontId).input)
    front.verifyInput = units.at(frontId).input;
  Object selected;
  std::vector<Exit> exits;
  Object description = manifest;
  std::map<std::string, FamilyBindings> handoffs;
  auto bindBranch = [&](const std::string &name, Object &branch,
                        llvm::StringRef group) {
    auto at = field(branch, "at");
    if (at != "entry") {
      auto parent = llvm::StringRef(at).split('@').first;
      auto *declaration = description.getObject("nodes")->getObject(parent);
      if (auto failure = declaration->getString("error")) {
        branch["valid"] = false;
        branch["error"] = failure->str();
        return;
      }
    }
    auto result = bindAttachment(c, *manifest.getObject("units"), branch,
                                 settings(config, group, name));
    branch["valid"] = bool(result);
    if (result)
      handoffs[name] = std::move(*result);
    else
      branch["error"] = llvm::toString(result.takeError());
  };
  for (auto &level : *manifest.getArray("levels")) {
    for (auto &[name, n] : *description.getObject("nodes"))
      if (n.getAsObject()->getString("level") ==
          level.getAsObject()->getString("name"))
        bindBranch(name.str(), *n.getAsObject(), "nodes");
  }
  for (auto &[key, value] : *manifest.getObject("exits")) {
    std::string name = key.str();
    auto &e = *value.getAsObject();
    auto *status = description.getObject("exits")->getObject(key);
    bindBranch(name, *status, "exits");
    auto buildExit = [&]() -> llvm::Expected<Exit> {
      if (auto failure = status->getString("error"))
        return createStringError(failure->str());
      if (invalidExits.count(name))
        return createStringError(invalidExits.at(name));
      if (!c.exits.count(name))
        return createStringError("missing emitter for " + name);
      auto &implementation = c.exits.at(name)->value;
      (*status)["emission"]=implementation.terminal?"recursive":"legacy";
      auto overrides = settings(config, "exits", name);
      auto profile = std::make_shared<TargetProfile>();
      profile->name = name;
      auto options = mergedField({}, overrides, "options");
      if (implementation.capabilities) {
        auto facts = implementation.capabilities(options);
        if (!facts)
          return facts.takeError();
        *profile = std::move(*facts);
      } else if (!options.empty())
        return createStringError(name + ": unsupported exit options");
      auto reuse=config.getString("reuse").value_or("vendor");
      profile->reuse=reuse=="native"?Reuse::Native:reuse=="library"?Reuse::Library:Reuse::Vendor;
      if(auto *deps=manifest.getObject("product")->getArray("optional_dependencies"))
        for(auto &decl:*deps){
          auto &d=*decl.getAsObject();
          if(field(d,"owner")!="exits/"+name)continue;
          auto probe=c.probes.find(field(d,"probe"));
          if(probe==c.probes.end())return createStringError("unregistered dependency probe "+field(d,"probe"));
          profile->dependencies.emplace(field(d,"id"),probe->second);
        }
      profile->implementations=c.contracts[name];
      for(auto &[id,i]:profile->implementations){
        profile->implementationOps.insert(i.operation);
        auto est=c.costModels.find(i.estimator);
        if(est==c.costModels.end() || field(*manifest.getObject("units")->getObject(field(*c.catalog.getObject(i.estimator),"unit")),"owner")!="exits/"+name)
          return createStringError(id+": unknown exit-local estimator "+i.estimator);
        for(auto &dep:i.dependencies)if(!profile->dependencies.count(dep))return createStringError(id+": unknown dependency "+dep);
      }
      if (!profile->services.calibrationProfile.empty()) {
        auto *all = manifest.getObject("calibrations");
        auto *profiles = all ? all->getObject(name) : nullptr;
        auto *doc =
            profiles ? profiles->getObject(profile->services.calibrationProfile)
                     : nullptr;
        if (!doc)
          return createStringError(name + ": missing installed calibration " +
                                   profile->services.calibrationProfile);
        if (auto error = profile->services.facts.loadCalibration(*doc))
          return std::move(error);
      }
      Exit t;
      t.name = name;
      t.emit = implementation.emit;
      t.dependencies=profile->dependencies;
      if(implementation.terminal) {
        auto &printer=*implementation.terminal;
        if(!printer.package || printer.operations.empty() ||
           llvm::any_of(printer.operations,[](const auto &p){return p.first.empty() || !p.second;}))
          return createStringError(name+": incomplete terminal printer");
        for(auto &op:profile->implementationOps)if(!printer.operations.count(op))return createStringError(name+": no print rule for "+op);
        t.emit=[printer](mlir::ModuleOp m,llvm::raw_pwrite_stream &out){return printer.emit(m,out);};
      }
      std::set<std::string> owners{"exits/" + name};
      for (auto &eligible : *manifest.getObject("units")
                                 ->getObject(field(e, "unit"))
                                 ->getArray("allowed"))
        owners.insert(field(
            *manifest.getObject("units")->getObject(*eligible.getAsString()),
            "owner"));
      auto available = [&](const std::string &id) {
        auto unit = field(*c.catalog.getObject(id), "unit");
        return owners.count(
            field(*manifest.getObject("units")->getObject(unit), "owner"));
      };
      std::map<std::string, detail::DecisionProgram> pending;
      std::set<std::string> chosen;
      FamilyBindings pathBindings;
      for (auto &b : *e.getArray("path")) {
        const auto &bindings =
            handoffs.at(b.getAsString()->split('@').first.str());
        pathBindings.insert(bindings.begin(), bindings.end());
      }
      std::string previousOwner;
      for (auto &b : *e.getArray("path")) {
        auto at = b.getAsString()->str();
        auto node = at.substr(0, at.find('@'));
        if (!c.boundaries.count(at))
          return createStringError("missing implementation of " + at);
        auto spec = c.boundaries.at(at)->value;
        auto wrap = [profile](ModelPasses build) -> BuildPasses {
          return [profile, build](mlir::OpPassManager &pm) {
            return build(pm, *profile);
          };
        };
        Stage stage;
        stage.name = at;
        stage.verifyOutput = [spec, profile](mlir::ModuleOp m) {
          return spec.verify(m, *profile);
        };
        if (node != previousOwner) {
          previousOwner = node;
          for (auto &[id, program] : pending) {
            program.materialize = true;
            stage.groups.push_back(PassGroup::forDecision(program));
          }
          pending.clear();
        }
        stage.groups.push_back(PassGroup::required(wrap(spec.lower)));
        auto owner = field(*manifest.getObject("units")->getObject(
                               field(*c.catalog.getObject(at), "unit")),
                           "owner");
        auto defaults = owner == "exits/" + name
                            ? Object{}
                            : settings(config, "nodes", node);
        auto opts = mergedField(defaults, overrides, "optimizations");
        std::vector<std::string> ordered;
        if (auto err = orderContributions(c, at, ordered))
          return std::move(err);
        for (auto &id : ordered) {
          auto &v = c.contributions.at(id).value;
          bool enabled = opts.getBoolean(id).value_or(v.enabled);
          if (enabled)
            chosen.insert(id);
          stage.groups.push_back(
              PassGroup::optimization(id, wrap(v.build), enabled));
        }
        for (auto &[id, v] : c.families) {
          if (v.value.decideAt != at)
            continue;
          auto table = std::make_shared<detail::DecisionTable>();
          table->family = v.value.family;
          table->target = profile;
          for (auto &[sid, s] : c.strategies)
            if (s.value.family == id && available(sid))
              table->strategies.emplace(sid, s.value);
          for (auto &[rid, r] : c.realizers)
            if (r.value.family == id && available(rid))
              table->realizers.emplace(rid, r.value);
          for (auto &[mid, m] : c.costModels)
            if (m.value.family == id && available(mid)) {
              bool local =
                  field(*c.catalog.getObject(mid), "kind") == "local_estimator";
              (local ? table->localModels : table->models)
                  .emplace(mid, m.value);
            }
          auto selections = mergedField(defaults, overrides, "decisions");
          auto *selection = selections.getObject(id);
          if (pathBindings.count(id)) {
            if (auto *explicitChoices = overrides.getObject("decisions"))
              if (auto *requested = explicitChoices->getObject(id))
                if (requested->getString("realizer") != pathBindings.at(id))
                  return createStringError(
                      name + ": realizer for " + id +
                      " is fixed by the branch attachment");
            if (selection && selection->size() == 1 &&
                selection->getString("realizer") &&
                defaults.getObject("decisions"))
              selections = *defaults.getObject("decisions");
          }
          auto choice = choiceFor(selections, id);
          if (!choice)
            return choice.takeError();
          if (auto privateBinding = pathBindings.find(id);
              privateBinding != pathBindings.end())
            choice->realizer = privateBinding->second;
          if (choice->model.empty() && !table->models.empty()) {
            if (table->models.size() != 1)
              return createStringError(
                  id + ": configuration must choose a cost model");
            choice->model = table->models.begin()->first;
          }
          if (!choice->model.empty())
            chosen.insert(choice->model);
          detail::DecisionProgram program{table, *choice, false};
          if (auto err = program.validate())
            return std::move(err);
          chosen.insert(id);
          chosen.insert(choice->strategies.begin(), choice->strategies.end());
          chosen.insert(choice->realizer);
          stage.groups.push_back(PassGroup::forDecision(program));
          pending.emplace(id, std::move(program));
        }
        for (auto &[id, v] : c.families) {
          if (v.value.materializeAt != at)
            continue;
          if (!pending.count(id))
            return createStringError("materialization precedes decision for " +
                                     id);
          auto program = pending.at(id);
          program.materialize = true;
          stage.groups.push_back(PassGroup::forDecision(std::move(program)));
          pending.erase(id);
        }
        t.stages.push_back(std::move(stage));
      }
      if (!pending.empty())
        return createStringError(name + ": exit before materialization of " +
                                 pending.begin()->first);
      std::vector<RegisterDialects> dialects;
      for (auto &[id, unit] : units)
        if (unit.dialects &&
            owners.count(
                field(*manifest.getObject("units")->getObject(id), "owner")))
          dialects.push_back(unit.dialects);
      t.registerDialects = [dialects](mlir::DialectRegistry &r) {
        for (auto &add : dialects)
          add(r);
      };
      if(implementation.terminal){
        auto &verify=t.stages.back().verifyOutput;
        verify=[prior=verify,printer=*implementation.terminal,profile](mlir::ModuleOp m){
          if(mlir::failed(prior(m)))return mlir::failure();
          return printer.verify(m,*profile);
        };
      }
      selected[name] = Array(chosen);
      return t;
    };
    auto made = buildExit();
    (*status)["valid"] = bool(made);
    if (made)
      exits.push_back(std::move(*made));
    else {
      Exit invalid;
      invalid.name = name;
      invalid.error = llvm::toString(made.takeError());
      (*status)["error"] = invalid.error;
      selected[name] = Array{};
      exits.push_back(std::move(invalid));
    }
  }
  description["configuration"] = Object(config);
  description["components"] = std::move(c.catalog);
  description["selected"] = std::move(selected);
  return Compiler::create(std::move(front), std::move(exits),
                          std::move(description));
}
