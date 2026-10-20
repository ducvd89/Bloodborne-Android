package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/** The Online menu: the "Play online" switch, the pool (Seamless Hunt or The Hunter's Dream), the account of the chosen
 *  pool, advanced addresses and switches, the connection test, and the cheats notice. Changes are saved as they are
 *  made and apply at the next start of the game; closing the menu while the game runs offers a restart when they differ
 *  from what it started with. The online code is from bloodborne_pc / shadPS4 (GPL-2.0-or-later), The Hunter's Dream's
 *  backend from droogie/bbhost (GPL-3.0-or-later). */
final class OnlineDialog {
    /** What the dialog needs from the activity. */
    interface Host {
        boolean destroyed();
        void closed();
        /** The game runs and was started with other online settings than the saved ones. */
        boolean needsRestart();
        void restart();
        /** The online switch or the account changed: cheats.state must follow (off while playing online). */
        void cheatsChanged();
        /** The running online game's state (OnlineStatus), or null when no online game runs or it has not reported yet. */
        OnlineStatus liveStatus();
        /** A game runs that was started online. */
        boolean runningOnline();
    }

    private static final int GOLD=0xffc6ad76, GOOD=0xff7fbf7f, BAD=0xffe07a6a;

    private final Activity activity;
    private final OnlineSettings settings;
    private final File base;
    private final Host host;
    private final Handler main=new Handler(Looper.getMainLooper());
    private AlertDialog dialog;
    private Switch enable, upnp, bbsc, detailedLog;
    private RadioGroup pools, relays;
    private LinearLayout seamlessBox, thdBox, advancedBox, seamlessAdvanced, thdAdvanced;
    private EditText npid, password, server, webapi, community, thdServer, p2pPort;
    private TextView status, liveLine, thdStatus, code, codeNote, testResult, moduleNote;
    private Button thdButton, openPage, advancedToggle, test;
    private boolean setting; // the controls are being set by the code
    private volatile boolean closed;
    private boolean signing, testing;
    private final AtomicBoolean cancelSignIn=new AtomicBoolean();
    private String codePage="";

    OnlineDialog(Activity activity,OnlineSettings settings,File base,Host host) {
        this.activity=activity; this.settings=settings; this.base=base; this.host=host;
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }
    private TextView note(LinearLayout box,String text,float alpha) {
        TextView t=new TextView(activity); t.setTextSize(13); t.setAlpha(alpha); t.setText(text);
        t.setPadding(0,dp(4),0,dp(4)); box.addView(t); return t;
    }
    private TextView heading(LinearLayout box,String text) {
        TextView t=new TextView(activity); t.setText(text); t.setTextSize(15);
        t.setTypeface(Typeface.DEFAULT,Typeface.BOLD); t.setPadding(0,dp(12),0,dp(2)); box.addView(t); return t;
    }
    private Button button(String label,View.OnClickListener click) {
        Button b=new Button(activity); b.setAllCaps(false); b.setText(label); b.setOnClickListener(click); return b;
    }
    private EditText field(LinearLayout box,String hint,String value) {
        EditText e=new EditText(activity); e.setHint(hint); e.setText(value); e.setSingleLine(true); e.setTextSize(15);
        e.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_URI|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        box.addView(e,new LinearLayout.LayoutParams(-1,-2)); return e;
    }
    private Switch toggle(LinearLayout box,String title,boolean on) {
        Switch s=new Switch(activity); s.setText(title); s.setChecked(on); s.setTextSize(15);
        s.setPadding(0,dp(6),0,dp(6)); box.addView(s); return s;
    }

