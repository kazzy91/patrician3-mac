// Writes "addr\tsize\tname" for every function, plus thunk/external flags, to the path given as the first script arg.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class ExportFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        String out = getScriptArgs()[0];
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal()) continue;
                w.printf("%08X\t%d\t%s%s%n", f.getEntryPoint().getOffset(), f.getBody().getNumAddresses(),
                         f.getName(), f.isThunk() ? "\tthunk" : "");
            }
        }
        println("exported " + currentProgram.getFunctionManager().getFunctionCount() + " functions to " + out);
    }
}
