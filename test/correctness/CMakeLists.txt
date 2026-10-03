# Wires run_correctness.py into `ctest`, pointing it at the clang used to
# configure this build and the just-built HeimdallCFF plugin.

find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  add_test(
    NAME heimdall-correctness
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/run_correctness.py
      --clang ${LLVM_TOOLS_BINARY_DIR}/clang
      --plugin $<TARGET_FILE:HeimdallCFF>
      --programs-dir ${CMAKE_CURRENT_SOURCE_DIR}/programs
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/correctness-work
  )
else()
  message(WARNING "Python3 interpreter not found; skipping heimdall-correctness ctest registration. Run test/correctness/run_correctness.py manually.")
endif()