    void show() {
        closed=false;
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        note(box,"Play with other hunters: bells, summons, messages, bloodstains and ghosts through a community server. "
            +"Everyone in a session needs the same pool and Bloodborne 1.09. It needs the game restarted to apply.",0.8f);

        enable=toggle(box,"Play online",settings.enabled());
        enable.setTextSize(16);
        enable.setOnCheckedChangeListener((b,on) -> { if (!setting) { commit(); settings.setEnabled(on); changed(); } });
        status=note(box,"",0.9f); status.setTextColor(GOLD);
        liveLine=note(box,"",1f); liveLine.setTextSize(14); liveLine.setTypeface(Typeface.DEFAULT,Typeface.BOLD); liveLine.setVisibility(View.GONE);
        note(box,"Cheats are switched off while you play online: they would change the game for the other hunters too.",0.65f);
        moduleNote=note(box,"",0.9f); moduleNote.setTextColor(BAD);
        if (!new File(base,"bionic/lib/libbbnet20.so").isFile()) {
            // The runtime of this build may have no online module (run-bionic.sh then starts the game offline).
            moduleNote.setText("This runtime has no online module (libbbnet20.so): the game starts offline until it is installed.");
        } else moduleNote.setVisibility(View.GONE);

        heading(box,"Pool");
        pools=new RadioGroup(activity);
        final String[] poolIds={OnlineSettings.POOL_SEAMLESS,OnlineSettings.POOL_THD};
        String[] poolNames={"Seamless Hunt (seamlesshunt.com)","The Hunter's Dream (thehuntersdream.com)"};
        for (int i=0;i<poolIds.length;++i) {
            RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setText(poolNames[i]); r.setTextSize(15);
            pools.addView(r); if (poolIds[i].equals(settings.pool())) pools.check(r.getId());
        }
        pools.setOnCheckedChangeListener((g,id) -> {
            if (setting) return;
            for (int i=0;i<g.getChildCount();++i) if (g.getChildAt(i).getId()==id) { commit(); settings.setPool(poolIds[i]); }
            changed();
        });
        box.addView(pools);

        // ---- Seamless Hunt ----
        seamlessBox=new LinearLayout(activity); seamlessBox.setOrientation(LinearLayout.VERTICAL);
        note(seamlessBox,"Bells, summons, messages and bloodstains on Seamless Hunt's servers, with optional seamless co-op. "
            +"Everyone must use Seamless Hunt.",0.7f);
        heading(seamlessBox,"Seamless Hunt account");
        npid=field(seamlessBox,"Online ID (the account name, not an email)",settings.npid());
        npid.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        password=field(seamlessBox,"Password",settings.password());
        password.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_PASSWORD);
        password.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        npid.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        seamlessBox.addView(button("Create an account (seamlesshunt.com)",v -> openUrl(OnlineAddr.SEAMLESS_SITE)),new LinearLayout.LayoutParams(-2,-2));
        note(seamlessBox,"The account is made on the Seamless Hunt website. Its server protocol sends the password unencrypted, "
            +"so use one you use nowhere else. The password is kept only in this app's private storage and is never written to "
            +"the log or the problem report.",0.65f);
        box.addView(seamlessBox);

        // ---- The Hunter's Dream ----
        thdBox=new LinearLayout(activity); thdBox.setOrientation(LinearLayout.VERTICAL);
        note(thdBox,"Co-op with players of bbhost through The Hunter's Dream (no seamless co-op there). Everyone must use "
            +"The Hunter's Dream.",0.7f);
        heading(thdBox,"The Hunter's Dream account");
        thdStatus=note(thdBox,"",0.95f);
        code=new TextView(activity); code.setTextSize(34); code.setTypeface(Typeface.MONOSPACE,Typeface.BOLD); code.setTextColor(GOLD);
        code.setTextIsSelectable(true); code.setPadding(0,dp(6),0,dp(2)); code.setVisibility(View.GONE); thdBox.addView(code);
        codeNote=note(thdBox,"",0.8f); codeNote.setVisibility(View.GONE);
        LinearLayout thdRow=new LinearLayout(activity); thdRow.setOrientation(LinearLayout.HORIZONTAL);
        thdButton=button("Sign in with Discord",v -> onThdButton());
        thdRow.addView(thdButton,new LinearLayout.LayoutParams(-2,-2));
        openPage=button("Open the page",v -> { if (!codePage.isEmpty()) openUrl(codePage); });
        openPage.setVisibility(View.GONE); thdRow.addView(openPage,new LinearLayout.LayoutParams(-2,-2));
        thdBox.addView(thdRow);
        note(thdBox,"A code appears here and is copied to the clipboard; the sign-in page opens in your browser. On it: "
            +"1) press \"Sign in with Discord to link\" and sign in; 2) back on the page, enter the code in the code field "
            +"(not in \"Sign in with a code from the game\"). No password is needed: the app keeps a token in its private "
            +"storage, never in the log or the problem report.",0.65f);
        box.addView(thdBox);

