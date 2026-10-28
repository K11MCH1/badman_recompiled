// @category PSPRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.PrintWriter;

/** Export only instructions Ghidra actually defined, in bounded contiguous runs. */
public class ExportBadmanCode extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Expected output CSV path");
        int runs = 0;
        long instructions = 0;
        long start = -1, end = -1;
        Instruction previous = null;
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            out.println("name,address,size");
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = it.next();
                long address = instruction.getAddress().getOffset();
                // This profile supports one verified ELF; import wrappers are generated separately.
                if (address < 0x08804000L || address >= 0x08917430L ||
                    (address >= 0x088E9A30L && address < 0x088E9F50L)) continue;
                boolean split = start >= 0 && (address != end ||
                    (end - start >= 0x1000 && previous.getDelaySlotDepth() == 0));
                if (split) {
                    out.printf("badman_code_%08X,0x%08X,0x%08X%n", start, start, end - start);
                    runs++;
                    start = -1;
                }
                if (start < 0) start = address;
                end = address + instruction.getLength();
                previous = instruction;
                instructions++;
            }
            if (start >= 0) {
                out.printf("badman_code_%08X,0x%08X,0x%08X%n", start, start, end - start);
                runs++;
            }
        }
        println("Exported " + instructions + " defined instructions in " + runs + " runs");
    }
}
