//===- PassPlugin.cpp - New-PM plugin registration for Heimdall --------===//
//
// Registers heimdall::ControlFlowFlatteningPass under the name
// "heimdall-cff" so it can be invoked as:
//
//   opt -load-pass-plugin=libHeimdallCFF.so -passes=heimdall-cff ...
//   clang -fpass-plugin=libHeimdallCFF.so -mllvm -passes=heimdall-cff ...
//
//===----------------------------------------------------------------===//
#include "heimdall/ControlFlowFlattening.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {

bool parseHeimdallPipeline(StringRef Name, FunctionPassManager &FPM,
                            ArrayRef<PassBuilder::PipelineElement>) {
  if (Name != "heimdall-cff")
    return false;
  FPM.addPass(heimdall::ControlFlowFlatteningPass());
  return true;
}

void registerHeimdallCallbacks(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
        return parseHeimdallPipeline(Name, FPM, InnerPipeline);
      });
}

} // namespace

PassPluginLibraryInfo getHeimdallPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "HeimdallCFF", LLVM_VERSION_STRING,
          registerHeimdallCallbacks};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getHeimdallPluginInfo();
}
