#include "ComponentIndex.h"
#include "DecisionProgram.h"
using namespace compilerinfra;

namespace {
llvm::json::Object describe(const std::string &id, const detail::Entry &entry) {
  return llvm::json::Object{
      {"id", id}, {"layer", entry.layer},
      {"source", llvm::json::Object{{"file", entry.source.file},
                                    {"component", entry.source.component},
                                    {"line", entry.source.line}}},
      {"declaration", llvm::json::Object(entry.declaration)}};
}
} // namespace

llvm::json::Value Compiler::layout() const {
  return definition->index ? llvm::json::Object(definition->index->layout) : llvm::json::Object{};
}

llvm::json::Value Compiler::components() const {
  llvm::json::Array components;
  if (definition->index)
    for (const auto &[id, entry] : definition->index->entries)
      components.push_back(describe(id, entry));
  return components;
}

llvm::Expected<llvm::json::Value>
Compiler::impact(llvm::StringRef component) const {
  if (!definition->index)
    return llvm::createStringError(
        "component provenance requires an Assembly-linked product");
  auto found = definition->index->entries.find(component.str());
  if (found == definition->index->entries.end())
    return llvm::createStringError("unknown component " + component);
  const auto &[id, entry] = *found;
  auto result = describe(id, entry);
  llvm::json::Array applicable, excluded, offRoute;
  for (const Target &target : targets()) {
    auto use = entry.uses.find(target.name);
    if (use == entry.uses.end()) {
      offRoute.push_back(target.name);
    } else if (use->second.empty()) {
      applicable.push_back(target.name);
    } else {
      llvm::json::Array missing;
      for (const auto &feature : use->second)
        missing.push_back(feature);
      excluded.push_back(llvm::json::Object{{"target", target.name},
                                            {"missing", std::move(missing)}});
    }
  }
  result["applicable"] = std::move(applicable);
  result["excluded"] = std::move(excluded);
  result["off_route"] = std::move(offRoute);
  return llvm::json::Value(std::move(result));
}

llvm::Expected<llvm::StringRef>
Compiler::boundaryModel(llvm::StringRef name, llvm::StringRef boundary) const {
  auto found = llvm::find_if(
      targets(), [&](const auto &target) { return target.name == name; });
  if (found == targets().end())
    return llvm::createStringError("unknown target " + name);
  if (boundary == "input")
    return llvm::StringRef(definition->inputModel);
  if (llvm::none_of(found->stages,
                    [&](const auto &stage) { return stage.name == boundary; }))
    return llvm::createStringError(name + ": unknown boundary " + boundary);
  if (!definition->index)
    return llvm::StringRef{};
  return *definition->index->entries.at(("lowering:" + boundary).str())
              .declaration.getString("to");
}

llvm::Expected<llvm::json::Value> Compiler::plan(llvm::StringRef name) const {
  auto found = llvm::find_if(
      targets(), [&](const Target &target) { return target.name == name; });
  if (found == targets().end())
    return llvm::createStringError("unknown target " + name);
  const auto *index = definition->index.get();
  llvm::json::Object result{{"target", found->name}};
  result["input_model"] = definition->inputModel;
  if (index)
    result["route"] = index->targetRoutes.at(found->name);
  llvm::json::Array stages;
  for (const Stage &stage : found->stages) {
    llvm::json::Object step{{"boundary", stage.name}};
    if (index) {
      std::string id = "lowering:" + stage.name;
      step["component"] = describe(id, index->entries.at(id));
    }
    llvm::json::Array groups;
    for (const PassGroup &group : stage.groups) {
      llvm::json::Object slot{{"required", group.isRequired()}};
      if (const auto *p = group.decision()) {
        llvm::json::Array ordered;
        for (const auto &s : p->choice.strategies)
          ordered.push_back(s);
        slot["family"] = p->table->family.name;
        slot["phase"] = p->materialize ? "materialize" : "decide";
        slot["strategies"] = std::move(ordered);
        slot["realizer"] = p->choice.realizer;
      }
      if (!group.isRequired()) {
        slot["name"] = group.name().str();
        slot["default"] = group.defaultEnabled();
        if (index) {
          auto id = ("optimization:" + group.name()).str();
          slot["component"] = describe(id, index->entries.at(id));
        }
      }
      groups.push_back(std::move(slot));
    }
    step["groups"] = std::move(groups);
    stages.push_back(std::move(step));
  }
  result["stages"] = std::move(stages);
  return llvm::json::Value(std::move(result));
}
