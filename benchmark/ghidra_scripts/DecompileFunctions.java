// Ghidra headless post-script. Decompiles every defined function in the
// current program and writes {function_name: pseudocode} to the JSON file
// path given as the script's first argument.
//
// Invoked via:
//   analyzeHeadless <project_dir> <project_name> -import <binary> \
//       -postScript DecompileFunctions.java <output.json> \
//       -scriptPath benchmark/ghidra_scripts -deleteProject
//
// Java, not Python: Ghidra 11.3+ needs PyGhidra installed to run a .py
// postScript at all. See benchmark/README.md.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.PrintWriter;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

public class DecompileFunctions extends GhidraScript {

    private static String jsonEscape(String s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':
                    out.append("\\\"");
                    break;
                case '\\':
                    out.append("\\\\");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
            }
        }
        return out.toString();
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: DecompileFunctions.java <output.json>");
            return;
        }
        String outPath = args[0];

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        List<String> entries = new ArrayList<>();
        for (Function func : currentProgram.getFunctionManager().getFunctions(true)) {
            if (func.isThunk() || func.isExternal()) {
                continue;
            }
            String name = func.getName();
            String code;
            DecompileResults result = decompiler.decompileFunction(func, 60, monitor);
            if (result == null || !result.decompileCompleted()) {
                code = null;
            } else {
                code = result.getDecompiledFunction().getC();
            }
            String value = (code == null) ? "null" : ("\"" + jsonEscape(code) + "\"");
            entries.add("  \"" + jsonEscape(name) + "\": " + value);
        }
        decompiler.dispose();

        try (PrintWriter writer = new PrintWriter(Paths.get(outPath).toFile(), "UTF-8")) {
            writer.println("{");
            writer.println(String.join(",\n", entries));
            writer.println("}");
        }

        println("Wrote " + entries.size() + " decompiled function(s) to " + outPath);
    }
}
