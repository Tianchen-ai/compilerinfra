#include "compilerinfra/Product.h"
#include "compilerinfra/Runtime.h"
#include "compilerinfra/Session.h"
#include <cstdlib>
#include <cstring>
#include <new>

using namespace compilerinfra;
using llvm::json::Object;
struct ci_compiler {
  std::shared_ptr<const Assembly> assembly;
  std::shared_ptr<const Object> manifest;
  Compiler compiler;
};

static const char *error(llvm::Error e) {
  return ::strdup(llvm::toString(std::move(e)).c_str());
}
static ci_buffer copy(llvm::StringRef value) {
  auto *p = std::malloc(value.size() + 1);
  if (!p)
    throw std::bad_alloc();
  std::memcpy(p, value.data(), value.size());
  return {p, value.size()};
}
static std::string serialize(const llvm::json::Value &value) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << value;
  return text;
}
static llvm::Error merge(Object &base, const Object &delta) {
  // Configuration has three map levels: section -> scope -> settings group.
  // The values in a group are atomic, including a complete family's selection.
  for (auto &[section, scopes] : delta) {
    if (!scopes.getAsObject())
      return llvm::createStringError("expected scope map for " + section.str());
    if (!base.get(section))
      base[section] = Object{};
    for (auto &[id, settings] : *scopes.getAsObject()) {
      auto *group = base.getObject(section);
      if (!group || !settings.getAsObject())
        return llvm::createStringError("invalid configuration scope " +
                                       id.str());
      if (!group->get(id))
        (*group)[id] = Object{};
      for (auto &[kind, values] : *settings.getAsObject()) {
        auto *scope = group->getObject(id);
        if (!scope || !values.getAsObject())
          return llvm::createStringError("expected object for " + kind.str());
        if (!scope->get(kind))
          (*scope)[kind] = Object{};
        auto *map = scope->getObject(kind);
        if (!map)
          return llvm::createStringError("invalid default for " + kind.str());
        for (auto &[key, value] : *values.getAsObject())
          (*map)[key] = value;
      }
    }
  }
  return llvm::Error::success();
}

static const char *openConfigured(std::shared_ptr<const Assembly> assembly,
                                  std::shared_ptr<const Object> manifest,
                                  Object config, const char *changes,
                                  ci_compiler **out) {
  auto delta = llvm::json::parse(changes ? changes : "{}");
  if (!delta)
    return error(delta.takeError());
  if (!delta->getAsObject())
    return ::strdup("configuration must be an object");
  if (auto e = merge(config, *delta->getAsObject()))
    return error(std::move(e));
  auto linked = assembly->link(*manifest, config);
  if (!linked)
    return error(linked.takeError());
  *out = new ci_compiler{assembly, manifest, std::move(*linked)};
  return nullptr;
}
extern "C" {
uint32_t ci_abi_version() { return 1; }
void ci_free(const void *p) { std::free(const_cast<void *>(p)); }
void ci_close(ci_compiler *p) { delete p; }
const char *ci_open(const char *changes, ci_compiler **out) {
  if (!out)
    return ::strdup("missing output handle");
  *out = nullptr;
  auto raw = llvm::json::parse(ci_manifest());
  if (!raw)
    return error(raw.takeError());
  auto manifest = std::make_shared<Object>(std::move(*raw->getAsObject()));
  auto assembly = std::make_shared<Assembly>();
  ci_register(*assembly);
  return openConfigured(assembly, manifest,
                        *manifest->getObject("configuration"), changes, out);
}
const char *ci_derive(const ci_compiler *parent, const char *delta,
                      ci_compiler **out) {
  if (!parent || !out)
    return ::strdup("missing compiler handle");
  *out = nullptr;
  return openConfigured(parent->assembly, parent->manifest,
                        *parent->compiler.describe().getObject("configuration"),
                        delta, out);
}
const char *ci_describe(const ci_compiler *c, ci_buffer *out) {
  if (!c || !out)
    return ::strdup("missing compiler or output");
  *out = copy(serialize(Object(c->compiler.describe())));
  return nullptr;
}
const char *ci_compile(const ci_compiler *c, const char *exit,
                       const void *input, size_t size, const char *after,
                       const char *through, int text, ci_buffer *out,
                       ci_buffer *events) {
  if (!c || !exit || (!input && size) || !out)
    return ::strdup("invalid compilation arguments");
  llvm::json::Array log;
  SessionOptions opts;
  if (events)
    opts.observeDecision = [&](const DecisionEvent &e) {
      log.push_back(Object{{"family", e.family},
                           {"action", e.action},
                           {"implementation", e.implementation},
                           {"operation", e.operation},
                           {"location", e.location},
                           {"value", e.value}});
    };
  auto session = c->compiler.open(exit, opts);
  if (!session)
    return error(session.takeError());
  StageRange range;
  if (after)
    range.after = after;
  if (through)
    range.through = through;
  auto bytes = (*session)->compile(
      llvm::MemoryBufferRef(
          llvm::StringRef(static_cast<const char *>(input), size), "root"),
      range, text ? IRFormat::Assembly : IRFormat::Bytecode);
  if (!bytes)
    return error(bytes.takeError());
  *out = copy({bytes->data(), bytes->size()});
  if (events)
    *events = copy(serialize(std::move(log)));
  return nullptr;
}
}
