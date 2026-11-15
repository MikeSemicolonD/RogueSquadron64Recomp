package com.rs64recomp.app;

import android.Manifest;
import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.net.wifi.p2p.WifiP2pConfig;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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
        // The Discord Social SDK is only in APKs built with its .aar, so it is reached by name.
        try {
            Class.forName("com.discord.socialsdk.DiscordSocialSdkInit").getMethod("setEngineActivity", android.app.Activity.class).invoke(null, this);
        } catch (ReflectiveOperationException e) {
            // Built without the SDK: no Discord status.
        }
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

    // Co-op over Wi-Fi Direct (API 29+): a host makes a group, a joiner connects to it, and the lobby's LAN search finds the host on the group's network.
    // shortcut: one fixed group name, so two hosts in range collide; give each host its own name if that comes up.
    private static final String P2P_NAME = "DIRECT-RS-RogueSquadron64";
    private static final String P2P_PASS = "rs64coop";
    private static final int P2P_PERMISSION = 0x5254;
    private static final long P2P_RETRY_MS = 10000;
    private WifiP2pManager mP2p;
    private WifiP2pManager.Channel mP2pChannel;
    private final Handler mP2pHandler = new Handler(Looper.getMainLooper());
    private int mP2pMode;
    // 0 off or unavailable, 1 connecting, 2 linked.
    private static volatile int sP2pState;

    private final BroadcastReceiver mP2pReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final WifiP2pInfo info = intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_INFO);
            if (mP2pMode != 0) {
                sP2pState = (info != null && info.groupFormed) ? 2 : 1;
            }
        }
    };

    // Called from the native thread: 0 off, 1 host, 2 join, 3 joined (keep the link, stop retrying).
    public static void wifiDirect(int mode) {
        final MainActivity activity = (MainActivity) mSingleton;
        if (activity != null) {
            activity.runOnUiThread(() -> activity.p2pSetMode(mode));
        }
    }

    public static int wifiDirectState() {
        return sP2pState;
    }

    private static String p2pPermission() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ? Manifest.permission.NEARBY_WIFI_DEVICES : Manifest.permission.ACCESS_FINE_LOCATION;
    }

    private void p2pSetMode(int mode) {
        if (mode == mP2pMode) {
            return;
        }
        mP2pMode = mode;
        mP2pHandler.removeCallbacksAndMessages(null);
        if (mode == 0 || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            p2pStop();
            return;
        }
        // In session (3) the link stays as it is, and a linked joiner needs no new request.
        if (mode == 3 || (mode == 2 && sP2pState == 2)) {
            return;
        }
        // Connecting while the permission prompt is up, so the joiner's search waits for the answer.
        sP2pState = 1;
        if (checkSelfPermission(p2pPermission()) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { p2pPermission() }, P2P_PERMISSION);
            return;
        }
        p2pStart();
    }

    @SuppressLint("MissingPermission")
    private void p2pStart() {
        if (mP2p == null) {
            mP2p = (WifiP2pManager) getSystemService(WIFI_P2P_SERVICE);
            if (mP2p == null) {
                sP2pState = 0;
                return;
            }
            mP2pChannel = mP2p.initialize(this, getMainLooper(), null);
            final IntentFilter filter = new IntentFilter(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(mP2pReceiver, filter, RECEIVER_NOT_EXPORTED);
            } else {
                registerReceiver(mP2pReceiver, filter);
            }
        }
        final int mode = mP2pMode;
        final WifiP2pConfig config = new WifiP2pConfig.Builder().setNetworkName(P2P_NAME).setPassphrase(P2P_PASS).build();
        // A group or connect left over from before makes the next request fail as busy.
        final Runnable request = () -> {
            if (mP2pMode != mode) {
                return;
            }
            if (mode == 1) {
                mP2p.createGroup(mP2pChannel, config, null);
            } else {
                mP2p.connect(mP2pChannel, config, null);
            }
        };
        if (mode == 1) {
            mP2p.removeGroup(mP2pChannel, p2pThen(request));
        } else {
            mP2p.cancelConnect(mP2pChannel, p2pThen(request));
            mP2pHandler.postDelayed(this::p2pRetry, P2P_RETRY_MS);
        }
    }

    // A joiner keeps trying until a host's group is up.
    private void p2pRetry() {
        if (mP2pMode == 2 && sP2pState != 2) {
            p2pStart();
        }
    }

    private void p2pStop() {
        sP2pState = 0;
        if (mP2p != null) {
            mP2p.cancelConnect(mP2pChannel, null);
            mP2p.removeGroup(mP2pChannel, null);
        }
    }

    private static WifiP2pManager.ActionListener p2pThen(Runnable next) {
        return new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                next.run();
            }

            @Override
            public void onFailure(int reason) {
                next.run();
            }
        };
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != P2P_PERMISSION) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED && mP2pMode != 0) {
            p2pStart();
        } else {
            sP2pState = 0;
        }
    }

    @Override
    protected void onDestroy() {
        if (mMulticastLock != null && mMulticastLock.isHeld()) {
            mMulticastLock.release();
        }
        mP2pHandler.removeCallbacksAndMessages(null);
        if (mP2p != null) {
            p2pStop();
            unregisterReceiver(mP2pReceiver);
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
