package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PointF;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** Translucent PlayStation-style controls with a persistent, draggable layout. */
final class TouchControllerOverlay extends View {
    private static final String[] NAMES={
        "l2","l1","touchpad","options","r1","r2",
        "up","left","right","down","triangle","circle","cross","square",
        "leftstick","l3","r3","rightstick"
    };
    private static final float[][] DEFAULT={
        {87,105},{213,105},{640,105},{773,105},{1067,105},{1193,105},
        {160,395},{90,465},{230,465},{160,535},
        {1120,405},{1190,475},{1120,545},{1050,475},
        {320,612},{420,614},{860,614},{960,612}
    };
    private final PadBridge pad;
    private final SharedPreferences preferences;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<String,PointF> centers=new LinkedHashMap<>();
    private final SparseArray<String> controls=new SparseArray<>();
    private final SparseArray<PointF> positions=new SparseArray<>();
    private final Set<String> pressed=new LinkedHashSet<>();
    private boolean mouseDown,editing;
    private int editPointer=-1;
    private String dragged;
    private Runnable onEditDone;

    TouchControllerOverlay(Context context,PadBridge pad) {
        super(context); this.pad=pad;
        preferences=context.getSharedPreferences("bloodborne",Context.MODE_PRIVATE);
        for (int i=0;i<NAMES.length;i++) {
            String name=NAMES[i];
            centers.put(name,new PointF(preferences.getFloat("overlay_x_"+name,DEFAULT[i][0]),
                                        preferences.getFloat("overlay_y_"+name,DEFAULT[i][1])));
        }
        setContentDescription("On-screen PlayStation controller");
    }
    private PointF center(String name) { return centers.get(name); }
    private static float distance(float x,float y,PointF p) { return (float)Math.hypot(x-p.x,y-p.y); }
    private static boolean stick(String name) { return name.equals("leftstick") || name.equals("rightstick"); }
    private static boolean rectangle(String name) {
        return name.equals("l1") || name.equals("l2") || name.equals("r1") ||
               name.equals("r2") || name.equals("touchpad") || name.equals("options");
    }
    private PointF dpadCenter() {
        PointF u=center("up"),l=center("left"),r=center("right"),d=center("down");
        return new PointF((u.x+l.x+r.x+d.x)/4f,(u.y+l.y+r.y+d.y)/4f);
    }
    private String hit(float x,float y) {
        PointF dpad=dpadCenter();
        if (!editing && distance(x,y,dpad)<112 && distance(center("up").x,center("up").y,dpad)<115 &&
            distance(center("left").x,center("left").y,dpad)<115 &&
            distance(center("right").x,center("right").y,dpad)<115 &&
            distance(center("down").x,center("down").y,dpad)<115) return "dpad";
        String winner=null; float best=Float.MAX_VALUE;
        for (String name:NAMES) {
            PointF p=center(name);
            float dx=Math.abs(x-p.x),dy=Math.abs(y-p.y);
            boolean inside=rectangle(name) ? dx<(name.equals("touchpad") ? 72 : 62) && dy<38
                    : distance(x,y,p)<(stick(name) ? 70 : name.equals("l3") || name.equals("r3") ? 39 : 48);
            if (inside && distance(x,y,p)<best) { best=distance(x,y,p); winner=name; }
        }
        return winner;
    }
    private static int axis(float v) { return Math.max(0,Math.min(255,Math.round((v+1f)*127.5f))); }

