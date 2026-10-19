package ro.cobrabm.fexdroid;

/** The GPU drivers inside the APK (assets/drivers/<zip>, tools/android/fetch_bundled_drivers.sh pins and
 *  packs them), one per GPU family: StevenMXZ's Turnip Gen8 V37 for every Adreno (6xx/7xx too; the driver played
 *  on the AYN Thor's Adreno 740), PanVK for Mali. Other builds are download options (DriverCatalog). The automatic
 *  choice (AutoDriver) installs the one for the phone's family through DriverManager when no installed driver was
 *  made from that zip. */
final class BundledDrivers {
    /** One bundled driver: its family (GpuFamily), the zip's file name (also driver.json "zip"), what the menu
     *  calls it, and the lowest Android API it runs on (its meta.json minApi). */
    static final class Spec {
        final String family, zip, label;
        final int minApi;
        Spec(String family,String zip,String label,int minApi) { this.family=family; this.zip=zip; this.label=label; this.minApi=minApi; }
        String asset() { return "drivers/"+zip; }
    }

    private static final Spec[] ALL={
        new Spec(GpuFamily.MAIN,"Turnip_Gen8_V37.zip","Turnip Gen8 V37 (Adreno 6xx/7xx)",28),
        new Spec(GpuFamily.GEN8,"Turnip_Gen8_V37.zip","Turnip Gen8 V37 (Adreno 8xx)",28),
        new Spec(GpuFamily.MALI,"PanVK-android-v3.2-universal-2a9358a.zip","PanVK 3.2 (Mali)",35),
    };

    private BundledDrivers() {}

    /** The bundled driver for a GPU family, or null (unknown family). */
    static Spec forFamily(String family) {
        if (family!=null) for (Spec s:ALL) if (s.family.equals(family)) return s;
        return null;
    }
}
