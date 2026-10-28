// Export externally relocated ELF memory; no decryption or relocation decoder.
// @category Badman.Preparation
// Usage: output_directory prefix [expected_base_hex] [expected_relocations] [input_sha256]
// Defaults identify this profile's externally unpacked libfont PRX.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.reloc.Relocation;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import java.util.TreeMap;

public class ExportRelocatedModule extends GhidraScript {
    private static final String FONT_INPUT_SHA256 =
        "c0edf06ff6ec56d2824b3fe87ed53758df741fc339c783d4085e630a874e4ffc";

    private static String digest(byte[] data) throws Exception {
        byte[] hash = MessageDigest.getInstance("SHA-256").digest(data);
        StringBuilder result = new StringBuilder();
        for (byte value : hash) result.append(String.format("%02x", value & 255));
        return result.toString();
    }

    private static void writeChanged(Path path, byte[] bytes) throws Exception {
        if (Files.isRegularFile(path) && Arrays.equals(Files.readAllBytes(path), bytes)) return;
        Path temporary = path.resolveSibling(path.getFileName().toString() + ".tmp");
        Files.write(temporary, bytes);
        Files.move(temporary, path, StandardCopyOption.REPLACE_EXISTING);
    }

    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2 || args.length > 5 || !args[1].matches("[A-Za-z0-9_-]+")) {
            throw new IllegalArgumentException("Usage: ExportRelocatedModule.java output_directory prefix [expected_base_hex] [expected_relocations] [input_sha256]");
        }
        Path directory = Path.of(args[0]).toAbsolutePath().normalize();
        Files.createDirectories(directory);
        Path statusPath = directory.resolve(args[1] + "-snapshot.txt");
        Files.writeString(statusPath, "EXPORT_PENDING\n", StandardCharsets.UTF_8);
        long expectedBase = args.length > 2 ? Long.decode(args[2]) : 0x09fd0000L;
        int expectedRelocations = args.length > 3 ? Integer.parseInt(args[3]) : 902;
        String expectedHash = args.length > 4 ? args[4] : FONT_INPUT_SHA256;
        try {
            if (currentProgram == null || !"PSP Executable (ELF)".equals(currentProgram.getExecutableFormat()) ||
                !currentProgram.getLanguageID().toString().startsWith("Allegrex:LE:32:")) {
                throw new IllegalStateException("Expected an ELF imported with the external PSP/Allegrex loader");
            }
            if (currentProgram.getImageBase().getOffset() != expectedBase) {
                throw new IllegalStateException("Unexpected image base: " + currentProgram.getImageBase());
            }
            if (!expectedHash.matches("[a-fA-F0-9]{64}") ||
                !expectedHash.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
                throw new IllegalStateException("Imported ELF source SHA256 does not match expected identity");
            }
            List<String> records = new ArrayList<>();
            TreeMap<String,Integer> counts = new TreeMap<>();
            Iterator<Relocation> relocations = currentProgram.getRelocationTable().getRelocations();
            int total = 0;
            while (relocations.hasNext()) {
                Relocation relocation = relocations.next();
                String status = relocation.getStatus().toString();
                counts.put(status, counts.getOrDefault(status, 0) + 1);
                records.add("reloc=" + relocation.getAddress() + " type=" + relocation.getType() +
                    " status=" + status + " values=" + Arrays.toString(relocation.getValues()));
                total++;
            }
            if (total != expectedRelocations || counts.size() != 1 || counts.getOrDefault("APPLIED", 0) != total || total <= 0) {
                throw new IllegalStateException("Relocation verification failed: total=" + total + " statuses=" + counts);
            }
            List<MemoryBlock> blocks = new ArrayList<>();
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                // ELF header/relocation overlays are not loaded guest segments.
                if (block.getStart().getAddressSpace().equals(currentProgram.getAddressFactory().getDefaultAddressSpace())) {
                    if (!block.isInitialized() || block.getSize() <= 0 || block.getSize() > 32L * 1024L * 1024L) {
                        throw new IllegalStateException("Unexpected unloaded/oversized guest memory block: " + block.getName());
                    }
                    blocks.add(block);
                }
            }
            if (blocks.isEmpty()) throw new IllegalStateException("No relocated guest segments to export");
            try (PrintWriter out = new PrintWriter(Files.newBufferedWriter(statusPath, StandardCharsets.UTF_8))) {
                out.println("imageBase=" + currentProgram.getImageBase());
                out.println("format=" + currentProgram.getExecutableFormat());
                out.println("input_sha256=" + currentProgram.getExecutableSHA256());
                for (String record : records) out.println(record);
                out.println("relocations=" + total + " statuses=" + counts);
                for (MemoryBlock block : blocks) {
                    byte[] bytes = new byte[(int)block.getSize()];
                    if (block.getBytes(block.getStart(), bytes) != bytes.length) {
                        throw new IllegalStateException("Incomplete segment read: " + block.getName());
                    }
                    String filename = args[1] + "-" + String.format("%08x", block.getStart().getOffset()) + ".bin";
                    writeChanged(directory.resolve(filename), bytes);
                    out.println("block=" + block.getName() + " start=" + block.getStart() + " size=" + bytes.length +
                        " file=" + filename + " sha256=" + digest(bytes));
                }
                String success = String.format("EXPORT_OK image_base=0x%08X relocations=%d applied=%d segments=%d", expectedBase, total, total, blocks.size());
                out.println(success);
                out.flush();
                if (out.checkError()) throw new IllegalStateException("Failed to write snapshot verification report");
                println(success);
            }
        } catch (Exception error) {
            Files.writeString(statusPath, "EXPORT_FAILED " + error + "\n", StandardCharsets.UTF_8);
            throw error;
        }
    }
}
