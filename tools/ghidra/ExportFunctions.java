// Writes every function to CSV: address, size, namespace, name, plate comment.
// Usage (headless): -postScript ExportFunctions.java <out.csv>
// @category ViceCity
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : askString("Output", "CSV path");
        Listing listing = currentProgram.getListing();
        int count = 0;
        try (PrintWriter out = new PrintWriter(path)) {
            out.println("address,size,namespace,name,comment");
            for (Function f : listing.getFunctions(true)) {
                String ns = f.getParentNamespace().isGlobal() ? "" : f.getParentNamespace().getName();
                String c = f.getComment() == null ? "" : f.getComment().replace("\n", " | ").replace("\"", "'");
                out.printf("%s,%d,%s,%s,\"%s\"%n", f.getEntryPoint(), f.getBody().getNumAddresses(),
                           ns, f.getName(), c);
                count++;
            }
        }
        println("ExportFunctions: wrote " + count + " functions to " + path);
    }
}
