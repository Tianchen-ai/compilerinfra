#include "DecisionProgram.h"
using namespace compilerinfra;
using llvm::createStringError;

llvm::Expected<CompilerPreset>
compilerinfra::parsePreset(const llvm::json::Value &value) {
  CompilerPreset result;
  auto *targets = value.getAsObject();
  if (!targets)
    return createStringError("preset must be an object keyed by target");
  for (auto &[name, body] : *targets) {
    auto *fields = body.getAsObject();
    if (!fields)
      return createStringError("expected target preset object: " + name.str());
    auto &target = result[name.str()];
    for (auto &[key, item] : *fields) {
      if (key == "route") {
        if (!item.getAsString())
          return createStringError("route must be a name");
        target.route = item.getAsString()->str();
        continue;
      }
      auto *entries = item.getAsObject();
      if (!entries)
        return createStringError("expected preset object for " + key.str());
      if (key == "options") {
        target.options = *entries;
      } else if (key == "optimizations") {
        for (auto &[id, enabled] : *entries) {
          auto flag = enabled.getAsBoolean();
          if (!flag)
            return createStringError("expected boolean optimization " +
                                     id.str());
          target.optimizations[id.str()] = *flag;
        }
      } else if (key == "decisions") {
        for (auto &[id, selection] : *entries) {
          auto *s = selection.getAsObject();
          if (!s || s->size() != 2 || !s->getArray("strategies") ||
              !s->getString("realizer"))
            return createStringError("expected strategies and realizer for " +
                                     id.str());
          auto &choice = target.decisions[id.str()];
          choice.realizer = s->getString("realizer")->str();
          for (const auto &entry : *s->getArray("strategies")) {
            if (!entry.getAsString())
              return createStringError("expected strategy name for " +
                                       id.str());
            choice.strategies.push_back(entry.getAsString()->str());
          }
        }
      } else
        return createStringError("unknown preset field " + key.str());
    }
  }
  return result;
}

llvm::Expected<Compiler> Compiler::derive(const CompilerPreset &changes) const {
  if (changes.empty())
    return *this;
  auto next = std::make_shared<Definition>(*definition);
  bool relink = false;
  for (auto &[name, delta] : changes) {
    auto &preset = next->preset[name];
    if (delta.route) {
      relink = true;
      preset.route = delta.route;
    }
    for (auto &[key, v] : delta.options) {
      preset.options[key] = v;
      relink = true;
    }
    for (auto &[k, v] : delta.optimizations)
      preset.optimizations[k] = v;
    for (auto &[k, v] : delta.decisions)
      preset.decisions[k] = v;
  }
  if (relink) {
    if (!definition->assembly)
      return createStringError(
          "route and target options require a linked assembly");
    return definition->assembly->link(definition->frontend.import,
                                      definition->inputModel, next->preset);
  }
  for (const auto &[target, change] : changes) {
    auto t = llvm::find_if(next->targets,
                           [&](const Target &v) { return v.name == target; });
    if (t == next->targets.end())
      return createStringError("unknown preset target " + target);
    std::set<std::string> opts, choices;
    for (auto &stage : t->stages)
      for (auto &group : stage.groups) {
        if (!group.isRequired()) {
          auto it = change.optimizations.find(group.name().str());
          if (it != change.optimizations.end()) {
            if (group.enabled != it->second)
              stage.identity = std::make_shared<const char>(0);
            group.enabled = it->second;
            opts.insert(it->first);
          }
        }
        if (auto *program = group.decision()) {
          auto it = change.decisions.find(program->table->family.name);
          if (it != change.decisions.end()) {
            auto selected = *program;
            selected.choice = it->second;
            if (auto e = selected.validate())
              return std::move(e);
            group = PassGroup::forDecision(std::move(selected));
            stage.identity = std::make_shared<const char>(0);
            choices.insert(it->first);
          }
        }
      }
    for (auto &[k, v] : change.optimizations)
      if (!opts.count(k))
        return createStringError(target + ": unknown preset optimization " + k);
    for (auto &[k, v] : change.decisions)
      if (!choices.count(k))
        return createStringError(target + ": unknown preset family " + k);
  }
  return Compiler(std::move(next));
}

llvm::Expected<Compiler> Assembly::link(Import importer, llvm::StringRef input,
                                        const CompilerPreset &preset) const {
  if (preset.empty())
    return link(std::move(importer), input);
  Assembly selected = *this;
  CompilerPreset slots;
  for (const auto &[name, settings] : preset) {
    auto entry = llvm::find_if(selected.providers, [&](const auto &provider) {
      return provider.value.profile.name == name;
    });
    if (entry == selected.providers.end())
      return createStringError("unknown preset target " + name);
    auto &p = entry->value;
    if (settings.route)
      p.route = *settings.route;
    if (!settings.options.empty()) {
      if (!p.configurePreset)
        return createStringError("target does not accept product options: " +
                                 name);
      auto bound = p.configurePreset(p.profile, settings.options);
      if (!bound)
        return bound.takeError();
      if (bound->name != name)
        return createStringError("target binding changed provider identity");
      p.profile = std::move(*bound);
    }
    for (const auto &[family, choice] : settings.decisions)
      p.decisions[family] = choice;
    slots[name].decisions = settings.decisions;
    slots[name].optimizations = settings.optimizations;
  }
  auto compiler = selected.link(std::move(importer), input);
  if (!compiler)
    return compiler.takeError();
  auto configured = compiler->derive(slots);
  if (!configured)
    return configured.takeError();
  auto definition =
      std::make_shared<Compiler::Definition>(*configured->definition);
  definition->assembly = std::make_shared<Assembly>(*this);
  definition->preset = preset;
  return Compiler(std::move(definition));
}
