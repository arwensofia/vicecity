// Creates functions at code addresses that are only reached through pointers.
//
// Ghidra's analysis disassembles some code that is never called directly (thread entries,
// interrupt handlers, RenderWare plugin constructors, static-destructor callbacks, SIF RPC
// end callbacks ...) but leaves it outside any function. Such code is missing from the
// function list, so the recompiler never emits it and an indirect call to it fails.
//
// A candidate is an address that
//   - is the target of a non-flow (data/pointer) reference from an instruction that lies in
//     a function (typically a lui/addiu pair materialising the address),
//   - holds an instruction, and
//   - is not inside any existing function.
// Every candidate becomes a function with a plate comment naming the referencing sites.
// Addresses referenced only from initialised data are listed (kind "data") but never
// created: most of them are entries of switch jump tables whose owning function Ghidra
// did not bound, and turning those labels into functions would split the switch.
//
// Usage (headless):
//   -postScript CreatePointerFunctions.java [dry] [<out.csv>] [code <start> <end>]...
//   dry                only report candidates (run with -readOnly)
//   out.csv            write address,kind,referenced_from list
//   code <start> <end> restrict candidates to these hex ranges (half-open). For SLUS_205.52
//                      use the recompiler's: code 100000 42de78 code 4a7880 4a9110
//                      (keeps the kernel patches and RW data in .data out)
// @category ViceCity
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class CreatePointerFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean dry = args.length > 0 && args[0].equals("dry");
        String csv = null;
        AddressSet ranges = null;
        for (int i = 0; i < args.length; i++) {
            if (args[i].endsWith(".csv")) csv = args[i];
            if (args[i].equals("code") && i + 2 < args.length) {
                if (ranges == null) ranges = new AddressSet();
                ranges.add(toAddr(Long.parseLong(args[i + 1], 16)), toAddr(Long.parseLong(args[i + 2], 16) - 1));
                i += 2;
            }
        }

        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        Map<Address, List<Address>> cands = new TreeMap<>();

        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isExecute() || !b.isInitialized()) continue;
            AddressSetView set = new AddressSet(b.getStart(), b.getEnd());
            if (ranges != null) set = set.intersect(ranges);
            InstructionIterator it = listing.getInstructions(set, true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                Address a = ins.getAddress();
                if (fm.getFunctionContaining(a) != null) continue;
                if (a.getOffset() % 4 != 0) continue;
                List<Address> from = new ArrayList<>();
                for (Reference r : rm.getReferencesTo(a)) {
                    if (r.getReferenceType().isFlow()) continue;
                    Address src = r.getFromAddress();
                    Instruction si = listing.getInstructionAt(src);
                    if (si != null && fm.getFunctionContaining(src) == null) continue;
                    from.add(src);
                }
                if (!from.isEmpty()) cands.put(a, from);
            }
        }

        int created = 0, code = 0;
        PrintWriter out = csv != null ? new PrintWriter(csv) : null;
        if (out != null) out.println("address,kind,referenced_from");
        for (Map.Entry<Address, List<Address>> e : cands.entrySet()) {
            Address a = e.getKey();
            StringBuilder sb = new StringBuilder();
            boolean fromCode = false;
            for (Address s : e.getValue()) {
                Function sf = fm.getFunctionContaining(s);
                if (sf == null && s.getOffset() >= 0x20000000L) continue;   // uncached/mirror views
                if (sb.length() > 0) sb.append(' ');
                sb.append(s).append(sf != null ? "(" + sf.getName() + ")" : "(data)");
                fromCode |= sf != null;
            }
            if (out != null) out.printf("%s,%s,\"%s\"%n", a, fromCode ? "code" : "data", sb);
            if (!fromCode) continue;
            code++;
            if (dry) { println("candidate " + a + " <- " + sb); continue; }
            Function f = createFunction(a, null);
            if (f == null) { printerr("could not create function at " + a); continue; }
            String plate = "pointer-only entry (created by CreatePointerFunctions): address taken at " + sb;
            f.setComment(f.getComment() == null ? plate : f.getComment() + "\n" + plate);
            created++;
        }
        if (out != null) out.close();
        println("CreatePointerFunctions: " + code + " code-referenced candidates (" + (cands.size() - code)
                + " data-only, not created), " + created + " functions created");
    }
}
