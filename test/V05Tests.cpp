#include "compilerinfra/Assembly.h"
#include "compilerinfra/Contract.h"
#include "compilerinfra/Product.h"
#include "compilerinfra/Proofs.h"
#include "compilerinfra/Terminal.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <iostream>
#include <type_traits>

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

  // Audit fix 5: Operation& callbacks CANNOT implicitly convert to PrintRule;
  // they must be explicitly declared via PrintRule::custom(..., reason).
  auto rawOpFn = [](Operation &, PrintPoint, PrintContext &) {
    return llvm::Error::success();
  };
  static_assert(!std::is_constructible_v<PrintRule, decltype(rawOpFn)>,
                "Operation& callback must not implicitly convert to PrintRule");
  PrintRule explicitCustom = PrintRule::custom(rawOpFn, "audited_escape");
  assert(explicitCustom.isCustom());
  assert(explicitCustom.reason() == "audited_escape");

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
  // An explicit dynamic mode takes precedence over a fixed target fallback.
  setDenormalMode(fn, llvm::DenormalMode::getDynamic());
  assert(effectiveDenormalMode(addOp, llvm::DenormalMode::getPreserveSign()) ==
         llvm::DenormalMode::getDynamic());
  assert(!allowsSubnormalFlush(addOp));
  fn->removeAttr("denormal-fp-math");

  // Audit fix 4b: IsIsolatedFromAbove (e.g. func.func) without
  // MemoryEffectOpInterface must NOT be treated as known pure.
  bool effectsKnown = true;
  EffectMask fnEff = observedMemoryEffects(fn.getOperation(), &effectsKnown);
  assert(!effectsKnown && fnEff == EffectMask::All);

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
  tc.accumulatorTypes = {ScalarTypeKind::F32};
  tc.shape.minOperands = 2;
  tc.shape.maxOperands = 2;
  tc.shape.operandRanks = {2, 2};
  tc.layout.minAlignmentBytes = 64;
  tc.layout.requireContiguousMinorDim = true;
  tc.effects = EffectMask::Read | EffectMask::Write;
  tc.numerical.requireInputSubnormalFlush = true;
  tc.numerical.requireOutputSubnormalFlush = true;
  ftzImpl.typed = tc;
  populateTypedContractSummaries(ftzImpl);
  target.implementations["exits/demo::brgemm_bf16_ftz"] = ftzImpl;

  setDenormalMode(fn, llvm::DenormalMode::getPreserveSign());

  // Audit fix 4a: Missing accumulator_type and missing alignment MUST fail!
  {
    auto err =
        target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp);
    assert(bool(err) && "missing accumulator_type must be rejected");
    llvm::consumeError(std::move(err));
  }
  copyOp->setAttr("accumulator_type", TypeAttr::get(Float32Type::get(&ctx)));
  {
    auto err =
        target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp);
    assert(bool(err) && "missing alignment attribute must be rejected");
    llvm::consumeError(std::move(err));
  }
  copyOp->setAttr("alignment",
                  IntegerAttr::get(IntegerType::get(&ctx, 64), 64));
  for (unsigned i = 0; i != 2; ++i) {
    auto err = target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp);
    assert(bool(err) && "a consumer hint cannot prove either operand aligned");
    llvm::consumeError(std::move(err));
    fn.setArgAttr(i, "llvm.align", IntegerAttr::get(IntegerType::get(&ctx, 64), 64));
  }
  llvm::cantFail(target.checkImplementationForOp("exits/demo::brgemm_bf16_ftz", copyOp));

  // Audit fix 4c: Explicit fastmath=none MUST tighten global fastmath default!
  target.numerical.defaultFastMath = arith::FastMathFlags::fast;
  setFastMath(copyOp, arith::FastMathFlags::none);
  assert(!hasFastMath(copyOp, arith::FastMathFlags::reassoc, target.numerical));

  // Explicit denormal-fp-math="ieee,ieee" MUST tighten global FTZ default!
  target.numerical.defaultDenormal = llvm::DenormalMode::getPreserveSign();
  setDenormalMode(fn, llvm::DenormalMode::getIEEE());
  assert(!allowsSubnormalFlush(copyOp, target.numerical));
}

