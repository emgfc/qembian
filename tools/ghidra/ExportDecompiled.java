// Headless Ghidra post-script: write decompiled C for every function to the file given as
// the first script argument.   (syborg-pg; used by tools/ghidra/decompile.sh)
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportDecompiled extends GhidraScript {
	@Override
	public void run() throws Exception {
		String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "decompiled.c";
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			int n = 0;
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				DecompileResults r = ifc.decompileFunction(f, 60, monitor);
				w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
				if (r != null && r.decompileCompleted())
					w.println(r.getDecompiledFunction().getC());
				else
					w.println("// decompilation failed");
				n++;
			}
			println("decompiled " + n + " functions to " + out);
		}
	}
}
