import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class ExportC extends GhidraScript {
    public void run() throws Exception {
        String out = System.getenv("IPL_C_OUT");
        if (out == null) out = "/workspace/files/tools/ipl_decompiled.c";
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        PrintWriter fh = new PrintWriter(new FileWriter(out));
        fh.println("/* IPL decompiled by Ghidra 12.1.2, base 0xa0000000, ARM:LE:32:v7 */");
        fh.println();
        int n=0, fail=0;
        for (Function func : fm.getFunctions(true)) {
            DecompileResults res = di.decompileFunction(func, 90, monitor);
            if (res != null && res.decompileCompleted()) {
                fh.println("/* " + func.getName() + " @ " + func.getEntryPoint() + " */");
                fh.println(res.getDecompiledFunction().getC());
                fh.println();
                n++;
            } else {
                fh.println("// DECOMP FAILED: " + func.getName() + " @ " + func.getEntryPoint());
                fail++;
            }
        }
        fh.close();
        println("ExportC: wrote " + n + " functions (" + fail + " failed) -> " + out);
    }
}