        // ---- Advanced (collapsed) ----
        advancedToggle=button("Advanced ▸",v -> {
            boolean open=advancedBox.getVisibility()!=View.VISIBLE;
            advancedBox.setVisibility(open ? View.VISIBLE : View.GONE);
            advancedToggle.setText(open ? "Advanced ▾" : "Advanced ▸");
        });
        box.addView(advancedToggle,new LinearLayout.LayoutParams(-2,-2));
        advancedBox=new LinearLayout(activity); advancedBox.setOrientation(LinearLayout.VERTICAL); advancedBox.setVisibility(View.GONE);
        seamlessAdvanced=new LinearLayout(activity); seamlessAdvanced.setOrientation(LinearLayout.VERTICAL);
        heading(seamlessAdvanced,"Servers (empty: Seamless Hunt's)");
        server=field(seamlessAdvanced,"shadNet server, "+OnlineAddr.SHADNET_SERVER,settings.server());
        webapi=field(seamlessAdvanced,"WebAPI, "+OnlineAddr.WEBAPI_SERVER,settings.webapi());
        community=field(seamlessAdvanced,"Game server, "+OnlineAddr.COMMUNITY_SERVER,settings.community());
        note(seamlessAdvanced,"For a private server: the shadNet address as host:port. The WebAPI defaults to that host on port 31315.",0.6f);
        bbsc=toggle(seamlessAdvanced,"Seamless co-op (BBSC), experimental",settings.bbsc());
        note(seamlessAdvanced,"The session survives deaths, lamps, area changes and bosses, up to five hunters. Every player needs it on "
            +"and the same server. Experimental on Android.",0.65f);
        advancedBox.addView(seamlessAdvanced);
        thdAdvanced=new LinearLayout(activity); thdAdvanced.setOrientation(LinearLayout.VERTICAL);
        heading(thdAdvanced,"Server (empty: "+OnlineAddr.HUNTERS_DREAM_SERVER+")");
        thdServer=field(thdAdvanced,OnlineAddr.HUNTERS_DREAM_SERVER,settings.thdServer());
        advancedBox.addView(thdAdvanced);
        heading(advancedBox,"Connection");
        p2pPort=field(advancedBox,"P2P port (empty: 3658, 0: any free port)",settings.p2pPort());
        p2pPort.setInputType(InputType.TYPE_CLASS_NUMBER);
        upnp=toggle(advancedBox,"UPnP (open the port on the router)",settings.upnp());
        note(advancedBox,"Off by default on Android. Turn it on if direct connections to other hunters fail on your Wi-Fi.",0.65f);
        detailedLog=toggle(advancedBox,"Detailed online log",settings.detailedLog());
        note(advancedBox,"Logs the online module's messages at info level, for a problem report. Off: warnings and errors only.",0.65f);
        TextView relayTitle=new TextView(activity); relayTitle.setText("Relay through the server"); relayTitle.setTextSize(15);
        relayTitle.setPadding(0,dp(6),0,dp(2)); advancedBox.addView(relayTitle);
        relays=new RadioGroup(activity);
        final String[] relayIds={OnlineSettings.RELAY_SERVER,OnlineSettings.RELAY_AUTO,OnlineSettings.RELAY_OFF};
        String[] relayNames={"Always through the server (default, works behind mobile networks)","Direct first, then the server","Off (direct only)"};
        for (int i=0;i<relayIds.length;++i) {
            RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setText(relayNames[i]); r.setTextSize(14);
            relays.addView(r); if (relayIds[i].equals(settings.relay())) relays.check(r.getId());
        }
        advancedBox.addView(relays);
        box.addView(advancedBox);

        // ---- the connection test ----
        heading(box,"Connection test");
        test=button("Test connection",v -> runTest());
        box.addView(test,new LinearLayout.LayoutParams(-2,-2));
        testResult=note(box,"",0.95f);

