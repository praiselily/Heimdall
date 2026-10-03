//===- ControlFlowFlattening.cpp - CFG flattening obfuscation pass -----===//
//
// Heimdall: software IP-protection obfuscation for LLVM.
//
// See DESIGN.md for the transform description and rationale.
//
//===----------------------------------------------------------------===//
#include "heimdall/ControlFlowFlattening.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

#include <vector>

#define DEBUG_TYPE "heimdall-cff"

using namespace llvm;

STATISTIC(NumFunctionsFlattened, "Number of functions flattened");
STATISTIC(NumFunctionsSkipped, "Number of functions skipped (ineligible)");

namespace {

/// Reason a function was rejected for flattening, surfaced via
/// -debug-only=heimdall-cff.
enum class SkipReason {
  None,
  TooSmall,
  HasExceptionHandling,
  HasIndirectControlFlow,
  OptNone,
  Declaration,
  UnsupportedEntryTerminator,
};

StringRef skipReasonToString(SkipReason R) {
  switch (R) {
  case SkipReason::None:
    return "none";
  case SkipReason::TooSmall:
    return "function has fewer than 2 basic blocks";
  case SkipReason::HasExceptionHandling:
    return "function uses exception-handling control flow (invoke/landingpad/"
           "resume/catch*/cleanup*)";
  case SkipReason::HasIndirectControlFlow:
    return "function uses indirectbr or callbr";
  case SkipReason::OptNone:
    return "function is marked optnone";
  case SkipReason::Declaration:
    return "function is a declaration";
  case SkipReason::UnsupportedEntryTerminator:
    return "function's entry block does not end in a br/switch";
  }
  llvm_unreachable("unhandled SkipReason");
}

/// Returns the first reason F is ineligible for flattening, or
/// SkipReason::None if it can be transformed.
SkipReason findBailOutReason(Function &F) {
  if (F.isDeclaration())
    return SkipReason::Declaration;
  if (F.hasFnAttribute(Attribute::OptimizeNone))
    return SkipReason::OptNone;
  if (F.size() < 2)
    return SkipReason::TooSmall;

  // flattenFunction only knows how to redirect a br/switch terminator into
  // the dispatcher. An entry block can legally end in something else (a
  // direct ret, with other unreachable blocks elsewhere in F keeping
  // F.size() >= 2) -- checked here, before demoteCrossBlockValues runs, so
  // a rejected function is never left partially transformed.
  Instruction *EntryTerm = F.getEntryBlock().getTerminator();
  if (!isa<BranchInst>(EntryTerm) && !isa<SwitchInst>(EntryTerm))
    return SkipReason::UnsupportedEntryTerminator;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<InvokeInst>(I) || isa<ResumeInst>(I) || isa<LandingPadInst>(I) ||
          isa<CatchSwitchInst>(I) || isa<CatchPadInst>(I) ||
          isa<CatchReturnInst>(I) || isa<CleanupPadInst>(I) ||
          isa<CleanupReturnInst>(I))
        return SkipReason::HasExceptionHandling;
      if (isa<IndirectBrInst>(I) || isa<CallBrInst>(I))
        return SkipReason::HasIndirectControlFlow;
    }
  }

  return SkipReason::None;
}

