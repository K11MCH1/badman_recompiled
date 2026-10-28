// @category PSPRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

/** Seed entry points observed in startup code and its constructor table. */
public class DefineBadmanEntryPoints extends GhidraScript {
    private void define(long target, String name) throws Exception {
        Address address = toAddr(target);
        if (getFunctionAt(address) != null) return;
        disassemble(address);
        if (createFunction(address, name) == null && getFunctionContaining(address) == null)
            throw new IllegalStateException("Cannot define entry at " + address);
    }
    @Override
    public void run() throws Exception {
        define(0x088E88A4L, "InitializeBadmanUserMain");
        int count = 0;
        // FUN_088060B4 passes this exact table span to the CRT initialization walker.
        for (long slot = 0x089169C0L; slot < 0x08916B84L; slot += 4) {
            long target = Integer.toUnsignedLong(getInt(toAddr(slot)));
            if ((target & 3) != 0 || target < 0x08912C54L || target >= 0x089169C0L)
                throw new IllegalStateException("Unexpected constructor pointer at " + toAddr(slot));
            define(target, String.format("InitializeBadmanStatic_%08X", target));
            count++;
        }
        println("Seeded user_main and " + count + " constructor-table entry points");
    }
}
