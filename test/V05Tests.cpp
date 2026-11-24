#include "compilerinfra/Assembly.h"
#include "compilerinfra/Contract.h"
#include "compilerinfra/Proofs.h"
#include "compilerinfra/Terminal.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <iostream>

using namespace compilerinfra;
using namespace mlir;

namespace {
OwningOpRef<ModuleOp> parseIR(MLIRContext &ctx, llvm::StringRef ir) {
  auto mod = parseSourceString<ModuleOp>(ir, &ctx);
  assert(mod && "failed to parse test IR");
  return mod;
}

void testLocalPrinterAndTemplates(MLIRContext &ctx) {
  auto mod = parseIR(ctx, R"mlir(
    module {
      func.func @main(%a: i32, %b: i32) -> i32 {
        %c = arith.addi %a, %b {ci.implementation = "exits/demo::fast_add", tag = "vec4"} : i32
        return %c : i32
      }
    }
  )mlir");

  TerminalPrinter printer;
  printer.implementationSymbols["exits/demo::fast_add"] = "__demo_add_v4";
  printer.operations["builtin.module"] =
      PrintRule::regionFormat("/* module */", "", "", "", false);
  printer.operations["func.func"] = PrintRule::local(
      [](const LocalOpView &op, PrintPoint pt, PrintContext &p) {
        if (pt == PrintPoint::Enter) {
          auto sym = op.attrOfType<StringAttr>("sym_name");
          p.line("fn " + sym.getValue() + " {");
          ++p.indent;
        } else if (pt == PrintPoint::Leave) {
          --p.indent;
          p.line("}");
        }
        return llvm::Error::success();
      });
  printer.operations["arith.addi"] =
      PrintRule::format("$r0 = $symbol($0, $1) /* {tag} */;");
  printer.operations["func.return"] = PrintRule::format("return $0;");
  printer.package = [](DictionaryAttr, const auto &files,
                       llvm::raw_pwrite_stream &out) {
    out << files.at("");
    return llvm::Error::success();
  };

  assert(!printer.operations["arith.addi"].isCustom());
  assert(!printer.operations["func.func"].isCustom());

  // Legacy Operation& lambda automatically classifies as custom.
  PrintRule legacy = [](Operation &, PrintPoint, PrintContext &) {
    return llvm::Error::success();
  };
  assert(legacy.isCustom());

  llvm::SmallString<256> buf;
  llvm::raw_svector_ostream os(buf);
  llvm::cantFail(printer.emit(mod->getOperation(), os));
  llvm::StringRef text = buf.str();
  assert(text.contains("v2 = __demo_add_v4(v0, v1) /* vec4 */;"));
  assert(text.contains("return v2;"));
}

void testTypedContractsAndNumericalFlags(MLIRContext &ctx) {
  auto mod = parseIR(ctx, R"mlir(
    module {
      func.func @strict_fn(%x: memref<16x64xbf16>, %y: memref<16x64xbf16>) {
        %0 = arith.constant 0.0 : f32
        %1 = arith.addf %0, %0 : f32
        memref.copy %x, %y : memref<16x64xbf16> to memref<16x64xbf16>
        return
      }
    }
  )mlir");

  auto fn = *mod->getOps<func::FuncOp>().begin();
  Operation *addOp = nullptr;
  Operation *copyOp = nullptr;
  fn.walk([&](Operation *op) {
    if (llvm::isa<arith::AddFOp>(op))
      addOp = op;
    if (llvm::isa<memref::CopyOp>(op))
      copyOp = op;
  });
  assert(addOp && copyOp);

  TargetProfile target;
  target.name = "demo";
  target.reuse = Reuse::Vendor;

  ImplementationContract ftzImpl;
  ftzImpl.name = "brgemm_bf16_ftz";
  ftzImpl.operation = "memref.copy";
  ftzImpl.symbol = "brgemm_bf16";
  ftzImpl.estimator = "exits/demo::est";
  ftzImpl.available = [](const TargetProfile &) {
    return llvm::Error::success();
  };
  TypedContract tc;
  tc.operandTypes = {ScalarTypeKind::BF16};
  tc.shape.minOperands = 2;
  tc.shape.maxOperands = 2;
  tc.shape.operandRanks = {2, 2};
  tc.layout.requireContiguousMinorDim = true;
  tc.effects = EffectMask::Read | EffectMask::Write;
  tc.numerical.requireInputSubnormalFlush = true;
  tc.numerical.requireOutputSubnormalFlush = true;
  ftzImpl.typed = tc;
  populateTypedContractSummaries(ftzImpl);
  target.implementations["exits/demo::brgemm_bf16_ftz"] = ftzImpl;

  // Under default IEEE denormal mode, FTZ contract must be rejected!
  {
    auto err =
        target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp);
    assert(bool(err) && "expected FTZ rejection under IEEE denormal mode");
    llvm::consumeError(std::move(err));
  }

