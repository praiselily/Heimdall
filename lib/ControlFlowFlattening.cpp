//===- ControlFlowFlattening.cpp - CFG flattening obfuscation pass -----===//
//
// Heimdall: software IP-protection obfuscation for LLVM.
//
// See DESIGN.md for the transform description and rationale.
//
//===----------------------------------------------------------------===//
#include "heimdall/ControlFlowFlattening.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
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

/// Human-readable reason a function was not flattened, for -debug-only and
/// (optionally) remarks.
enum class SkipReason {
  None,
  TooSmall,
  HasExceptionHandling,
  HasIndirectControlFlow,
  IrreducibleCFG,
  OptNone,
  Declaration,
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
  case SkipReason::IrreducibleCFG:
    return "function has an irreducible control-flow graph";
  case SkipReason::OptNone:
    return "function is marked optnone";
  case SkipReason::Declaration:
    return "function is a declaration";
  }
  llvm_unreachable("unhandled SkipReason");
}

/// Walks F's instructions looking for constructs we deliberately don't
/// flatten in v1. Returns the first disqualifying reason found, or
/// SkipReason::None if F is eligible.
SkipReason findBailOutReason(Function &F, LoopInfo &LI) {
  if (F.isDeclaration())
    return SkipReason::Declaration;
  if (F.hasFnAttribute(Attribute::OptimizeNone))
    return SkipReason::OptNone;
  if (F.size() < 2)
    return SkipReason::TooSmall;

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

  // Irreducibility check: every block that is a loop header should be
  // reachable only via back-edges recognized by LoopInfo. A cheap proxy for
  // "LoopInfo didn't fully capture this CFG's cycles" is: count edges into
  // each block from blocks LoopInfo considers inside some loop, and compare
  // against what a reducible CFG's natural loop nest implies. LLVM doesn't
  // expose a single boolean for this, so we use the standard trick: a CFG
  // is irreducible iff it contains a cycle that LoopInfo's dominator-based
  // analysis does not report as part of any natural loop. We approximate
  // this by checking, for every block B with an edge from a block D that B
  // dominates (i.e. a back-edge), that LI recognizes B as a loop header.
  // Any back-edge whose target isn't a recognized loop header indicates a
  // cycle outside LoopInfo's model, i.e. irreducibility.
  DominatorTree DT(F);
  for (BasicBlock &BB : F) {
    for (BasicBlock *Pred : predecessors(&BB)) {
      if (DT.dominates(&BB, Pred)) {
        // (Pred -> BB) is a back-edge.
        if (!LI.isLoopHeader(&BB))
          return SkipReason::IrreducibleCFG;
      }
    }
  }

  return SkipReason::None;
}

/// Demotes every value defined in `BB` and used outside `BB` (including by
/// PHI nodes) to a stack slot, so flattening can freely reorder/indirect
/// control flow without violating SSA dominance. Thin wrapper around LLVM's
/// own DemoteRegToStack, reused rather than reimplemented (see DESIGN.md).
void demoteCrossBlockValues(Function &F) {
  std::vector<Instruction *> WorkList;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      WorkList.push_back(&I);

  BasicBlock *AllocaBlock = &F.getEntryBlock();
  for (Instruction *I : WorkList) {
    if (I->getType()->isVoidTy())
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
      DemoteRegToStack(*I, /*VolatileLoads=*/false,
                        AllocaBlock->getTerminator());
  }

  // PHI nodes themselves must also be demoted: DemotePHIToStack replaces a
  // PHI with loads/stores through a stack slot.
  std::vector<PHINode *> Phis;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *PN = dyn_cast<PHINode>(&I))
        Phis.push_back(PN);
  for (PHINode *PN : Phis)
    DemotePHIToStack(PN, AllocaBlock->getTerminator());
}

