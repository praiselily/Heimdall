# Ghidra headless post-script (Jython, run under Ghidra's own interpreter --
# NOT CPython). Decompiles every defined function in the current program and
# writes {function_name: pseudocode} to the JSON file path given as the
# script's first argument.
#
# Invoked via:
#   analyzeHeadless <project_dir> <project_name> -import <binary> \
#       -postScript DecompileFunctions.py <output.json> \
#       -scriptPath benchmark/ghidra_scripts -deleteProject
#
# See benchmark/README.md for the full headless invocation this project
# uses.
import json

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


def run():
    args = getScriptArgs()
    if len(args) < 1:
        print("usage: DecompileFunctions.py <output.json>")
        return
    out_path = args[0]

    decompiler = DecompInterface()
    decompiler.openProgram(currentProgram)
    monitor = ConsoleTaskMonitor()

    results = {}
    fm = currentProgram.getFunctionManager()
    for func in fm.getFunctions(True):
        if func.isThunk() or func.isExternal():
            continue
        decomp_result = decompiler.decompileFunction(func, 60, monitor)
        if decomp_result is None or not decomp_result.decompileCompleted():
            results[func.getName()] = None
            continue
        decompiled_fn = decomp_result.getDecompiledFunction()
        results[func.getName()] = decompiled_fn.getC()

    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)

    print("Wrote {} decompiled function(s) to {}".format(len(results), out_path))


run()
