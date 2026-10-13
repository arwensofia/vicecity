// Dumps the listing (functions, instructions, defined data) of an address range to a
// text file for grepping outside Ghidra. Each instruction line carries its references
// (calls, data refs) and, for data refs to C strings, the quoted string.
// Usage (headless): -postScript DumpListing.java <out.txt> [start end]   (hex addresses)
// @category ViceCity
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class DumpListing extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : askString("Output", "text path");
        AddressSetView set = currentProgram.getMemory().getLoadedAndInitializedAddressSet();
        if (args.length >= 3) set = set.intersect(new AddressSet(toAddr(Long.parseLong(args[1], 16)),
                                                                  toAddr(Long.parseLong(args[2], 16))));
        Listing listing = currentProgram.getListing();
        long n = 0;
        try (PrintWriter out = new PrintWriter(path)) {
            for (CodeUnit cu : listing.getCodeUnits(set, true)) {
                if (monitor.isCancelled()) break;
                Address a = cu.getAddress();
                Function f = listing.getFunctionAt(a);
                if (f != null) {
                    out.printf("%n==== FUNC %s %s size=%d%n", a, f.getName(true), f.getBody().getNumAddresses());
                    if (f.getComment() != null) out.println("     ; " + f.getComment().replace("\n", " | "));
                }
                if (cu instanceof Instruction) {
                    Instruction ins = (Instruction) cu;
                    StringBuilder sb = new StringBuilder();
                    for (Reference r : ins.getReferencesFrom()) {
                        if (r.getReferenceType().isFlow() && !r.getReferenceType().isCall()) continue;
                        sb.append(" ").append(r.getReferenceType().isCall() ? "CALL " : "-> ").append(r.getToAddress());
                        Function cf = listing.getFunctionAt(r.getToAddress());
                        if (cf != null && r.getReferenceType().isCall()) sb.append(" ").append(cf.getName(true));
                        String s = cstr(r.getToAddress());
                        if (s != null) sb.append(" \"").append(s).append("\"");
                    }
                    out.printf("%s  %s%s%n", a, ins.toString(), sb.length() > 0 ? "   ;" + sb : "");
                } else if (cu instanceof Data) {
                    Data d = (Data) cu;
                    if (!d.isDefined()) continue;
                    String v = d.getDefaultValueRepresentation();
                    if (v.length() > 100) v = v.substring(0, 100);
                    out.printf("%s  .%s %s%n", a, d.getDataType().getName(), v);
                }
                n++;
            }
        }
        println("DumpListing: " + n + " code units -> " + path);
    }

    private String cstr(Address a) {
        byte[] buf = new byte[96];
        int n;
        try { n = currentProgram.getMemory().getBytes(a, buf); } catch (Exception e) { return null; }
        int len = 0;
        while (len < n && buf[len] != 0) {
            int c = buf[len] & 0xff;
            if (c != '\t' && c != '\n' && (c < 0x20 || c > 0x7e)) return null;
            len++;
        }
        return len >= 4 ? new String(buf, 0, len).replace("\n", "\\n") : null;
    }
}
