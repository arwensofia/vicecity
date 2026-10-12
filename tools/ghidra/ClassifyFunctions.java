// Per-function hardware/library fingerprint for runtime-boundary analysis.
// Writes <outdir>/classified.csv, calls.csv, strings.csv and hwsites.csv.
//
// Columns: hardware register blocks touched (resolved from lui/offset pairs),
// syscall numbers, VU0 macro (COP2) and MMI instruction counts, and labels from
// any marker strings referenced (Sony "PsIIlib*"/"libxxx:", RenderWare $Id, *.cpp).
// Usage (headless): -postScript ClassifyFunctions.java <outdir>
// @category ViceCity
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.*;

public class ClassifyFunctions extends GhidraScript {
    private static final Pattern PSII = Pattern.compile("PsII(lib[a-z0-9]+)");
    private static final Pattern LIBMSG = Pattern.compile("^#*\\s*(?:internel error in )?(lib[a-z0-9]+)(?:\\.a)?[:! ]");
    private static final Pattern RWID = Pattern.compile("//RenderWare/[^/]+/dev/rwsdk/(.+?)#");
    private static final Pattern CPP = Pattern.compile("([A-Za-z0-9_]+\\.(?:cpp|CPP|c))\\b");

    private Listing listing;
    private Memory mem;
    private PrintWriter strs, sites;
    private Address curIns;
    private Function cur;
    private Set<Address> seen = new HashSet<>();

    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : askString("Output", "directory");
        listing = currentProgram.getListing();
        mem = currentProgram.getMemory();
        int n = 0;
        try (PrintWriter out = new PrintWriter(outDir + "/classified.csv");
             PrintWriter calls = new PrintWriter(outDir + "/calls.csv");
             PrintWriter strings = new PrintWriter(outDir + "/strings.csv");
             PrintWriter hwsites = new PrintWriter(outDir + "/hwsites.csv")) {
            sites = hwsites;
            sites.println("function,instruction,hw_address,block,disasm");
            strs = strings;
            strs.println("function,string_addr,text");
            out.println("address,size,name,hw,syscalls,cop2,mmi,labels");
            calls.println("caller,callee");
            for (Function f : listing.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                analyze(f, out, calls);
                n++;
            }
        }
        println("ClassifyFunctions: " + n + " functions -> " + outDir);
    }

    private void analyze(Function f, PrintWriter out, PrintWriter calls) {
        cur = f;
        seen.clear();
        Map<String, Long> hi = new HashMap<>();   // keyed by normalized GPR name
        Set<String> hw = new TreeSet<>(), labels = new TreeSet<>(), sys = new TreeSet<>();
        Set<Address> callees = new LinkedHashSet<>();
        Long v1 = null;
        int cop2 = 0, mmi = 0;

        for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
            String m = ins.getMnemonicString().toLowerCase();
            if (m.startsWith("_")) m = m.substring(1);   // delay-slot marker
            curIns = ins.getAddress();

            if (m.equals("syscall")) sys.add(v1 == null ? "?" : Long.toString(v1));
            if (isCop2(m)) cop2++;
            if (isMmi(m)) mmi++;

            if (m.equals("lui") && ins.getNumOperands() == 2) {
                Register rt = firstRegister(ins, 0);
                Scalar s = ins.getScalar(1);
                if (rt != null && s != null) {
                    long v = s.getUnsignedValue();
                    hi.put(gpr(rt), v <= 0xffff ? v << 16 : v);
                }
            } else if (ins.getNumOperands() >= 2) {
                boolean addrForm = isAddrForm(m);
                // base+offset forms: "addiu rt, rs, imm" / "ori rt, rs, imm" / "lw rt, off(rs)"
                String base = null; Long off = null;
                for (int i = 1; i < ins.getNumOperands(); i++) {
                    for (Object o : ins.getOpObjects(i)) {
                        if (o instanceof Register && hi.containsKey(gpr((Register) o))) base = gpr((Register) o);
                        else if (o instanceof Scalar) off = ((Scalar) o).getSignedValue();
                    }
                }
                Long a = null;
                if (addrForm && base != null && off != null) {
                    a = (m.equals("ori") ? (hi.get(base) | (off & 0xffff)) : hi.get(base) + off) & 0xffffffffL;
                    classifyAddress(a, hw, labels);
                }
                // Destination register no longer holds the lui value, except that
                // "addiu/ori rt, rs, imm" now holds the full address for later "lw x, off(rt)".
                Register dst = firstRegister(ins, 0);
                if (dst != null && writesOp0(m)) {
                    if (a != null && (m.equals("addiu") || m.equals("ori") || m.equals("daddiu"))) hi.put(gpr(dst), a);
                    else hi.remove(gpr(dst));
                }
            }
            // track "addiu v1, zero, N" for PS2 syscall numbers
            if ((m.equals("addiu") || m.equals("li")) && firstRegister(ins, 0) != null
                    && gpr(firstRegister(ins, 0)).equals("v1")) {
                Scalar s = ins.getScalar(ins.getNumOperands() - 1);
                v1 = s == null ? null : s.getSignedValue();
            }
            for (Reference r : ins.getReferencesFrom()) {
                if (r.getReferenceType().isCall()) callees.add(r.getToAddress());
                else if (r.getReferenceType().isData()) labelString(r.getToAddress(), labels);
            }
        }
        for (Address c : callees) calls.println(f.getEntryPoint() + "," + c);
        out.printf("%s,%d,%s,%s,%s,%d,%d,%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
                   f.getName(true).replace(",", "_"), String.join(" ", hw), String.join(" ", sys),
                   cop2, mmi, String.join(" ", labels).replace(",", "_"));
    }

    // The R5900 module exposes GPRs both as "v0" and as the low-half alias "v0_lo".
    private static String gpr(Register r) {
        String n = r.getName();
        return n.endsWith("_lo") ? n.substring(0, n.length() - 3) : n;
    }

    private void classifyAddress(long a, Set<String> hw, Set<String> labels) {
        String blk = null;
        if (a >= 0x70000000L && a < 0x70004000L) blk = "SPR";
        else if (a < 0x20000000L || (a >= 0x80000000L && a < 0xc0000000L)) {
            long p = a & 0x1fffffffL;   // fold KSEG0/KSEG1 mirrors only
            if (p >= 0x10000000L && p < 0x10010000L) blk = eeBlock(p);
            else if (p >= 0x12000000L && p < 0x12002000L) blk = "GS_PRIV";
            else if (p >= 0x11000000L && p < 0x11010000L) blk = "VU_MEM";
        }
        if (blk == null) { labelString(toAddr(a), labels); return; }
        hw.add(blk);
        sites.printf("%s,%s,%08x,%s,%s%n", cur.getEntryPoint(), curIns, a, blk,
                     getInstructionAt(curIns).toString().replace(",", ";"));
    }

    private static String eeBlock(long p) {
        int blk = (int) ((p >> 12) & 0xf);
        switch (blk) {
            case 0: case 1: return "TIMER";
            case 2: return "IPU";
            case 3: return "GIF";
            case 4: case 5: return "VIF";
            case 6: case 7: return "FIFO";
            case 8: case 9: case 0xa: case 0xb: case 0xc: case 0xd: return "DMAC";
            case 0xe: return "DMAC_CTRL";
            case 0xf: return (p >= 0x1000f200L && p < 0x1000f300L) ? "SIF" : "INTC";
        }
        return "EE?";
    }

    private void labelString(Address a, Set<String> labels) {
        if (a == null || !mem.contains(a)) return;
        String s = readCString(a);
        if (s == null) return;
        if (seen.add(a))
            strs.printf("%s,%s,\"%s\"%n", cur.getEntryPoint(), a, s.replace("\"", "'").replace("\n", "\\n"));
        Matcher m;
        if ((m = PSII.matcher(s)).find()) labels.add("sony:" + m.group(1));
        else if ((m = LIBMSG.matcher(s)).find()) labels.add("sony:" + m.group(1));
        else if ((m = RWID.matcher(s)).find()) labels.add("rw:" + m.group(1));
        else if ((m = CPP.matcher(s)).find()) labels.add("src:" + m.group(1));
    }

    private String readCString(Address a) {
        byte[] buf = new byte[160];
        int n;
        try { n = mem.getBytes(a, buf); } catch (Exception e) { return null; }
        int len = 0;
        while (len < n && buf[len] != 0) {
            int c = buf[len] & 0xff;
            if (c != '\t' && c != '\n' && (c < 0x20 || c > 0x7e)) return null;
            len++;
        }
        return len >= 4 ? new String(buf, 0, len) : null;
    }

    // getRegister() returns null for some 64/128-bit forms; operand objects are reliable.
    private static Register firstRegister(Instruction ins, int op) {
        for (Object o : ins.getOpObjects(op)) if (o instanceof Register) return (Register) o;
        return null;
    }

    private static final Set<String> ADDR_FORMS = new HashSet<>(Arrays.asList(
        "addiu", "daddiu", "ori", "lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lq", "lwl", "lwr",
        "ldl", "ldr", "lwc1", "lqc2", "sb", "sh", "sw", "sd", "sq", "swl", "swr", "sdl", "sdr",
        "swc1", "sqc2"));

    private static boolean isAddrForm(String m) { return ADDR_FORMS.contains(m); }

    private static boolean writesOp0(String m) {
        if (m.startsWith("b") || m.startsWith("j") || m.equals("syscall") || m.equals("sync")) return false;
        switch (m) {
            case "sb": case "sh": case "sw": case "sd": case "sq": case "swl": case "swr":
            case "sdl": case "sdr": case "swc1": case "sqc2": case "sdc1": case "cache": case "pref":
                return false;
        }
        return true;
    }

    private static boolean isCop2(String m) {
        return m.startsWith("v") || m.equals("qmfc2") || m.equals("qmtc2") || m.equals("lqc2")
            || m.equals("sqc2") || m.equals("cfc2") || m.equals("ctc2") || m.startsWith("bc2");
    }

    private static boolean isMmi(String m) {
        // lq/sq excluded: plain 128-bit loads/stores the compiler emits for vector copies
        return (m.startsWith("p") && !m.equals("pref")) || m.startsWith("qf") || m.equals("mfhi1") || m.equals("mflo1")
            || m.equals("mthi1") || m.equals("mtlo1") || m.startsWith("mult1") || m.startsWith("div1")
            || m.startsWith("madd1") || m.equals("mtsab") || m.equals("mfsa") || m.equals("mtsa");
    }
}
