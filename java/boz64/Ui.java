// Window setup for the 64-bit port, added to the APK as a second dex file. The native wrapper calls
// install() from JNI_OnLoad; from then on every activity of the game is shown full screen: drawn under the
// camera cutout, with the status and navigation bars hidden (a swipe from the edge shows them briefly).
// It also starts the sound watchdog (Sound.java) once the game's loader activity exists, and while the game is in
// front it holds a Wi-Fi multicast lock, so the LAN game announcements (broadcasts, see src/lan.cpp) get through.
package boz64;

import android.app.Activity;
import android.app.Application;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

public final class Ui implements Application.ActivityLifecycleCallbacks {
    private static final String TAG = "boz64";
    private static final int IMMERSIVE = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
    private static boolean installed;
    private static Application app;
    private static android.net.wifi.WifiManager.MulticastLock lanLock;

    private static void lanLock(boolean hold) {
        try {
            if (lanLock == null) {
                android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager)
                        app.getApplicationContext().getSystemService(android.content.Context.WIFI_SERVICE);
                if (wm == null) return;
                lanLock = wm.createMulticastLock("boz64-lan");
                lanLock.setReferenceCounted(false);
            }
            if (hold) lanLock.acquire();
            else lanLock.release();
        } catch (Throwable t) {
            Log.w(TAG, "Wi-Fi multicast lock: " + t);
        }
    }

    public static synchronized void install() {
        if (installed) return;
        try {
            app = (Application) Class.forName("android.app.ActivityThread")
                    .getMethod("currentApplication").invoke(null);
            app.registerActivityLifecycleCallbacks(new Ui());
            installed = true;
            Log.i(TAG, "full-screen window setup installed");
        } catch (Throwable t) {
            Log.w(TAG, "full-screen window setup failed", t);
        }
    }

    /** The app's private files folder (the port's settings file lives there). */
    public static String filesDir() {
        return app != null ? app.getFilesDir().getAbsolutePath() : null;
    }

    /** The display's current refresh rate in Hz. */
    public static float refreshRate() {
        try {
            WindowManager wm = (WindowManager) app.getSystemService(android.content.Context.WINDOW_SERVICE);
            return wm.getDefaultDisplay().getRefreshRate();
        } catch (Throwable t) {
            return 60;
        }
    }

    private static void apply(Activity a) {
        Window w = a.getWindow();
        if (w == null) return;
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = w.getAttributes();
            int mode = Build.VERSION.SDK_INT >= 30 ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            if (lp.layoutInDisplayCutoutMode != mode) {
                lp.layoutInDisplayCutoutMode = mode;
                w.setAttributes(lp);
            }
        }
        final View decor = w.getDecorView();
        decor.setSystemUiVisibility(IMMERSIVE);
        // the game's loader changes the flags on focus changes; put them back
        decor.setOnSystemUiVisibilityChangeListener(new View.OnSystemUiVisibilityChangeListener() {
            @Override
            public void onSystemUiVisibilityChange(int visibility) {
                if ((visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0) decor.setSystemUiVisibility(IMMERSIVE);
            }
        });
    }

    @Override public void onActivityCreated(Activity a, Bundle b) {
        apply(a);
        Sound.watch(a);
        Pad.attach(a);
    }
    @Override public void onActivityResumed(Activity a) {
        apply(a);
        Sound.watch(a);  // the loader activity was created before install() ran
        Pad.attach(a);
        lanLock(true);
    }
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityPaused(Activity a) { lanLock(false); }
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
    @Override public void onActivityDestroyed(Activity a) { Sound.forget(a); }
}
