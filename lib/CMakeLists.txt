add_llvm_pass_plugin(HeimdallCFF
  ControlFlowFlattening.cpp
  PassPlugin.cpp

  DEPENDS
  intrinsics_gen
)

target_include_directories(HeimdallCFF PRIVATE ${CMAKE_SOURCE_DIR}/include)