    private void publish() {
        pressed.clear();
        int lx=128,ly=128,rx=128,ry=128,l2=0,r2=0;
        boolean left=false,right=false,mouse=false;
        for (int i=0;i<controls.size();i++) {
            String control=controls.valueAt(i);
            PointF p=positions.get(controls.keyAt(i));
            if (p==null) continue;
            if (control.equals("dpad")) {
                PointF c=dpadCenter();
                if (p.x<c.x-22) pressed.add("left");
                if (p.x>c.x+22) pressed.add("right");
                if (p.y<c.y-22) pressed.add("up");
                if (p.y>c.y+22) pressed.add("down");
            } else if (control.equals("leftstick")) {
                left=true; PointF c=center(control);
                lx=axis(Math.max(-1f,Math.min(1f,(p.x-c.x)/56f)));
                ly=axis(Math.max(-1f,Math.min(1f,(p.y-c.y)/56f)));
            } else if (control.equals("rightstick")) {
                right=true; PointF c=center(control);
                rx=axis(Math.max(-1f,Math.min(1f,(p.x-c.x)/56f)));
                ry=axis(Math.max(-1f,Math.min(1f,(p.y-c.y)/56f)));
            } else if (control.equals("l2")) l2=255;
            else if (control.equals("r2")) r2=255;
            else if (control.equals("mouse")) {
                if (!mouse) XInput.moveTo(Math.round(p.x),Math.round(p.y));
                mouse=true;
            } else pressed.add(control);
        }
        if (mouse!=mouseDown) { XInput.button(1,mouse); mouseDown=mouse; }
        pad.virtual(pressed,lx,ly,rx,ry,left,right,l2,r2);
        invalidate();
    }
    void release() { controls.clear(); positions.clear(); publish(); }
    void edit(Runnable done) { release(); editing=true; onEditDone=done; invalidate(); }
    private void finishEdit() {
        editPointer=-1; dragged=null; editing=false; invalidate();
        if (onEditDone!=null) onEditDone.run();
        onEditDone=null;
    }
    void resetLayout() {
        SharedPreferences.Editor editor=preferences.edit();
        for (int i=0;i<NAMES.length;i++) {
            String name=NAMES[i]; centers.get(name).set(DEFAULT[i][0],DEFAULT[i][1]);
            editor.remove("overlay_x_"+name).remove("overlay_y_"+name);
        }
        editor.apply(); release(); invalidate();
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action=e.getActionMasked(),at=e.getActionIndex(),id=e.getPointerId(at);
        if (editing) {
            if (action==MotionEvent.ACTION_CANCEL) { editPointer=-1; dragged=null; return true; }
            if (action==MotionEvent.ACTION_DOWN) {
                float x=e.getX(at)*1280f/getWidth(),y=e.getY(at)*720f/getHeight();
                if (x>548 && x<732 && y<72) { finishEdit(); return true; }
                dragged=hit(x,y); editPointer=dragged==null ? -1 : id;
            } else if (action==MotionEvent.ACTION_MOVE && editPointer>=0 && dragged!=null) {
                int index=e.findPointerIndex(editPointer);
                if (index>=0) center(dragged).set(
                    Math.max(36,Math.min(1244,e.getX(index)*1280f/getWidth())),
                    Math.max(36,Math.min(684,e.getY(index)*720f/getHeight())));
                invalidate();
            } else if ((action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP)
                       && id==editPointer && dragged!=null) {
                PointF p=center(dragged);
                preferences.edit().putFloat("overlay_x_"+dragged,p.x)
                    .putFloat("overlay_y_"+dragged,p.y).apply();
                editPointer=-1; dragged=null;
            }
            return true;
        }
        if (action==MotionEvent.ACTION_CANCEL) { release(); return true; }
        if (action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN) {
            float x=e.getX(at)*1280f/getWidth(),y=e.getY(at)*720f/getHeight();
            String name=hit(x,y);
            controls.put(id,name==null ? "mouse" : name);
            positions.put(id,new PointF(x,y));
        } else if (action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {
            controls.remove(id); positions.remove(id);
        }
        if (action==MotionEvent.ACTION_MOVE) {
            for (int i=0;i<e.getPointerCount();i++) {
                PointF p=positions.get(e.getPointerId(i));
                if (p!=null) p.set(e.getX(i)*1280f/getWidth(),e.getY(i)*720f/getHeight());
            }
        }
        publish(); return true;
    }

    private void circle(Canvas c,String name,float r,String label,int accent) {
        PointF p=center(name);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(name.equals(dragged) ? 0xffe8dfc9 : pressed.contains(name) ? 0xaae8dfc9 : 0x6630394c);
        c.drawCircle(p.x,p.y,r,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(3); paint.setColor(accent);
        c.drawCircle(p.x,p.y,r,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextSize(label.length()>2 ? 19 : 33); paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(label,p.x,p.y-(paint.ascent()+paint.descent())/2,paint);
    }
    private void rectangle(Canvas c,String name,float width,String label) {
        PointF p=center(name);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(name.equals(dragged) ? 0xffe8dfc9 : pressed.contains(name) ? 0xaae8dfc9 : 0x6630394c);
        c.drawRoundRect(p.x-width/2,p.y-25,p.x+width/2,p.y+25,14,14,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setColor(0xbbc6ad76); paint.setStrokeWidth(2);
        c.drawRoundRect(p.x-width/2,p.y-25,p.x+width/2,p.y+25,14,14,paint);
        paint.setStyle(Paint.Style.FILL); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextSize(19); paint.setTextAlign(Paint.Align.CENTER);
        c.drawText(label,p.x,p.y-(paint.ascent()+paint.descent())/2,paint);
    }
    private void drawStick(Canvas c,String name) {
        PointF center=center(name);
        paint.setStyle(Paint.Style.FILL); paint.setColor(name.equals(dragged) ? 0xffe8dfc9 : 0x6630394c);
        c.drawCircle(center.x,center.y,57,paint);
        paint.setStyle(Paint.Style.STROKE); paint.setColor(0xffa0aec0); paint.setStrokeWidth(3);
        c.drawCircle(center.x,center.y,57,paint);
        float knobX=center.x,knobY=center.y;
        for (int i=0;i<controls.size();i++) {
            if (name.equals(controls.valueAt(i))) {
                PointF p=positions.get(controls.keyAt(i));
                if (p!=null) {
                    knobX=center.x+Math.max(-27,Math.min(27,p.x-center.x));
                    knobY=center.y+Math.max(-27,Math.min(27,p.y-center.y));
                }
                break;
            }
        }
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xbba0aec0);
        c.drawCircle(knobX,knobY,28,paint);
        paint.setColor(0xff15202c); paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(22);
        c.drawText(name.equals("leftstick") ? "L" : "R",knobX,
                   knobY-(paint.ascent()+paint.descent())/2,paint);
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c); c.save(); c.scale(getWidth()/1280f,getHeight()/720f);
        rectangle(c,"l2",108,"L2"); rectangle(c,"l1",108,"L1");
        rectangle(c,"r1",108,"R1"); rectangle(c,"r2",108,"R2");
        rectangle(c,"touchpad",126,"TOUCH"); rectangle(c,"options",108,"OPTIONS");
        circle(c,"up",39,"▲",0xffc6ad76); circle(c,"left",39,"◀",0xffc6ad76);
        circle(c,"right",39,"▶",0xffc6ad76); circle(c,"down",39,"▼",0xffc6ad76);
        circle(c,"triangle",39,"△",0xff83cdaa); circle(c,"circle",39,"○",0xffe38288);
        circle(c,"cross",39,"×",0xff88aaff); circle(c,"square",39,"□",0xffd6a0ce);
        drawStick(c,"leftstick"); drawStick(c,"rightstick");
        circle(c,"l3",29,"L3",0xffa0aec0); circle(c,"r3",29,"R3",0xffa0aec0);
        if (editing) {
            paint.setStyle(Paint.Style.FILL); paint.setColor(0xcc15171d);
            c.drawRoundRect(548,9,732,62,13,13,paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2); paint.setColor(0xffc6ad76);
            c.drawRoundRect(548,9,732,62,13,13,paint);
            paint.setStyle(Paint.Style.FILL); paint.setTextSize(20);
            paint.setTextAlign(Paint.Align.CENTER);
            c.drawText("DONE",640,36-(paint.ascent()+paint.descent())/2,paint);
        }
        c.restore();
    }
}
