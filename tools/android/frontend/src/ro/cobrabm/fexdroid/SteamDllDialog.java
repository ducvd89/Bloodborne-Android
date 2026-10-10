package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.EncodeHintType;
import com.google.zxing.common.BitMatrix;
import com.google.zxing.qrcode.QRCodeWriter;
import in.dragonbra.javasteam.enums.EResult;
import in.dragonbra.javasteam.steam.authentication.AuthPollResult;
import in.dragonbra.javasteam.steam.authentication.AuthSessionDetails;
import in.dragonbra.javasteam.steam.authentication.IAuthenticator;
import in.dragonbra.javasteam.steam.authentication.QrAuthSession;
import in.dragonbra.javasteam.steam.cdn.Client;
import in.dragonbra.javasteam.steam.cdn.Server;
import in.dragonbra.javasteam.steam.handlers.steamapps.PICSProductInfo;
import in.dragonbra.javasteam.steam.handlers.steamapps.PICSRequest;
import in.dragonbra.javasteam.steam.handlers.steamapps.SteamApps;
import in.dragonbra.javasteam.steam.handlers.steamapps.callback.DepotKeyCallback;
import in.dragonbra.javasteam.steam.handlers.steamapps.callback.PICSProductInfoCallback;
import in.dragonbra.javasteam.steam.handlers.steamapps.callback.PICSTokensCallback;
import in.dragonbra.javasteam.steam.handlers.steamcontent.SteamContent;
import in.dragonbra.javasteam.steam.handlers.steamuser.LogOnDetails;
import in.dragonbra.javasteam.steam.handlers.steamuser.SteamUser;
import in.dragonbra.javasteam.steam.handlers.steamuser.callback.LoggedOnCallback;
import in.dragonbra.javasteam.steam.steamclient.SteamClient;
import in.dragonbra.javasteam.steam.steamclient.callbackmgr.CallbackManager;
import in.dragonbra.javasteam.steam.steamclient.callbacks.ConnectedCallback;
import in.dragonbra.javasteam.steam.steamclient.callbacks.DisconnectedCallback;
import in.dragonbra.javasteam.types.ChunkData;
import in.dragonbra.javasteam.types.DepotManifest;
import in.dragonbra.javasteam.types.FileData;
import in.dragonbra.javasteam.types.KeyValue;
import java.io.File;
import java.io.FileOutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import kotlinx.coroutines.CoroutineScope;
import kotlinx.coroutines.CoroutineScopeKt;
import kotlinx.coroutines.Dispatchers;
import kotlinx.coroutines.future.FutureKt;

/** Downloads Lossless.dll from the player's own Lossless Scaling (Steam app 993090), which Lossless
 *  Scaling frame generation needs (run-thor.sh looks for it in bbport/). The player signs in with a
 *  QR code scanned in the Steam mobile app, or with name, password and Steam Guard code; then only
 *  that one file is downloaded from the app's Windows depot and checked against the manifest's
 *  SHA-1. Steam grants the depot key only to owners. Nothing else is kept: no password, no sign-in
 *  token. JavaSteam (MIT) speaks Steam's protocol. */
final class SteamDllDialog implements IAuthenticator {
    private static final int APP=993090; // Lossless Scaling
    private static final long TIMEOUT=60;
    private final Activity activity;
    private final File target;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final CompletableFuture<AuthPollResult> auth=new CompletableFuture<>();
    private AlertDialog dialog;
    private TextView status;
    private ImageView qr;
    private LinearLayout passwordBox;
    private Button passwordToggle;
    private volatile SteamClient client;
    private volatile boolean finished;