  // Setting preserve-sign denormal mode on enclosing function enables it!
  setDenormalMode(fn, llvm::DenormalMode::getPreserveSign());
  {
    auto err =
        target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp);
    assert(!err && "expected FTZ contract to pass under preserve-sign mode");
  }

  // Test FastMath propagation intersection.
  auto *cloneA = addOp->clone();
  auto *cloneB = addOp->clone();
  fn.getBody().front().push_front(cloneB);
  fn.getBody().front().push_front(cloneA);
  setFastMath(cloneA, arith::FastMathFlags::reassoc |
                          arith::FastMathFlags::contract);
  setFastMath(cloneB, arith::FastMathFlags::reassoc | arith::FastMathFlags::nnan);
  propagateNumericalFlags({cloneA, cloneB}, addOp);
  assert(hasFastMath(addOp, arith::FastMathFlags::reassoc));
  assert(!hasFastMath(addOp, arith::FastMathFlags::contract));
}

void testNarrowAddressProof(MLIRContext &ctx) {
  // Multi-region module: region 0 is safe [0, 1024), second func has huge i64!
  auto mod = parseIR(ctx, R"mlir(
    module {
      func.func @safe_kernel(%buf: memref<4096xf32>) {
        %c0 = arith.constant 0 : index
        %c64 = arith.constant 64 : index
        %c1 = arith.constant 1 : index
        %c16 = arith.constant 16 : index
        scf.for %iv = %c0 to %c64 step %c1 {
          %idx = arith.muli %iv, %c16 : index
          %v = memref.load %buf[%idx] : memref<4096xf32>
          memref.store %v, %buf[%idx] : memref<4096xf32>
        }
        return
      }
      func.func @overflow_second_region(%buf: memref<?xf32>) {
        %big = arith.constant 5000000000 : i64
        %narrow = arith.trunci %big : i64 to i32
        %idx = arith.index_cast %narrow : i32 to index
        %v = memref.load %buf[%idx] : memref<?xf32>
        return
      }
    }
  )mlir");

  auto funcs = llvm::to_vector(mod->getOps<func::FuncOp>());
  assert(funcs.size() == 2);

  // 1. Safe kernel with 4-byte element scale: max byte offset = 63*16*4 = 4032.
  AddressProofOptions opts;
  opts.targetBitwidth = 32;
  opts.isSigned = true;
  opts.requireNonNegative = true;
  opts.elementByteScale = 4;
  auto safeProof = proveNarrowAddress(funcs[0], {}, opts);
  assert(safeProof.proven && safeProof.chosenBitwidth == 32);

  // 2. Whole-module scan MUST NOT stop at first region — catches trunci
  // wraparound in second function!
  auto modProof = proveNarrowAddress(mod->getOperation(), {}, opts);
  assert(!modProof.proven && modProof.chosenBitwidth == 64);
}