void testNarrowAddressProof(MLIRContext &ctx) {
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
      func.func @empty_no_mem(%x: f32) -> f32 {
        %y = arith.addf %x, %x : f32
        return %y : f32
      }
      func.func @vec_and_shr(%buf: memref<4096xf32>) -> vector<4xi64> {
        %v_bad = arith.constant dense<[0, 1, 2, 5000000000]> : vector<4xi64>
        %v_ok  = arith.constant dense<[0, 1, 2, 3]> : vector<4xi64>
        %c100 = arith.constant 100 : i64
        %c32 = arith.constant 32 : i64
        %c4 = arith.constant 4 : i64
        %shr_bad = arith.shrsi %c100, %c32 : i64
        %shr_ok  = arith.shrsi %c100, %c4 : i64
        %sum_vec = arith.addi %v_bad, %v_ok : vector<4xi64>
        %idx_bad = arith.index_cast %shr_bad : i64 to index
        %idx_ok  = arith.index_cast %shr_ok : i64 to index
        %ld0 = memref.load %buf[%idx_bad] : memref<4096xf32>
        %ld1 = memref.load %buf[%idx_ok] : memref<4096xf32>
        return %sum_vec : vector<4xi64>
      }
    }
  )mlir");

  auto funcs = llvm::to_vector(mod->getOps<func::FuncOp>());
  assert(funcs.size() == 4);

  AddressProofOptions opts;
  opts.targetBitwidth = 32;
  opts.isSigned = true;
  opts.requireNonNegative = true;
  opts.elementByteScale = 4;

  // 1. Safe kernel -> proven 32-bit.
  auto safeProof = proveNarrowAddress(funcs[0], {}, opts);
  assert(safeProof.proven && safeProof.chosenBitwidth == 32);

  // 2. Whole-module scan catches trunci wraparound in second function.
  auto modProof = proveNarrowAddress(mod->getOperation(), {}, opts);
  assert(!modProof.proven && modProof.chosenBitwidth == 64);

  // 3. Audit fix 1b (before/after): Empty root set MUST NOT approve narrowing!
  auto emptyProof = proveNarrowAddress(funcs[2], {}, opts);
  assert(!emptyProof.proven && emptyProof.chosenBitwidth == 64);

  // 4. Audit fix 1a (before/after): Explicit vector<4xi64> address root!
  Value vBad, vOk, idxBad, idxOk;
  funcs[3].walk([&](arith::ConstantOp cst) {
    if (auto d = llvm::dyn_cast<DenseIntElementsAttr>(cst.getValue())) {
      if (llvm::any_of(d.getValues<APInt>(),
                       [](const APInt &x) { return x.sgt(100); }))
        vBad = cst.getResult();
      else
        vOk = cst.getResult();
    }
  });
  SmallVector<arith::IndexCastOp, 2> casts;
  funcs[3].walk([&](arith::IndexCastOp ic) { casts.push_back(ic); });
  assert(vBad && vOk && casts.size() == 2);
  idxBad = casts[0].getResult();
  idxOk = casts[1].getResult();

  auto vecBadProof = proveNarrowAddress(funcs[3], {vBad}, opts);
  assert(!vecBadProof.proven &&
         "vector<4xi64> exceeding i32 must not be skipped");
  auto vecOkProof = proveNarrowAddress(funcs[3], {vOk}, opts);
  assert(vecOkProof.proven && vecOkProof.chosenBitwidth == 32);

  // 5. Audit fix 1c (before/after): Right shift by >= 32 must be rejected!
  auto shrBadProof = proveNarrowAddress(funcs[3], {idxBad}, opts);
  assert(!shrBadProof.proven &&
         "right shift by >= targetBitwidth must be rejected");
  auto shrOkProof = proveNarrowAddress(funcs[3], {idxOk}, opts);
  assert(shrOkProof.proven && shrOkProof.chosenBitwidth == 32);

  // 6. Audit v0.5.1 item 1 (before/after): Unsigned remui(-1, 7) changes from
  // 1 (in i64) to 3 (in i32) and MUST be rejected; remui(100, 7) is proven;
  // unwhitelisted custom ops MUST be rejected!
  auto remuiMod = parseIR(ctx, R"mlir(
    module {
      func.func @remui_and_whitelist(%buf: memref<16xf32>) {
        %neg1 = arith.constant -1 : i64
        %pos100 = arith.constant 100 : i64
        %c7 = arith.constant 7 : i64
        %rem_bad = arith.remui %neg1, %c7 : i64
        %rem_ok  = arith.remui %pos100, %c7 : i64
        %idx_bad = arith.index_cast %rem_bad : i64 to index
        %idx_ok  = arith.index_cast %rem_ok : i64 to index
        %unk     = "custom.unmodeled_index"(%idx_ok) : (index) -> index
        %v0 = memref.load %buf[%idx_bad] : memref<16xf32>
        %v1 = memref.load %buf[%idx_ok] : memref<16xf32>
        return
      }
    }
  )mlir");
  auto remFn = *remuiMod->getOps<func::FuncOp>().begin();
  SmallVector<arith::IndexCastOp, 2> remCasts;
  Value unkVal;
  remFn.walk([&](Operation *op) {
    if (auto ic = llvm::dyn_cast<arith::IndexCastOp>(op))
      remCasts.push_back(ic);
    if (op->getName().getStringRef() == "custom.unmodeled_index")
      unkVal = op->getResult(0);
  });
  assert(remCasts.size() == 2 && unkVal);
  assert(!proveNarrowAddress(remFn, {remCasts[0].getResult()}, opts).proven &&
         "remui(-1, 7) must be rejected when narrowing i64 -> i32");
  assert(proveNarrowAddress(remFn, {remCasts[1].getResult()}, opts).proven &&
         "remui(100, 7) is non-negative and fits unsigned/signed i32");
  assert(!proveNarrowAddress(remFn, {unkVal}, opts).proven &&
         "unwhitelisted operation must be rejected");
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
  assert(proveValueReplay(loads[0], loads[1]).proven);

  // Case B (Audit fix 2a before/after): Arithmetic op fed by prior load must
  // trace transitive use-def reads and reject intervening clobber!
  auto arithFromLoadMod = parseIR(ctx, R"mlir(
    module {
      func.func @arith_from_load(%src: memref<64xf32>, %cst: f32) -> f32 {
        %c0 = arith.constant 0 : index
        %ld = memref.load %src[%c0] : memref<64xf32>
        %mul1 = arith.mulf %ld, %cst : f32
        memref.store %cst, %src[%c0] : memref<64xf32>
        %ld2 = memref.load %src[%c0] : memref<64xf32>
        %mul2 = arith.mulf %ld2, %cst : f32
        %res = arith.addf %mul1, %mul2 : f32
        return %res : f32
      }
    }
  )mlir");
  SmallVector<arith::MulFOp, 2> muls;
  arithFromLoadMod->walk([&](arith::MulFOp m) { muls.push_back(m); });
  assert(muls.size() == 2);
  auto arithReplay = proveValueReplay(muls[0], muls[1]);
  assert(!arithReplay.proven &&
         "arithmetic op fed by load must include transitive read locations");

  // Case C (Audit fix 2b before/after): Store in sibling `else` branch of a
  // nested `scf.if` inside `scf.for` executes on subsequent loop iterations!
  auto nestedElseClobberMod = parseIR(ctx, R"mlir(
    module {
      func.func @nested_else_clobber(%src: memref<64xf32>, %cst: f32) {
        %c0 = arith.constant 0 : index
        %c4 = arith.constant 4 : index
        %c1 = arith.constant 1 : index
        %v1 = memref.load %src[%c0] : memref<64xf32>
        scf.for %i = %c0 to %c4 step %c1 {
          %gt = arith.cmpi sgt, %i, %c0 : index
          scf.if %gt {
            %v2 = memref.load %src[%c0] : memref<64xf32>
          } else {
            memref.store %cst, %src[%i] : memref<64xf32>
          }
        }
        return
      }
    }
  )mlir");
  loads.clear();
  nestedElseClobberMod->walk([&](memref::LoadOp l) { loads.push_back(l); });
  assert(loads.size() == 2);
  auto elseClobber = proveValueReplay(loads[0], loads[1]);
  assert(!elseClobber.proven &&
         "sibling else-branch store in enclosing loop must invalidate replay");

  // Case D (Audit v0.5.1 item 2 before/after): Non-entry CFG block argument
  // fed by a predecessor load must NOT be treated as an immutable parameter!
  auto nonEntryCfgMod = parseIR(ctx, R"mlir(
    module {
      func.func @non_entry_cfg(%src: memref<64xf32>, %cst: f32) -> f32 {
        %c0 = arith.constant 0 : index
        %ld = memref.load %src[%c0] : memref<64xf32>
        "custom.br"(%ld)[^bb1] : (f32) -> ()
      ^bb1(%arg: f32):
        %mul1 = arith.mulf %arg, %cst : f32
        memref.store %cst, %src[%c0] : memref<64xf32>
        %ld2 = memref.load %src[%c0] : memref<64xf32>
        %mul2 = arith.mulf %ld2, %cst : f32
        %sum = arith.addf %mul1, %mul2 : f32
        return %sum : f32
      }
    }
  )mlir");
  SmallVector<arith::MulFOp, 2> cfgMuls;
  nonEntryCfgMod->walk([&](arith::MulFOp m) { cfgMuls.push_back(m); });
  assert(cfgMuls.size() == 2);
  assert(!proveValueReplay(cfgMuls[0], cfgMuls[1]).proven &&
         "non-entry CFG block argument must not pass as read-free");
}

