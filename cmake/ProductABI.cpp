#include "compilerinfra/Product.h"
#include "compilerinfra/Runtime.h"
#include "compilerinfra/Session.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
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
  struct Checkpoint {
    std::string boundary, digest, root;
  };
  mutable std::deque<Checkpoint> checkpointRoots;
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
  if (delta.get("reuse")) {
    if (auto *exits = base.getObject("exits")) {
      Object cleanedExits;
      for (auto &[exitName, local] : *exits) {
        if (auto *settings = local.getAsObject()) {
          Object cleanedSettings;
          for (auto &[k, v] : *settings)
            if (k != "reuse")
              cleanedSettings[k] = std::move(v);
          if (!cleanedSettings.empty())
            cleanedExits[exitName] = std::move(cleanedSettings);
        } else {
          cleanedExits[exitName] = std::move(local);
        }
      }
      Object cleanedBase;
      for (auto &[k, v] : base)
        if (k != "exits")
          cleanedBase[k] = std::move(v);
      if (!cleanedExits.empty())
        cleanedBase["exits"] = std::move(cleanedExits);
      base = std::move(cleanedBase);
    }
  }
  for (auto &[section, scopes] : delta) {
    if (section == "reuse") {
      base[section] = scopes;
      continue;
    }
    if (section == "numerical") {
      if (!scopes.getAsObject())
        return llvm::createStringError("expected object for numerical");
      if (!base.getObject("numerical"))
        base["numerical"] = Object{};
      auto *dst = base.getObject("numerical");
      for (auto &[k, v] : *scopes.getAsObject())
        (*dst)[k] = v;
      continue;
    }
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
        if (!scope)
          return llvm::createStringError("expected object for " + kind.str());
        if (section == "exits" && kind == "reuse") {
          (*scope)[kind] = values;
          continue;
        }
        if (!values.getAsObject())
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
const char *ci_doctor(const ci_compiler *c, ci_buffer *out) {
  if (!c || !out)
    return ::strdup("missing compiler or output");
  Object results;
  auto declarations =
      c->manifest->getObject("product")->getArray("optional_dependencies");
  for (const auto &exit : c->compiler.exits()) {
    Object entry;
    if (!exit.error.empty())
      entry["error"] = exit.error;
    for (auto &[id, probe] : exit.dependencies) {
      auto report = probe();
      if (declarations)
        for (auto &d : *declarations)
          if (d.getAsObject()->getString("id") == id)
            report["declaration"] = d;
      entry[id] = std::move(report);
    }
    results[exit.name] = std::move(entry);
  }
  *out = copy(serialize(std::move(results)));
  return nullptr;
}
static const char *compile(const ci_compiler *c, const char *exit,
                           const void *input, size_t size, const char *after,
                           const char *through, int text, ci_buffer *out,
                           ci_buffer *events, bool trace, const char *origin,
                           ci_buffer *idOut = nullptr, bool package = false,
                           ci_buffer *receipt = nullptr) {
  if (!c || !exit || (!input && size) || !out)
    return ::strdup("invalid compilation arguments");
  llvm::json::Array log;
  auto id = nextId();
  if (idOut)
    *idOut = copy(id);
  auto hash = [](const void *p, size_t n) {
    return llvm::toHex(
        llvm::SHA256::hash(llvm::ArrayRef(static_cast<const uint8_t *>(p), n)),
        true);
  };
  std::string inputHash = trace ? hash(input, size) : std::string{},
              root = inputHash;
  std::string artifactHash, packageHash;
  llvm::json::Array decisions;
  auto configuration = [&] { return Object(*c->compiler.describe().getObject("configuration")); };
  auto defaultPolicy = [&] {
    auto policy = [](Object config) {
      if (auto *exits = config.getObject("exits")) {
        Object active;
        for (auto &[name, local] : *exits) {
          Object policy;
          if (auto *settings = local.getAsObject())
            for (auto &[key, value] : *settings)
              if (key != "options")
                policy[key] = value;
          if (!policy.empty())
            active[name] = std::move(policy);
        }
        config["exits"] = std::move(active);
      }
      return config;
    };
    return policy(configuration()) == policy(Object(*c->manifest->getObject("configuration")));
  };
  auto finish = [&](const char *failure) {
    if (events) {
      if (trace) {
        std::string calProfile;
        if (const auto *exitsObj = c->compiler.describe().getObject("exits"))
          if (const auto *exObj = exitsObj->getObject(exit))
            if (auto cp = exObj->getString("calibration_profile"))
              calProfile = cp->str();
        *events = copy(serialize(Object{{"compilation_id", id},
                                        {"candidate", c->id},
                                        {"error", failure ? failure : ""},
                                        {"exit", exit},
                                        {"calibration_profile", calProfile},
                                        {"root_sha256", root},
                                        {"input_sha256", inputHash}, {"artifact_sha256", artifactHash},
                                        {"after", after ? after : ""},
                                        {"through", through ? through : ""},
                                        {"package_sha256", packageHash},
                                        {"identity", Object{{"compilation_id", id}, {"compiler_candidate", c->id},
                                                           {"artifact_sha256", artifactHash}, {"decisions", std::move(decisions)}}},
                                        {"product_default", defaultPolicy()}, {"configuration", configuration()},
                                        {"fingerprint", *c->manifest->get("fingerprint")},
                                        {"events", std::move(log)}}));
      } else if (!failure)
        *events = copy(serialize(std::move(log)));
    }
    return failure;
  };
  if (trace && after) {
    root.clear();
    std::lock_guard guard(c->lock);
    std::set<std::string> known;
    for (auto &cp : c->checkpointRoots)
      if (cp.boundary == after && cp.digest == inputHash)
        known.insert(cp.root);
    if (origin) {
      auto provided = llvm::StringRef(origin).lower();
      if (provided.size() != 64 || !llvm::all_of(provided, llvm::isHexDigit) ||
          (!known.empty() && !known.count(provided)))
        return finish(::strdup("invalid root SHA-256 for checkpoint"));
      root = std::move(provided);
    } else if (known.size() == 1)
      root = *known.begin();
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
      if (trace && e.action == "candidate" && e.details.getBoolean("selected").value_or(false)) {
        llvm::json::Array rows;
        if (auto *domain = e.details.getObject("rows"))
          for (const auto &[row, prediction] : *domain) rows.push_back(row.str());
        decisions.push_back(Object{{"family", e.family}, {"structure", *e.details.get("candidate")},
                                   {"rows", std::move(rows)}});
      }
      log.push_back(Object{{"family", e.family},
                           {"action", e.action},
                           {"implementation", e.implementation},
                           {"operation", e.operation},
                           {"location", e.location},
                           {"value", e.value},
                           {"details", Object(e.details)}});
    };
  if (receipt)
    opts.publishSelection = [&](llvm::StringRef family, llvm::StringRef structure) {
      decisions.push_back(Object{{"family",family.str()},{"structure",structure.str()}});
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
  if (receipt) *receipt = copy(serialize(Object{{"compilation_id", id}, {"compiler_candidate", c->id},
                                                {"decisions", llvm::json::Array(decisions)}}));
  if (trace && !through) artifactHash=hash(bytes->data(),bytes->size());
  if (package && !through) {
    auto header = serialize(Object{{"compilation_id", id}, {"compiler_candidate", c->id},
        {"artifact_sha256", artifactHash}, {"decisions", llvm::json::Array(decisions)},
        {"root_sha256", root}, {"exit", exit}, {"configuration", configuration()},
        {"fingerprint", *c->manifest->get("fingerprint")}});
    auto contents = std::string("CIMEAS1\n") + llvm::utostr(header.size()) + "\n" + header;
    contents.append(bytes->data(), bytes->size());
    packageHash = hash(contents.data(), contents.size());
    *out = copy(contents);
  } else {
    *out = copy({bytes->data(), bytes->size()});
  }
  if (through && !root.empty()) {
    std::lock_guard guard(c->lock);
    if (c->checkpointRoots.size() == 128)
      c->checkpointRoots.pop_front();
    c->checkpointRoots.push_back(
        {through, hash(bytes->data(), bytes->size()), root});
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
const char *ci_compile_plain(const ci_compiler *c, const char *exit,
                             const void *input, size_t size, const char *after,
                             const char *through, int text, ci_buffer *out,
                             ci_buffer *id) {
  return compile(c, exit, input, size, after, through, text, out, nullptr,
                 false, nullptr, id);
}
const char *ci_compile_runtime(const ci_compiler *c, const char *exit,
                               const void *input, size_t size, ci_buffer *out, ci_buffer *receipt) {
  return compile(c,exit,input,size,nullptr,nullptr,false,out,nullptr,false,nullptr,nullptr,false,receipt);
}
const char *ci_compile_measured(const ci_compiler *c, const char *exit,
                               const void *input, size_t size,
                               ci_buffer *out, ci_buffer *report) {
  return compile(c, exit, input, size, nullptr, nullptr, false, out, report,
                 true, nullptr, nullptr, true);
}
const char *ci_compile_trace(const ci_compiler *c, const char *exit,
                             const void *input, size_t size, const char *after,
                             const char *through, int text, const char *root,
                             ci_buffer *out, ci_buffer *report) {
  return compile(c, exit, input, size, after, through, text, out, report, true,
                 root);
}
}
