package com.ducvd89.bloodborne;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CancellationException;
import java.util.function.BooleanSupplier;

/** Checks a PS4 PKG for damage (a cut-short or corrupted download or copy) before it is unpacked, from the SHA-256
 *  digests the PKG carries itself. No Android classes, so it is tested on a desktop JDK.
 *
 *  What is checked (all big-endian; every check was run on real fake PKGs - the Bloodborne game, its 1.09 update and
 *  a mod - and matches on all of them):
 *   - the file is at least pkg_size (0x430) bytes long;
 *   - the header: SHA-256 of bytes 0x000..0xFE0 against pkg_digest (0xFE0);
 *   - the body (the entry table's data, body_offset/body_size at 0x20/0x28): against digest_body (0x160);
 *   - the digest table (the entry with id 0x0001): against digest_table (0x140);
 *   - the game data, the PFS image (pfs_image_offset/size at 0x410/0x418): against pfs_image_digest (0x440), and its
 *     first pfs_signed_size (0x438) bytes against pfs_signed_digest (0x460).
 *  An all-zero digest means "not present" (the mod PKG has no pfs_image_digest): skipped. A field that does not
 *  make sense (an offset outside the file) is skipped too: only a digest that can be computed and differs is damage.
 *  The per-entry digests (inside the digest table) are not checked: the body digest covers those entries, and one
 *  entry (0x403) does not match its slot in any of the real PKGs. */
final class PkgVerifier {
    static final String LABEL = "Checking the PKG for damage…";
    static final int CHUNK = 8 << 20;
    private static final int HEADER = 0x1000, HEADER_DIGESTED = 0xFE0;
    private static final long MAX_BODY = 1L << 30; // the real ones are 10-12 MB

    /** What was found: the parts that do not match their digest, and the ones that matched. */
    static final class Report {
        final List<String> damaged = new ArrayList<>(), matched = new ArrayList<>();
        /** The PFS image was not read because the PKG's header or body is already damaged. */
        boolean skippedImage;
        long bytesRead;
        boolean ok() { return damaged.isEmpty(); }
        /** The text for the player: which parts are damaged. */
        String describe() {
            StringBuilder s = new StringBuilder();
            for (String d : damaged) { if (s.length() > 0) s.append('\n'); s.append("- ").append(d); }
            return s.toString();
        }
    }

    private final java.nio.channels.FileChannel channel;
    private final long fileSize;
    private final PkgInstaller.Progress progress;
    private final BooleanSupplier cancelled;
    private final ByteBuffer buffer = ByteBuffer.allocateDirect(CHUNK);
    private long total, done, lastReport;
    private final Report report = new Report();

    private PkgVerifier(FileChannel channel, long fileSize, PkgInstaller.Progress progress, BooleanSupplier cancelled) {
        this.channel = channel; this.fileSize = fileSize; this.progress = progress; this.cancelled = cancelled;
    }

    /** Checks `pkg`. IOException when it cannot be read; CancellationException when cancelled. A file that is not
     *  a PKG (or too short for its header) gives an empty report: PkgInstaller.readPkg reports that. */
    static Report verify(java.io.File pkg, PkgInstaller.Progress progress, BooleanSupplier cancelled) throws IOException {
        try (RandomAccessFile file = new RandomAccessFile(pkg, "r"); FileChannel channel = file.getChannel()) {
            return new PkgVerifier(channel, channel.size(), progress, cancelled).run();
        }
    }

