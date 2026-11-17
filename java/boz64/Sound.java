// Keeps the game's sound effects playing.
//
// The Marmalade loader's SoundPlayer streams the game's mixer (nearly every sound effect) through an
// AudioTrack: it primes the track with 4 * (buffer frames / 4) frames and afterwards writes a quarter of the
// buffer each time the track reports that a quarter has played. Android starts a stream only once its buffer
// is full, so when the buffer's frame count is not a multiple of 4 (on a Galaxy S22 with Bluetooth audio it
// is 4810) the primed stream never starts, never reports progress, and stays silent for good. Any underrun
// long enough for Android to disable the stream ends in the same dead end. Music and voice clips use
// MediaPlayer and are not affected.
//
// This watches the track; when it should be playing but its position has not moved for 300 ms, it tops
// the buffer up with silence, which lets Android start (or restart) it.
package boz64;

import android.app.Activity;
import android.media.AudioTrack;
import android.os.Build;
import android.util.Log;

import java.lang.reflect.Field;

final class Sound implements Runnable {
    private static final String TAG = "boz64";
    private static final int CHECK_MS = 100, STALLED_CHECKS = 3;

    private static volatile Activity activity;  // the loader activity (the one with m_LoaderThread)
    private static Thread thread;

    /** Called for each new activity; starts watching once the game's loader activity exists. */
    static synchronized void watch(Activity a) {
        if (find(a.getClass(), "m_LoaderThread") == null) return;
        activity = a;
        if (thread == null) {
            thread = new Thread(new Sound(), "boz64-sound");
            thread.setDaemon(true);
            thread.start();
        }
    }

    static void forget(Activity a) {
        if (activity == a) activity = null;
    }

    private Field loaderField, playerField, trackField, bufSizeField, frameSizeField, periodField;
    private AudioTrack last;
    private int lastPos, still;

    @Override
    public void run() {
        for (;;) {
            try {
                Thread.sleep(CHECK_MS);
            } catch (InterruptedException e) {
                return;
            }
            try {
                check();
            } catch (Throwable t) {
                last = null;  // the track was stopped or released meanwhile
            }
        }
    }

    private static Field find(Class<?> c, String name) {
        for (; c != null; c = c.getSuperclass()) {
            try {
                Field f = c.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                // look in the superclass
            }
        }
        return null;
    }

    private void check() throws Exception {
        Activity a = activity;
        if (a == null) return;
        if (loaderField == null) loaderField = find(a.getClass(), "m_LoaderThread");
        Object loader = loaderField.get(a);
        if (loader == null) return;
        if (playerField == null) playerField = find(loader.getClass(), "m_SoundPlayer");
        Object player = playerField != null ? playerField.get(loader) : null;
        if (player == null) return;
        if (trackField == null) {
            trackField = find(player.getClass(), "m_Track");
            bufSizeField = find(player.getClass(), "m_BufSize");
            frameSizeField = find(player.getClass(), "m_FrameSize");
            periodField = find(player.getClass(), "m_Period");
        }
        AudioTrack t = trackField != null ? (AudioTrack) trackField.get(player) : null;
        if (t == null || t.getState() != AudioTrack.STATE_INITIALIZED
                || t.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) {
            last = null;
            return;
        }
        int pos = t.getPlaybackHeadPosition();
        if (t != last || pos != lastPos) {
            last = t;
            lastPos = pos;
            still = 0;
            return;
        }
        if (++still < STALLED_CHECKS) return;
        still = 0;

        int channels = Math.max(1, t.getChannelCount());
        int frames;
        if (Build.VERSION.SDK_INT >= 23) {
            // fill whatever room is left; never blocks
            short[] zeros = new short[t.getBufferSizeInFrames() * channels];
            frames = t.write(zeros, 0, zeros.length, AudioTrack.WRITE_NON_BLOCKING) / channels;
        } else {
            // before Android 6 only the priming shortfall can be filled safely (a blocking write must fit)
            int bufFrames = bufSizeField.getInt(player) / Math.max(1, frameSizeField.getInt(player));
            int shortfall = bufFrames - 4 * periodField.getInt(player);
            frames = shortfall > 0 ? t.write(new short[shortfall * channels], 0, shortfall * channels) / channels : 0;
        }
        if (frames <= 0) {
            // the buffer is full but Android disabled the stream: restart it
            t.pause();
            t.play();
        }
        Log.i(TAG, "sound stream was stalled at frame " + pos + "; added " + Math.max(frames, 0)
                + " frames of silence to start it");
    }
}
