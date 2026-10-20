package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PointF;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** Translucent PlayStation-style controls with a persistent, draggable layout. A touch outside every
 *  control turns the camera: a right stick anchored where the finger landed (no mouse input).
 *
 *  The editor also places the floating mini map of Hunter's Atlas (the element "minimap", when the floating map is
 *  on: setMiniMap): a square drawn in the editor as a labelled rectangle, moved by dragging and resized by pinching or
 *  with the bar's − and +, saved like a control (same profile keys and edge anchors; a factor of 32% of the screen's
 *  shorter side, 18% to 70%). Not edited, it sits in the top right corner (12 dp from the top and right edges, past a
 *  display cutout). MiniListener gets its rect in this view's pixels (the view fills the game's FrameLayout) at every
 *  change of size or placement, so MainActivity can lay the map out there. */
final class TouchControllerOverlay extends View {
    private static final String[] NAMES={
        "l2","l1","touchpad","options","r1","r2",
        "up","left","right","down","triangle","circle","cross","square",
        "leftstick","l3","r3"
    };
    private static final float[][] DEFAULT={
        {87,105},{213,105},{640,105},{773,105},{1067,105},{1193,105},
        {160,395},{90,465},{230,465},{160,535},
        {1120,405},{1190,475},{1120,545},{1050,475},
        {320,612},{420,614},{860,614}
    };
    /** Camera touch: the drag (layout pixels) from where it landed that tilts the right stick fully. */
    private static final float CAMERA_RANGE=64;
    /** A control's size in the editor: a factor of its default size. */
    private static final float MIN_SIZE=0.6f, MAX_SIZE=2f, SIZE_STEP=1.1f;
    private final PadBridge pad;
    private final SharedPreferences preferences;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<String,PointF> centers=new LinkedHashMap<>();
    private final Map<String,Float> sizes=new HashMap<>();
    private final SparseArray<String> controls=new SparseArray<>();
    private final SparseArray<PointF> positions=new SparseArray<>();
    private int cameraPointer=-1;
    private final PointF cameraAnchor=new PointF();
    private final Set<String> pressed=new LinkedHashSet<>();
    private boolean editing;
    // Editor: the finger dragging `dragged`, a second finger pinching it, and the control −/+ resize.
    private int editPointer=-1, pinchPointer=-1;
    private float pinchDistance, pinchSize;
    private String dragged, selected;
    private Runnable onEditDone;

    // The mini map element: stored like a control (centers/anchors/sizes under MINI, keys overlay_*_minimap[@shape]);
    // while miniDefault nothing is stored and its rect is the top right default, computed from the current size.
    interface MiniListener { void rect(int left,int top,int side); void editing(boolean on); }
    private static final String MINI="minimap";
    private static final float MINI_BASE=0.32f, MINI_MIN=0.18f, MINI_MAX=0.70f;
    private boolean miniOn, miniDefault=true;
    private MiniListener miniListener;
    private int miniLeft=-1, miniTop=-1, miniSide=-1;
    private WindowInsets lastInsets;
    private final RectF miniPx=new RectF();

    TouchControllerOverlay(Context context,PadBridge pad) {
        super(context); this.pad=pad;
        preferences=context.getSharedPreferences("bloodborne",Context.MODE_PRIVATE);
        load(null);
        setContentDescription("On-screen PlayStation controller");
    }