/// Performs the actual flattening transform on an eligible function.
/// Returns true if the function's IR was modified.
bool flattenFunction(Function &F) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock &Entry = F.getEntryBlock();

  // Step 1: eliminate cross-block SSA values up front so every later step
  // only has to deal with straight-line, block-local instructions plus
  // loads/stores through allocas.
  demoteCrossBlockValues(F);

  // Step 2: collect all blocks except the entry block; the entry block
  // stays in place and simply jumps into the dispatcher at the end.
  std::vector<BasicBlock *> Blocks;
  for (BasicBlock &BB : F)
    if (&BB != &Entry)
      Blocks.push_back(&BB);

  if (Blocks.empty())
    return false; // Nothing to flatten (e.g. entry unconditionally returns).

  // Step 3: assign each collected block a distinct state ID.
  IntegerType *I32 = Type::getInt32Ty(Ctx);
  DenseMap<BasicBlock *, uint32_t> StateOf;
  for (uint32_t I = 0; I < Blocks.size(); ++I)
    StateOf[Blocks[I]] = I;

  // Step 4: create the state variable in the entry block and the dispatcher
  // block that switches on it.
  IRBuilder<> EntryBuilder(Entry.getTerminator());
  AllocaInst *StateVar =
      EntryBuilder.CreateAlloca(I32, nullptr, "heimdall.state");

  auto *Dispatcher = BasicBlock::Create(Ctx, "heimdall.dispatch", &F);
  IRBuilder<> DispatchBuilder(Dispatcher);
  LoadInst *StateLoad =
      DispatchBuilder.CreateLoad(I32, StateVar, "heimdall.state.load");

  // Default case: should be unreachable in correct executions, since every
  // store to StateVar writes a valid case value.
  auto *Unreachable = BasicBlock::Create(Ctx, "heimdall.unreachable", &F);
  IRBuilder<>(Unreachable).CreateUnreachable();

  SwitchInst *Switch =
      DispatchBuilder.CreateSwitch(StateLoad, Unreachable, Blocks.size());
  for (BasicBlock *BB : Blocks)
    Switch->addCase(ConstantInt::get(I32, StateOf[BB]), BB);

  // Step 5: point the entry block's terminator at the dispatcher instead of
  // its original successor(s), after recording that original target as the
  // first state to run.
  Instruction *EntryTerm = Entry.getTerminator();
  assert(!isa<ReturnInst>(EntryTerm) &&
         "entry-only function should have bailed out as too small");
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
    // Build a chain of selects: default first, then override per case.
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
    // Any other terminator on a block with successors (shouldn't occur
    // given the bail-out checks) — be conservative and refuse to flatten.
    Dispatcher->eraseFromParent();
    Unreachable->eraseFromParent();
    StateVar->eraseFromParent();
    return false;
  }
  EntryTerm->eraseFromParent();
  BranchInst::Create(Dispatcher, &Entry);

  // Step 6: rewrite every collected block's terminator the same way, then
  // redirect it to the dispatcher.
  for (BasicBlock *BB : Blocks) {
    Instruction *Term = BB->getTerminator();
    IRBuilder<> Builder(Term);

    if (auto *Ret = dyn_cast<ReturnInst>(Term)) {
      // Returns are left alone entirely — they don't feed the dispatcher.
      (void)Ret;
      continue;
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
    } else if (isa<UnreachableInst>(Term)) {
      continue; // Nothing to redirect.
    } else {
      // Conservative: leave any terminator kind we don't explicitly handle
      // untouched rather than guess. (Bail-out checks should have already
      // excluded invoke/indirectbr/callbr/exception-handling terminators.)
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
  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);

  SkipReason Reason = findBailOutReason(F, LI);
  if (Reason != SkipReason::None) {
    LLVM_DEBUG(dbgs() << "heimdall-cff: skipping '" << F.getName()
                       << "': " << skipReasonToString(Reason) << "\n");
    ++NumFunctionsSkipped;
    return PreservedAnalyses::all();
  }

  LLVM_DEBUG(dbgs() << "heimdall-cff: flattening '" << F.getName() << "'\n");
  bool Changed = flattenFunction(F);

  if (Changed) {
    if (verifyFunction(F, &errs())) {
      // Should never happen; flattenFunction is designed to preserve
      // well-formed IR. Treat as a hard bug rather than shipping broken
      // output silently.
      report_fatal_error("heimdall-cff produced invalid IR for function '" +
                          F.getName() + "' -- this is a bug in the pass");
    }
    ++NumFunctionsFlattened;
    return PreservedAnalyses::none();
  }

  ++NumFunctionsSkipped;
  return PreservedAnalyses::all();
}