void testTailAccessProof(MLIRContext &ctx) {
  auto mod = parseIR(ctx, R"mlir(
    module {
      func.func @tail(%unknown_off: index) {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %c8 = arith.constant 8 : index
        %c16 = arith.constant 16 : index
        %c120 = arith.constant 120 : index
        %ramp = arith.constant dense<[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]> : vector<16xindex>
        %lim120 = arith.constant dense<120> : vector<16xindex>
        %lim128 = arith.constant dense<128> : vector<16xindex>
        scf.for %blk = %c0 to %c8 step %c1 {
          %off_aligned = arith.muli %blk, %c16 : index
          %splat = "vector.splat"(%off_aligned) : (index) -> vector<16xindex>
          %coords = arith.addi %splat, %ramp : vector<16xindex>
          %good_mask = arith.cmpi slt, %coords, %lim120 : vector<16xindex>
          %bad_mask  = arith.cmpi slt, %coords, %lim128 : vector<16xindex>
        }
        scf.for %unaligned_iv = %c0 to %c120 step %c1 {
          "arith.addi"(%unaligned_iv, %c0) : (index, index) -> index
        }
        return
      }
    }
  )mlir");
  IntegerRangeContext ranges(mod->getOperation());
  auto fn = *mod->getOps<func::FuncOp>().begin();
  Value unknownOff = fn.getArgument(0);
  Value alignedOff, unalignedIv, goodMask, badMask;
  SmallVector<scf::ForOp, 2> loops;
  fn.walk([&](scf::ForOp l) { loops.push_back(l); });
  assert(loops.size() == 2);
  unalignedIv = loops[1].getInductionVar();
  loops[0].walk([&](arith::MulIOp mul) { alignedOff = mul.getResult(); });
  SmallVector<arith::CmpIOp, 2> cmps;
  loops[0].walk([&](arith::CmpIOp c) { cmps.push_back(c); });
  assert(alignedOff && cmps.size() == 2);
  goodMask = cmps[0].getResult();
  badMask = cmps[1].getResult();

  // 1. Exact divisible 8 * 16 == 128 -> ExactInBounds without mask!
  TailDimension exact;
  exact.staticExtent = 128;
  exact.dynamicOffset = alignedOff;
  exact.tileSize = 16;
  exact.step = 1;
  auto pExact = proveTailAccess({exact}, &ranges);
  assert(pExact.proven && pExact.kind == TailKind::ExactInBounds);

  // 2. Audit fix 3a (before/after): Bogus caller `tileAlignment = 16` on an
  // unaligned offset `unalignedIv` in [0, 120) MUST NOT be trusted!
  TailDimension bogusAlign;
  bogusAlign.staticExtent = 128;
  bogusAlign.dynamicOffset = unalignedIv;
  bogusAlign.tileSize = 16;
  bogusAlign.step = 1;
  bogusAlign.tileAlignment = 16; // False claim: step is 1, not 16!
  auto pBogusAlign = proveTailAccess({bogusAlign}, &ranges);
  assert(!pBogusAlign.proven &&
         "unverified tileAlignment claim must be rejected");

  // 3. Audit fix 3b (before/after): Unknown offset + `hasExplicitMask=true`
  // MUST NOT be approved, and missing/wrong maskPredicate MUST NOT be approved!
  TailDimension unknownMasked;
  unknownMasked.staticExtent = 120;
  unknownMasked.dynamicOffset = unknownOff;
  unknownMasked.tileSize = 16;
  unknownMasked.hasExplicitMask = true;
  unknownMasked.maskPredicate = goodMask;
  assert(!proveTailAccess({unknownMasked}, &ranges).proven &&
         "unknown offset must not pass MaskedSafe");

  TailDimension unverifiedMask = exact;
  unverifiedMask.staticExtent = 120;
  unverifiedMask.hasExplicitMask = true; // No maskPredicate!
  assert(!proveTailAccess({unverifiedMask}, &ranges).proven &&
         "hasExplicitMask=true without maskPredicate must be rejected");

  unverifiedMask.maskPredicate = badMask; // Compares < 128 instead of < 120!
  assert(!proveTailAccess({unverifiedMask}, &ranges).proven &&
         "maskPredicate exceeding logical extent 120 must be rejected");

  // 4. Verified lane mask comparing `off + [0..15] < 120` -> MaskedSafe!
  unverifiedMask.maskPredicate = goodMask;
  auto pMasked = proveTailAccess({unverifiedMask}, &ranges);
  assert(pMasked.proven && pMasked.kind == TailKind::MaskedSafe);

  // 5. Audit v0.5.1 item 3a (before/after): Zero-extending negative i8(-3)
  // gives 253 (NOT divisible by 3!), so extui must reject negative sources!
  // And item 3b (before/after): Narrow i8 lane coordinates `120 + [0..15]` wrap
  // 128 -> -128 in signed i8 and fool `slt < 120`, so must be rejected!
  auto v052TailMod = parseIR(ctx, R"mlir(
    module {
      func.func @v052_tail() {
        %neg1_i8 = arith.constant -1 : i8
        %pos2_i8 = arith.constant 2 : i8
        %c3_i8 = arith.constant 3 : i8
        %neg3_i8 = arith.muli %neg1_i8, %c3_i8 : i8
        %pos6_i8 = arith.muli %pos2_i8, %c3_i8 : i8
        %zext_bad = arith.extui %neg3_i8 : i8 to i32
        %zext_ok  = arith.extui %pos6_i8 : i8 to i32
        %off_bad  = arith.index_cast %zext_bad : i32 to index
        %off_ok   = arith.index_cast %zext_ok  : i32 to index

        %off120_i8 = arith.constant 120 : i8
        %splat120 = "vector.splat"(%off120_i8) : (i8) -> vector<16xi8>
        %ramp_i8 = arith.constant dense<[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]> : vector<16xi8>
        %lim120_i8 = arith.constant dense<120> : vector<16xi8>
        %wrap_coords = arith.addi %splat120, %ramp_i8 : vector<16xi8>
        %wrap_mask = arith.cmpi slt, %wrap_coords, %lim120_i8 : vector<16xi8>
        return
      }
    }
  )mlir");
  IntegerRangeContext r2(v052TailMod->getOperation());
  SmallVector<arith::IndexCastOp, 2> tailCasts;
  Value off120I8, wrapMask;
  v052TailMod->walk([&](Operation *op) {
    if (auto ic = llvm::dyn_cast<arith::IndexCastOp>(op))
      tailCasts.push_back(ic);
    if (auto cst = llvm::dyn_cast<arith::ConstantOp>(op))
      if (auto ia = llvm::dyn_cast<IntegerAttr>(cst.getValue()))
        if (ia.getType().isInteger(8) && ia.getInt() == 120)
          off120I8 = cst.getResult();
    if (auto cmp = llvm::dyn_cast<arith::CmpIOp>(op))
      wrapMask = cmp.getResult();
  });
  assert(tailCasts.size() == 2 && off120I8 && wrapMask);

  // Note: staticExtent = 255 is divisible by 3, and off_bad = 253 < 255 with
  // 253 + 2 == 255 >= 255, so ONLY the divisibility branch could approve it!
  TailDimension zextBadDim;
  zextBadDim.staticExtent = 255;
  zextBadDim.dynamicOffset = tailCasts[0].getResult();
  zextBadDim.tileSize = 3;
  zextBadDim.step = 1;
  zextBadDim.tileAlignment = 3;
  assert(!proveTailAccess({zextBadDim}, &r2).proven &&
         "extui of negative i8(-3) -> 253 must NOT be treated as multiple of 3");

  TailDimension zextOkDim = zextBadDim;
  zextOkDim.staticExtent = 9;
  zextOkDim.dynamicOffset = tailCasts[1].getResult(); // 6, and 6+2=8 < 9 or staticExtent=9: test staticExtent=9 -> 6+2=8 < 9; use staticExtent=9 vs 6:
  // With staticExtent = 9 and off in {0, 6} via select, maxCoord = 6+2 = 8 < 9.
  // Test alignment branch directly with select(cond, 0, 6) and staticExtent = 9? 6+2=8 < 9;
  // For off=6, tileSize=3, staticExtent=9 -> 8 < 9; to hit alignment branch specifically,
  // off range [0, 6] with staticExtent = 6 won't satisfy off < 6; with off in [0, 6] (max=6) and staticExtent=9, 6+2=8<9!
  // Wait: if off has range [0, 7] (e.g. two branches 3 and 6 -> range [3, 6], staticExtent=9 -> 6+2=8<9;
  // what if range is [0, 8] because IntegerRangeAnalysis widens, e.g. 0..6, staticExtent = 9 -> 8 < 9;
  // if staticExtent = 9 and range is [0, 7]: maxCoord = 7+2 = 9 >= 9, off.hi = 7 < 9, aligned to 3 -> ExactInBounds!).
  assert(proveTailAccess({zextOkDim}, &r2).proven);

  TailDimension wrapMaskDim;
  wrapMaskDim.staticExtent = 120;
  wrapMaskDim.dynamicOffset = off120I8;
  wrapMaskDim.tileSize = 16;
  wrapMaskDim.step = 1;
  wrapMaskDim.hasExplicitMask = true;
  wrapMaskDim.maskPredicate = wrapMask;
  assert(!proveTailAccess({wrapMaskDim}, &r2).proven &&
         "i8 mask coordinates wrapping 128 -> -128 must be rejected");
}

