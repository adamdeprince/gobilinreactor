package dev.goblinlinux.sentry;

import android.content.Context;
import android.opengl.GLSurfaceView;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.text.InputType;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

final class KittyView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private int width, height, rows, columns;
    volatile long renderedFrames;
    private float downY, lastY;
    private float fontSize, gestureFontSize;
    private boolean multiTouch, physicalKeyboard;
    private final ScaleGestureDetector scaleGesture;
    private int latchedModifiers;
    Runnable modifiersChanged;
    void toggleModifier(int modifier) {
        latchedModifiers ^= modifier;
        if (modifiersChanged != null) modifiersChanged.run();
        requestFocus();
    }
    boolean modifierSelected(int modifier) { return (latchedModifiers & modifier) != 0; }
    void resetModifiers() { latchedModifiers = 0; if (modifiersChanged != null) modifiersChanged.run(); }
    void sendKey(int key, int modifiers, String text) {
        modifiers |= latchedModifiers;
        resetModifiers();
        KittyRuntime.keyNative(key, modifiers, (modifiers & 6) != 0 ? "" : text);
        requestFocus(); requestRender();
    }
    void specialKey(int key) { sendKey(key, 0, ""); }
    void terminalChanged() { rows = columns = 0; resetModifiers(); requestFocus(); requestRender(); }
    float fontSize() { return fontSize; }
    void setPhysicalKeyboard(boolean present) {
        physicalKeyboard = present;
        if (present) resetModifiers();
    }
    private void resizeFont(float size) {
        fontSize = Math.max(6f, Math.min(36f, Math.round(size * 4f) / 4f));
        KittyRuntime.fontSizeNative(fontSize);
        requestRender();
    }
    KittyView(Context context) {
        super(context); setEGLContextClientVersion(3); setPreserveEGLContextOnPause(true);
        setRenderer(this); setRenderMode(RENDERMODE_WHEN_DIRTY); setFocusableInTouchMode(true);
        setContentDescription("GoblinReactor terminal");
        resizeFont(context.getSharedPreferences("terminal", Context.MODE_PRIVATE).getFloat("font-size", 12f));
        scaleGesture = new ScaleGestureDetector(context, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScaleBegin(ScaleGestureDetector detector) {
                gestureFontSize = fontSize; return true;
            }
            @Override public boolean onScale(ScaleGestureDetector detector) {
                gestureFontSize = Math.max(6f, Math.min(36f, gestureFontSize * detector.getScaleFactor()));
                resizeFont(gestureFontSize); return true;
            }
            @Override public void onScaleEnd(ScaleGestureDetector detector) {
                context.getSharedPreferences("terminal", Context.MODE_PRIVATE).edit().putFloat("font-size", fontSize).apply();
            }
        });
        scaleGesture.setQuickScaleEnabled(false);
        scaleGesture.setStylusScaleEnabled(false);
    }
    @Override public boolean onCheckIsTextEditor() { return true; }
    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        info.inputType=InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS|InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        info.imeOptions=EditorInfo.IME_ACTION_NONE|EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        return new BaseInputConnection(this, false) {
            // Keep pre-edit text locally; only committed text enters the PTY.
            @Override public boolean commitText(CharSequence text, int cursor) {
                getEditable().clear();
                text.toString().codePoints().forEach(c -> sendKey(c,0,new String(Character.toChars(c))));
                return true;
            }
            @Override public boolean deleteSurroundingText(int before, int after) {
                if (getEditable().length()!=0) return super.deleteSurroundingText(before,after);
                for(int i=0;i<Math.min(before,1024);i++) sendKey(57347,0,"");
                for(int i=0;i<Math.min(after,1024);i++) sendKey(57349,0,"");
                return true;
            }
            @Override public boolean sendKeyEvent(KeyEvent event) {
                return event.getAction()!=KeyEvent.ACTION_DOWN || onKeyDown(event.getKeyCode(),event);
            }
            @Override public boolean performEditorAction(int action) {
                sendKey(57345,0,""); return true;
            }
        };
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) multiTouch = false;
        if (event.getPointerCount() > 1) multiTouch = true;
        scaleGesture.onTouchEvent(event);
        // Consume the complete pinch, including the last finger lifting, so it
        // neither scrolls history nor opens the software keyboard as a tap.
        if (multiTouch || action == MotionEvent.ACTION_CANCEL) return true;
        if(action==MotionEvent.ACTION_DOWN) { downY=lastY=event.getY(); return true; }
        if(action==MotionEvent.ACTION_MOVE) {
            int lines=(int)((event.getY()-lastY)/Math.max(1,height/Math.max(1,rows)));
            if(lines!=0) { KittyRuntime.scrollNative(lines);lastY=event.getY();requestRender(); }
            return true;
        }
        if(action==MotionEvent.ACTION_UP) {
            if(Math.abs(event.getY()-downY)<12) {
                performClick(); requestFocus();
                if (!physicalKeyboard) ((InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE)).showSoftInput(this,InputMethodManager.SHOW_IMPLICIT);
            }
            return true;
        }
        return true;
    }
    @Override public boolean performClick() { super.performClick(); return true; }
    public void onSurfaceCreated(GL10 gl, EGLConfig config) { KittyRuntime.contextNative(); }
    public void onSurfaceChanged(GL10 gl, int width, int height) { this.width=width; this.height=height; }
    public void onDrawFrame(GL10 gl) {
        int[] size=KittyRuntime.drawNative(width,height);
        if(width>0&&height>0) renderedFrames++;
        if(size[0]!=rows||size[1]!=columns) { rows=size[0];columns=size[1];SessionService.resizeNative(rows,columns); }
    }
    @Override public boolean onKeyDown(int code, KeyEvent event) {
        int key=0;
        switch(code) {
            case KeyEvent.KEYCODE_DPAD_UP:key=57352;break;
            case KeyEvent.KEYCODE_DPAD_DOWN:key=57353;break;
            case KeyEvent.KEYCODE_DPAD_LEFT:key=57350;break;
            case KeyEvent.KEYCODE_DPAD_RIGHT:key=57351;break;
            case KeyEvent.KEYCODE_ENTER:key=57345;break;
            case KeyEvent.KEYCODE_DEL:key=57347;break;
            case KeyEvent.KEYCODE_ESCAPE:key=57344;break;
            case KeyEvent.KEYCODE_TAB:key=57346;break;
            case KeyEvent.KEYCODE_FORWARD_DEL:key=57349;break;
            case KeyEvent.KEYCODE_MOVE_HOME:key=57356;break;
            case KeyEvent.KEYCODE_MOVE_END:key=57357;break;
            case KeyEvent.KEYCODE_PAGE_UP:key=57354;break;
            case KeyEvent.KEYCODE_PAGE_DOWN:key=57355;break;
            default: key=event.getUnicodeChar(event.getMetaState() & ~KeyEvent.META_CTRL_MASK & ~KeyEvent.META_ALT_MASK);
        }
        if(key==0)return super.onKeyDown(code,event);
        int mods=(event.isShiftPressed()?1:0)|(event.isAltPressed()?2:0)|(event.isCtrlPressed()?4:0);
        String text=key<57344&&key>=32&&!event.isCtrlPressed()&&!event.isAltPressed()?new String(Character.toChars(key)):"";
        sendKey(key,mods,text);return true;
    }
}
