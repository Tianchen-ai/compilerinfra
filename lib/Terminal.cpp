#include "compilerinfra/Assembly.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>

using namespace compilerinfra;
using namespace mlir;

llvm::StringRef compilerinfra::spelling(Reuse level) {
  switch (level) {
  case Reuse::Native:
    return "native";
  case Reuse::Vendor:
    return "vendor";
  case Reuse::Library:
    return "library";
  }
  llvm_unreachable("invalid reuse level");
}

llvm::Expected<llvm::StringRef>
PrintContext::source(llvm::StringRef implementation) const {
  if (!sources)
    return llvm::createStringError("no runtime sources bound to this printer");
  auto it = sources->find(implementation.str());
  if (it == sources->end())
    return llvm::createStringError("unbound runtime source " + implementation);
  return llvm::StringRef(it->second);
}

std::string PrintContext::name(Value v) {
  auto id = names.try_emplace(v, names.size()).first->second;
  return "v" + std::to_string(id);
}

std::string PrintContext::label(Block *b) {
  return "bb" +
         std::to_string(labels.try_emplace(b, labels.size()).first->second);
}

void PrintContext::line(const llvm::Twine &text) {
  files[file] += std::string(indent * 4, ' ') + text.str() + "\n";
}

LocalOpView LocalOpView::build(Operation &op, PrintPoint point,
                               PrintContext &ctx, llvm::StringRef boundSymbol) {
  LocalOpView view(op.getLoc());
  view.opName = op.getName().getStringRef().str();
  if (auto b = op.getAttrOfType<StringAttr>("ci.implementation"))
    view.implKey = b.getValue().str();
  view.implSymbol = boundSymbol.str();
  view.attrs = op.getAttrDictionary();

  view.operandNames.reserve(op.getNumOperands());
  view.inTypes.reserve(op.getNumOperands());
  for (Value v : op.getOperands()) {
    view.operandNames.push_back(ctx.name(v));
    view.inTypes.push_back(v.getType());
  }

  view.resultNames.reserve(op.getNumResults());
  view.outTypes.reserve(op.getNumResults());
  for (Value r : op.getResults()) {
    view.resultNames.push_back(ctx.name(r));
    view.outTypes.push_back(r.getType());
  }

  view.regionBlockCounts.reserve(op.getNumRegions());
  for (Region &reg : op.getRegions())
    view.regionBlockCounts.push_back(reg.getBlocks().size());

  if ((point == PrintPoint::BeginBlock || point == PrintPoint::EndBlock) &&
      ctx.region < op.getNumRegions()) {
    Region &reg = op.getRegion(ctx.region);
    if (ctx.block < reg.getBlocks().size()) {
      auto it = reg.begin();
      std::advance(it, ctx.block);
      Block &blk = *it;
      view.currentBlockLabel = ctx.label(&blk);
      for (BlockArgument arg : blk.getArguments()) {
        view.blockArgNames.push_back(ctx.name(arg));
        view.blockArgTypes.push_back(arg.getType());
      }
    }
  }

  for (Block *succ : op.getSuccessors())
    view.succLabels.push_back(ctx.label(succ));

  return view;
}

