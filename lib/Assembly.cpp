#include "compilerinfra/Assembly.h"
#include "ComponentIndex.h"
#include "DecisionProgram.h"
#include <algorithm>

using namespace compilerinfra;
using llvm::createStringError;

void Assembly::add(Model v, SourceRef s) {
  models.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Lowering v, SourceRef s) {
  lowerings.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Route v, SourceRef s) {
  routes.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Contribution v, SourceRef s) {
  contributions.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Provider v, SourceRef s) {
  providers.push_back({std::move(v), std::move(s)});
}
void Assembly::add(DecisionFamily v, SourceRef s) {
  families.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Strategy v, SourceRef s) {
  strategies.push_back({std::move(v), std::move(s)});
}
void Assembly::add(Realizer v, SourceRef s) {
  realizers.push_back({std::move(v), std::move(s)});
}

namespace {
using Point = std::pair<std::string, std::string>;
using Chain = std::vector<const Lowering *>;

template <typename Items> llvm::json::Array strings(const Items &items) {
  llvm::json::Array result;
  for (const auto &item : items)
    result.push_back(item);
  return result;
}

std::string location(const SourceRef &s) {
  return s.file + ":" + std::to_string(s.line);
}

template <typename T> const std::string &nameOf(const T &v) { return v.name; }
const std::string &nameOf(const Provider &v) { return v.profile.name; }

BuildPasses bindPasses(ModelPasses build,
                       const std::shared_ptr<const TargetProfile> &profile) {
  return [build = std::move(build), profile](mlir::OpPassManager &pm) {
    return build(pm, *profile);
  };
}
BoundaryCheck bindCheck(ModelCheck check,
                        const std::shared_ptr<const TargetProfile> &profile) {
  return [check = std::move(check), profile](mlir::ModuleOp m) {
    return check(m, *profile);
  };
}
} // namespace

namespace compilerinfra {
class AssemblyLinker {
public:
  explicit AssemblyLinker(const Assembly &assembly) : assembly(assembly) {}

  llvm::Expected<Compiler> run(Import importer, llvm::StringRef input);

private:
  const Assembly &assembly;
  std::shared_ptr<detail::ComponentIndex> index =
      std::make_shared<detail::ComponentIndex>();
  template <typename T> using Names = std::map<std::string, const T *>;
  Names<Model> models;
  Names<Lowering> lowerings;
  Names<Route> routes;
  Names<Contribution> contributions;
  Names<Provider> providers;
  Names<DecisionFamily> families;
  Names<Strategy> strategies;
  Names<Realizer> realizers;
  std::map<Point, std::vector<const Contribution *>> points;
  std::map<std::string, Chain> chains;
  std::map<std::string, std::set<std::string>> includedRoutes;
  std::set<std::string> visiting;