    /** One layout per screen shape: a device with several screens or window sizes (a foldable's inner and outer
     *  screens) keeps a layout for each, applied when the overlay takes that shape. A shape is the aspect ratio in
     *  steps of 0.05 (profile "a43" = 2.15:1). A shape never edited uses the shared layout (the keys without a
     *  profile, as saved before profiles), adapted by the edge anchors; editing or resetting on it saves a layout
     *  of its own (overlay_profile_<shape> marks it, its keys end in @<shape>). */
    private String profile;
    private static String shapeOf(int width,int height) { return "a"+Math.round(width*20f/Math.max(1,height)); }
    private String key(String base,String name,String p) { return base+name+(p==null ? "" : "@"+p); }
    private void load(String p) {
        String from=p!=null && preferences.getBoolean("overlay_profile_"+p,false) ? p : null;
        for (int i=0;i<NAMES.length;i++) {
            String name=NAMES[i];
            float x=preferences.getFloat(key("overlay_x_",name,from),DEFAULT[i][0]);
            float y=preferences.getFloat(key("overlay_y_",name,from),DEFAULT[i][1]);
            centers.put(name,new PointF(x,y));
            sizes.put(name,clampSize(preferences.getFloat(key("overlay_s_",name,from),1f)));
            anchors.put(name,new int[]{preferences.getInt(key("overlay_ax_",name,from),anchorX(x)),
                                       preferences.getInt(key("overlay_ay_",name,from),anchorY(y))});
        }
        loadMini(from);
        profile=p;
    }
    private void loadMini(String from) {
        boolean has=preferences.contains(key("overlay_x_",MINI,from)) && preferences.contains(key("overlay_y_",MINI,from));
        miniDefault=!has;
        if (has) {
            float x=preferences.getFloat(key("overlay_x_",MINI,from),0), y=preferences.getFloat(key("overlay_y_",MINI,from),0);
            centers.put(MINI,new PointF(x,y));
            anchors.put(MINI,new int[]{preferences.getInt(key("overlay_ax_",MINI,from),1),preferences.getInt(key("overlay_ay_",MINI,from),-1)});
        } else {
            centers.put(MINI,new PointF());
            anchors.put(MINI,new int[]{1,-1}); // the top right corner
        }
        // The size is saved on its own too (the dialog's slider resizes a map that stays in its default corner).
        sizes.put(MINI,clampSize(MINI,preferences.getFloat(key("overlay_s_",MINI,from),1f)));
    }
    /** Saves the whole layout as this screen shape's own. */
    private void save() {
        if (profile==null) return;
        SharedPreferences.Editor editor=preferences.edit().putBoolean("overlay_profile_"+profile,true);
        for (String name:NAMES) {
            PointF p=centers.get(name); int[] a=anchors.get(name);
            editor.putFloat(key("overlay_x_",name,profile),p.x).putFloat(key("overlay_y_",name,profile),p.y)
                  .putFloat(key("overlay_s_",name,profile),size(name))
                  .putInt(key("overlay_ax_",name,profile),a[0]).putInt(key("overlay_ay_",name,profile),a[1]);
        }
        if (!miniDefault) {
            PointF p=centers.get(MINI); int[] a=anchors.get(MINI);
            editor.putFloat(key("overlay_x_",MINI,profile),p.x).putFloat(key("overlay_y_",MINI,profile),p.y)
                  .putFloat(key("overlay_s_",MINI,profile),size(MINI))
                  .putInt(key("overlay_ax_",MINI,profile),a[0]).putInt(key("overlay_ay_",MINI,profile),a[1]);
        } else if (size(MINI)!=1f) editor.putFloat(key("overlay_s_",MINI,profile),size(MINI));
        else editor.remove(key("overlay_s_",MINI,profile));
        editor.apply();
    }
    /** The layout of the new screen shape (a fold, a window resize). */
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh) {
        super.onSizeChanged(w,h,oldw,oldh);
        if (w<=0 || h<=0) return;
        String p=shapeOf(w,h);
        if (!p.equals(profile)) {
            release(); dragged=null; editPointer=pinchPointer=-1;
            load(p); invalidate();
        }
        notifyMini(false);
    }
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        lastInsets=insets;
        WindowInsets result=super.onApplyWindowInsets(insets);
        notifyMini(false);
        return result;
    }
    private String shapeName() {
        if (getHeight()<=0) return "";
        return String.format(java.util.Locale.ROOT,"%.2f:1",getWidth()/(float)getHeight());
    }
    /** Stored positions are in the 1280x720 layout (saved layouts stay valid). The layout follows the screen's
     *  aspect ratio: each control keeps to the edges it was placed near. Horizontally (on a screen wider than
     *  16:9) a control in the left third follows the left edge, one in the right third the right edge, one in the
     *  middle stays in the middle; vertically (on a screen taller than 16:9) one in the top half follows the top
     *  edge, one in the bottom half the bottom edge. The editor sets a control's edges where it is dropped, on the
     *  screen as shown (overlay_ax_/overlay_ay_: -1 left/top, 0 middle, 1 right/bottom); a layout saved before
     *  that takes them from its 16:9 positions. */
    private final Map<String,int[]> anchors=new HashMap<>();
    private static int anchorX(float x) { return x<1280f/3f ? -1 : x>1280f*2f/3f ? 1 : 0; }
    private static int anchorY(float y) { return y<360f ? -1 : 1; }
    private float extraX() { return Math.max(0f,getWidth()/scale()-1280f); }
    private float extraY() { return Math.max(0f,getHeight()/scale()-720f); }
    private float shift(String name) { return anchors.get(name)[0]*extraX()/2f; }
    private float shiftY(String name) { return anchors.get(name)[1]*extraY()/2f; }
    /** The edges of a control shown at (x, y) (layout units): by its place on the whole screen. */
    private void anchorAt(String name,float x,float y) {
        float ex=extraX(), ey=extraY();
        int[] a=anchors.get(name);
        a[0]=anchorX((x+ex/2f)*1280f/(1280f+ex));
        a[1]=anchorY((y+ey/2f)*720f/(720f+ey));
    }
    private final Map<String,PointF> shown=new HashMap<>();
    /** The control's position on screen (layout units): the stored one plus the edge shifts. */
    private PointF center(String name) {
        PointF stored=centers.get(name), p=shown.get(name);
        if (p==null) { p=new PointF(); shown.put(name,p); }
        // Kept on the screen (a layout saved on another screen shape may put it past an edge).
        float ex=extraX()/2f, ey=extraY()/2f;
        p.set(Math.max(24-ex,Math.min(1256+ex,stored.x+shift(name))),Math.max(24-ey,Math.min(696+ey,stored.y+shiftY(name))));
        return p;
    }
    private float size(String name) { Float s=sizes.get(name); return s==null ? 1f : s; }
    private static float clampSize(float s) { return Math.max(MIN_SIZE,Math.min(MAX_SIZE,s)); }
    /** The mini map's factor keeps its side between 18% and 70% of the screen's shorter side. */
    private static float clampSize(String name,float s) {
        return MINI.equals(name) ? Math.max(MINI_MIN/MINI_BASE,Math.min(MINI_MAX/MINI_BASE,s)) : clampSize(s);
    }
    // The 1280x720 layout drawn at one scale in the centred 16:9 area, as the game (MainActivity):
    // round on any screen shape. Controls may be dragged into the bars around it.
    private float scale() { return Math.max(1e-3f,Math.min(getWidth()/1280f,getHeight()/720f)); }
    private float offsetX() { return (getWidth()-1280f*scale())/2f; }
    private float offsetY() { return (getHeight()-720f*scale())/2f; }
    private float layoutX(MotionEvent e,int index) { return (e.getX(index)-offsetX())/scale(); }
    private float layoutY(MotionEvent e,int index) { return (e.getY(index)-offsetY())/scale(); }
    private static float distance(float x,float y,PointF p) { return (float)Math.hypot(x-p.x,y-p.y); }
    private static boolean stick(String name) { return name.equals("leftstick"); }
    private static boolean rectangle(String name) {
        return name.equals("l1") || name.equals("l2") || name.equals("r1") ||
               name.equals("r2") || name.equals("touchpad") || name.equals("options");
    }
    private PointF dpadCenter() {
        PointF u=center("up"),l=center("left"),r=center("right"),d=center("down");
        return new PointF((u.x+l.x+r.x+d.x)/4f,(u.y+l.y+r.y+d.y)/4f);
    }
    /** The d-pad's size: the mean of its four buttons'. */
    private float dpadSize() { return (size("up")+size("left")+size("right")+size("down"))/4f; }
    private String hit(float x,float y) {
        if (editing && miniOn && MINI.equals(selected) && miniContains(x,y)) return MINI; // the selected map first, even under a button
        String control=hitControl(x,y);
        if (control==null && editing && miniOn && miniContains(x,y)) return MINI;
        return control;
    }
    private String hitControl(float x,float y) {
        PointF dpad=dpadCenter(); float k=dpadSize();
        if (!editing && distance(x,y,dpad)<112*k && distance(center("up").x,center("up").y,dpad)<115*k &&
            distance(center("left").x,center("left").y,dpad)<115*k &&
            distance(center("right").x,center("right").y,dpad)<115*k &&
            distance(center("down").x,center("down").y,dpad)<115*k) return "dpad";
        String winner=null; float best=Float.MAX_VALUE;
        for (String name:NAMES) {
            PointF p=center(name); float s=size(name);
            float dx=Math.abs(x-p.x),dy=Math.abs(y-p.y);
            boolean inside=rectangle(name) ? dx<(name.equals("touchpad") ? 72 : 62)*s && dy<38*s
                    : distance(x,y,p)<(stick(name) ? 70 : name.equals("l3") || name.equals("r3") ? 39 : 48)*s;
            if (inside && distance(x,y,p)<best) { best=distance(x,y,p); winner=name; }
        }
        return winner;
    }
    private static int axis(float v) { return Math.max(0,Math.min(255,Math.round((v+1f)*127.5f))); }

    private void publish() {
        pressed.clear();
        int lx=128,ly=128,rx=128,ry=128,l2=0,r2=0;
        boolean left=false,right=false;
        for (int i=0;i<controls.size();i++) {
            String control=controls.valueAt(i);
            PointF p=positions.get(controls.keyAt(i));
            if (p==null) continue;
            if (control.equals("dpad")) {
                PointF c=dpadCenter(); float t=22*dpadSize();
                if (p.x<c.x-t) pressed.add("left");
                if (p.x>c.x+t) pressed.add("right");
                if (p.y<c.y-t) pressed.add("up");
                if (p.y>c.y+t) pressed.add("down");
            } else if (control.equals("leftstick")) {
                left=true; PointF c=center(control); float range=56*size(control);
                lx=axis(Math.max(-1f,Math.min(1f,(p.x-c.x)/range)));
                ly=axis(Math.max(-1f,Math.min(1f,(p.y-c.y)/range)));
            } else if (control.equals("camera")) {
                right=true;
                rx=axis(Math.max(-1f,Math.min(1f,(p.x-cameraAnchor.x)/CAMERA_RANGE)));
                ry=axis(Math.max(-1f,Math.min(1f,(p.y-cameraAnchor.y)/CAMERA_RANGE)));
            } else if (control.equals("l2")) l2=255;
            else if (control.equals("r2")) r2=255;
            else if (!control.equals("none")) pressed.add(control);
        }
        pad.virtual(pressed,lx,ly,rx,ry,left,right,l2,r2);
        invalidate();
    }
    void release() { controls.clear(); positions.clear(); cameraPointer=-1; publish(); }
    void edit(Runnable done) {
        release(); editing=true; selected=null; onEditDone=done; invalidate();
        if (miniListener!=null) miniListener.editing(true);
    }
    boolean isEditing() { return editing; }
    private void finishEdit() {
        editPointer=pinchPointer=-1; dragged=selected=null; editing=false; invalidate();
        notifyMini(false);
        if (miniListener!=null) miniListener.editing(false);
        if (onEditDone!=null) onEditDone.run();
        onEditDone=null;
    }
    /** The default layout for this screen shape (saved as its own); before the overlay has a size, the shared one. */
    void resetLayout() {
        SharedPreferences.Editor editor=preferences.edit();
        for (int i=0;i<NAMES.length;i++) {
            String name=NAMES[i]; centers.get(name).set(DEFAULT[i][0],DEFAULT[i][1]); sizes.put(name,1f);
            anchors.put(name,new int[]{anchorX(DEFAULT[i][0]),anchorY(DEFAULT[i][1])});
            if (profile==null) editor.remove("overlay_x_"+name).remove("overlay_y_"+name).remove("overlay_s_"+name)
                                     .remove("overlay_ax_"+name).remove("overlay_ay_"+name);
        }
        miniDefault=true; sizes.put(MINI,1f); anchors.put(MINI,new int[]{1,-1}); // back to the top right corner
        for (String base:new String[]{"overlay_x_","overlay_y_","overlay_s_","overlay_ax_","overlay_ay_"}) editor.remove(key(base,MINI,profile));
        editor.apply(); save(); release(); invalidate(); notifyMini(false);
    }
    private void confirmReset() {
        new android.app.AlertDialog.Builder(getContext()).setTitle("Reset controller layout?")
            .setMessage("All buttons"+(miniOn ? " and the mini map" : "")+" go back to their default places and sizes on this screen shape ("+shapeName()+").")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Reset",(d,w) -> { resetLayout(); selected=null; invalidate(); }).show();
    }
    private void resize(String name,float s) {
        if (MINI.equals(name)) materializeMini();
        sizes.put(name,clampSize(name,s));
        if (profile==null) preferences.edit().putFloat("overlay_s_"+name,size(name)).apply(); else save();
        invalidate();
    }

    // The editor's bar at the top: RESET (asks first), − (smaller), DONE, + (larger).
    private static final float BAR_TOP=9, BAR_BOTTOM=62;
    private static final float[] RESET={358,458}, MINUS={468,538}, DONE={548,732}, PLUS={742,812};
    private static boolean in(float[] box,float x,float y) { return x>box[0] && x<box[1] && y<BAR_BOTTOM+10; }

    private boolean editTouch(MotionEvent e,int action,int at,int id) {
        if (action==MotionEvent.ACTION_CANCEL) { editPointer=pinchPointer=-1; dragged=null; return true; }
        if (action==MotionEvent.ACTION_DOWN) {
            float x=layoutX(e,at),y=layoutY(e,at);
            if (in(DONE,x,y)) { finishEdit(); return true; }
            if (in(RESET,x,y)) { confirmReset(); return true; }
            if (in(MINUS,x,y) || in(PLUS,x,y)) {
                if (selected!=null) resize(selected,size(selected)*(in(PLUS,x,y) ? SIZE_STEP : 1f/SIZE_STEP));
                return true;
            }
            dragged=hit(x,y); editPointer=dragged==null ? -1 : id;
            if (dragged!=null) selected=dragged;
            if (MINI.equals(dragged)) materializeMini();
            invalidate();
        } else if (action==MotionEvent.ACTION_POINTER_DOWN && dragged!=null && pinchPointer<0) {
            // A second finger while one holds a control: pinch to resize it.
            int index=e.findPointerIndex(editPointer);
            if (index>=0) {
                pinchPointer=id;
                pinchDistance=Math.max(1f,(float)Math.hypot(e.getX(at)-e.getX(index),e.getY(at)-e.getY(index)));
                pinchSize=size(dragged);
            }
        } else if (action==MotionEvent.ACTION_MOVE && editPointer>=0 && dragged!=null) {
            int index=e.findPointerIndex(editPointer);
            int other=pinchPointer<0 ? -1 : e.findPointerIndex(pinchPointer);
            if (index>=0 && other>=0) {
                float now=(float)Math.hypot(e.getX(other)-e.getX(index),e.getY(other)-e.getY(index));
                sizes.put(dragged,clampSize(dragged,pinchSize*now/pinchDistance));
            } else if (index>=0) {
                float barX=offsetX()/scale(),barY=offsetY()/scale();
                float x=Math.max(36-barX,Math.min(1244+barX,layoutX(e,index)));
                float y=Math.max(36-barY,Math.min(684+barY,layoutY(e,index)));
                anchorAt(dragged,x,y); // the edges where it is now; the stored position is relative to them
                centers.get(dragged).set(x-shift(dragged),y-shiftY(dragged));
            }
            invalidate();
        } else if (action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {
            if (id==pinchPointer) {
                pinchPointer=-1;
                if (dragged!=null) resize(dragged,size(dragged));
            } else if (id==editPointer && dragged!=null) {
                if (profile==null) {
                    PointF p=centers.get(dragged); int[] a=anchors.get(dragged);
                    preferences.edit().putFloat("overlay_x_"+dragged,p.x).putFloat("overlay_y_"+dragged,p.y)
                        .putInt("overlay_ax_"+dragged,a[0]).putInt("overlay_ay_"+dragged,a[1])
                        .putFloat("overlay_s_"+dragged,size(dragged)).apply();
                } else save(); // this screen shape's own layout
                editPointer=pinchPointer=-1; dragged=null;
                invalidate();
            }
        }
        return true;
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action=e.getActionMasked(),at=e.getActionIndex(),id=e.getPointerId(at);
        if (editing) return editTouch(e,action,at,id);
        if (action==MotionEvent.ACTION_CANCEL) { release(); return true; }
        if (action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN) {
            float x=layoutX(e,at),y=layoutY(e,at);
            String name=hit(x,y);
            // Outside every control: the camera, for one finger at a time (more fingers there do nothing).
            if (name==null) {
                if (cameraPointer<0) { name="camera"; cameraPointer=id; cameraAnchor.set(x,y); }
                else name="none";
            }
            controls.put(id,name);
            positions.put(id,new PointF(x,y));
        } else if (action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {
            controls.remove(id); positions.remove(id);
            if (id==cameraPointer) cameraPointer=-1;
        }
        if (action==MotionEvent.ACTION_MOVE) {
            for (int i=0;i<e.getPointerCount();i++) {
                PointF p=positions.get(e.getPointerId(i));
                if (p!=null) p.set(layoutX(e,i),layoutY(e,i));
            }
        }
        publish(); return true;
    }

    private int fill(String name) {
        return name.equals(dragged) ? 0xffe8dfc9 : pressed.contains(name) ? 0xaae8dfc9 : 0x6630394c;
    }
    /** The control the editor resizes: an outline around it. */
    private void selection(Canvas c,String name,float halfWidth,float halfHeight) {
        if (!editing || !name.equals(selected)) return;
        PointF p=center(name);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(3); paint.setColor(0xffe8dfc9);
        c.drawRoundRect(p.x-halfWidth-8,p.y-halfHeight-8,p.x+halfWidth+8,p.y+halfHeight+8,16,16,paint);
    }
    private void circle(Canvas c,String name,float r,String label,int accent) {
        PointF p=center(name); float s=size(name); r*=s;
        paint.setStyle(Paint.Style.FILL); paint.setColor(fill(name));
        c.drawCircle(p.x,p.y,r,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(3); paint.setColor(accent);
        c.drawCircle(p.x,p.y,r,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextSize((label.length()>2 ? 19 : 33)*s); paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(label,p.x,p.y-(paint.ascent()+paint.descent())/2,paint);
        selection(c,name,r,r);
    }
    private void rectangle(Canvas c,String name,float width,String label) {
        PointF p=center(name); float s=size(name), w=width*s/2, h=25*s;
        paint.setStyle(Paint.Style.FILL); paint.setColor(fill(name));
        c.drawRoundRect(p.x-w,p.y-h,p.x+w,p.y+h,14*s,14*s,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setColor(0xbbc6ad76); paint.setStrokeWidth(2);
        c.drawRoundRect(p.x-w,p.y-h,p.x+w,p.y+h,14*s,14*s,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextSize(19*s); paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(label,p.x,p.y-(paint.ascent()+paint.descent())/2,paint);
        selection(c,name,w,h);
    }
    private void drawStick(Canvas c,String name) {
        PointF center=center(name); float s=size(name), travel=27*s;
        paint.setStyle(Paint.Style.FILL); paint.setColor(name.equals(dragged) ? 0xffe8dfc9 : 0x6630394c);
        c.drawCircle(center.x,center.y,57*s,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setColor(0xffa0aec0); paint.setStrokeWidth(3);
        c.drawCircle(center.x,center.y,57*s,paint);
        float knobX=center.x,knobY=center.y;
        for (int i=0;i<controls.size();i++) {
            if (name.equals(controls.valueAt(i))) {
                PointF p=positions.get(controls.keyAt(i));
                if (p!=null) {
                    knobX=center.x+Math.max(-travel,Math.min(travel,p.x-center.x));
                    knobY=center.y+Math.max(-travel,Math.min(travel,p.y-center.y));
                }
                break;
            }
        }
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xbba0aec0);
        c.drawCircle(knobX,knobY,28*s,paint);
        paint.setColor(0xff15202c); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(22*s);
        c.drawText("L",knobX,knobY-(paint.ascent()+paint.descent())/2,paint);
        selection(c,name,57*s,57*s);
    }
    /** The camera touch: a faint ring where it landed and the knob under the finger. */
    private void drawCamera(Canvas c) {
        PointF p=cameraPointer<0 ? null : positions.get(cameraPointer);
        if (p==null) return;
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(3); paint.setColor(0x66a0aec0);
        c.drawCircle(cameraAnchor.x,cameraAnchor.y,CAMERA_RANGE,paint);
        float dx=p.x-cameraAnchor.x,dy=p.y-cameraAnchor.y,length=(float)Math.hypot(dx,dy);
        float k=length>CAMERA_RANGE ? CAMERA_RANGE/length : 1f;
        paint.setStyle(Paint.Style.FILL); paint.setColor(0x55a0aec0);
        c.drawCircle(cameraAnchor.x+dx*k,cameraAnchor.y+dy*k,28,paint);
    }
    private void barButton(Canvas c,float[] box,String label,boolean enabled) {
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xcc15171d);
        c.drawRoundRect(box[0],BAR_TOP,box[1],BAR_BOTTOM,13,13,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2); paint.setColor(enabled ? 0xffc6ad76 : 0x66c6ad76);
        c.drawRoundRect(box[0],BAR_TOP,box[1],BAR_BOTTOM,13,13,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTextSize(label.length()>1 ? 20 : 30);
        paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(label,(box[0]+box[1])/2,(BAR_TOP+BAR_BOTTOM)/2-(paint.ascent()+paint.descent())/2,paint);
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (editing && miniOn) drawMiniEditor(c); // beneath the controls
        c.save(); c.translate(offsetX(),offsetY()); c.scale(scale(),scale());
        rectangle(c,"l2",108,"L2"); rectangle(c,"l1",108,"L1");
        rectangle(c,"r1",108,"R1"); rectangle(c,"r2",108,"R2");
        rectangle(c,"touchpad",126,"TOUCH"); rectangle(c,"options",108,"OPTIONS");
        circle(c,"up",39,"▲",0xffc6ad76); circle(c,"left",39,"◀",0xffc6ad76);
        circle(c,"right",39,"▶",0xffc6ad76); circle(c,"down",39,"▼",0xffc6ad76);
        circle(c,"triangle",39,"△",0xff83cdaa); circle(c,"circle",39,"○",0xffe38288);
        circle(c,"cross",39,"×",0xff88aaff); circle(c,"square",39,"□",0xffd6a0ce);
        drawStick(c,"leftstick"); drawCamera(c);
        circle(c,"l3",29,"L3",0xffa0aec0); circle(c,"r3",29,"R3",0xffa0aec0);
        if (editing) {
            barButton(c,RESET,"RESET",true); barButton(c,MINUS,"−",selected!=null); barButton(c,DONE,"DONE",true); barButton(c,PLUS,"+",selected!=null);
            paint.setStyle(Paint.Style.FILL); paint.setTextSize(17); paint.setColor(0xffe8dfc9);
            paint.setTextAlign(Paint.Align.CENTER);
            c.drawText(selected==null ? "Drag a button to move it" : "Drag to move · pinch or −/+ to resize",640,BAR_BOTTOM+110,paint);
            paint.setTextSize(14); paint.setColor(0xb0e8dfc9);
            c.drawText("Layout for this screen shape ("+shapeName()+"), used whenever the screen has this shape",640,BAR_BOTTOM+136,paint);
        }
        c.restore();
    }

    // ---- the mini map ----

    void setMiniListener(MiniListener l) { miniListener=l; miniLeft=-1; notifyMini(true); }
    /** The floating map is on: the editor offers its place. */
    void setMiniMap(boolean on) { if (miniOn!=on) { miniOn=on; if (!on && MINI.equals(selected)) selected=null; invalidate(); } }

    /** The mini map's side as a percentage of the screen's shorter side (18 to 70; 32 by default), for the Hunter's Atlas
     *  menu's Size slider. The overlay's own size, so the editor and the slider show and save the same value. */
    int miniPercent() {
        if (!miniRect(miniPx)) return Math.round(MINI_BASE*100*size(MINI));
        return Math.round(miniPx.width()*100f/Math.min(getWidth(),getHeight()));
    }
    /** Sets the mini map's side (percent of the shorter side, clamped to 18 to 70). The corner or edge it is anchored to
     *  stays where it is (a top right map grows down and left), it stays on the screen, the layout is saved as the editor
     *  saves it, and the listener moves the floating map at once. */
    void setMiniPercent(int percent) {
        float factor=clampSize(MINI,percent/100f/MINI_BASE);
        if (miniDefault || !miniRect(miniPx)) sizes.put(MINI,factor); // the default is the top right corner whatever the size
        else {
            float oldLeft=miniPx.left, oldTop=miniPx.top, oldRight=miniPx.right, oldBottom=miniPx.bottom, oldCx=miniPx.centerX(), oldCy=miniPx.centerY();
            sizes.put(MINI,factor);
            float shorter=Math.min(getWidth(),getHeight());
            float side=Math.max(MINI_MIN*shorter,Math.min(MINI_MAX*shorter,factor*MINI_BASE*shorter));
            int[] a=anchors.get(MINI);
            float left=a[0]>0 ? oldRight-side : a[0]<0 ? oldLeft : oldCx-side/2;
            float top=a[1]>0 ? oldBottom-side : a[1]<0 ? oldTop : oldCy-side/2;
            left=Math.max(0,Math.min(getWidth()-side,left)); top=Math.max(0,Math.min(getHeight()-side,top));
            float lx=(left+side/2-offsetX())/scale(), ly=(top+side/2-offsetY())/scale();
            centers.get(MINI).set(lx-shift(MINI),ly-shiftY(MINI));
        }
        if (profile==null) {
            SharedPreferences.Editor editor=preferences.edit();
            if (miniDefault) editor.putFloat("overlay_s_"+MINI,size(MINI));
            else {
                PointF p=centers.get(MINI); int[] a=anchors.get(MINI);
                editor.putFloat("overlay_x_"+MINI,p.x).putFloat("overlay_y_"+MINI,p.y).putFloat("overlay_s_"+MINI,size(MINI))
                      .putInt("overlay_ax_"+MINI,a[0]).putInt("overlay_ay_"+MINI,a[1]);
            }
            editor.apply();
        } else save();
        notifyMini(false); invalidate();
    }

    private float density() { return getResources().getDisplayMetrics().density; }
    /** The top right default's distance to the screen's top and right edges: 12 dp, and past a display cutout. */
    private float[] miniInsets() {
        float top=0, right=0;
        WindowInsets wi=lastInsets!=null ? lastInsets : getRootWindowInsets();
        if (wi!=null && wi.getDisplayCutout()!=null) { top=wi.getDisplayCutout().getSafeInsetTop(); right=wi.getDisplayCutout().getSafeInsetRight(); }
        float margin=12*density();
        return new float[]{margin+top,margin+right};
    }
    /** The mini map's square in this view's pixels (kept on the screen). False before the view has a size. */
    private boolean miniRect(RectF r) {
        int w=getWidth(), h=getHeight();
        if (w<=0 || h<=0) return false;
        float shorter=Math.min(w,h), sc=scale();
        float side=Math.max(MINI_MIN*shorter,Math.min(MINI_MAX*shorter,size(MINI)*MINI_BASE*shorter));
        float cx, cy;
        if (miniDefault) {
            float[] inset=miniInsets();
            cx=w-inset[1]-side/2; cy=inset[0]+side/2;
        } else {
            PointF stored=centers.get(MINI);
            cx=offsetX()+(stored.x+shift(MINI))*sc; cy=offsetY()+(stored.y+shiftY(MINI))*sc;
        }
        float left=Math.max(0,Math.min(w-side,cx-side/2)), top=Math.max(0,Math.min(h-side,cy-side/2));
        r.set(left,top,left+side,top+side);
        return true;
    }
    private boolean miniContains(float layoutX,float layoutY) {
        if (!miniRect(miniPx)) return false;
        return miniPx.contains(offsetX()+layoutX*scale(),offsetY()+layoutY*scale());
    }
    /** Editing a default-placed map: stores where it is now (top right edges) so that it can be moved and resized. */
    private void materializeMini() {
        if (!miniDefault || !miniRect(miniPx)) return;
        float lx=(miniPx.centerX()-offsetX())/scale(), ly=(miniPx.centerY()-offsetY())/scale();
        anchors.put(MINI,new int[]{1,-1});
        centers.get(MINI).set(lx-shift(MINI),ly-shiftY(MINI));
        miniDefault=false;
    }
    private void notifyMini(boolean force) {
        if (miniListener==null || !miniRect(miniPx)) return;
        int l=Math.round(miniPx.left), t=Math.round(miniPx.top), side=Math.round(miniPx.width());
        if (!force && l==miniLeft && t==miniTop && side==miniSide) return;
        miniLeft=l; miniTop=t; miniSide=side;
        miniListener.rect(l,t,side);
    }
    /** The editor's picture of the mini map: a labelled rectangle at its rect. */
    private void drawMiniEditor(Canvas c) {
        if (!miniRect(miniPx)) return;
        float d=density(), r=10*d;
        boolean chosen=MINI.equals(selected), held=MINI.equals(dragged);
        paint.setStyle(Paint.Style.FILL); paint.setColor(held ? 0x88e8dfc9 : 0x5530394c);
        c.drawRoundRect(miniPx,r,r,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(chosen ? 3*d : 2*d); paint.setColor(chosen ? 0xffe8dfc9 : 0xbbc6ad76);
        c.drawRoundRect(miniPx,r,r,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Typeface.DEFAULT_BOLD); paint.setColor(0xffe8dfc9);
        paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(16*d);
        float width=paint.measureText("Mini map");
        if (width>miniPx.width()*.9f) paint.setTextSize(16*d*miniPx.width()*.9f/width);
        c.drawText("Mini map",miniPx.centerX(),miniPx.centerY()-(paint.ascent()+paint.descent())/2,paint);
    }
}
