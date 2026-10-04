//===- ControlFlowFlattening.h - CFG flattening obfuscation pass -------===//
//
// Heimdall: software IP-protection obfuscation for LLVM.
//
//===----------------------------------------------------------------===//
#ifndef HEIMDALL_CONTROL_FLOW_FLATTENING_H
#define HEIMDALL_CONTROL_FLOW_FLATTENING_H

#include "llvm/IR/PassManager.h"

namespace heimdall {

/// Flattens a function's control flow into a single dispatcher loop driven
/// by a state variable. See DESIGN.md for the transform and eligibility
/// rules.
class ControlFlowFlatteningPass
    : public llvm::PassInfoMixin<ControlFlowFlatteningPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &FAM);

  static bool isRequired() { return false; }
};

} // namespace heimdall

#endif // HEIMDALL_CONTROL_FLOW_FLATTENING_H
