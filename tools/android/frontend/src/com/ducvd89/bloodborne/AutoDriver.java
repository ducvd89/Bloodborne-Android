package com.ducvd89.bloodborne;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.function.Consumer;

/** The "auto" value of the gpu_driver setting: the driver bundled in the APK for this phone's GPU family
 *  (BundledDrivers). At each game start resolve() picks it, installs it from the APK (through DriverManager, with
 *  the checks of a driver zip the player chose) when no installed driver was made from that zip, and names the
 *  driver folder run-bionic.sh gets as BB_GPU_DRIVER. Anything that cannot be done means the phone's own driver:
 *  a Mali phone before Android 15 (PanVK's minApi), an unknown GPU, a failed install (the game still starts). */
final class AutoDriver {
    static final String SETTING="auto";

    /** id: the driver folder to use, or null for the phone's own driver. line: one log line saying which and why.
     *  notice: something to tell the player (a toast), or null. */
    static final class Result {
        final String id, line, notice;
        Result(String id,String line,String notice) { this.id=id; this.line=line; this.notice=notice; }
    }

    private AutoDriver() {}

    /** What auto means on this phone, in words for the menu: "Turnip Gen8 V39 (Adreno 8xx)", or the phone's own
     *  driver with the reason. gpu may be null (not detected yet). */
    static String describe(GpuFamily gpu,int sdk) {
        if (gpu==null) return "detecting the GPU…";
        BundledDrivers.Spec spec=BundledDrivers.forFamily(gpu.family);
        if (spec==null) return "Phone's own driver (GPU not recognised)";
        if (sdk<spec.minApi) return "Phone's own driver ("+spec.label.replaceAll(" .*","")+" needs Android "+androidRelease(spec.minApi)+")";
        return spec.label;
    }
    private static String androidRelease(int api) { return api==35 ? "15" : api==34 ? "14" : api==33 ? "13" : "(API "+api+")"; }

    /** Resolves auto for a start. Blocks (it may unpack a driver): not for the UI thread. progress gets the
     *  status text while a driver is unpacked. Never throws. */
    static Result resolve(Context context,DriverManager drivers,GpuFamily gpu,int sdk,Consumer<String> progress) {
        BundledDrivers.Spec spec=gpu==null ? null : BundledDrivers.forFamily(gpu.family);
        if (spec==null)
            return new Result(null,"GPU driver auto: phone's own driver (GPU "+(gpu==null ? "not detected" : "not recognised")+")",null);
        if (sdk<spec.minApi) {
            String why=spec.label+" needs Android "+androidRelease(spec.minApi)+" or newer (this is API "+sdk+")";
            return new Result(null,"GPU driver auto: phone's own driver ("+why+")","Using the phone's own GPU driver: "+why+".");
        }
        try {
            DriverManager.Driver have=null;
            for (DriverManager.Driver d:drivers.list())
                if (spec.zip.equals(d.zip)) { if (drivers.intact(d.id)) { have=d; break; } else drivers.delete(d.id); }
            if (have==null) {
                progress.accept("Installing the GPU driver ("+spec.label+")…");
                try (InputStream in=context.getAssets().open(spec.asset())) { have=drivers.install(in,spec.zip); }
            }
            return new Result(have.id,"GPU driver auto: "+spec.label+" ("+gpu.label()+"), folder "+have.id,null);
        } catch (IOException | RuntimeException ex) {
            String problem=ex.getMessage()!=null ? ex.getMessage() : ex.toString();
            android.util.Log.e("Bloodborne","bundled driver "+spec.zip+": "+ex);
            return new Result(null,"GPU driver auto: ERROR installing the bundled "+spec.label+" ("+problem+"): using the phone's own driver",
                "The bundled GPU driver could not be installed ("+problem+"). Using the phone's own driver.");
        }
    }
}
