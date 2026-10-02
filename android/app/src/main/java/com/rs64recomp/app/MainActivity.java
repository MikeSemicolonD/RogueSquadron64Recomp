package com.rs64recomp.app;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.view.WindowInsets;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    private static final int PICK_ROM = 0x5253;
    private static CountDownLatch sRomLatch;
    private static String sPickedRom;

    // Wi-Fi drivers drop broadcast packets for apps without this lock, and the co-op lobby finds hosts by broadcast.
    private WifiManager.MulticastLock mMulticastLock;

    // On-screen keyboard height as a fraction of the window, read by the game to slide the picture up while typing.
    private static volatile float sImeFraction;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        if (wifi != null) {
            mMulticastLock = wifi.createMulticastLock("rs64-lobby");
            mMulticastLock.setReferenceCounted(false);
            mMulticastLock.acquire();
        }
        // The window is fullscreen, so the keyboard never resizes it; its height arrives only as an inset (API 30+).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
                final int h = v.getHeight();
                final int ime = insets.getInsets(WindowInsets.Type.ime()).bottom;
                sImeFraction = h > 0 ? (float) ime / h : 0.0f;
                return v.onApplyWindowInsets(insets);
            });
        }
    }

    public static float imeFraction() {
        return sImeFraction;
    }

    @Override
    protected void onDestroy() {
        if (mMulticastLock != null && mMulticastLock.isHeld()) {
            mMulticastLock.release();
        }
        super.onDestroy();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    // SDL would allow any orientation for a resizable window; the game is landscape-only (either side).
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    // Called from the native thread: opens the system file picker and blocks until it closes.
    // Returns the path of a private copy of the chosen file, or null if the player cancelled.
    public static String pickRom() {
        final SDLActivity activity = mSingleton;
        if (activity == null) {
            return null;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        sRomLatch = latch;
        sPickedRom = null;
        activity.runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            activity.startActivityForResult(intent, PICK_ROM);
        });
        try {
            latch.await();
        } catch (InterruptedException e) {
            return null;
        }
        return sPickedRom;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_ROM) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final CountDownLatch latch = sRomLatch;
        final Uri uri = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
        if (uri == null) {
            latch.countDown();
            return;
        }
        // Copy off the UI thread; the native side validates the copy and imports it.
        new Thread(() -> {
            File dst = new File(getCacheDir(), "picked_rom.z64");
            try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
                sPickedRom = dst.getAbsolutePath();
            } catch (Exception e) {
                sPickedRom = null;
            }
            latch.countDown();
        }).start();
    }
}
