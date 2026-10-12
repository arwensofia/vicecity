// Uses leftover debug strings to label functions.
//  - "<message>, <Function>, <File>.cpp" -> renames the referencing function to
//    <Function> in a namespace named after the source file.
//  - "...<File>.cpp..." / RenderWare "$Id: //RenderWare/.../<file>.c#n $" -> adds a
//    "source:" plate comment to every function referencing the string.
// @category ViceCity
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.regex.*;

public class NameFromAsserts extends GhidraScript {
    private static final Pattern WITH_FUNC =
        Pattern.compile(",\\s*([A-Za-z_][A-Za-z0-9_:~]*),\\s*([A-Za-z0-9_]+)\\.(cpp|c)\\s*$");
    private static final Pattern SOURCE =
        Pattern.compile("([A-Za-z0-9_]+\\.(?:cpp|CPP|c))\\b");

    @Override
    public void run() throws Exception {
        int renamed = 0, tagged = 0;
        SymbolTable symtab = currentProgram.getSymbolTable();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!(d.getValue() instanceof String)) continue;
            String s = (String) d.getValue();
            Matcher src = SOURCE.matcher(s);
            if (!src.find()) continue;
            Matcher fm = WITH_FUNC.matcher(s);

            for (Reference ref : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(ref.getFromAddress());
                if (f == null) continue;
                String plate = f.getComment();
                String tag = "source: " + src.group(1);
                if (plate == null || !plate.contains(tag)) {
                    f.setComment(plate == null ? tag : plate + "\n" + tag);
                    tagged++;
                }
                if (fm.find(0) && f.getSymbol().getSource() == SourceType.DEFAULT) {
                    Namespace ns = getOrCreateNamespace(symtab, fm.group(2));
                    f.setParentNamespace(ns);
                    f.setName(fm.group(1), SourceType.ANALYSIS);
                    println("named " + f.getEntryPoint() + " " + ns.getName() + "::" + fm.group(1));
                    renamed++;
                }
            }
        }
        println("NameFromAsserts: renamed " + renamed + ", tagged " + tagged + " functions");
    }

    private Namespace getOrCreateNamespace(SymbolTable symtab, String name) throws Exception {
        Namespace ns = symtab.getNamespace(name, currentProgram.getGlobalNamespace());
        return ns != null ? ns : symtab.createNameSpace(null, name, SourceType.ANALYSIS);
    }
}