namespace {
std::string formatAttributeValue(Attribute attr) {
  if (auto s = llvm::dyn_cast<StringAttr>(attr))
    return s.getValue().str();
  if (auto b = llvm::dyn_cast<BoolAttr>(attr))
    return b.getValue() ? "true" : "false";
  if (auto i = llvm::dyn_cast<IntegerAttr>(attr)) {
    llvm::SmallString<32> buf;
    if (i.getType().isUnsignedInteger())
      i.getValue().toStringUnsigned(buf);
    else
      i.getValue().toStringSigned(buf);
    return std::string(buf);
  }
  if (auto f = llvm::dyn_cast<FloatAttr>(attr)) {
    llvm::SmallString<32> buf;
    f.getValue().toString(buf);
    return std::string(buf);
  }
  if (auto sym = llvm::dyn_cast<FlatSymbolRefAttr>(attr))
    return sym.getValue().str();
  std::string out;
  llvm::raw_string_ostream os(out);
  attr.print(os);
  return out;
}

llvm::Expected<std::string> expandTemplate(llvm::StringRef tmpl,
                                           const LocalOpView &view) {
  std::string out;
  out.reserve(tmpl.size() + 16);
  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] == '$') {
      out.push_back('$');
      i += 2;
      continue;
    }
    if (tmpl[i] == '$') {
      size_t start = i + 1;
      if (tmpl.substr(start).starts_with("operands")) {
        out += llvm::join(view.operands(), ", ");
        i = start + 8;
        continue;
      }
      if (tmpl.substr(start).starts_with("results")) {
        out += llvm::join(view.results(), ", ");
        i = start + 7;
        continue;
      }
      if (tmpl.substr(start).starts_with("args")) {
        out += llvm::join(view.blockArguments(), ", ");
        i = start + 4;
        continue;
      }
      if (tmpl.substr(start).starts_with("symbol")) {
        if (view.symbol().empty())
          return llvm::createStringError(view.operation() +
                                         ": unbound $symbol in format rule");
        out += view.symbol();
        i = start + 6;
        continue;
      }
      if (tmpl.substr(start).starts_with("impl")) {
        if (view.implementation().empty())
          return llvm::createStringError(view.operation() +
                                         ": unbound $impl in format rule");
        out += view.implementation();
        i = start + 4;
        continue;
      }
      if (tmpl.substr(start).starts_with("block")) {
        out += view.blockLabel();
        i = start + 5;
        continue;
      }
      char prefix = tmpl[start];
      size_t numStart =
          (prefix == 'r' || prefix == 'b' || prefix == 's') ? start + 1 : start;
      size_t numEnd = numStart;
      while (numEnd < tmpl.size() &&
             std::isdigit(static_cast<unsigned char>(tmpl[numEnd])))
        ++numEnd;
      if (numEnd > numStart) {
        unsigned idx = 0;
        tmpl.slice(numStart, numEnd).getAsInteger(10, idx);
        if (prefix == 'r') {
          if (idx >= view.numResults())
            return llvm::createStringError(view.operation() +
                                           ": result index out of bounds: $r" +
                                           std::to_string(idx));
          out += view.result(idx);
        } else if (prefix == 'b') {
          if (idx >= view.blockArguments().size())
            return llvm::createStringError(
                view.operation() + ": block argument out of bounds: $b" +
                std::to_string(idx));
          out += view.blockArguments()[idx];
        } else if (prefix == 's') {
          if (idx >= view.successorLabels().size())
            return llvm::createStringError(view.operation() +
                                           ": successor out of bounds: $s" +
                                           std::to_string(idx));
          out += view.successorLabels()[idx];
        } else {
          if (idx >= view.numOperands())
            return llvm::createStringError(view.operation() +
                                           ": operand index out of bounds: $" +
                                           std::to_string(idx));
          out += view.operand(idx);
        }
        i = numEnd;
        continue;
      }
    }
    if (tmpl[i] == '{') {
      if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
        out.push_back('{');
        i += 2;
        continue;
      }
      size_t close = tmpl.find('}', i + 1);
      if (close != llvm::StringRef::npos) {
        llvm::StringRef key = tmpl.slice(i + 1, close).trim();
        if (!key.empty() &&
            llvm::all_of(key, [](char c) {
              return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                     c == '.';
            })) {
          Attribute a = view.attr(key);
          if (!a)
            return llvm::createStringError(
                view.operation() + ": missing attribute {" + key.str() + "}");
          out += formatAttributeValue(a);
          i = close + 1;
          continue;
        }
      }
    }
    if (tmpl[i] == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
      out.push_back('}');
      i += 2;
      continue;
    }
    out.push_back(tmpl[i++]);
  }
  return out;
}
} // namespace

PrintRule PrintRule::local(LocalFn fn) {
  PrintRule r;
  r.ruleKind = Kind::Local;
  r.localFn = std::move(fn);
  return r;
}

