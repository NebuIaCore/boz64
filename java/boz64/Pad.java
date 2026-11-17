// Game controller, keyboard and mouse input for the 64-bit port (the layouts are described in src/pad.cpp).
// Every activity's window callback is wrapped so controller, keyboard and mouse events are seen first. The
// controller's state goes to the native side (src/pad.cpp), which applies it during play. Outside play (menus,
// the pause menu) the left stick or D-pad moves an on-screen cursor instead: A touches the screen where it
// points (hold to drag a slider), B and Start act as the Back key.
// The keyboard and mouse work as in the PC version (pc/src/window_win.cpp): every key and mouse button goes to the
// native side; during play they are controls only, in menus keys also go to the game (Esc as Back) and the left
// button touches. In zombies play the mouse is captured (Android's pointer capture: the pointer is hidden and its
// motion turns the view); in Dead Ops Arcade the pointer stays and the player shoots toward it.
package boz64;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;
import android.view.ActionMode;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.KeyboardShortcutGroup;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.SearchEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;

import java.lang.reflect.Method;
import java.util.BitSet;
import java.util.List;

public final class Pad {
    private static final String TAG = "boz64";

    // buttons, as src/pad.cpp reads them
    static final int A = 1, B = 1 << 1, X = 1 << 2, Y = 1 << 3, LB = 1 << 4, RB = 1 << 5, L3 = 1 << 6, R3 = 1 << 7,
            START = 1 << 8, BACK = 1 << 9, UP = 1 << 10, DOWN = 1 << 11, LEFT = 1 << 12, RIGHT = 1 << 13,
            LT = 1 << 14, RT = 1 << 15;
    private static final int DPAD = UP | DOWN | LEFT | RIGHT;

    static native void nativeInput(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);
    static native void nativeTouch();
    static native boolean nativeInGame();
    // the keyboard and mouse (src/pad.h)
    static native void nativeKey(int code, boolean down);
    static native void nativeMouseMove(float dx, float dy);
    static native void nativeWheel(int notches);
    static native void nativeCursor(float x, float y);
    static native void nativeReleaseAll();
    static native int nativeMouseMode();

    // mouse buttons as src/pad.h numbers them, in the order of MotionEvent's BUTTON_PRIMARY .. BUTTON_FORWARD bits
    private static final int[] MOUSE_KEYS = {0x1001, 0x1002, 0x1003, 0x1004, 0x1005};
    // what the mouse is for (src/pad.h's KbmMouse)
    private static final int MOUSE_TOUCH = 0, MOUSE_LOOK = 1, MOUSE_AIM = 2;
    private static final long CAPTURE_RETRY = 500_000_000L;  // ns between requests for the pointer while it is refused

    private static int keyButtons, hatButtons;
    private static float lx, ly, rx, ry, lt, rt;
    private static boolean nativeOk = true;
    private static boolean announced;

    /** Wraps the activity's window callback (once). */
    static void attach(Activity a) {
        Window w = a.getWindow();
        if (w == null) return;
        Window.Callback cb = w.getCallback();
        if (cb == null || cb instanceof Wrapper) return;
        w.setCallback(new Wrapper(a, cb));
    }

