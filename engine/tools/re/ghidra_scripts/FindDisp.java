// Finds functions whose instructions match ALL given regexes (on the instruction text).
//   analyzeHeadless <proj> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript FindDisp.java <out.txt> <regex>...
// e.g. regexes "\+ 0x3c0\]" and "MOV .*\[.*\+ 0x60\],": who loads the NetAI manipulator and stores +0x60.
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class FindDisp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int n = args.length - 1;
        Pattern[] p = new Pattern[n];
        for (int i = 0; i < n; ++i) {
            p[i] = Pattern.compile(args[i + 1]);
        }
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
            while (fi.hasNext() && !monitor.isCancelled()) {
                Function f = fi.next();
                String[] first = new String[n];
                InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (ii.hasNext()) {
                    Instruction in = ii.next();
                    String s = in.toString();
                    for (int i = 0; i < n; ++i) {
                        if (first[i] == null && p[i].matcher(s).find()) {
                            first[i] = in.getAddress() + " " + s;
                        }
                    }
                }
                boolean all = true;
                for (int i = 0; i < n; ++i) {
                    all &= first[i] != null;
                }
                if (all) {
                    out.printf("%s %s size %d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
                    for (int i = 0; i < n; ++i) {
                        out.printf("    %s%n", first[i]);
                    }
                }
            }
        }
    }
}
