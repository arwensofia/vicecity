// Applies evidence-backed library identifications from a CSV to the program.
//
// CSV columns: address,namespace,name,kind,confidence,evidence
//   kind=func   rename the function at address (creating it if missing) to
//               <namespace>::<name>; evidence goes into the plate comment
//   kind=label  add a label <namespace>::<name> (e.g. unreferenced syscall stubs)
//   kind=guess  no rename; add a "guess: <namespace>::<name> ..." plate comment
//   kind=create create a function at address if none exists (name/namespace ignored)
//   kind=note   append evidence to the plate comment only
// Optional trailing args "ctors <start> <end>" create functions for every non-zero
// pointer in a constructor table [start, end).
//
// Usage (headless, without -readOnly):
//   -postScript ApplyLibraryNames.java tools/library_names.csv [ctors 4a9110 4a923c]
// @category ViceCity
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.*;

public class ApplyLibraryNames extends GhidraScript {
    private int renamed, labelled, guessed, created, noted;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : askString("Input", "CSV path");
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line = in.readLine();   // header
            while ((line = in.readLine()) != null) {
                if (line.isBlank() || line.startsWith("#")) continue;
                List<String> c = splitCsv(line);
                if (c.size() < 6) { printerr("bad row: " + line); continue; }
                apply(toAddr(Long.parseLong(c.get(0), 16)), c.get(1), c.get(2), c.get(3), c.get(4), c.get(5));
            }
        }
        if (args.length >= 4 && args[1].equals("ctors")) {
            long start = Long.parseLong(args[2], 16), end = Long.parseLong(args[3], 16);
            for (long a = start; a < end; a += 4) {
                long p = getInt(toAddr(a)) & 0xffffffffL;
                if (p != 0) ensureFunction(toAddr(p), "static constructor (table entry at " + Long.toHexString(a) + ")");
            }
        }
        println(String.format("ApplyLibraryNames: renamed %d, labels %d, guesses %d, created %d, notes %d",
                              renamed, labelled, guessed, created, noted));
    }

    private void apply(Address a, String ns, String name, String kind, String conf, String ev) throws Exception {
        String tag = "id(" + conf + "): " + ev;
        switch (kind) {
            case "func": {
                Function f = ensureFunction(a, null);
                if (f == null) return;
                Namespace n = namespace(ns);
                if (!f.getName().equals(name) || !f.getParentNamespace().equals(n)) {
                    f.setParentNamespace(n);
                    f.setName(name, SourceType.USER_DEFINED);
                    renamed++;
                }
                addPlate(f, tag);
                break;
            }
            case "label": {
                Symbol s = getSymbolAt(a);
                if (s == null || !s.getName().equals(name)) {
                    createLabel(a, name, namespace(ns), true, SourceType.USER_DEFINED);
                    labelled++;
                }
                setPlateAt(a, tag);
                break;
            }
            case "guess": {
                String g = "guess: " + (ns.isEmpty() ? "" : ns + "::") + name + " (" + conf + "): " + ev;
                Function f = getFunctionAt(a);
                if (f != null) addPlate(f, g); else setPlateAt(a, g);
                guessed++;
                break;
            }
            case "create":
                ensureFunction(a, ev.isEmpty() ? null : ev);
                break;
            case "note": {
                Function f = getFunctionAt(a);
                if (f != null) addPlate(f, ev); else setPlateAt(a, ev);
                noted++;
                break;
            }
            default:
                printerr("unknown kind " + kind + " at " + a);
        }
    }

    private Function ensureFunction(Address a, String plate) throws Exception {
        Function f = getFunctionAt(a);
        if (f != null) return f;
        if (getInstructionAt(a) == null) {
            clearListing(a, a.add(3));
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
        }
        f = createFunction(a, null);
        if (f == null) { printerr("could not create function at " + a); return null; }
        created++;
        if (plate != null) addPlate(f, plate);
        return f;
    }

    private Namespace namespace(String ns) throws Exception {
        if (ns == null || ns.isEmpty()) return currentProgram.getGlobalNamespace();
        SymbolTable st = currentProgram.getSymbolTable();
        Namespace n = st.getNamespace(ns, currentProgram.getGlobalNamespace());
        return n != null ? n : st.createNameSpace(null, ns, SourceType.USER_DEFINED);
    }

    private void addPlate(Function f, String text) {
        String p = f.getComment();
        if (p != null && p.contains(text)) return;
        f.setComment(p == null ? text : p + "\n" + text);
    }

    private void setPlateAt(Address a, String text) {
        String p = getPlateComment(a);
        if (p != null && p.contains(text)) return;
        setPlateComment(a, p == null ? text : p + "\n" + text);
    }

    private static List<String> splitCsv(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder sb = new StringBuilder();
        boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char ch = line.charAt(i);
            if (q) {
                if (ch == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') { sb.append('"'); i++; }
                else if (ch == '"') q = false;
                else sb.append(ch);
            } else if (ch == '"') q = true;
            else if (ch == ',') { out.add(sb.toString()); sb.setLength(0); }
            else sb.append(ch);
        }
        out.add(sb.toString());
        return out;
    }
}