// Minimal custom dialect ops exercising IndexedRegionOpInterface,
// TiledAccessOpInterface, and AddressOpInterface via ExternalModel.
class DemoDialect : public Dialect {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DemoDialect)
  explicit DemoDialect(MLIRContext *context)
      : Dialect(getDialectNamespace(), context, TypeID::get<DemoDialect>()) {
    initialize();
  }
  static StringRef getDialectNamespace() { return "demo"; }
  void initialize();
};

class DemoOwnerSpaceOp
    : public Op<DemoOwnerSpaceOp, OpTrait::SingleBlock,
                OpTrait::NoTerminator> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DemoOwnerSpaceOp)
  using Op::Op;
  static StringRef getOperationName() { return "demo.owner_space"; }
  static ArrayRef<StringRef> getAttributeNames() {
    static StringRef names[] = {"extent", "owner_tile"};
    return names;
  }
};

class DemoSliceReadOp
    : public Op<DemoSliceReadOp, OpTrait::ZeroRegions, OpTrait::OneResult> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DemoSliceReadOp)
  using Op::Op;
  static StringRef getOperationName() { return "demo.slice_read"; }
  static ArrayRef<StringRef> getAttributeNames() {
    static StringRef names[] = {"owner_tile"};
    return names;
  }
};

class DemoAddressOp
    : public Op<DemoAddressOp, OpTrait::ZeroRegions, OpTrait::OneResult> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DemoAddressOp)
  using Op::Op;
  static StringRef getOperationName() { return "demo.address"; }
  static ArrayRef<StringRef> getAttributeNames() { return {}; }
};

