//===- ControlFlowFlattening.h - CFG flattening obfuscation pass -------===//
//
// Heimdall: software IP-protection obfuscation for LLVM.
//
//===----------------------------------------------------------------===//
#ifndef HEIMDALL_CONTROL_FLOW_FLATTENING_H
#define HEIMDALL_CONTROL_FLOW_FLATTENING_H

#include "llvm/IR/PassManager.h"

namespace heimdall {

/// Flattens a function's control-flow graph into a single dispatcher loop
/// driven by a state variable, so the function's original CFG shape is no
/// longer directly visible to static analysis or decompiler output.
///
/// See DESIGN.md for the full transform description, eligibility/bail-out
/// rules, and correctness strategy.
class ControlFlowFlatteningPass
    : public llvm::PassInfoMixin<ControlFlowFlatteningPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &FAM);

  // Opt-out of skipping on functions with "optnone" etc. is handled inside
  // run(); this pass does not require any particular analysis to be
  // preserved, so the default (invalidate everything on functions it
  // transforms) is correct and conservative.
  static bool isRequired() { return false; }
};

} // namespace heimdall

#endif // HEIMDALL_CONTROL_FLOW_FLATTENING_H
