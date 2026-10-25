// Prints N qwords at each ADDR:N argument (vtable slots, pointer tables) with the function each points into.
//   analyzeHeadless <proj> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript ReadQwords.java <out.txt> 0x538ebc0:4 ...
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.reloc.Relocation;
import java.util.List;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ReadQwords extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                String[] a = args[i].split(":");
                long base = Long.parseUnsignedLong(a[0].replace("0x", ""), 16);
                int n = a.length > 1 ? Integer.parseInt(a[1]) : 1;
                for (int k = 0; k < n; ++k) {
                    Address ad = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(base + 8L * k);
                    long v = currentProgram.getMemory().getLong(ad);
                    Address t = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
                    Function f = getFunctionContaining(t);
                    List<Relocation> rl = currentProgram.getRelocationTable().getRelocations(ad);
                    if (rl != null && !rl.isEmpty()) {
                        Relocation r = rl.get(0);
                        out.printf("  reloc type %d values %s%n", r.getType(), java.util.Arrays.toString(r.getValues()));
                    }
                    out.printf("%s (+0x%x) = 0x%x %s%n", ad, 8L * k, v, f == null ? "" : f.getName());
                }
            }
        }
    }
}