struct DemoOwnerSpaceBounds
    : public IndexedRegionOpInterface::ExternalModel<DemoOwnerSpaceBounds,
                                                     DemoOwnerSpaceOp> {
  LogicalResult getRegionArgBounds(Operation *op, BlockArgument arg,
                                   RegionIndexBounds &out) const {
    if (arg.getArgNumber() != 0)
      return failure();
    auto ext = op->getAttrOfType<IntegerAttr>("extent");
    auto tile = op->getAttrOfType<IntegerAttr>("owner_tile");
    if (!ext || !tile || ext.getInt() <= 0 || tile.getInt() <= 0)
      return failure();
    int64_t t = tile.getInt();
    int64_t groups = (ext.getInt() + t - 1) / t;
    out.staticLower = 0;
    out.staticUpperExclusive = groups * t;
    out.staticStep = t;
    return success();
  }
};

struct DemoSliceReadAccess
    : public TiledAccessOpInterface::ExternalModel<DemoSliceReadAccess,
                                                   DemoSliceReadOp> {
  LogicalResult getAccessedAxes(Operation *op,
                                SmallVectorImpl<AccessAxis> &axes) const {
    if (op->getNumOperands() != 2)
      return failure();
    auto tile = op->getAttrOfType<IntegerAttr>("owner_tile");
    if (!tile)
      return failure();
    AccessAxis ownerAx;
    ownerAx.container = op->getOperand(0);
    ownerAx.containerDim = 0;
    ownerAx.coord = op->getOperand(1);
    ownerAx.tileSize = tile.getInt();
    ownerAx.step = 1;
    axes.push_back(ownerAx);

    AccessAxis laneAx;
    laneAx.container = op->getOperand(0);
    laneAx.containerDim = 1;
    laneAx.entireAxisFromZero = true;
    laneAx.tileSize = 0; // Derived from containerDim 1 static extent!
    laneAx.step = 1;
    axes.push_back(laneAx);
    return success();
  }
};

