#pragma once

#include "compilerinfra/Contract.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace compilerinfra {
struct TargetProfile;

// The visitor owns recursion. A rule spells only its own syntax at each point.
enum class PrintPoint {
  Enter,
  BeginRegion,
  BeginBlock,
  EndBlock,
  EndRegion,
  Leave
};

class PrintContext {
public:
  std::string name(mlir::Value value);
  std::string label(mlir::Block *block);
  llvm::Expected<llvm::StringRef> source(llvm::StringRef implementation) const;
  void line(const llvm::Twine &text);
  unsigned indent = 0;
  unsigned region = 0, block = 0;
  std::map<std::string, std::string> files;
  std::string file;

private:
  friend struct TerminalPrinter;
  const std::map<std::string, std::string> *sources = nullptr;
  llvm::DenseMap<mlir::Value, unsigned> names;
  llvm::DenseMap<mlir::Block *, unsigned> labels;
};

// Strictly local view of a single terminal IR operation: exposes only its own
// pre-resolved operand/result names, types, attribute dictionary and current
// region/block header. No Operation*, parent/sibling traversal, or MachineFacts.
class LocalOpView {
public:
  llvm::StringRef operation() const { return opName; }
  mlir::Location location() const { return loc; }
  llvm::StringRef implementation() const { return implKey; }
  llvm::StringRef symbol() const { return implSymbol; }

  unsigned numOperands() const { return operandNames.size(); }
  llvm::ArrayRef<std::string> operands() const { return operandNames; }
  llvm::StringRef operand(unsigned i) const { return operandNames[i]; }
  llvm::ArrayRef<mlir::Type> operandTypes() const { return inTypes; }
  mlir::Type operandType(unsigned i) const { return inTypes[i]; }

  unsigned numResults() const { return resultNames.size(); }
  llvm::ArrayRef<std::string> results() const { return resultNames; }
  llvm::StringRef result(unsigned i) const { return resultNames[i]; }
  llvm::ArrayRef<mlir::Type> resultTypes() const { return outTypes; }
  mlir::Type resultType(unsigned i) const { return outTypes[i]; }

  mlir::DictionaryAttr attributes() const { return attrs; }
  mlir::Attribute attr(llvm::StringRef key) const { return attrs.get(key); }
  template <typename AttrT> AttrT attrOfType(llvm::StringRef key) const {
    return llvm::dyn_cast_or_null<AttrT>(attrs.get(key));
  }

  unsigned numRegions() const { return regionBlockCounts.size(); }
  unsigned numBlocks(unsigned regionIdx) const {
    return regionBlockCounts[regionIdx];
  }
  llvm::StringRef blockLabel() const { return currentBlockLabel; }
  llvm::ArrayRef<std::string> blockArguments() const { return blockArgNames; }
  llvm::ArrayRef<mlir::Type> blockArgumentTypes() const { return blockArgTypes; }
  llvm::ArrayRef<std::string> successorLabels() const { return succLabels; }

  static LocalOpView build(mlir::Operation &op, PrintPoint point,
                           PrintContext &ctx, llvm::StringRef boundSymbol = "");

private:
  std::string opName, implKey, implSymbol, currentBlockLabel;
  mlir::Location loc;
  std::vector<std::string> operandNames, resultNames, blockArgNames, succLabels;
  std::vector<mlir::Type> inTypes, outTypes, blockArgTypes;
  std::vector<unsigned> regionBlockCounts;
  mlir::DictionaryAttr attrs;
  LocalOpView(mlir::Location loc) : loc(loc) {}
};

// PrintRule supports declarative templates, local-view rules (default in v0.5),
// and explicit/legacy custom Operation& rules audited in `inspect`.
class PrintRule {
public:
  enum class Kind : uint8_t { Declarative, Local, Custom };
  using LocalFn = std::function<llvm::Error(const LocalOpView &, PrintPoint,
                                            PrintContext &)>;
  using CustomFn =
      std::function<llvm::Error(mlir::Operation &, PrintPoint, PrintContext &)>;

  PrintRule() = default;

  // Implicit compatibility: callables taking LocalOpView become Kind::Local;
  // legacy callables taking mlir::Operation& become Kind::Custom automatically.
  template <
      typename Fn,
      std::enable_if_t<
          !std::is_same_v<std::decay_t<Fn>, PrintRule> &&
              std::is_invocable_r_v<llvm::Error, Fn, const LocalOpView &,
                                    PrintPoint, PrintContext &> &&
              !std::is_invocable_r_v<llvm::Error, Fn, mlir::Operation &,
                                     PrintPoint, PrintContext &>,
          int> = 0>
  PrintRule(Fn &&fn)
      : ruleKind(Kind::Local), localFn(std::forward<Fn>(fn)) {}

  template <typename Fn,
            std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, PrintRule> &&
                                 std::is_invocable_r_v<llvm::Error, Fn,
                                                       mlir::Operation &,
                                                       PrintPoint, PrintContext &>,
                             int> = 0>
  PrintRule(Fn &&fn)
      : ruleKind(Kind::Custom), customFn(std::forward<Fn>(fn)),
        note("legacy_operation_rule") {}

  static PrintRule local(LocalFn fn);
  static PrintRule format(std::string enterTemplate,
                          std::string leaveTemplate = "",
                          bool indentRegions = false);
  static PrintRule regionFormat(std::string enterTemplate,
                                std::string beginRegionTemplate,
                                std::string endRegionTemplate,
                                std::string leaveTemplate = "",
                                bool indentRegions = true);
  static PrintRule custom(CustomFn fn, std::string reason = "custom");

  explicit operator bool() const { return bool(localFn) || bool(customFn); }
  Kind kind() const { return ruleKind; }
  bool isCustom() const { return ruleKind == Kind::Custom; }
  llvm::StringRef kindSpelling() const;
  llvm::StringRef reason() const { return note; }

  llvm::Error operator()(mlir::Operation &op, PrintPoint point,
                         PrintContext &ctx,
                         llvm::StringRef boundSymbol = "") const;

private:
  Kind ruleKind = Kind::Local;
  LocalFn localFn;
  CustomFn customFn;
  std::string note;
};

struct TerminalPrinter {
  std::map<std::string, std::string> runtimeSources;
  std::map<std::string, std::string> implementationSymbols;
  std::map<std::string, PrintRule> operations;
  std::function<llvm::Error(mlir::DictionaryAttr,
                            const std::map<std::string, std::string> &,
                            llvm::raw_pwrite_stream &)>
      package;
  mlir::LogicalResult verify(mlir::Operation *, const TargetProfile &) const;
  llvm::Error emit(mlir::Operation *, llvm::raw_pwrite_stream &) const;
};

enum class Reuse { Native, Vendor, Library };
llvm::StringRef spelling(Reuse level);
enum class ImplementationSource { Builtin, Runtime, External };
struct ImplementationContract {
  std::string name, operation, symbol, resource;
  ImplementationSource source = ImplementationSource::Builtin;
  Reuse minimum = Reuse::Native;
  std::string operands, layout, accumulator, effects, numerical, estimator;
  std::vector<std::string> costParameters, dependencies;
  std::function<llvm::Error(const TargetProfile &)> available;
  std::function<mlir::LogicalResult(mlir::Operation *)> verify;
  std::optional<TypedContract> typed;
};
} // namespace compilerinfra