void testValueReplayProof(MLIRContext &ctx) {
  // Case A: Non-aliasing store to separate alloc %tmp -> replay proven!
  auto cleanMod = parseIR(ctx, R"mlir(
    module {
      func.func @clean(%src: memref<64xf32>) -> f32 {
        %c0 = arith.constant 0 : index
        %tmp = memref.alloc() : memref<64xf32>
        %v1 = memref.load %src[%c0] : memref<64xf32>
        memref.store %v1, %tmp[%c0] : memref<64xf32>
        %v2 = memref.load %src[%c0] : memref<64xf32>
        %sum = arith.addf %v1, %v2 : f32
        return %sum : f32
      }
    }
  )mlir");
  SmallVector<memref::LoadOp, 2> loads;
  cleanMod->walk([&](memref::LoadOp l) { loads.push_back(l); });
  assert(loads.size() == 2);
  auto okReplay = proveValueReplay(loads[0], loads[1]);
  assert(okReplay.proven && "distinct alloc store should not block replay");

  // Case B: Intervening aliasing store to %src before second load -> rejected!
  auto clobberMod = parseIR(ctx, R"mlir(
    module {
      func.func @clobber(%src: memref<64xf32>, %cst: f32) -> f32 {
        %c0 = arith.constant 0 : index
        %v1 = memref.load %src[%c0] : memref<64xf32>
        memref.store %cst, %src[%c0] : memref<64xf32>
        %v2 = memref.load %src[%c0] : memref<64xf32>
        %sum = arith.addf %v1, %v2 : f32
        return %sum : f32
      }
    }
  )mlir");
  loads.clear();
  clobberMod->walk([&](memref::LoadOp l) { loads.push_back(l); });
  auto badReplay = proveValueReplay(loads[0], loads[1]);
  assert(!badReplay.proven && badReplay.clobberOp != nullptr);

  // Case C: Replay inside a loop with clobbering store AFTER replay in the same
  // loop body (cross-iteration alias hazard!) -> must be rejected!
  auto loopClobberMod = parseIR(ctx, R"mlir(
    module {
      func.func @loop_clobber(%src: memref<64xf32>, %cst: f32) {
        %c0 = arith.constant 0 : index
        %c4 = arith.constant 4 : index
        %c1 = arith.constant 1 : index
        %v1 = memref.load %src[%c0] : memref<64xf32>
        scf.for %i = %c0 to %c4 step %c1 {
          %v2 = memref.load %src[%c0] : memref<64xf32>
          memref.store %cst, %src[%i] : memref<64xf32>
        }
        return
      }
    }
  )mlir");
  loads.clear();
  loopClobberMod->walk([&](memref::LoadOp l) { loads.push_back(l); });
  auto loopReplay = proveValueReplay(loads[0], loads[1]);
  assert(!loopReplay.proven && "loop backedge store must invalidate replay");
}

void testTailAccessProof(MLIRContext &ctx) {
  auto mod = parseIR(ctx, R"mlir(
    module {
      func.func @tail(%n: index) {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %c8 = arith.constant 8 : index
        %c16 = arith.constant 16 : index
        scf.for %blk = %c0 to %c8 step %c1 {
          %off = arith.muli %blk, %c16 : index
          "arith.addi"(%off, %c0) : (index, index) -> index
        }
        return
      }
    }
  )mlir");
  IntegerRangeContext ranges(mod->getOperation());
  Value offVal;
  mod->walk([&](arith::MulIOp mul) { offVal = mul.getResult(); });
  assert(offVal);

  // 1. Exact divisible 8 * 16 == 128 -> ExactInBounds without mask!
  TailDimension exact;
  exact.staticExtent = 128;
  exact.dynamicOffset = offVal;
  exact.tileSize = 16;
  exact.step = 1;
  auto pExact = proveTailAccess({exact}, &ranges);
  assert(pExact.proven && pExact.kind == TailKind::ExactInBounds);

  // 2. Non-divisible extent 120 without mask -> Unproven (RVV tail overflow)!
  TailDimension unaligned = exact;
  unaligned.staticExtent = 120;
  unaligned.hasExplicitMask = false;
  auto pBad = proveTailAccess({unaligned}, &ranges);
  assert(!pBad.proven &&
         pBad.fallback == TailFallback::RequireMaskOrScalarTail);

  // 3. Same non-divisible extent 120 WITH explicit mask -> MaskedSafe!
  unaligned.hasExplicitMask = true;
  auto pMasked = proveTailAccess({unaligned}, &ranges);
  assert(pMasked.proven && pMasked.kind == TailKind::MaskedSafe);
}
} // namespace

int main() {
  DialectRegistry registry;
  registry.insert<arith::ArithDialect, func::FuncDialect, memref::MemRefDialect,
                  scf::SCFDialect>();
  MLIRContext ctx(registry);
  ctx.loadAllAvailableDialects();

  testLocalPrinterAndTemplates(ctx);
  testTypedContractsAndNumericalFlags(ctx);
  testNarrowAddressProof(ctx);
  testValueReplayProof(ctx);
  testTailAccessProof(ctx);

  std::cout << "ALL V0.5 P0 TESTS PASSED\n";
  return 0;
}