struct DemoLinearAddress
    : public AddressOpInterface::ExternalModel<DemoLinearAddress,
                                               DemoAddressOp> {
  LogicalResult getLinearAddress(Operation *op,
                                 LinearAddressDescriptor &desc) const {
    if (op->getNumOperands() != 3)
      return failure();
    desc.baseContainer = op->getOperand(0);
    desc.coordinates.push_back(op->getOperand(1));
    desc.dynamicStrides.push_back(op->getOperand(2));
    desc.constantOffset = 0;
    return success();
  }
};

void DemoDialect::initialize() {
  addOperations<DemoOwnerSpaceOp, DemoSliceReadOp, DemoAddressOp>();
  auto *ctx = getContext();
  DemoOwnerSpaceOp::attachInterface<DemoOwnerSpaceBounds>(*ctx);
  DemoSliceReadOp::attachInterface<DemoSliceReadAccess>(*ctx);
  DemoAddressOp::attachInterface<DemoLinearAddress>(*ctx);
}

void testV053OpAndValueDerivedProofs(MLIRContext &ctx) {
  ctx.getOrLoadDialect<DemoDialect>();

  // 1. Audit-mig Reproducer 2A (Mojo K=4097, depth=64 tail overread):
  // Passing the actual memref.subview Operation* or its result Value derives
  // offset = %pIv in {0, 64, ..., 4096} and extent = 4097 from IR, rejecting
  // [4096, 4160) > 4097 while approving K=4096!
  auto mojoMod = parseIR(ctx, R"mlir(
    module {
      func.func @mojo_tail_4097(%panel: memref<4097x64xf32>) {
        %c0 = arith.constant 0 : index
        %c64 = arith.constant 64 : index
        %c4097 = arith.constant 4097 : index
        scf.for %p = %c0 to %c4097 step %c64 {
          %sub = memref.subview %panel[%p, 0] [64, 64] [1, 1]
              : memref<4097x64xf32> to memref<64x64xf32, strided<[64, 1], offset: ?>>
        }
        return
      }
      func.func @mojo_tail_4096(%panel: memref<4096x64xf32>) {
        %c0 = arith.constant 0 : index
        %c64 = arith.constant 64 : index
        %c4096 = arith.constant 4096 : index
        scf.for %p = %c0 to %c4096 step %c64 {
          %sub = memref.subview %panel[%p, 0] [64, 64] [1, 1]
              : memref<4096x64xf32> to memref<64x64xf32, strided<[64, 1], offset: ?>>
        }
        return
      }
    }
  )mlir");
  SmallVector<memref::SubViewOp, 2> subviews;
  mojoMod->walk([&](memref::SubViewOp sv) { subviews.push_back(sv); });
  assert(subviews.size() == 2);
  auto p4097Op = proveTailAccess(subviews[0].getOperation());
  auto p4097Val = proveTailAccess(subviews[0].getResult());
  assert(!p4097Op.proven && !p4097Val.proven &&
         "K=4097, depth=64 last tile [4096, 4160) must be rejected from IR");
  auto p4096Op = proveTailAccess(subviews[1].getOperation());
  auto p4096Val = proveTailAccess(subviews[1].getResult());
  assert(p4096Op.proven && p4096Op.kind == TailKind::ExactInBounds);
  assert(p4096Val.proven && p4096Val.kind == TailKind::ExactInBounds);

  // 2. Audit-mig Reproducer 2B (Triton/cuTile extent=100, ownerTile=64 bug):
  // IndexedRegionOpInterface + TiledAccessOpInterface derive CTA base %owner in
  // {0, 64} and reject extent=100 while approving extent=128!
  auto ownerMod = parseIR(ctx, R"mlir(
    module {
      func.func @owner_100(%v100: memref<100x64xf16>) {
        "demo.owner_space"() ({
        ^bb0(%owner: index):
          %s = "demo.slice_read"(%v100, %owner) {owner_tile = 64 : i64}
              : (memref<100x64xf16>, index) -> tensor<64x64xf16>
        }) {extent = 100 : i64, owner_tile = 64 : i64} : () -> ()
        return
      }
      func.func @owner_128(%v128: memref<128x64xf16>) {
        "demo.owner_space"() ({
        ^bb0(%owner: index):
          %s = "demo.slice_read"(%v128, %owner) {owner_tile = 64 : i64}
              : (memref<128x64xf16>, index) -> tensor<64x64xf16>
        }) {extent = 128 : i64, owner_tile = 64 : i64} : () -> ()
        return
      }
    }
  )mlir");
  SmallVector<DemoSliceReadOp, 2> sliceReads;
  ownerMod->walk([&](DemoSliceReadOp sr) { sliceReads.push_back(sr); });
  assert(sliceReads.size() == 2);
  assert(!proveTailAccess(sliceReads[0].getOperation()).proven &&
         "extent=100, ownerTile=64 must be rejected without fake extent-tile");
  auto p128 = proveTailAccess(sliceReads[1].getOperation());
  assert(p128.proven && p128.kind == TailKind::ExactInBounds);

  // 3. Standard vector.maskedload + (container, coordinates, mask) Value API:
  auto vecMod = parseIR(ctx, R"mlir(
    module {
      func.func @masked_vec(%buf: memref<120xf32>) {
        %c0 = arith.constant 0 : index
        %c16 = arith.constant 16 : index
        %c128 = arith.constant 128 : index
        %pass = arith.constant dense<0.0> : vector<16xf32>
        %lim120 = arith.constant dense<120> : vector<16xindex>
        %lim128 = arith.constant dense<128> : vector<16xindex>
        %ramp = vector.step : vector<16xindex>
        scf.for %off = %c0 to %c128 step %c16 {
          %splat = vector.splat %off : vector<16xindex>
          %coords = arith.addi %splat, %ramp : vector<16xindex>
          %good_mask = arith.cmpi slt, %coords, %lim120 : vector<16xindex>
          %bad_mask  = arith.cmpi slt, %coords, %lim128 : vector<16xindex>
          %v_ok  = vector.maskedload %buf[%off], %good_mask, %pass
              : memref<120xf32>, vector<16xi1>, vector<16xf32> into vector<16xf32>
          %v_bad = vector.maskedload %buf[%off], %bad_mask, %pass
              : memref<120xf32>, vector<16xi1>, vector<16xf32> into vector<16xf32>
        }
        return
      }
    }
  )mlir");
  SmallVector<vector::MaskedLoadOp, 2> mloads;
  Value vecCoords, goodMaskVal, bufArg;
  vecMod->walk([&](func::FuncOp f) { bufArg = f.getArgument(0); });
  vecMod->walk([&](arith::AddIOp a) { vecCoords = a.getResult(); });
  vecMod->walk([&](vector::MaskedLoadOp ml) { mloads.push_back(ml); });
  assert(mloads.size() == 2 && vecCoords && bufArg);
  goodMaskVal = mloads[0].getMask();
  auto mlOk = proveTailAccess(mloads[0].getOperation());
  assert(mlOk.proven && mlOk.kind == TailKind::MaskedSafe);
  assert(!proveTailAccess(mloads[1].getOperation()).proven &&
         "vector.maskedload with < 128 mask on memref<120> must fail");
  auto coordApiOk = proveTailAccess(bufArg, {vecCoords}, goodMaskVal);
  assert(coordApiOk.proven && coordApiOk.kind == TailKind::MaskedSafe);
  assert(!proveTailAccess(bufArg, {vecCoords}).proven &&
         "unmasked 128-span vector coordinates on memref<120> must fail");

  // 4. AddressOpInterface & Value-based proveNarrowAddress:
  // Even when coord (65536) and stride (65536) each fit i32, coord * stride =
  // 2^32 overflows signed i32 and MUST be rejected by proveNarrowAddress(op)!
  auto addrMod = parseIR(ctx, R"mlir(
    module {
      func.func @addr_test(%base: memref<?xf32>) {
        %c0 = arith.constant 0 : i64
        %c1 = arith.constant 1 : i64
        %c65537 = arith.constant 65537 : i64
        %s65536 = arith.constant 65536 : i64
        %c1024 = arith.constant 1024 : i64
        scf.for %i = %c0 to %c65537 step %c1 : i64 {
          %bad_ptr = "demo.address"(%base, %i, %s65536)
              : (memref<?xf32>, i64, i64) -> memref<?xf32>
        }
        scf.for %j = %c0 to %c1024 step %c1 : i64 {
          %ok_ptr = "demo.address"(%base, %j, %c1024)
              : (memref<?xf32>, i64, i64) -> memref<?xf32>
        }
        return
      }
    }
  )mlir");
  AddressProofOptions addrOpts;
  addrOpts.targetBitwidth = 32;
  addrOpts.isSigned = true;
  addrOpts.requireNonNegative = true;
  addrOpts.elementByteScale = 4;
  SmallVector<DemoAddressOp, 2> demoAddrs;
  addrMod->walk([&](DemoAddressOp a) { demoAddrs.push_back(a); });
  assert(demoAddrs.size() == 2);
  assert(!proveNarrowAddress(demoAddrs[0].getOperation(), addrOpts).proven &&
         "65536 * 65536 overflows i32 and must be rejected");
  assert(!proveNarrowAddress(demoAddrs[0]->getResult(0), addrOpts).proven);
  auto addrOk = proveNarrowAddress(demoAddrs[1].getOperation(), addrOpts);
  assert(addrOk.proven && addrOk.chosenBitwidth == 32);
  assert(proveNarrowAddress(demoAddrs[1]->getResult(0), addrOpts).proven);
}

