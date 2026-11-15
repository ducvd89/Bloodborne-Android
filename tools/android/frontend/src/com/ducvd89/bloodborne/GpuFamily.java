package com.ducvd89.bloodborne;

import java.io.File;

/** The phone's GPU family, for the automatic GPU driver (AutoDriver), the driver download list and the problem
 *  report. One place decides it:
 *  - Adreno (Qualcomm, /dev/kgsl-3d0): from the chip id kgsl-chip-id reads (LogReport.chipId): 0x44... is an
 *    Adreno 8xx ("gen8"); any other chip id it reports is "main" (Adreno 6xx/7xx, and the drivers' default).
 *  - Mali (/dev/mali0, no KGSL): "mali". A Qualcomm phone (Build.HARDWARE qcom, or a Qualcomm/QTI SoC
 *    manufacturer) is never taken for Mali.
 *  - Anything else, or an Adreno whose chip id cannot be read: unknown (family null), which uses the phone's own driver. */
final class GpuFamily {
    static final String MAIN="main", GEN8="gen8", MALI="mali";

    /** "main", "gen8", "mali", or null when unknown. */
    final String family;
    /** kgsl-chip-id's first line (e.g. "0x44050001"), or null. */
    final String chip;

    private GpuFamily(String family,String chip) { this.family=family; this.chip=chip; }

    /** The decision itself, from what was observed. chip: kgsl-chip-id's line or null; qualcomm: the phone is
     *  a Qualcomm one by its hardware / SoC manufacturer. */
    static GpuFamily classify(String chip,boolean kgslNode,boolean maliNode,boolean qualcomm) {
        if (chip!=null && chip.startsWith("0x")) return new GpuFamily(chip.startsWith("0x44") ? GEN8 : MAIN,chip);
        if (maliNode && !kgslNode && !qualcomm) return new GpuFamily(MALI,null);
        return new GpuFamily(null,chip);
    }

    /** Looks at the phone. Blocks (it runs kgsl-chip-id, up to 5 s): not for the UI thread. base: files/bbport. */
    static GpuFamily detect(String base) {
        String chip=LogReport.chipId(base);
        boolean kgsl=new File("/dev/kgsl-3d0").exists(), mali=new File("/dev/mali0").exists();
        String hardware=android.os.Build.HARDWARE==null ? "" : android.os.Build.HARDWARE.toLowerCase(java.util.Locale.ROOT);
        String soc=android.os.Build.VERSION.SDK_INT>=31 && android.os.Build.SOC_MANUFACTURER!=null
            ? android.os.Build.SOC_MANUFACTURER.toLowerCase(java.util.Locale.ROOT) : "";
        boolean qualcomm=hardware.equals("qcom") || soc.contains("qualcomm") || soc.equals("qti");
        return classify(chip,kgsl,mali,qualcomm);
    }

    boolean adreno() { return MAIN.equals(family) || GEN8.equals(family); }

    /** "Adreno 8xx", "Adreno 6xx/7xx", "Mali" or "unknown". */
    String label() {
        return GEN8.equals(family) ? "Adreno 8xx" : MAIN.equals(family) ? "Adreno 6xx/7xx" : MALI.equals(family) ? "Mali" : "unknown";
    }
    /** For report.txt: the label with the chip id when there is one. */
    String describe() { return chip==null ? label() : label()+" ("+chip+")"; }
}