    private static MessageDigest sha256() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }
    private static long be32(byte[] d, int at) { return ((d[at] & 0xffL) << 24) | ((d[at + 1] & 0xffL) << 16) | ((d[at + 2] & 0xffL) << 8) | (d[at + 3] & 0xffL); }
    private static long be64(byte[] d, int at) { return (be32(d, at) << 32) | be32(d, at + 4); }
    private static byte[] slice(byte[] d, int at, int n) { return Arrays.copyOfRange(d, at, at + n); }
    private static boolean zero(byte[] d) { for (byte b : d) if (b != 0) return false; return true; }
    /** [offset, offset + size) lies inside the file (both unsigned 64-bit from the header). */
    private boolean inside(long offset, long size) {
        return offset >= 0 && size > 0 && offset <= fileSize && size <= fileSize - offset;
    }

    private void tick(boolean force) {
        long now = System.nanoTime();
        if (!force && now - lastReport < 100_000_000L) return;
        lastReport = now;
        progress.progress(Math.min(done, total), total, LABEL);
    }
    private void checkCancelled() { if (cancelled.getAsBoolean()) throw new CancellationException(); }

    /** Reads `size` bytes at `offset` (inside the file) in chunks, feeding each digest in `digests` (and the first
     *  `signedLength` of them to `signed`, when not null); returns when all were read. */
    private void hash(long offset, long size, MessageDigest main, MessageDigest signed, long signedLength) throws IOException {
        long position = offset, remaining = size;
        while (remaining > 0) {
            checkCancelled();
            buffer.clear();
            buffer.limit((int)Math.min(CHUNK, remaining));
            while (buffer.hasRemaining()) {
                int n = channel.read(buffer, position + buffer.position());
                if (n < 0) throw new IOException("The PKG ended early (file size changed while it was read)");
            }
            buffer.flip();
            int got = buffer.remaining();
            if (signed != null && position - offset < signedLength) {
                ByteBuffer head = buffer.duplicate();
                head.limit((int)Math.min(got, signedLength - (position - offset)));
                signed.update(head);
            }
            if (main != null) main.update(buffer);
            position += got; remaining -= got; done += got; report.bytesRead += got;
            tick(false);
        }
    }

    private byte[] read(long offset, int size) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(size);
        while (b.hasRemaining()) {
            int n = channel.read(b, offset + b.position());
            if (n < 0) throw new IOException("The PKG ended early (file size changed while it was read)");
        }
        return b.array();
    }

    private Report run() throws IOException {
        if (fileSize < HEADER) return report;
        byte[] head = read(0, HEADER);
        if (head[0] != 0x7f || head[1] != 'C' || head[2] != 'N' || head[3] != 'T') return report;

        // Plan: the digests present and the bytes they cover.
        long bodyOffset = be64(head, 0x20), bodySize = be64(head, 0x28);
        long pfsOffset = be64(head, 0x410), pfsSize = be64(head, 0x418), signedLength = be32(head, 0x438);
        byte[] headerDigest = slice(head, 0xFE0, 32), tableDigest = slice(head, 0x140, 32), bodyDigest = slice(head, 0x160, 32);
        byte[] imageDigest = slice(head, 0x440, 32), signedDigest = slice(head, 0x460, 32);
        boolean doBody = !zero(bodyDigest) && inside(bodyOffset, bodySize) && bodySize <= MAX_BODY;
        long tableOffset = 0, tableSize = 0;
        if (!zero(tableDigest)) {
            long count = be32(head, 0x10), table = be32(head, 0x18);
            if (count > 0 && count <= 4096 && inside(table, count * 32)) {
                byte[] entries = read(table, (int)(count * 32));
                for (int i = 0; i < count; ++i) {
                    if (be32(entries, i * 32) == 1) { // DIGESTS
                        long off = be32(entries, i * 32 + 16), size = be32(entries, i * 32 + 20);
                        if (size <= 1 << 20 && inside(off, size)) { tableOffset = off; tableSize = size; }
                        break;
                    }
                }
            }
        }
        boolean imageKnown = inside(pfsOffset, pfsSize);
        boolean doImage = imageKnown && !zero(imageDigest);
        boolean doSigned = imageKnown && !zero(signedDigest) && signedLength > 0 && signedLength <= pfsSize;
        long imageBytes = doImage ? pfsSize : doSigned ? signedLength : 0;
        long packageSize = be64(head, 0x430); // the size the PKG says it has (PkgInstaller.readPkg rejects a shorter file too)
        if (Long.compareUnsigned(packageSize, fileSize) > 0)
            report.damaged.add("The file is shorter than the PKG says it should be (" + PkgProgress.bytes(fileSize) + " of "
                + PkgProgress.bytes(packageSize) + "): it is incomplete");
        total = HEADER_DIGESTED + (doBody ? bodySize : 0) + tableSize + imageBytes;
        if (total <= HEADER_DIGESTED && zero(headerDigest)) return report;
        tick(true);

        // The header, the body and the digest table: a few MB, first, so a damaged one stops before the long read.
        if (!zero(headerDigest)) {
            MessageDigest d = sha256();
            d.update(head, 0, HEADER_DIGESTED);
            done += HEADER_DIGESTED;
            note(Arrays.equals(d.digest(), headerDigest), "header", "the PKG header doesn't match its checksum");
        }
        if (doBody) {
            MessageDigest d = sha256();
            hash(bodyOffset, bodySize, d, null, 0);
            note(Arrays.equals(d.digest(), bodyDigest), "body", "the PKG's file table and its small files (the body) don't match their checksum");
        }
        if (tableSize > 0) {
            MessageDigest d = sha256();
            hash(tableOffset, tableSize, d, null, 0);
            note(Arrays.equals(d.digest(), tableDigest), "digest table", "the PKG's list of checksums (digest table) doesn't match its own checksum");
        }
        if (imageBytes > 0) {
            if (!report.ok()) { report.skippedImage = true; return report; }
            MessageDigest full = doImage ? sha256() : null;
            MessageDigest signed = doSigned ? sha256() : null;
            hash(pfsOffset, imageBytes, full, signed, doSigned ? signedLength : 0);
            if (doImage) note(Arrays.equals(full.digest(), imageDigest), "image", "the game data (PFS image) doesn't match its checksum");
            if (doSigned) note(Arrays.equals(signed.digest(), signedDigest), "image start", "the start of the game data (PFS image) doesn't match its checksum");
        }
        done = total;
        tick(true);
        return report;
    }

    private void note(boolean same, String part, String problem) {
        if (same) report.matched.add(part);
        else report.damaged.add(problem.substring(0, 1).toUpperCase(java.util.Locale.ROOT) + problem.substring(1));
    }
}