  template <typename T>
  llvm::Error collect(llvm::StringRef kind,
                      const std::vector<Assembly::Located<T>> &items,
                      Names<T> &names);
  llvm::Error resolvePoint(const Point &point,
                           std::vector<const Contribution *> &members);
  llvm::Error validate(const std::string &input);
  llvm::Error resolveRoute(const Route &route);
  llvm::Expected<Target> makeTarget(const Provider &provider,
                                    const Model &input);
  llvm::Error invalid(const std::string &id, const llvm::Twine &reason) const {
    return createStringError(
        id + " at " + location(index->entries.at(id).source) + ": " + reason);
  }
};

template <typename T>
llvm::Error
AssemblyLinker::collect(llvm::StringRef kind,
                        const std::vector<Assembly::Located<T>> &items,
                        Names<T> &names) {
  for (const auto &item : items) {
    const std::string &name = nameOf(item.value);
    std::string id = (kind + ":" + name).str();
    if (name.empty() || item.source.file.empty() || !item.source.line)
      return createStringError(
          "component needs a name and a source location: " + id);
    if (!names.emplace(name, &item.value).second)
      return invalid(id, "also declared at " + location(item.source));
    index->entries.emplace(id, detail::Entry{item.source, {}, {}});
  }
  return llvm::Error::success();
}

// Validate and sort once for the whole point. 'after' expresses precedence
// when both contributions are present, including after target filtering.
llvm::Error
AssemblyLinker::resolvePoint(const Point &point,
                             std::vector<const Contribution *> &members) {
  std::map<std::string, std::set<std::string>> followers;
  std::map<std::string, size_t> pending;
  std::set<std::string> ready;
  for (const Contribution *member : members) {
    auto &count = pending[member->name];
    auto predecessors = member->after;
    for (const auto &name : member->afterIfPresent)
      if (contributions.count(name))
        predecessors.push_back(name);
    for (const auto &dependency : predecessors) {
      auto found = contributions.find(dependency);
      if (found == contributions.end() ||
          found->second->lowering != point.first ||
          found->second->point != point.second)
        return invalid("optimization:" + member->name,
                       "unknown predecessor at this point: " + dependency);
      if (followers[dependency].insert(member->name).second)
        ++count;
    }
    if (!count)
      ready.insert(member->name);
  }
  members.clear();
  while (!ready.empty()) {
    auto next = ready.extract(ready.begin()).value();
    members.push_back(contributions.at(next));
    for (const auto &dependent : followers[next])
      if (!--pending[dependent])
        ready.insert(dependent);
  }
  if (members.size() != pending.size()) {
    std::string blocked;
    for (const auto &[name, count] : pending)
      if (count)
        blocked += " " + name + "@" +
                   location(index->entries.at("optimization:" + name).source);
    return createStringError("cyclic precedence at " + point.first + "/" +
                             point.second + ":" + blocked);
  }
  return llvm::Error::success();
}

llvm::Error AssemblyLinker::resolveRoute(const Route &route) {
  if (chains.count(route.name))
    return llvm::Error::success();
  const std::string id = "route:" + route.name;
  if (!visiting.insert(route.name).second)
    return invalid(id, "cyclic route reference");
  Chain chain;
  auto &included = includedRoutes[route.name];
  included.insert(route.name);
  std::set<std::string> used;
  auto append = [&](const Lowering *edge) -> llvm::Error {
    if (!chain.empty() && edge->from != chain.back()->to)
      return invalid(id, edge->name + " consumes " + edge->from +
                             ", previous boundary produces " +
                             chain.back()->to);
    if (!used.insert(edge->name).second)
      return invalid(id, "repeated lowering boundary " + edge->name);
    chain.push_back(edge);
    return llvm::Error::success();
  };
  llvm::json::Array steps;
  for (const auto &step : route.steps) {
    if (const auto *lowering = std::get_if<UseLowering>(&step)) {
      auto found = lowerings.find(lowering->name);
      if (found == lowerings.end())
        return invalid(id, "unknown lowering " + lowering->name);
      if (auto error = append(found->second))
        return error;
      steps.push_back("lowering:" + lowering->name);
    } else {
      const auto &name = std::get<UseRoute>(step).name;
      auto child = routes.find(name);
      if (child == routes.end())
        return invalid(id, "unknown route " + name);
      if (auto error = resolveRoute(*child->second))
        return error;
      for (const auto *edge : chains.at(name))
        if (auto error = append(edge))
          return error;
      const auto &children = includedRoutes.at(name);
      included.insert(children.begin(), children.end());
      steps.push_back("route:" + name);
    }
  }
  index->entries.at(id).declaration =
      llvm::json::Object{{"steps", std::move(steps)}};
  chains.emplace(route.name, std::move(chain));
  visiting.erase(route.name);
  return llvm::Error::success();
}

llvm::Error AssemblyLinker::validate(const std::string &input) {
  if (auto e = collect("model", assembly.models, models))
    return e;
  if (auto e = collect("lowering", assembly.lowerings, lowerings))
    return e;
  if (auto e = collect("route", assembly.routes, routes))
    return e;
  if (auto e = collect("optimization", assembly.contributions, contributions))
    return e;
  if (auto e = collect("provider", assembly.providers, providers))
    return e;
  if (auto e = collect("family", assembly.families, families))
    return e;
  if (auto e = collect("strategy", assembly.strategies, strategies))
    return e;
  if (auto e = collect("realizer", assembly.realizers, realizers))
    return e;
  for (auto &[name, f] : families)
    if (!f->applies || !f->read || !f->write || !f->verify)
      return invalid("family:" + name, "incomplete IR carrier interface");
  for (auto &[name, s] : strategies) {
    if (!s->propose || !families.count(s->family))
      return invalid("strategy:" + name,
                     "missing strategy callback or unknown family " +
                         s->family);
    index->entries.at("strategy:" + name).declaration["family"] = s->family;
  }
  for (auto &[name, r] : realizers) {
    if (!r->apply || !families.count(r->family))
      return invalid("realizer:" + name,
                     "missing realizer callback or unknown family " +
                         r->family);
    index->entries.at("realizer:" + name).declaration["family"] = r->family;
  }
  if (!models.count(input))
    return createStringError("unknown frontend model: " + input);
  for (const auto &[name, model] : models)
    if (!model->verify)
      return invalid("model:" + name, "missing model verifier");
  for (const auto &[name, edge] : lowerings) {
    const std::string id = "lowering:" + name;
    if (name == "input" || !models.count(edge->from) || !models.count(edge->to))
      return invalid(id,
                     "reserved boundary name or unknown input/output model");
    llvm::json::Array declared;
    for (const auto &step : edge->steps) {
      if (const auto *hook = std::get_if<ExtensionPoint>(&step)) {
        if (hook->name.empty() ||
            !points.try_emplace({name, hook->name}).second)
          return invalid(id,
                         "empty or repeated extension point: " + hook->name);
        declared.push_back(hook->name);
      } else if (auto *select = std::get_if<Decide>(&step)) {
        if (!families.count(select->family))
          return invalid(id, "unknown decision family " + select->family);
      } else if (auto *consume = std::get_if<Materialize>(&step)) {
        if (!families.count(consume->family))
          return invalid(id, "unknown decision family " + consume->family);
      } else if (!std::get<ModelPasses>(step))
        return invalid(id, "empty required pass builder");
    }
    index->entries.at(id).declaration =
        llvm::json::Object{{"from", edge->from},
                           {"to", edge->to},
                           {"points", std::move(declared)}};
  }
  for (const auto &[name, member] : contributions) {
    const std::string id = "optimization:" + name;
    auto point = points.find({member->lowering, member->point});
    if (point == points.end() || !member->build)
      return invalid(id, "missing builder or unknown point " +
                             member->lowering + "/" + member->point);
    point->second.push_back(member);
    index->entries.at(id).declaration = llvm::json::Object{
        {"lowering", member->lowering},
        {"point", member->point},
        {"requires", strings(member->requiresFeatures)},
        {"after", strings(member->after)},
        {"after_if_present", strings(member->afterIfPresent)},
        {"default", member->enabledByDefault}};
  }
  for (auto &[point, members] : points)
    if (auto error = resolvePoint(point, members))
      return error;
  for (const auto &[name, route] : routes)
    if (auto error = resolveRoute(*route))
      return error;
  return llvm::Error::success();
}

llvm::Expected<Target> AssemblyLinker::makeTarget(const Provider &provider,
                                                  const Model &input) {
  const auto &name = provider.profile.name;
  const std::string id = "provider:" + name;
  auto chain = chains.find(provider.route);
  if (chain == chains.end())
    return invalid(id, "unknown route " + provider.route);
  if (!chain->second.empty() && chain->second.front()->from != input.name)
    return invalid(id, "route starts at " + chain->second.front()->from +
                           ", expected " + input.name);
  const auto &last =
      chain->second.empty() ? input.name : chain->second.back()->to;
  if (last != provider.accepts || !provider.emit)
    return invalid(id, "expected an emitter for " + last + ", declared " +
                           provider.accepts);

  auto profile = std::make_shared<const TargetProfile>(provider.profile);
  Target target{name, {}, {}, provider.emit, bindCheck(input.verify, profile)};
  for (const auto &r : includedRoutes.at(provider.route))
    index->entries.at("route:" + r).uses[name] = {};
  auto &entry = index->entries.at(id);
  entry.uses[name] = {};
  entry.declaration =
      llvm::json::Object{{"route", provider.route},
                         {"accepts", provider.accepts},
                         {"features", strings(profile->features)}};
  index->targetRoutes.emplace(name, provider.route);
  std::vector<RegisterDialects> registrations;
  std::set<std::string> visited;
  auto useModel = [&](const Model &model) {
    index->entries.at("model:" + model.name).uses[name] = {};
    if (visited.insert(model.name).second && model.registerDialects)
      registrations.push_back(model.registerDialects);
  };
  useModel(input);
  std::set<std::string> pending, decided;
  for (const Lowering *edge : chain->second) {
    index->entries.at("lowering:" + edge->name).uses[name] = {};
    const Model &output = *models.at(edge->to);
    useModel(output);
    Stage stage{edge->name, {}, bindCheck(output.verify, profile)};
    for (const auto &step : edge->steps) {
      if (const auto *required = std::get_if<ModelPasses>(&step)) {
        stage.groups.push_back(
            PassGroup::required(bindPasses(*required, profile)));
        continue;
      }
      if (std::holds_alternative<Decide>(step) ||
          std::holds_alternative<Materialize>(step)) {
        const bool consume = std::holds_alternative<Materialize>(step);
        const auto &family = consume ? std::get<Materialize>(step).family
                                     : std::get<Decide>(step).family;
        if (consume ? !pending.erase(family) : !decided.insert(family).second)
          return invalid(id, "unpaired or repeated decision boundary for " +
                                 family);
        if (!consume)
          pending.insert(family);
        auto choice = provider.decisions.find(family);
        if (choice == provider.decisions.end())
          return invalid(id, "missing decision preset for " + family);
        auto table = std::make_shared<detail::DecisionTable>();
        table->family = *families.at(family);
        table->target = profile;
        for (auto &[n, s] : strategies)
          if (s->family == family) {
            table->strategies.emplace(n, *s);
            index->entries.at("strategy:" + n).uses[name] = {};
          }
        for (auto &[n, r] : realizers)
          if (r->family == family) {
            table->realizers.emplace(n, *r);
            index->entries.at("realizer:" + n).uses[name] = {};
          }
        index->entries.at("family:" + family).uses[name] = {};
        detail::DecisionProgram program{std::move(table), choice->second,
                                        consume};
        if (auto error = program.validate())
          return invalid(id, llvm::toString(std::move(error)));
        stage.groups.push_back(PassGroup::forDecision(std::move(program)));
        continue;
      }
      for (const auto *member :
           points.at({edge->name, std::get<ExtensionPoint>(step).name})) {
        auto &missing =
            index->entries.at("optimization:" + member->name).uses[name];
        for (const auto &feature : member->requiresFeatures)
          if (!profile->features.count(feature))
            missing.push_back(feature);
        if (missing.empty())
          stage.groups.push_back(PassGroup::optimization(
              member->name, bindPasses(member->build, profile),
              member->enabledByDefault, member->point));
      }
    }
    target.stages.push_back(std::move(stage));
  }
  if (!pending.empty())
    return invalid(id, "missing materializer boundary for " + *pending.begin());
  for (const auto &binding : provider.decisions)
    if (!families.count(binding.first))
      return invalid(id,
                     "unknown family in provider defaults " + binding.first);
  if (provider.registerDialects)
    registrations.push_back(provider.registerDialects);
  target.registerDialects =
      [callbacks = std::move(registrations)](mlir::DialectRegistry &registry) {
        for (const auto &registerDialects : callbacks)
          registerDialects(registry);
      };
  return target;
}

llvm::Expected<Compiler> AssemblyLinker::run(Import importer,
                                             llvm::StringRef input) {
  if (auto error = validate(input.str()))
    return error;
  std::vector<Target> targets;
  for (const auto &[name, provider] : providers) {
    auto target = makeTarget(*provider, *models.at(input.str()));
    if (!target)
      return target.takeError();
    targets.push_back(std::move(*target));
  }
  auto compiler = Compiler::create(Frontend{{}, std::move(importer), {}},
                                   std::move(targets), std::move(index));
  if (!compiler)
    return compiler.takeError();
  auto definition =
      std::make_shared<Compiler::Definition>(*compiler->definition);
  definition->assembly = std::make_shared<Assembly>(assembly);
  definition->inputModel = input.str();
  return Compiler(std::move(definition));
}

llvm::Expected<Compiler> Assembly::link(Import importer,
                                        llvm::StringRef inputModel) const {
  return AssemblyLinker(*this).run(std::move(importer), inputModel);
}
} // namespace compilerinfra