    SteamDllDialog(Activity activity,File target) { this.activity=activity; this.target=target; }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }

    void show() {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(8),dp(24),dp(8)); box.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView intro=new TextView(activity); intro.setTextSize(13); intro.setAlpha(0.8f);
        intro.setText("Lossless Scaling frame generation needs Lossless.dll from your own copy of Lossless "
            +"Scaling on Steam. Sign in to download just that file. Your password and sign-in are not saved.");
        box.addView(intro);
        status=new TextView(activity); status.setTextSize(15); status.setTextColor(0xffe8dfc9);
        status.setPadding(0,dp(12),0,dp(8)); status.setGravity(Gravity.CENTER);
        status.setText("Connecting to Steam…");
        box.addView(status,new LinearLayout.LayoutParams(-1,-2));
        qr=new ImageView(activity); qr.setVisibility(View.GONE); qr.setBackgroundColor(0xffffffff);
        qr.setPadding(dp(8),dp(8),dp(8),dp(8));
        box.addView(qr,new LinearLayout.LayoutParams(dp(220),dp(220)));

        passwordBox=new LinearLayout(activity); passwordBox.setOrientation(LinearLayout.VERTICAL);
        passwordBox.setVisibility(View.GONE);
        final EditText name=new EditText(activity); name.setHint("Steam account name"); name.setSingleLine(true);
        final EditText password=new EditText(activity); password.setHint("Password"); password.setSingleLine(true);
        password.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_PASSWORD);
        Button signIn=new Button(activity); signIn.setText("Sign in"); signIn.setAllCaps(false);
        signIn.setOnClickListener(v -> {
            String user=name.getText().toString().trim(), pass=password.getText().toString();
            if (user.isEmpty() || pass.isEmpty() || client==null) return;
            signIn.setEnabled(false); status("Signing in…");
            new Thread(() -> signInWithPassword(user,pass),"Steam-sign-in").start();
        });
        passwordBox.addView(name,new LinearLayout.LayoutParams(-1,-2));
        passwordBox.addView(password,new LinearLayout.LayoutParams(-1,-2));
        passwordBox.addView(signIn,new LinearLayout.LayoutParams(-1,-2));
        box.addView(passwordBox,new LinearLayout.LayoutParams(-1,-2));
        passwordToggle=new Button(activity); passwordToggle.setAllCaps(false);
        passwordToggle.setText("Sign in with password instead");
        passwordToggle.setOnClickListener(v -> {
            boolean show=passwordBox.getVisibility()!=View.VISIBLE;
            passwordBox.setVisibility(show ? View.VISIBLE : View.GONE);
            qr.setVisibility(show ? View.GONE : View.VISIBLE);
            passwordToggle.setText(show ? "Sign in with a QR code instead" : "Sign in with password instead");
        });
        passwordToggle.setVisibility(View.GONE);
        box.addView(passwordToggle,new LinearLayout.LayoutParams(-2,-2));

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        dialog=new AlertDialog.Builder(activity).setTitle("Get Lossless.dll from Steam").setView(scroll)
            .setNegativeButton("Close",null).create();
        dialog.setOnDismissListener(d -> cancel());
        dialog.show();
        new Thread(this::run,"Steam-Lossless").start();
    }

    private void status(String text) {
        android.util.Log.i("Bloodborne","Steam: "+(text.startsWith("Signed in as ") ? "signed in" : text)); // no account name
        main.post(() -> status.setText(text));
    }
    private void cancel() {
        if (finished) return;
        finished=true; auth.completeExceptionally(new CancellationException());
        SteamClient c=client; if (c!=null) c.disconnect();
    }

    private void run() {
        CoroutineScope scope=CoroutineScopeKt.CoroutineScope(Dispatchers.getIO());
        try {
            // JavaSteam's crypto provider (Spongy Castle), set up here: a failure in its static
            // initializer on one of JavaSteam's own threads would end the app instead of this dialog.
            if (in.dragonbra.javasteam.util.crypto.CryptoHelper.SEC_PROV==null)
                throw new IllegalStateException("No crypto provider");
            SteamClient steam=new SteamClient(); client=steam;
            CallbackManager callbacks=new CallbackManager(steam);
            CompletableFuture<Void> connected=new CompletableFuture<>();
            CompletableFuture<LoggedOnCallback> loggedOn=new CompletableFuture<>();
            callbacks.subscribe(ConnectedCallback.class,cb -> connected.complete(null));
            callbacks.subscribe(DisconnectedCallback.class,cb -> {
                RuntimeException gone=new RuntimeException("Disconnected from Steam");
                connected.completeExceptionally(gone); loggedOn.completeExceptionally(gone); auth.completeExceptionally(gone);
            });
            callbacks.subscribe(LoggedOnCallback.class,loggedOn::complete);
            Thread pump=new Thread(() -> { while (!finished) callbacks.runWaitCallbacks(500L); },"Steam-callbacks");
            pump.setDaemon(true); pump.start();
            steam.connect();
            connected.get(TIMEOUT,TimeUnit.SECONDS);

            AuthSessionDetails details=new AuthSessionDetails();
            details.deviceFriendlyName="Bloodborne for Android";
            details.persistentSession=false;
            QrAuthSession session=steam.getAuthentication().beginAuthSessionViaQR(details).get(TIMEOUT,TimeUnit.SECONDS);
            session.setChallengeUrlChanged(s -> showQr(s.getChallengeUrl()));
            showQr(session.getChallengeUrl());
            session.pollingWaitForResult().whenComplete((result,error) -> {
                if (error==null) auth.complete(result); // a password sign-in may have completed it first
            });
            AuthPollResult signedIn=auth.get();

            status("Signed in as "+signedIn.getAccountName()+". Checking your Lossless Scaling license…");
            main.post(() -> { qr.setVisibility(View.GONE); passwordBox.setVisibility(View.GONE); passwordToggle.setVisibility(View.GONE); });
            LogOnDetails logOn=new LogOnDetails();
            logOn.setUsername(signedIn.getAccountName());
            logOn.setAccessToken(signedIn.getRefreshToken());
            logOn.setShouldRememberPassword(false);
            steam.getHandler(SteamUser.class).logOn(logOn);
            LoggedOnCallback result=loggedOn.get(TIMEOUT,TimeUnit.SECONDS);
            if (result.getResult()!=EResult.OK) throw new IllegalStateException("Steam sign-in failed: "+result.getResult());

            download(steam,scope);
            finished=true;
            status("Lossless.dll is ready. Turn on Lossless Scaling in the menu; it applies when the game restarts.");
        } catch (Throwable e) {
            android.util.Log.w("Bloodborne","Steam: Lossless.dll download failed",e);
            if (!finished) status(message(e));
        } finally {
            finished=true;
            CoroutineScopeKt.cancel(scope,null);
            SteamClient c=client; if (c!=null) c.disconnect();
        }
    }

    private void signInWithPassword(String user,String password) {
        try {
            AuthSessionDetails details=new AuthSessionDetails();
            details.username=user; details.password=password;
            details.deviceFriendlyName="Bloodborne for Android";
            details.persistentSession=false;
            details.authenticator=this;
            AuthPollResult result=client.getAuthentication().beginAuthSessionViaCredentials(details)
                .get(TIMEOUT,TimeUnit.SECONDS).pollingWaitForResult().get();
            auth.complete(result);
        } catch (Exception e) {
            if (!finished) {
                status(message(e)+" Check the name and password, then try again.");
                main.post(() -> {
                    View button=passwordBox.getChildAt(2); if (button!=null) button.setEnabled(true);
                });
            }
        }
    }

    /** Only Lossless.dll, from the depot of the app's Windows build. */
    private void download(SteamClient steam,CoroutineScope scope) throws Exception {
        SteamApps apps=steam.getHandler(SteamApps.class);
        SteamContent content=steam.getHandler(SteamContent.class);
        PICSTokensCallback tokens=apps.picsGetAccessTokens(APP).toFuture().get(TIMEOUT,TimeUnit.SECONDS);
        Long token=tokens.getAppTokens().get(APP);
        PICSProductInfo app=null;
        for (PICSProductInfoCallback info:apps.picsGetProductInfo(new PICSRequest(APP,token==null ? 0L : token))
                .toFuture().get(TIMEOUT,TimeUnit.SECONDS).getResults())
            if (info.getApps().containsKey(APP)) app=info.getApps().get(APP);
        if (app==null) throw new IllegalStateException("Steam did not return Lossless Scaling's details.");

        List<long[]> depots=new ArrayList<>(); // {depot id, public manifest id}
        for (KeyValue depot:app.getKeyValues().get("depots").getChildren()) {
            int id;
            try { id=Integer.parseInt(depot.getName()); } catch (Exception notADepot) { continue; }
            KeyValue publicManifest=depot.get("manifests").get("public");
            long manifest=publicManifest.get("gid").asLong(0L);
            if (manifest==0) manifest=publicManifest.asLong(0L);
            String os=depot.get("config").get("oslist").asString();
            if (manifest!=0 && (os==null || os.isEmpty() || os.contains("windows"))) depots.add(new long[]{id,manifest});
        }
        if (depots.isEmpty()) throw new IllegalStateException("Lossless Scaling has no Windows download on Steam.");

        boolean owned=false;
        for (long[] depot:depots) {
            int depotId=(int)depot[0]; long manifestId=depot[1];
            DepotKeyCallback key=apps.getDepotDecryptionKey(depotId,APP).toFuture().get(TIMEOUT,TimeUnit.SECONDS);
            if (key.getResult()!=EResult.OK) continue;
            owned=true;
            status("Downloading Lossless.dll…");
            List<Server> servers=FutureKt.asCompletableFuture(content.getServersForSteamPipe(null,null,scope))
                .get(TIMEOUT,TimeUnit.SECONDS);
            Long code=FutureKt.asCompletableFuture(content.getManifestRequestCode(depotId,APP,manifestId,"public",null,scope))
                .get(TIMEOUT,TimeUnit.SECONDS);
            Exception last=null;
            try (Client cdn=new Client(steam)) {
                for (Server server:servers) {
                    if (finished) throw new CancellationException();
                    try {
                        DepotManifest manifest=cdn.downloadManifestFuture(depotId,manifestId,code==null ? 0L : code,
                            server,key.getDepotKey()).get(TIMEOUT,TimeUnit.SECONDS);
                        FileData file=findDll(manifest);
                        if (file==null) break; // not in this depot
                        save(fetch(cdn,depotId,server,key.getDepotKey(),file),file);
                        return;
                    } catch (CancellationException e) { throw e;
                    } catch (Exception e) {
                        android.util.Log.w("Bloodborne","Steam: content server "+server.getHost()+" failed",e);
                        last=e; // the next content server
                    }
                }
            }
            if (last!=null) throw last;
        }
        throw new IllegalStateException(owned ? "Lossless.dll was not found in Lossless Scaling's files."
            : "This Steam account does not own Lossless Scaling.");
    }

    private static FileData findDll(DepotManifest manifest) {
        FileData best=null;
        for (FileData file:manifest.getFiles()) {
            String name=file.getFileName().replace('\\','/');
            if (name.equalsIgnoreCase("Lossless.dll")) return file;
            if (name.toLowerCase(java.util.Locale.ROOT).endsWith("/lossless.dll") && best==null) best=file;
        }
        return best;
    }

    private byte[] fetch(Client cdn,int depotId,Server server,byte[] key,FileData file) throws Exception {
        if (file.getTotalSize()<=0 || file.getTotalSize()>64L<<20) throw new IllegalStateException("Unexpected Lossless.dll size");
        byte[] data=new byte[(int)file.getTotalSize()];
        List<ChunkData> chunks=new ArrayList<>(file.getChunks());
        Collections.sort(chunks,(a,b) -> Long.compare(a.getOffset(),b.getOffset()));
        long done=0;
        for (ChunkData chunk:chunks) {
            if (finished) throw new CancellationException();
            byte[] buffer=new byte[chunk.getUncompressedLength()];
            int written=cdn.downloadDepotChunkFuture(depotId,chunk,server,buffer,key).get(TIMEOUT,TimeUnit.SECONDS);
            if (chunk.getOffset()+written>data.length) throw new IllegalStateException("Corrupt chunk");
            System.arraycopy(buffer,0,data,(int)chunk.getOffset(),written);
            done+=written;
            status("Downloading Lossless.dll… "+(done*100/data.length)+"%");
        }
        return data;
    }

    private void save(byte[] data,FileData file) throws Exception {
        byte[] hash=MessageDigest.getInstance("SHA-1").digest(data);
        if (file.getFileHash().length==hash.length && !Arrays.equals(hash,file.getFileHash()))
            throw new IllegalStateException("Lossless.dll failed its checksum");
        File partial=new File(target.getPath()+".part");
        try (FileOutputStream out=new FileOutputStream(partial)) { out.write(data); out.getFD().sync(); }
        if (!partial.renameTo(target)) throw new IllegalStateException("Could not save Lossless.dll");
    }

    private void showQr(String url) {
        try {
            Map<EncodeHintType,Object> hints=new EnumMap<>(EncodeHintType.class);
            hints.put(EncodeHintType.MARGIN,0);
            BitMatrix matrix=new QRCodeWriter().encode(url,BarcodeFormat.QR_CODE,0,0,hints);
            int w=matrix.getWidth(), h=matrix.getHeight();
            int[] pixels=new int[w*h];
            for (int y=0;y<h;++y) for (int x=0;x<w;++x) pixels[y*w+x]=matrix.get(x,y) ? 0xff000000 : 0xffffffff;
            Bitmap bitmap=Bitmap.createBitmap(pixels,w,h,Bitmap.Config.ARGB_8888);
            main.post(() -> {
                BitmapDrawable sharp=new BitmapDrawable(activity.getResources(),bitmap); sharp.setFilterBitmap(false);
                qr.setImageDrawable(sharp);
                if (passwordBox.getVisibility()!=View.VISIBLE) qr.setVisibility(View.VISIBLE);
                passwordToggle.setVisibility(View.VISIBLE);
                status.setText("Scan with the Steam app on your phone (Steam Guard → scan QR code)");
            });
        } catch (Exception e) { status("Could not show the QR code: "+e.getMessage()); }
    }

    private static String message(Throwable e) {
        while ((e instanceof ExecutionException || e instanceof java.util.concurrent.CompletionException) && e.getCause()!=null)
            e=e.getCause();
        if (e instanceof java.util.concurrent.TimeoutException) return "Steam did not answer in time. Check the connection and try again.";
        String text=e.getMessage();
        return text==null || text.isEmpty() ? e.getClass().getSimpleName() : text;
    }

    // ---- Steam Guard for the password sign-in: a code from the app or the e-mail ----
    @Override public CompletableFuture<String> getDeviceCode(boolean previousCodeWasIncorrect) {
        return askCode(previousCodeWasIncorrect ? "That code was wrong. Enter the Steam Guard code from the Steam app:"
            : "Enter the Steam Guard code from the Steam app:");
    }
    @Override public CompletableFuture<String> getEmailCode(String email,boolean previousCodeWasIncorrect) {
        return askCode((previousCodeWasIncorrect ? "That code was wrong. " : "")
            +"Enter the Steam Guard code sent to "+(email==null ? "your e-mail" : email)+":");
    }
    @Override public CompletableFuture<Boolean> acceptDeviceConfirmation() { return CompletableFuture.completedFuture(false); }

    private CompletableFuture<String> askCode(String prompt) {
        CompletableFuture<String> code=new CompletableFuture<>();
        main.post(() -> {
            EditText field=new EditText(activity); field.setSingleLine(true);
            field.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
            new AlertDialog.Builder(activity).setTitle("Steam Guard").setMessage(prompt).setView(field)
                .setPositiveButton("OK",(d,w) -> code.complete(field.getText().toString().trim()))
                .setNegativeButton("Cancel",(d,w) -> code.cancel(false))
                .setOnCancelListener(d -> code.cancel(false)).show();
        });
        return code;
    }
}