/// Demotes every value defined in one block and used in another (including
/// PHI nodes and the values feeding them) to a stack slot, via LLVM's own
/// DemoteRegToStack/DemotePHIToStack, so flattening can freely redirect
/// control flow without violating SSA dominance.
void demoteCrossBlockValues(Function &F) {
  std::vector<Instruction *> WorkList;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      WorkList.push_back(&I);

  // Every demotion alloca must dominate all its defs/uses, which for a
  // function-wide pass means the top of the entry block -- entry has no
  // predecessors, so getFirstInsertionPt() is exactly that point.
  BasicBlock::iterator AllocaInsertPt = F.getEntryBlock().getFirstInsertionPt();
  for (Instruction *I : WorkList) {
    if (I->getType()->isVoidTy())
      continue;
    // An existing alloca already lives in the entry block and already
    // dominates every use; it needs no demotion. Wrapping its pointer in
    // another stack slot would replace direct uses of the alloca with a
    // loaded copy of the pointer, which breaks anything that requires the
    // literal alloca (llvm.lifetime.start/end, llvm.dbg.declare).
    if (isa<AllocaInst>(I))
      continue;
    bool UsedOutsideDefiningBlock = false;
    for (User *U : I->users()) {
      auto *UI = cast<Instruction>(U);
      if (UI->getParent() != I->getParent() || isa<PHINode>(UI)) {
        UsedOutsideDefiningBlock = true;
        break;
      }
    }
    if (UsedOutsideDefiningBlock)
      DemoteRegToStack(*I, /*VolatileLoads=*/false, AllocaInsertPt);
  }

  std::vector<PHINode *> Phis;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *PN = dyn_cast<PHINode>(&I))
        Phis.push_back(PN);
  for (PHINode *PN : Phis)
    DemotePHIToStack(PN, AllocaInsertPt);
}