    private static boolean fromController(InputEvent e) {
        int s = e.source;
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** A tiny holder so the key and motion paths share one check. */
    private static final class InputEvent {
        int source;
    }
    private static final InputEvent tmp = new InputEvent();

    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_DPAD_CENTER: return A;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BACK: return B;
            case KeyEvent.KEYCODE_BUTTON_X: return X;
            case KeyEvent.KEYCODE_BUTTON_Y: return Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return LB;
            case KeyEvent.KEYCODE_BUTTON_R1: return RB;
            case KeyEvent.KEYCODE_BUTTON_L2: return LT;
            case KeyEvent.KEYCODE_BUTTON_R2: return RT;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return L3;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return R3;
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_MENU: return START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return BACK;
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            default: return 0;
        }
    }

    private static int buttons() {
        int b = keyButtons | hatButtons;
        return b;
    }

    private static void send() {
        if (!announced) {
            announced = true;
            Log.i(TAG, "controller: first input");
        }
        if (!nativeOk) return;
        try {
            nativeInput(buttons(), lx, ly, rx, ry, Math.max(lt, (keyButtons & LT) != 0 ? 1 : 0),
                    Math.max(rt, (keyButtons & RT) != 0 ? 1 : 0));
        } catch (Throwable t) {
            nativeOk = false;
            Log.w(TAG, "controller: native side missing", t);
        }
    }

    private static boolean inGame() {
        if (!nativeOk) return false;
        try {
            return nativeInGame();
        } catch (Throwable t) {
            nativeOk = false;
            return false;
        }
    }

    // ---- keyboard and mouse: the native side ----

    private static void lost(Throwable t) {
        nativeOk = false;
        Log.w(TAG, "controller: native side missing", t);
    }

    private static void kbmKey(int code, boolean down) {
        if (!nativeOk) return;
        try {
            nativeKey(code, down);
        } catch (Throwable t) {
            lost(t);
        }
    }

    private static void kbmMove(float dx, float dy) {
        if (!nativeOk) return;
        try {
            nativeMouseMove(dx, dy);
        } catch (Throwable t) {
            lost(t);
        }
    }

    private static void kbmWheel(int notches) {
        if (!nativeOk) return;
        try {
            nativeWheel(notches);
        } catch (Throwable t) {
            lost(t);
        }
    }

    private static void kbmCursor(float x, float y) {
        if (!nativeOk) return;
        try {
            nativeCursor(x, y);
        } catch (Throwable t) {
            lost(t);
        }
    }

    private static void kbmReleaseAll() {
        if (!nativeOk) return;
        try {
            nativeReleaseAll();
        } catch (Throwable t) {
            lost(t);
        }
    }

    private static int mouseMode() {
        if (!nativeOk) return MOUSE_TOUCH;
        try {
            return nativeMouseMode();
        } catch (Throwable t) {
            lost(t);
            return MOUSE_TOUCH;
        }
    }

    /** A key from a real keyboard (not a controller, the phone's own buttons, or the navigation bar's Back). */
    private static boolean fromKeyboard(KeyEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_KEYBOARD) != InputDevice.SOURCE_KEYBOARD) return false;
        if (e.getDeviceId() == KeyCharacterMap.VIRTUAL_KEYBOARD) return false;
        InputDevice d = e.getDevice();
        return d != null && !d.isVirtual() && d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
    }

    /** From a mouse or touchpad, moving the pointer or captured. */
    private static boolean fromMouse(int source) {
        return (source & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE
                || (source & InputDevice.SOURCE_MOUSE_RELATIVE) == InputDevice.SOURCE_MOUSE_RELATIVE;
    }

    /** The game's own view (the first child of the content view). */
    private static View gameView(Activity a) {
        View content = a.findViewById(android.R.id.content);
        return content instanceof ViewGroup && ((ViewGroup) content).getChildCount() > 0
                ? ((ViewGroup) content).getChildAt(0) : content;
    }

    private static float axis(MotionEvent e, int a) {
        InputDevice d = e.getDevice();
        float v = e.getAxisValue(a);
        if (d != null) {
            InputDevice.MotionRange r = d.getMotionRange(a, e.getSource());
            if (r != null && Math.abs(v) <= r.getFlat()) return 0;
        }
        return v;
    }

    private static boolean hasAxis(MotionEvent e, int a) {
        InputDevice d = e.getDevice();
        return d != null && d.getMotionRange(a, e.getSource()) != null;
    }

    // ---- menus: an on-screen cursor ----

    private static final class Cursor extends View implements Choreographer.FrameCallback {
        final Activity activity;
        final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        float x = -1, y = -1;
        boolean shown, running, touching;
        long lastFrame;
        Method onMotion;
        Object loaderThread;

        Cursor(Activity a) {
            super(a);
            activity = a;
            fill.setColor(0xccffffff);
            edge.setColor(0xff000000);
            edge.setStyle(Paint.Style.STROKE);
            setWillNotDraw(false);
            setClickable(false);
            setFocusable(false);
        }

        @Override public boolean onTouchEvent(MotionEvent e) { return false; }

        /** Starts the cursor in the middle of the screen, once the overlay has a size. */
        boolean placed() {
            if (x < 0 && getWidth() > 0) {
                x = getWidth() / 2f;
                y = getHeight() / 2f;
            }
            return x >= 0;
        }

        void show() {
            placed();
            if (!shown) {
                shown = true;
                setVisibility(VISIBLE);
                invalidate();
            }
            if (!running) {
                running = true;
                lastFrame = 0;
                Choreographer.getInstance().postFrameCallback(this);
            }
        }

        void hide() {
            if (touching) touch(5);
            if (shown) {
                shown = false;
                setVisibility(INVISIBLE);
            }
        }

        @Override public void doFrame(long frameTimeNanos) {
            running = false;
            if (!shown) return;
            if (inGame()) {
                hide();
                return;
            }
            if (!placed()) {
                running = true;
                Choreographer.getInstance().postFrameCallback(this);
                return;
            }
            float dt = lastFrame == 0 ? 0 : Math.min(0.05f, (frameTimeNanos - lastFrame) / 1e9f);
            lastFrame = frameTimeNanos;
            float sx = lx, sy = ly;
            float m = (float) Math.sqrt(sx * sx + sy * sy);
            if (m < 0.2f) sx = sy = 0;
            int b = buttons();
            if ((b & LEFT) != 0) sx = -0.6f;
            if ((b & RIGHT) != 0) sx = 0.6f;
            if ((b & UP) != 0) sy = -0.6f;
            if ((b & DOWN) != 0) sy = 0.6f;
            if (sx != 0 || sy != 0) {
                float speed = getHeight() * 1.1f;  // full stick: about a screen height a second
                x = Math.max(0, Math.min(getWidth() - 1, x + sx * Math.abs(sx) * speed * dt));
                y = Math.max(0, Math.min(getHeight() - 1, y + sy * Math.abs(sy) * speed * dt));
                if (touching) touch(6);
                invalidate();
            }
            running = true;
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override protected void onDraw(Canvas c) {
            if (!shown || !placed()) return;
            float r = getResources().getDisplayMetrics().density * (touching ? 9 : 12);
            edge.setStrokeWidth(getResources().getDisplayMetrics().density * 2);
            c.drawCircle(x, y, r, fill);
            c.drawCircle(x, y, r, edge);
        }

        /** Sends a touch at the cursor to the game, as its own touch handling would (4 down, 5 up, 6 move). */
        void touch(int action) {
            if (!placed()) return;
            touching = action != 5;
            try {
                if (onMotion == null) {
                    Class<?> la = Class.forName("com.ideaworks3d.marmalade.LoaderActivity");
                    java.lang.reflect.Field f = la.getDeclaredField("m_Activity");
                    f.setAccessible(true);
                    Object act = f.get(null);
                    Method m = la.getDeclaredMethod("LoaderThread");
                    m.setAccessible(true);
                    loaderThread = m.invoke(act);
                    onMotion = loaderThread.getClass().getDeclaredMethod("onMotionEvent", int.class, int.class,
                            int.class, int.class);
                    onMotion.setAccessible(true);
                }
                int[] mine = new int[2], game = new int[2];
                getLocationOnScreen(mine);
                View g = gameView(activity);
                if (g != null) g.getLocationOnScreen(game);
                onMotion.invoke(loaderThread, 0, action, (int) (x + mine[0] - game[0]), (int) (y + mine[1] - game[1]));
            } catch (Throwable t) {
                Log.w(TAG, "controller: could not send a touch", t);
                onMotion = null;
            }
            invalidate();
        }
    }

    // ---- the window callback wrapper ----

    private static final class Wrapper implements Window.Callback, Choreographer.FrameCallback {
        final Activity activity;
        final Window.Callback orig;
        Cursor cursor;
        // keyboard and mouse
        final BitSet passed = new BitSet();  // keyboard keys whose press went to the game (their release goes too)
        int mouseButtons;                    // MotionEvent button bits down, as last seen
        boolean mouseTouching;               // a left-button touch is going to the game
        float mouseX, mouseY;
        long mouseDownTime;
        float wheelRest;                     // wheel motion short of a notch (touchpads scroll in fractions)
        boolean kbmUsed, watching;           // a keyboard or mouse was used: the mouse mode is checked every frame
        long captureAsked;                   // when the pointer was last asked for (frame time, ns)
        int pointerMode = MOUSE_TOUCH;       // the mode the pointer's icon was last set for

        Wrapper(Activity a, Window.Callback cb) {
            activity = a;
            orig = cb;
        }

        // ---- keyboard and mouse ----

        /** A keyboard key. During play keys are controls only; in menus they also go to the game. */
        boolean keyboardKey(KeyEvent e) {
            int code = e.getKeyCode(), action = e.getAction();
            if (action != KeyEvent.ACTION_DOWN && action != KeyEvent.ACTION_UP) return orig.dispatchKeyEvent(e);
            boolean down = action == KeyEvent.ACTION_DOWN;
            kbmKey(code, down);
            kbmUsed();
            if (down) {
                if (inGame()) return true;
                passed.set(code);
            } else {
                if (!passed.get(code)) return true;
                passed.clear(code);
            }
            if (code == KeyEvent.KEYCODE_ESCAPE) {  // the game's Back, as on the PC
                orig.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), action, KeyEvent.KEYCODE_BACK,
                        e.getRepeatCount(), e.getMetaState(), e.getDeviceId(), e.getScanCode(), e.getFlags(),
                        e.getSource()));
                return true;  // handled either way (Android would otherwise send its own Back for Esc)
            }
            return orig.dispatchKeyEvent(e);
        }

        /** Every mouse event: button changes go to the native side, and so does the pointer (Dead Ops aims at it). */
        void mouseSeen(MotionEvent e) {
            kbmUsed();
            int b = e.getButtonState() & 0x1f, changed = b ^ mouseButtons;
            mouseButtons = b;
            for (int i = 0; i < MOUSE_KEYS.length; i++)
                if ((changed & 1 << i) != 0) kbmKey(MOUSE_KEYS[i], (b & 1 << i) != 0);
            int a = e.getActionMasked();
            if ((e.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE
                    && (a == MotionEvent.ACTION_HOVER_MOVE || a == MotionEvent.ACTION_MOVE
                    || a == MotionEvent.ACTION_DOWN)) {
                View d = activity.getWindow().getDecorView();
                kbmCursor(e.getX() - d.getWidth() / 2f, e.getY() - d.getHeight() / 2f);
            }
            if (a == MotionEvent.ACTION_SCROLL) {
                wheelRest += e.getAxisValue(MotionEvent.AXIS_VSCROLL);  // positive away from the user
                int n = (int) wheelRest;
                wheelRest -= n;
                if (n != 0) kbmWheel(n);
            }
        }

        /** A mouse button stream (Android sends these as touches). In menus the left button touches the game. */
        boolean mouseTouch(MotionEvent e) {
            mouseSeen(e);
            int a = e.getActionMasked();
            if (!mouseTouching) {
                if (a != MotionEvent.ACTION_DOWN || inGame()) return true;  // during play the buttons are controls
                int b = e.getButtonState();
                if (b != 0 && (b & MotionEvent.BUTTON_PRIMARY) == 0) return true;  // only the left button touches
                mouseTouching = true;
            } else if (inGame()) {  // play started during the touch (the click on "Resume")
                endMouseTouch();
                return true;
            }
            mouseX = e.getX();
            mouseY = e.getY();
            mouseDownTime = e.getDownTime();
            if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) mouseTouching = false;
            return orig.dispatchTouchEvent(e);
        }

        /** Ends a left-button touch the game is in the middle of, where it is. */
        void endMouseTouch() {
            if (!mouseTouching) return;
            mouseTouching = false;
            MotionEvent up = MotionEvent.obtain(mouseDownTime, SystemClock.uptimeMillis(), MotionEvent.ACTION_UP,
                    mouseX, mouseY, 0);
            up.setSource(InputDevice.SOURCE_MOUSE);
            orig.dispatchTouchEvent(up);
            up.recycle();
        }

        /** Captured mouse motion (zombies play): it turns the view. */
        void capturedMouse(MotionEvent e) {
            mouseSeen(e);
            int a = e.getActionMasked();
            // each report comes as one of these with the motion, plus button and scroll events that repeat it
            if (a != MotionEvent.ACTION_MOVE && a != MotionEvent.ACTION_DOWN && a != MotionEvent.ACTION_UP
                    && a != MotionEvent.ACTION_HOVER_MOVE) return;
            float dx = e.getX(), dy = e.getY();
            for (int h = 0; h < e.getHistorySize(); h++) {
                dx += e.getHistoricalX(h);
                dy += e.getHistoricalY(h);
            }
            if (dx != 0 || dy != 0) kbmMove(dx, dy);
        }

        void kbmUsed() {
            kbmUsed = true;
            watch();
        }

        void watch() {
            if (watching || !activity.hasWindowFocus()) return;
            watching = true;
            Choreographer.getInstance().postFrameCallback(this);
        }

        /** Each frame while a keyboard or mouse is in use and the game is in front: capture the mouse in zombies play. */
        @Override public void doFrame(long frameTimeNanos) {
            watching = false;
            boolean front = activity.hasWindowFocus();
            int m = front ? mouseMode() : MOUSE_TOUCH;
            if (Build.VERSION.SDK_INT >= 26) {
                View d = activity.getWindow().getDecorView();
                if (m == MOUSE_LOOK) {
                    if (!d.hasPointerCapture() && frameTimeNanos - captureAsked > CAPTURE_RETRY) {
                        endMouseTouch();
                        d.requestPointerCapture();
                        captureAsked = frameTimeNanos;
                    }
                } else if (d.hasPointerCapture()) {
                    d.releasePointerCapture();
                }
            }
            if (m != pointerMode && Build.VERSION.SDK_INT >= 24) {  // a crosshair to aim with in Dead Ops
                pointerMode = m;
                View g = gameView(activity);
                if (g != null)
                    g.setPointerIcon(m == MOUSE_AIM ? PointerIcon.getSystemIcon(activity, PointerIcon.TYPE_CROSSHAIR) : null);
            }
            if (front) watch();
        }

        Cursor cursor() {
            if (cursor != null && cursor.getParent() == null) cursor = null;  // the game replaced its views
            if (cursor == null) {
                cursor = new Cursor(activity);
                activity.addContentView(cursor, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
            }
            return cursor;
        }

        void back() {
            long t = SystemClock.uptimeMillis();
            orig.dispatchKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
            orig.dispatchKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
        }

        /** Controller input changed: to the game during play, to the cursor otherwise. */
        void changed(int before) {
            send();
            if (inGame()) {
                if (cursor != null) cursor.hide();
                return;
            }
            Cursor c = cursor();
            c.show();
            int b = buttons(), pressed = b & ~before, released = before & ~b;
            if ((pressed & A) != 0) c.touch(4);
            if ((released & A) != 0 && c.touching) c.touch(5);
            if ((pressed & (B | START)) != 0) back();
        }

        @Override public boolean dispatchKeyEvent(KeyEvent e) {
            tmp.source = e.getSource();
            int bit = buttonFor(e.getKeyCode());
            boolean controller = fromController(tmp) || (bit != 0 && KeyEvent.isGamepadButton(e.getKeyCode()));
            if (controller && bit != 0) {
                int before = buttons();
                if (e.getAction() == KeyEvent.ACTION_DOWN) keyButtons |= bit;
                else if (e.getAction() == KeyEvent.ACTION_UP) keyButtons &= ~bit;
                if (buttons() != before || e.getRepeatCount() == 0) changed(before);
                return true;
            }
            if (fromKeyboard(e) && !e.isSystem()) return keyboardKey(e);  // volume and media keys stay the system's
            // a mouse's side buttons also come as Back and Forward keys: during play they are controls only
            if (fromMouse(e.getSource()) && inGame()) return true;
            return orig.dispatchKeyEvent(e);
        }

        @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
            tmp.source = e.getSource();
            if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                    && e.getAction() == MotionEvent.ACTION_MOVE) {
                int before = buttons();
                lx = axis(e, MotionEvent.AXIS_X);
                ly = axis(e, MotionEvent.AXIS_Y);
                if (hasAxis(e, MotionEvent.AXIS_RZ)) {
                    rx = axis(e, MotionEvent.AXIS_Z);
                    ry = axis(e, MotionEvent.AXIS_RZ);
                } else {
                    rx = axis(e, MotionEvent.AXIS_RX);
                    ry = axis(e, MotionEvent.AXIS_RY);
                }
                lt = Math.max(axis(e, MotionEvent.AXIS_LTRIGGER), axis(e, MotionEvent.AXIS_BRAKE));
                rt = Math.max(axis(e, MotionEvent.AXIS_RTRIGGER), axis(e, MotionEvent.AXIS_GAS));
                float hx = axis(e, MotionEvent.AXIS_HAT_X), hy = axis(e, MotionEvent.AXIS_HAT_Y);
                hatButtons = (hx < -0.5f ? LEFT : 0) | (hx > 0.5f ? RIGHT : 0) | (hy < -0.5f ? UP : 0)
                        | (hy > 0.5f ? DOWN : 0);
                changed(before);
                return true;
            }
            if (fromMouse(e.getSource())) {  // the pointer moving, the wheel, button presses
                mouseSeen(e);
                int a = e.getActionMasked();
                boolean hover = a == MotionEvent.ACTION_HOVER_MOVE || a == MotionEvent.ACTION_HOVER_ENTER
                        || a == MotionEvent.ACTION_HOVER_EXIT;
                if (!hover && inGame()) return true;
            }
            return orig.dispatchGenericMotionEvent(e);
        }

        @Override public boolean dispatchTouchEvent(MotionEvent e) {
            if (fromMouse(e.getSource())) return mouseTouch(e);
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN
                    && (e.getSource() & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN) {
                if (cursor != null) cursor.hide();
                if (nativeOk) {
                    try {
                        nativeTouch();
                    } catch (Throwable t) {
                        nativeOk = false;
                    }
                }
            }
            return orig.dispatchTouchEvent(e);
        }

        @Override public void onWindowFocusChanged(boolean hasFocus) {
            if (!hasFocus) {  // nothing stays held while the game is in the background
                keyButtons = hatButtons = 0;
                lx = ly = rx = ry = lt = rt = 0;
                send();
                if (cursor != null) cursor.hide();
                kbmReleaseAll();
                mouseButtons = 0;
                wheelRest = 0;
                passed.clear();
            } else if (kbmUsed) {
                watch();  // capture the mouse again if play is on
            }
            orig.onWindowFocusChanged(hasFocus);
        }

        @Override public boolean dispatchKeyShortcutEvent(KeyEvent e) { return orig.dispatchKeyShortcutEvent(e); }
        // Captured mouse events reach the window callback here (no view takes them first)
        @Override public boolean dispatchTrackballEvent(MotionEvent e) {
            if (fromMouse(e.getSource())) {
                capturedMouse(e);
                return true;
            }
            return orig.dispatchTrackballEvent(e);
        }
        @Override public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent e) {
            return orig.dispatchPopulateAccessibilityEvent(e);
        }
        @Override public View onCreatePanelView(int id) { return orig.onCreatePanelView(id); }
        @Override public boolean onCreatePanelMenu(int id, Menu m) { return orig.onCreatePanelMenu(id, m); }
        @Override public boolean onPreparePanel(int id, View v, Menu m) { return orig.onPreparePanel(id, v, m); }
        @Override public boolean onMenuOpened(int id, Menu m) { return orig.onMenuOpened(id, m); }
        @Override public boolean onMenuItemSelected(int id, MenuItem i) { return orig.onMenuItemSelected(id, i); }
        @Override public void onWindowAttributesChanged(WindowManager.LayoutParams p) {
            orig.onWindowAttributesChanged(p);
        }
        @Override public void onContentChanged() { orig.onContentChanged(); }
        @Override public void onAttachedToWindow() { orig.onAttachedToWindow(); }
        @Override public void onDetachedFromWindow() { orig.onDetachedFromWindow(); }
        @Override public void onPanelClosed(int id, Menu m) { orig.onPanelClosed(id, m); }
        @Override public boolean onSearchRequested() { return orig.onSearchRequested(); }
        @Override public boolean onSearchRequested(SearchEvent e) { return orig.onSearchRequested(e); }
        @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback c) {
            return orig.onWindowStartingActionMode(c);
        }
        @Override public ActionMode onWindowStartingActionMode(ActionMode.Callback c, int type) {
            return orig.onWindowStartingActionMode(c, type);
        }
        @Override public void onActionModeStarted(ActionMode m) { orig.onActionModeStarted(m); }
        @Override public void onActionModeFinished(ActionMode m) { orig.onActionModeFinished(m); }
        @Override public void onProvideKeyboardShortcuts(List<KeyboardShortcutGroup> data, Menu menu, int deviceId) {
            if (Build.VERSION.SDK_INT >= 24) orig.onProvideKeyboardShortcuts(data, menu, deviceId);
        }
        @Override public void onPointerCaptureChanged(boolean hasCapture) {
            if (Build.VERSION.SDK_INT >= 26) orig.onPointerCaptureChanged(hasCapture);
        }
    }
}
