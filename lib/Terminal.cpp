#include "compilerinfra/Assembly.h"
using namespace compilerinfra;
using namespace mlir;

llvm::StringRef compilerinfra::spelling(Reuse level) {
  switch(level) {
  case Reuse::Native:return "native";
  case Reuse::Vendor:return "vendor";
  case Reuse::Library:return "library";
  }
  llvm_unreachable("invalid reuse level");
}
std::string PrintContext::name(Value v) {
  auto id=names.try_emplace(v,names.size()).first->second;
  return "v"+std::to_string(id);
}
std::string PrintContext::label(Block *b) {
  return "bb"+std::to_string(labels.try_emplace(b,labels.size()).first->second);
}
void PrintContext::line(const llvm::Twine &text) {
  files[file]+=std::string(indent*4,' ')+text.str()+"\n";
}
llvm::Error TargetProfile::checkImplementation(llvm::StringRef key) const {
  auto it=implementations.find(key.str());
  if(it==implementations.end()) return llvm::createStringError("unknown target implementation "+key);
  auto &impl=it->second;
  if(impl.minimum>reuse)
    return llvm::createStringError(key+": exceeds reuse="+spelling(reuse));
  for(auto &id:impl.dependencies){
    auto report=dependencies.at(id)();
    if(!report.getBoolean("available").value_or(false))
      return llvm::createStringError(key+": "+id+": "+report.getString("reason").value_or("unavailable"));
  }
  return impl.available(*this);
}
LogicalResult TerminalPrinter::verify(Operation *root,const TargetProfile &target) const {
  return success(!root->walk([&](Operation *op) {
    auto opName=op->getName().getStringRef().str();
    if(!operations.count(opName)){
      op->emitError("no terminal print rule for ")<<opName;
      return WalkResult::interrupt();
    }
    auto binding=op->getAttrOfType<StringAttr>("ci.implementation");
    if(!binding){
      if(!target.implementationOps.count(opName))return WalkResult::advance();
      op->emitError("terminal operation requires a bound implementation");
      return WalkResult::interrupt();
    }
    auto key=binding.getValue().str();
    if(auto err=target.checkImplementation(key)){
      op->emitError()<<llvm::toString(std::move(err));
      return WalkResult::interrupt();
    }
    auto &contract=target.implementations.at(key);
    if(contract.operation!=opName || failed(contract.verify(op))){
      op->emitError("operation does not satisfy implementation ")<<key;
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  }).wasInterrupted());
}
static llvm::Error visit(const TerminalPrinter &printer,Operation &op,PrintContext &ctx) {
  auto rule=printer.operations.find(op.getName().getStringRef().str());
  if(rule==printer.operations.end())return llvm::createStringError("missing print rule for "+op.getName().getStringRef());
  auto print=[&](PrintPoint point){return rule->second(op,point,ctx);};
  if(auto e=print(PrintPoint::Enter))return e;
  for(auto [i,region]:llvm::enumerate(op.getRegions())){
    ctx.region=i;
    if(auto e=print(PrintPoint::BeginRegion))return e;
    for(auto [j,block]:llvm::enumerate(region)){
      ctx.region=i;ctx.block=j;
      if(auto e=print(PrintPoint::BeginBlock))return e;
      for(auto &child:block)if(auto e=visit(printer,child,ctx))return e;
      ctx.region=i;ctx.block=j;
      if(auto e=print(PrintPoint::EndBlock))return e;
    }
    ctx.region=i;
    if(auto e=print(PrintPoint::EndRegion))return e;
  }
  return print(PrintPoint::Leave);
}
llvm::Error TerminalPrinter::emit(Operation *root,llvm::raw_pwrite_stream &out) const {
  PrintContext ctx;
  if(auto e=visit(*this,*root,ctx))return e;
  return package(root->getAttrDictionary(),ctx.files,out);
}