/// Performs the flattening transform on an eligible function. Returns true
/// if the function's IR was modified.
bool flattenFunction(Function &F) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock &Entry = F.getEntryBlock();

  // Eliminate cross-block SSA values up front so every later step only
  // has to deal with block-local instructions plus loads/stores through
  // allocas.
  demoteCrossBlockValues(F);

  // The entry block stays in place and jumps into the dispatcher at the
  // end; every other block gets a state ID and its terminator rewritten.
  std::vector<BasicBlock *> Blocks;
  for (BasicBlock &BB : F)
    if (&BB != &Entry)
      Blocks.push_back(&BB);

  if (Blocks.empty())
    return false;

  IntegerType *I32 = Type::getInt32Ty(Ctx);
  DenseMap<BasicBlock *, uint32_t> StateOf;
  for (uint32_t I = 0; I < Blocks.size(); ++I)
    StateOf[Blocks[I]] = I;

  IRBuilder<> EntryBuilder(Entry.getTerminator());
  AllocaInst *StateVar =
      EntryBuilder.CreateAlloca(I32, nullptr, "heimdall.state");

  auto *Dispatcher = BasicBlock::Create(Ctx, "heimdall.dispatch", &F);
  IRBuilder<> DispatchBuilder(Dispatcher);
  LoadInst *StateLoad =
      DispatchBuilder.CreateLoad(I32, StateVar, "heimdall.state.load");

  // Default case is unreachable: every store to StateVar writes a valid
  // case value.
  auto *Unreachable = BasicBlock::Create(Ctx, "heimdall.unreachable", &F);
  IRBuilder<>(Unreachable).CreateUnreachable();

  SwitchInst *Switch =
      DispatchBuilder.CreateSwitch(StateLoad, Unreachable, Blocks.size());
  for (BasicBlock *BB : Blocks)
    Switch->addCase(ConstantInt::get(I32, StateOf[BB]), BB);

  // Redirect the entry block's terminator into the dispatcher, after
  // recording its original target(s) as the initial state.
  Instruction *EntryTerm = Entry.getTerminator();
  if (auto *Br = dyn_cast<BranchInst>(EntryTerm)) {
    if (Br->isUnconditional()) {
      BasicBlock *Target = Br->getSuccessor(0);
      EntryBuilder.CreateStore(ConstantInt::get(I32, StateOf[Target]),
                                StateVar);
    } else {
      Value *Cond = Br->getCondition();
      Value *TrueState = ConstantInt::get(I32, StateOf[Br->getSuccessor(0)]);
      Value *FalseState = ConstantInt::get(I32, StateOf[Br->getSuccessor(1)]);
      Value *Sel = EntryBuilder.CreateSelect(Cond, TrueState, FalseState,
                                              "heimdall.sel");
      EntryBuilder.CreateStore(Sel, StateVar);
    }
  } else if (auto *Sw = dyn_cast<SwitchInst>(EntryTerm)) {
    Value *Acc = ConstantInt::get(I32, StateOf[Sw->getDefaultDest()]);
    for (auto Case : Sw->cases()) {
      Value *CaseMatches = EntryBuilder.CreateICmpEQ(
          Sw->getCondition(), Case.getCaseValue(), "heimdall.case");
      Value *CaseState =
          ConstantInt::get(I32, StateOf[Case.getCaseSuccessor()]);
      Acc = EntryBuilder.CreateSelect(CaseMatches, CaseState, Acc,
                                       "heimdall.sel");
    }
    EntryBuilder.CreateStore(Acc, StateVar);
  } else {
    Dispatcher->eraseFromParent();
    Unreachable->eraseFromParent();
    StateVar->eraseFromParent();
    return false;
  }
  EntryTerm->eraseFromParent();
  BranchInst::Create(Dispatcher, &Entry);

  // Rewrite every remaining block's terminator the same way and redirect
  // it to the dispatcher.
  for (BasicBlock *BB : Blocks) {
    Instruction *Term = BB->getTerminator();
    IRBuilder<> Builder(Term);

    if (isa<ReturnInst>(Term)) {
      continue; // Returns don't feed the dispatcher.
    }
    if (auto *Br = dyn_cast<BranchInst>(Term)) {
      if (Br->isUnconditional()) {
        BasicBlock *Target = Br->getSuccessor(0);
        Builder.CreateStore(ConstantInt::get(I32, StateOf[Target]), StateVar);
      } else {
        Value *Cond = Br->getCondition();
        Value *TrueState =
            ConstantInt::get(I32, StateOf[Br->getSuccessor(0)]);
        Value *FalseState =
            ConstantInt::get(I32, StateOf[Br->getSuccessor(1)]);
        Value *Sel = Builder.CreateSelect(Cond, TrueState, FalseState,
                                           "heimdall.sel");
        Builder.CreateStore(Sel, StateVar);
      }
    } else if (auto *Sw = dyn_cast<SwitchInst>(Term)) {
      Value *Acc = ConstantInt::get(I32, StateOf[Sw->getDefaultDest()]);
      for (auto Case : Sw->cases()) {
        Value *CaseMatches = Builder.CreateICmpEQ(
            Sw->getCondition(), Case.getCaseValue(), "heimdall.case");
        Value *CaseState =
            ConstantInt::get(I32, StateOf[Case.getCaseSuccessor()]);
        Acc = Builder.CreateSelect(CaseMatches, CaseState, Acc,
                                    "heimdall.sel");
      }
      Builder.CreateStore(Acc, StateVar);
    } else {
      // Unreachable terminators, and any other kind the eligibility check
      // didn't already exclude: leave untouched rather than guess.
      continue;
    }

    Term->eraseFromParent();
    BranchInst::Create(Dispatcher, BB);
  }

  return true;
}

} // namespace

PreservedAnalyses
heimdall::ControlFlowFlatteningPass::run(Function &F,
                                          FunctionAnalysisManager &FAM) {
  SkipReason Reason = findBailOutReason(F);
  if (Reason != SkipReason::None) {
    LLVM_DEBUG(dbgs() << "heimdall-cff: skipping '" << F.getName()
                       << "': " << skipReasonToString(Reason) << "\n");
    ++NumFunctionsSkipped;
    return PreservedAnalyses::all();
  }

  LLVM_DEBUG(dbgs() << "heimdall-cff: flattening '" << F.getName() << "'\n");
  bool Changed = flattenFunction(F);

  if (Changed) {
    if (verifyFunction(F, &errs()))
      report_fatal_error("heimdall-cff produced invalid IR for function '" +
                          F.getName() + "'");
    ++NumFunctionsFlattened;
    return PreservedAnalyses::none();
  }

  ++NumFunctionsSkipped;
  return PreservedAnalyses::all();
}
