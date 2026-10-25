// Dumps instructions with raw bytes for [start,end) (guest hex offsets) to a file.
//   -postScript DumpInsns.java <out.txt> <start> <end>
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpInsns extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        var sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address s = sp.getAddress(Long.parseUnsignedLong(a[1].replace("0x", ""), 16));
        Address e = sp.getAddress(Long.parseUnsignedLong(a[2].replace("0x", ""), 16));
        try (PrintWriter out = new PrintWriter(new FileWriter(a[0]))) {
            for (Instruction i = getInstructionAt(s); i != null && i.getAddress().compareTo(e) < 0; i = i.getNext()) {
                StringBuilder b = new StringBuilder();
                for (byte x : i.getBytes()) b.append(String.format("%02x ", x & 0xff));
                out.printf("%s  %-30s %s%n", i.getAddress(), b, i);
            }
        }
    }
}