        note(box,"Online code from bloodborne_pc and shadPS4 (GPL-2.0-or-later); seamless co-op from BBSC (GPL-2.0-or-later); "
            +"The Hunter's Dream backend from droogie/bbhost (GPL-3.0-or-later). The servers are run by Seamless Hunt and "
            +"The Hunter's Dream, who are not affiliated with this app.",0.55f);

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        dialog=new AlertDialog.Builder(activity).setTitle("Online").setView(scroll).setPositiveButton("Close",null).create();
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        dialog.setOnDismissListener(d -> dismissed());
        refreshUi();
        dialog.show();
    }

    void close() { if (dialog!=null && dialog.isShowing()) dialog.dismiss(); }

    /** The line at the top: how the running game's online session is doing (the host calls this on every change). */
    void refreshLive() {
        if (liveLine==null || closed) return;
        OnlineStatus s=host.liveStatus();
        if (s==null && !host.runningOnline()) { liveLine.setVisibility(View.GONE); return; }
        SpannableStringBuilder text=new SpannableStringBuilder("\u25CF ");
        text.setSpan(new ForegroundColorSpan(s==null ? OnlineStatus.AMBER : s.color()),0,1,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        text.append(s==null ? "Starting online play\u2026" : s.shortText()+(s.detail.isEmpty() ? "" : ": "+s.detail));
        liveLine.setText(text); liveLine.setVisibility(View.VISIBLE);
    }

    private void dismissed() {
        closed=true; cancelSignIn.set(true);
        commit();
        host.cheatsChanged();
        if (!host.destroyed() && !activity.isFinishing() && host.needsRestart()) {
            new AlertDialog.Builder(activity).setTitle("Restart Bloodborne?")
                .setMessage("The online settings apply when the game starts. Restart now? Any unsaved progress will be lost.")
                .setNegativeButton("Later",null).setPositiveButton("Restart",(d,w) -> host.restart())
                .setOnDismissListener(d -> host.closed()).show();
        } else host.closed();
    }

    /** Saves the text fields (the account, the addresses) to the private preferences. */
    private void commit() {
        if (npid==null) return;
        settings.saveSeamless(npid.getText().toString(),password.getText().toString(),server.getText().toString(),
            webapi.getText().toString(),community.getText().toString());
        String relay=OnlineSettings.RELAY_SERVER; String[] ids={OnlineSettings.RELAY_SERVER,OnlineSettings.RELAY_AUTO,OnlineSettings.RELAY_OFF};
        for (int i=0;i<relays.getChildCount();++i) if (((RadioButton)relays.getChildAt(i)).isChecked()) relay=ids[i];
        settings.setDetailedLog(detailedLog.isChecked());
        settings.saveCommon(thdServer.getText().toString(),p2pPort.getText().toString(),upnp.isChecked(),bbsc.isChecked(),relay);
    }

    /** The switch, the pool or the account changed: cheats.state follows at once, the boxes follow the pool. */
    private void changed() {
        host.cheatsChanged();
        refreshUi();
    }

    private void refreshUi() {
        boolean thd=settings.pool().equals(OnlineSettings.POOL_THD);
        seamlessBox.setVisibility(thd ? View.GONE : View.VISIBLE);
        thdBox.setVisibility(thd ? View.VISIBLE : View.GONE);
        seamlessAdvanced.setVisibility(thd ? View.GONE : View.VISIBLE);
        thdAdvanced.setVisibility(thd ? View.VISIBLE : View.GONE);
        setting=true; enable.setChecked(settings.enabled()); setting=false;
        if (!settings.enabled()) status.setText("Off: the game plays offline.");
        else if (settings.hasCredentials()) status.setText("On: the game starts online with "+(thd ? "The Hunter's Dream" : "Seamless Hunt")+".");
        else if (thd) status.setText("On, but sign in to The Hunter's Dream below first: until then the game starts offline.");
        else if (settings.npid().indexOf('@')>=0) status.setText("The Online ID is the account name, not an email address.");
        else status.setText("On, but enter the Online ID and password below first: until then the game starts offline.");
        if (signing) {
            thdStatus.setText("Waiting for you to approve the code on the website…");
            thdButton.setText("Cancel");
        } else {
            thdStatus.setText(settings.thdSignedIn() ? "Signed in as "+settings.thdOnlineId() : "Not signed in. A Discord account linked to The Hunter's Dream is needed.");
            thdButton.setText(settings.thdSignedIn() ? "Sign out" : "Sign in with Discord");
            code.setVisibility(View.GONE); codeNote.setVisibility(View.GONE); openPage.setVisibility(View.GONE);
        }
        test.setEnabled(!testing);
        refreshLive();
    }

    // ---- The Hunter's Dream sign-in ----

    private void onThdButton() {
        if (signing) { cancelSignIn.set(true); return; }
        if (settings.thdSignedIn()) { settings.signOutThd(); changed(); return; }
        commit();
        cancelSignIn.set(false); signing=true; refreshUi();
        final String address=settings.thdServer();
        Thread t=new Thread(() -> {
            String[] result=null; String error=null;
            try {
                result=OnlineNet.thdSignIn(address,"bbport on Android",(page,userCode) -> main.post(() -> { if (live()) showCode(page,userCode); }),
                    cancelSignIn::get);
            } catch (OnlineNet.NetException e) { error=e.getMessage(); }
            final String[] r=result; final String err=error;
            main.post(() -> signedIn(r,err));
        },"Online-signin");
        t.setDaemon(true); t.start();
    }

    /** The code to approve: big, copied to the clipboard, and the page that takes it opens in the browser. */
    private void showCode(String page,String userCode) {
        if (!signing) return;
        code.setText(userCode); code.setVisibility(View.VISIBLE);
        codeNote.setText("Enter this code at "+hostOf(page)+". It has been copied to the clipboard.");
        codeNote.setVisibility(View.VISIBLE);
        try {
            ClipboardManager clipboard=(ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard!=null) clipboard.setPrimaryClip(ClipData.newPlainText("The Hunter's Dream code",userCode));
        } catch (RuntimeException e) { android.util.Log.w("Bloodborne","clipboard: "+e.getClass().getSimpleName()); }
        // The page comes from the server's reply: only a web address is opened, else the sign-in page of the server itself.
        String address=page.startsWith("https://") || page.startsWith("http://") ? page
            : OnlineAddr.thdBase(settings.thdServer())+"/link";
        codePage=address; openPage.setVisibility(View.VISIBLE);
        openUrl(address);
    }
    private static String hostOf(String url) {
        int sep=url.indexOf("://");
        String rest=sep>=0 ? url.substring(sep+3) : url;
        int slash=rest.indexOf('/');
        return slash>=0 ? rest.substring(0,slash) : rest;
    }

    private void signedIn(String[] result,String error) {
        signing=false;
        if (!live()) return;
        if (result!=null) { settings.saveThdAccount(result[0],result[1]); changed(); }
        else {
            refreshUi();
            if (error!=null && !error.equals("cancelled")) thdStatus.setText("The Hunter's Dream sign-in: "+error);
        }
    }

    // ---- the connection test ----

    private void runTest() {
        if (testing) return;
        commit();
        testing=true; test.setEnabled(false); testResult.setText("Testing…");
        final boolean thd=settings.pool().equals(OnlineSettings.POOL_THD);
        final String serverValue=settings.server().isEmpty() ? OnlineAddr.SHADNET_SERVER : settings.server();
        final String communityValue=OnlineAddr.communityAddress(settings.community());
        final String webapiValue=OnlineAddr.webapiSetting(settings.webapi(),serverValue,communityValue);
        final String thdValue=settings.thdServer();
        Thread t=new Thread(() -> {
            final List<OnlineNet.Check> results=thd ? OnlineNet.checkThd(thdValue)
                : OnlineNet.checkSeamless(serverValue,webapiValue,communityValue);
            main.post(() -> {
                testing=false;
                if (!live()) return;
                test.setEnabled(true);
                SpannableStringBuilder text=new SpannableStringBuilder();
                int problems=0;
                for (OnlineNet.Check c:results) {
                    int start=text.length();
                    text.append(c.ok ? "OK  " : c.note ? "No  " : "Not reachable  ");
                    text.setSpan(new ForegroundColorSpan(c.ok ? GOOD : c.note ? GOLD : BAD),start,text.length(),Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                    text.append(c.name).append(": ").append(c.address);
                    if (!c.detail.isEmpty()) text.append(" (").append(c.detail).append(')');
                    text.append('\n');
                    if (!c.ok && !c.note && !c.name.equals("Summon service")) ++problems;
                }
                text.append(problems==0 ? "The servers answer." : "Some servers did not answer: check the network and the addresses.");
                testResult.setText(text);
            });
        },"Online-test");
        t.setDaemon(true); t.start();
    }

    private void openUrl(String url) {
        try { activity.startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(url))); }
        catch (Exception e) { android.widget.Toast.makeText(activity,"No browser to open "+url,android.widget.Toast.LENGTH_LONG).show(); }
    }
}
