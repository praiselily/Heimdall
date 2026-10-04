//===- ControlFlowFlattening.cpp - CFG flattening obfuscation pass -----===//
//
// Heimdall: software IP-protection obfuscation for LLVM.
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

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#define DEBUG_TYPE "heimdall-cff"

using namespace llvm;

STATISTIC(NumFunctionsFlattened, "Number of functions flattened");
STATISTIC(NumFunctionsSkipped, "Number of functions skipped (ineligible)");

namespace {

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

SkipReason findBailOutReason(Function &F) {
  if (F.isDeclaration())
    return SkipReason::Declaration;
  if (F.hasFnAttribute(Attribute::OptimizeNone))
    return SkipReason::OptNone;
  if (F.size() < 2)
    return SkipReason::TooSmall;

  // Entry can end in a plain ret even when F has >= 2 blocks (dead code
  // elsewhere keeps the count up). Catch that here, before any mutation.
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

// Demotes every value defined in one block and used in another to a stack
// slot via DemoteRegToStack/DemotePHIToStack, so the CFG rewrite below
// doesn't have to preserve SSA dominance across the new edges.
void demoteCrossBlockValues(Function &F) {
  std::vector<Instruction *> WorkList;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      WorkList.push_back(&I);

  BasicBlock::iterator AllocaInsertPt = F.getEntryBlock().getFirstInsertionPt();
  for (Instruction *I : WorkList) {
    if (I->getType()->isVoidTy())
      continue;
    // Existing allocas already dominate everything and don't need this;
    // demoting one would replace its direct uses with a loaded copy of the
    // pointer, which breaks llvm.lifetime.start/end and dbg.declare.
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

bool flattenFunction(Function &F) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock &Entry = F.getEntryBlock();

  demoteCrossBlockValues(F);

  std::vector<BasicBlock *> Blocks;
  for (BasicBlock &BB : F)
    if (&BB != &Entry)
      Blocks.push_back(&BB);

  if (Blocks.empty())
    return false;

  // Shuffle the state IDs rather than handing them out in block order.
  // Block order here is still close to source order, so a sequential
  // assignment would let the constants themselves leak the original
  // layout even with the real edges gone. Seeded on the function name so
  // the mapping is stable across rebuilds.
  IntegerType *I32 = Type::getInt32Ty(Ctx);
  std::vector<uint32_t> StateIds(Blocks.size());
  std::iota(StateIds.begin(), StateIds.end(), 0);
  std::seed_seq Seed{std::hash<std::string>{}(F.getName().str()),
                      static_cast<size_t>(Blocks.size())};
  std::mt19937 RNG(Seed);
  std::shuffle(StateIds.begin(), StateIds.end(), RNG);

  DenseMap<BasicBlock *, uint32_t> StateOf;
  for (uint32_t I = 0; I < Blocks.size(); ++I)
    StateOf[Blocks[I]] = StateIds[I];

  // No names on any of this -- it should look like every other anonymous
  // compiler temporary, not carry a signature into IR or bitcode output.
  IRBuilder<> EntryBuilder(Entry.getTerminator());
  AllocaInst *StateVar = EntryBuilder.CreateAlloca(I32);

  auto *Dispatcher = BasicBlock::Create(Ctx, "", &F);
  IRBuilder<> DispatchBuilder(Dispatcher);
  LoadInst *StateLoad = DispatchBuilder.CreateLoad(I32, StateVar);

  auto *Unreachable = BasicBlock::Create(Ctx, "", &F);
  IRBuilder<>(Unreachable).CreateUnreachable();

  SwitchInst *Switch =
      DispatchBuilder.CreateSwitch(StateLoad, Unreachable, Blocks.size());
  for (BasicBlock *BB : Blocks)
    Switch->addCase(ConstantInt::get(I32, StateOf[BB]), BB);

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
      Value *Sel = EntryBuilder.CreateSelect(Cond, TrueState, FalseState);
      EntryBuilder.CreateStore(Sel, StateVar);
    }
  } else if (auto *Sw = dyn_cast<SwitchInst>(EntryTerm)) {
    Value *Acc = ConstantInt::get(I32, StateOf[Sw->getDefaultDest()]);
    for (auto Case : Sw->cases()) {
      Value *CaseMatches = EntryBuilder.CreateICmpEQ(Sw->getCondition(),
                                                       Case.getCaseValue());
      Value *CaseState =
          ConstantInt::get(I32, StateOf[Case.getCaseSuccessor()]);
      Acc = EntryBuilder.CreateSelect(CaseMatches, CaseState, Acc);
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

  for (BasicBlock *BB : Blocks) {
    Instruction *Term = BB->getTerminator();
    IRBuilder<> Builder(Term);

    if (isa<ReturnInst>(Term))
      continue;

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
        Value *Sel = Builder.CreateSelect(Cond, TrueState, FalseState);
        Builder.CreateStore(Sel, StateVar);
      }
    } else if (auto *Sw = dyn_cast<SwitchInst>(Term)) {
      Value *Acc = ConstantInt::get(I32, StateOf[Sw->getDefaultDest()]);
      for (auto Case : Sw->cases()) {
        Value *CaseMatches =
            Builder.CreateICmpEQ(Sw->getCondition(), Case.getCaseValue());
        Value *CaseState =
            ConstantInt::get(I32, StateOf[Case.getCaseSuccessor()]);
        Acc = Builder.CreateSelect(CaseMatches, CaseState, Acc);
      }
      Builder.CreateStore(Acc, StateVar);
    } else {
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
