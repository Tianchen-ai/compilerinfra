#include "compilerinfra/Product.h"
#include "compilerinfra/Runtime.h"
#include "compilerinfra/Session.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <random>

using namespace compilerinfra;
using llvm::json::Object;
static std::string nextId() {
  static const std::string prefix = [] {
    std::random_device entropy;
    return llvm::utohexstr(entropy()) + llvm::utohexstr(entropy());
  }();
  static std::atomic<uint64_t> next{0};
  return prefix + "-" + std::to_string(++next);
}
struct ci_compiler {
  std::shared_ptr<const Assembly> assembly;
  std::shared_ptr<const Object> manifest;
  Compiler compiler;
  const std::string id = nextId();
  mutable std::mutex lock;
  mutable std::map<std::pair<std::string, std::string>, std::set<std::string>>
      checkpointRoots;
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

namespace compilerinfra {
class InstalledProduct {
public:
  static llvm::Expected<Compiler>
  link(const Assembly &a, const Object &manifest, const Object &config) {
    return a.link(manifest, config);
  }
};
} // namespace compilerinfra
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
  auto linked = InstalledProduct::link(*assembly, *manifest, config);
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
static const char *compile(const ci_compiler *c, const char *exit,
                           const void *input, size_t size, const char *after,
                           const char *through, int text, ci_buffer *out,
                           ci_buffer *events, bool trace, const char *origin) {
  if (!c || !exit || (!input && size) || !out)
    return ::strdup("invalid compilation arguments");
  llvm::json::Array log;
  auto id = nextId();
  auto hash = [](const void *p, size_t n) {
    return llvm::toHex(
        llvm::SHA256::hash(llvm::ArrayRef(static_cast<const uint8_t *>(p), n)),
        true);
  };
  std::string inputHash = hash(input, size), root = inputHash;
  auto finish = [&](const char *failure) {
    if (events) {
      if (trace)
        *events = copy(serialize(Object{{"compilation_id", id},
                                        {"candidate", c->id},
                                        {"error", failure ? failure : ""},
                                        {"exit", exit},
                                        {"root_sha256", root},
                                        {"input_sha256", inputHash},
                                        {"after", after ? after : ""},
                                        {"through", through ? through : ""},
                                        {"events", std::move(log)}}));
      else if (!failure)
        *events = copy(serialize(std::move(log)));
    }
    return failure;
  };
  if (after) {
    root.clear();
    std::lock_guard guard(c->lock);
    auto it = c->checkpointRoots.find({after, inputHash});
    if (origin) {
      auto provided = llvm::StringRef(origin).lower();
      if (provided.size() != 64 || !llvm::all_of(provided, llvm::isHexDigit) ||
          (it != c->checkpointRoots.end() && !it->second.count(provided)))
        return finish(::strdup("invalid root SHA-256 for checkpoint"));
      root = std::move(provided);
    } else if (it != c->checkpointRoots.end() && it->second.size() == 1)
      root = *it->second.begin();
    else if (trace)
      return finish(
          ::strdup("supply root_sha256 for external or ambiguous checkpoint"));
  }
  SessionOptions opts;
  if (events)
    opts.observeContribution = [&](llvm::StringRef component,
                                   llvm::StringRef at, bool ran, bool changed) {
      log.push_back(Object{{"action", "contribution"},
                           {"implementation", component},
                           {"boundary", at},
                           {"executed", ran},
                           {"changed", changed}});
    };
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
    return finish(error(session.takeError()));
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
    return finish(error(bytes.takeError()));
  *out = copy({bytes->data(), bytes->size()});
  if (through && !root.empty()) {
    std::lock_guard guard(c->lock);
    c->checkpointRoots[{through, hash(bytes->data(), bytes->size())}].insert(
        root);
  }
  return finish(nullptr);
}
const char *ci_compile(const ci_compiler *c, const char *exit,
                       const void *input, size_t size, const char *after,
                       const char *through, int text, ci_buffer *out,
                       ci_buffer *events) {
  return compile(c, exit, input, size, after, through, text, out, events, false,
                 nullptr);
}
const char *ci_compile_trace(const ci_compiler *c, const char *exit,
                             const void *input, size_t size, const char *after,
                             const char *through, int text, const char *root,
                             ci_buffer *out, ci_buffer *report) {
  return compile(c, exit, input, size, after, through, text, out, report, true,
                 root);
}
}
