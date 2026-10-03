//===- PassPlugin.cpp - New-PM plugin registration for Heimdall --------===//
//
// Registers heimdall::ControlFlowFlatteningPass under the name
// "heimdall-cff" so it can be invoked as:
//
//   opt -load-pass-plugin=libHeimdallCFF.so -passes=heimdall-cff input.ll -S
//
// See README.md for the full clang + opt pipeline.
//
//===----------------------------------------------------------------===//
#include "heimdall/ControlFlowFlattening.h"

#include "llvm/Passes/PassBuilder.h"

// PassPlugin.h moved from llvm/Passes/ to llvm/Plugins/ in LLVM 21.
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif

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