PrintRule PrintRule::format(std::string enterTemplate,
                            std::string leaveTemplate, bool indentRegions) {
  return regionFormat(std::move(enterTemplate), "", "",
                      std::move(leaveTemplate), indentRegions);
}

PrintRule PrintRule::regionFormat(std::string enterTemplate,
                                  std::string beginRegionTemplate,
                                  std::string endRegionTemplate,
                                  std::string leaveTemplate,
                                  bool indentRegions) {
  PrintRule r;
  r.ruleKind = Kind::Declarative;
  r.localFn = [enter = std::move(enterTemplate),
               begReg = std::move(beginRegionTemplate),
               endReg = std::move(endRegionTemplate),
               leave = std::move(leaveTemplate), indentRegions](
                  const LocalOpView &view, PrintPoint pt,
                  PrintContext &ctx) -> llvm::Error {
    auto emitIf = [&](llvm::StringRef t) -> llvm::Error {
      if (t.empty())
        return llvm::Error::success();
      auto expanded = expandTemplate(t, view);
      if (!expanded)
        return expanded.takeError();
      ctx.line(*expanded);
      return llvm::Error::success();
    };
    if (pt == PrintPoint::Enter)
      return emitIf(enter);
    if (pt == PrintPoint::BeginRegion) {
      if (auto e = emitIf(begReg))
        return e;
      if (indentRegions)
        ++ctx.indent;
      return llvm::Error::success();
    }
    if (pt == PrintPoint::EndRegion) {
      if (indentRegions && ctx.indent > 0)
        --ctx.indent;
      return emitIf(endReg);
    }
    if (pt == PrintPoint::Leave)
      return emitIf(leave);
    return llvm::Error::success();
  };
  return r;
}

PrintRule PrintRule::custom(CustomFn fn, std::string reason) {
  PrintRule r;
  r.ruleKind = Kind::Custom;
  r.customFn = std::move(fn);
  r.note = reason.empty() ? "custom" : std::move(reason);
  return r;
}

llvm::StringRef PrintRule::kindSpelling() const {
  switch (ruleKind) {
  case Kind::Declarative:
    return "declarative";
  case Kind::Local:
    return "local";
  case Kind::Custom:
    return "custom";
  }
  llvm_unreachable("invalid PrintRule::Kind");
}

llvm::Error PrintRule::operator()(Operation &op, PrintPoint point,
                                  PrintContext &ctx,
                                  llvm::StringRef boundSymbol) const {
  if (ruleKind == Kind::Custom)
    return customFn(op, point, ctx);
  LocalOpView view = LocalOpView::build(op, point, ctx, boundSymbol);
  return localFn(view, point, ctx);
}

llvm::Error TargetProfile::checkImplementation(llvm::StringRef key) const {
  auto it = implementations.find(key.str());
  if (it == implementations.end())
    return llvm::createStringError("unknown target implementation " + key);
  auto &impl = it->second;
  if (impl.minimum > reuse)
    return llvm::createStringError(key + ": exceeds reuse=" + spelling(reuse));
  for (auto &id : impl.dependencies) {
    auto report = dependencies.at(id)();
    if (!report.getBoolean("available").value_or(false))
      return llvm::createStringError(
          key + ": " + id + ": " +
          report.getString("reason").value_or("unavailable"));
  }
  return impl.available(*this);
}