void testParityAndReadinessMetadata() {
  assert(spelling(ParityStatus::Implemented) == "implemented");
  assert(spelling(ParityStatus::Unsupported) == "unsupported");
  assert(spelling(ParityStatus::Todo) == "todo");

  auto parsed = llvm::cantFail(NumericalOptions::parse(llvm::json::Object{
      {"fastmath", "reassoc,contract"},
      {"denormal_mode", "preserve-sign"},
  }));
  assert(arith::bitEnumContainsAll(parsed.defaultFastMath,
                                   arith::FastMathFlags::reassoc |
                                       arith::FastMathFlags::contract));
  assert(parsed.defaultDenormal == llvm::DenormalMode::getPreserveSign());
}
} // namespace

int main() {
  DialectRegistry registry;
  registry.insert<arith::ArithDialect, func::FuncDialect, memref::MemRefDialect,
                  scf::SCFDialect, tensor::TensorDialect,
                  vector::VectorDialect>();
  MLIRContext ctx(registry);
  ctx.loadAllAvailableDialects();
  ctx.allowUnregisteredDialects();

  testLocalPrinterAndTemplates(ctx);
  testTypedContractsAndNumericalFlags(ctx);
  testNarrowAddressProof(ctx);
  testValueReplayProof(ctx);
  testTailAccessProof(ctx);
  testV053OpAndValueDerivedProofs(ctx);
  testParityAndReadinessMetadata();

  std::cout << "ALL V0.5.3 AUDIT REGRESSION TESTS PASSED\n";
  return 0;
}