llvm::Error TargetProfile::checkImplementationForOp(llvm::StringRef key,
                                                    Operation *op) const {
  if (auto err = checkImplementation(key))
    return err;
  if (!op)
    return llvm::Error::success();
  const auto &impl = implementations.at(key.str());
  if (!impl.typed)
    return llvm::Error::success();
  // At DecisionFamily candidate time, `op` may still be the high-level op
  // before materialization, so validate numerical permissions (fastmath +
  // function denormal mode) and any explicit target precondition here; full
  // terminal shape/layout/effect contract runs on the materialized op.
  const auto &tc = *impl.typed;
  if (!hasFastMath(op, tc.numerical.requiredFastMath, numerical)) {
    auto actual = effectiveFastMath(op, numerical.defaultFastMath);
    auto missing = tc.numerical.requiredFastMath & ~actual;
    return llvm::createStringError(
        key + ": missing required fastmath flags (" +
        mlir::arith::stringifyFastMathFlags(missing) + ")");
  }
  if (tc.numerical.requireInputSubnormalFlush &&
      !allowsInputSubnormalFlush(op, numerical))
    return llvm::createStringError(
        key + ": requires input denormal flush permission (denormal-fp-math)");
  if (tc.numerical.requireOutputSubnormalFlush &&
      !allowsOutputSubnormalFlush(op, numerical))
    return llvm::createStringError(
        key + ": requires output denormal flush permission (denormal-fp-math)");
  if (op->getName().getStringRef() == impl.operation)
    return checkTypedContract(op, impl, *this);
  if (tc.precondition)
    return tc.precondition(op, *this);
  return llvm::Error::success();
}

LogicalResult TerminalPrinter::verify(Operation *root,
                                      const TargetProfile &target) const {
  return success(
      !root->walk([&](Operation *op) {
             auto opName = op->getName().getStringRef().str();
             if (!operations.count(opName)) {
               op->emitError("no terminal print rule for ") << opName;
               return WalkResult::interrupt();
             }
             if (failed(verifyContractOp(op, target)))
               return WalkResult::interrupt();
             auto binding = op->getAttrOfType<StringAttr>("ci.implementation");
             if (!binding) {
               if (!target.implementationOps.count(opName))
                 return WalkResult::advance();
               op->emitError(
                   "terminal operation requires a bound implementation");
               return WalkResult::interrupt();
             }
             auto key = binding.getValue().str();
             if (auto err = target.checkImplementation(key)) {
               op->emitError() << llvm::toString(std::move(err));
               return WalkResult::interrupt();
             }
             auto &contract = target.implementations.at(key);
             if (contract.operation != opName ||
                 failed(verifyBoundContract(op, contract, target))) {
               op->emitError("operation does not satisfy implementation ")
                   << key;
               return WalkResult::interrupt();
             }
             return WalkResult::advance();
           })
           .wasInterrupted());
}

static llvm::Error visit(const TerminalPrinter &printer, Operation &op,
                         PrintContext &ctx) {
  auto rule = printer.operations.find(op.getName().getStringRef().str());
  if (rule == printer.operations.end())
    return llvm::createStringError("missing print rule for " +
                                   op.getName().getStringRef());
  llvm::StringRef boundSymbol;
  if (auto b = op.getAttrOfType<StringAttr>("ci.implementation")) {
    auto sit = printer.implementationSymbols.find(b.getValue().str());
    if (sit != printer.implementationSymbols.end())
      boundSymbol = sit->second;
  }
  auto print = [&](PrintPoint point) {
    return rule->second(op, point, ctx, boundSymbol);
  };
  if (auto e = print(PrintPoint::Enter))
    return e;
  for (auto [i, region] : llvm::enumerate(op.getRegions())) {
    ctx.region = i;
    if (auto e = print(PrintPoint::BeginRegion))
      return e;
    for (auto [j, block] : llvm::enumerate(region)) {
      ctx.region = i;
      ctx.block = j;
      if (auto e = print(PrintPoint::BeginBlock))
        return e;
      for (auto &child : block)
        if (auto e = visit(printer, child, ctx))
          return e;
      ctx.region = i;
      ctx.block = j;
      if (auto e = print(PrintPoint::EndBlock))
        return e;
    }
    ctx.region = i;
    if (auto e = print(PrintPoint::EndRegion))
      return e;
  }
  return print(PrintPoint::Leave);
}

llvm::Error TerminalPrinter::emit(Operation *root,
                                  llvm::raw_pwrite_stream &out) const {
  PrintContext ctx;
  ctx.sources = &runtimeSources;
  if (auto e = visit(*this, *root, ctx))
    return e;
  return package(root->getAttrDictionary(), ctx.files, out);
}
