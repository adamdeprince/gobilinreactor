package dev.goblinlinux.sentry;

import android.content.Context;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.zip.*;

final class KittyRuntime {
    static { System.loadLibrary("goblinkitty"); }
    static native String initializeNative(String directory);
    static native void contextNative();
    static native int[] drawNative(int width, int height);
    static native byte[] dumpNative();
    static native void keyNative(int key, int modifiers, String text);
    static native void scrollNative(int lines);
    static native void fontSizeNative(float points);
    private static String result;
    private static volatile String progress = "Preparing terminal…";
    static String progress() { return progress; }
    private static void removePartial(File directory) throws IOException {
        File[] children=directory.listFiles();
        if(children!=null) for(File child:children) removePartial(child);
        if(directory.exists()&&!directory.delete()) throw new IOException("Cannot remove partial runtime");
    }
    static synchronized String prepare(Context context) {
        if (result != null) return result;
        try {
            byte[] versionBytes = new byte[32];
            int n;
            try (InputStream input = context.getAssets().open("kitty-runtime-version.txt")) { n = input.read(versionBytes); }
            String version = new String(versionBytes, 0, n, StandardCharsets.US_ASCII).trim();
            if (!version.matches("[a-f0-9]{16}")) throw new IOException("Invalid kitty runtime version");
            File base = new File(context.getFilesDir(), "kitty/" + version);
            File ready = new File(base, "ready");
            if (!ready.exists()) {
                progress = "Preparing terminal files…";
                // Publish only a complete extraction. Killed installs are retried.
                File staging=new File(base.getParentFile(),version+".partial");
                removePartial(staging); removePartial(base);
                if (!staging.mkdirs()) throw new IOException("Cannot create kitty runtime");
                String prefix = staging.getCanonicalPath() + "/";
                long total = 0;
                try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("kitty-runtime.zip"))) {
                    ZipEntry entry; byte[] buffer = new byte[65536];
                    while ((entry = zip.getNextEntry()) != null) {
                        File file = new File(staging, entry.getName());
                        if (!file.getCanonicalPath().startsWith(prefix)) throw new IOException("Invalid runtime path");
                        if (entry.isDirectory()) { file.mkdirs(); continue; }
                        file.getParentFile().mkdirs();
                        try (FileOutputStream output = new FileOutputStream(file)) {
                            while ((n = zip.read(buffer)) != -1) {
                                if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException("Terminal setup interrupted");
                                total += n;
                                progress = "Preparing terminal files · " + android.text.format.Formatter.formatFileSize(context, total);
                                output.write(buffer, 0, n);
                            }
                            output.getFD().sync();
                        }
                        if (file.getName().endsWith(".so")) file.setReadOnly();
                    }
                }
                new File(staging, "cache").mkdirs();
                String conf = "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"fonts.dtd\"><fontconfig><dir>/system/fonts</dir><cachedir>" + new File(base,"cache").getAbsolutePath() + "</cachedir><alias><family>monospace</family><prefer><family>Droid Sans Mono</family></prefer></alias></fontconfig>";
                try (FileOutputStream output = new FileOutputStream(new File(staging,"fonts.conf"))) { output.write(conf.getBytes(StandardCharsets.UTF_8)); output.getFD().sync(); }
                if (!new File(staging,"ready").createNewFile()||!staging.renameTo(base)) throw new IOException("Cannot publish kitty runtime");
            }
            progress = "Starting terminal…";
            String error = initializeNative(base.getAbsolutePath());
            if (error.isEmpty()) result = error;
            return error;
        } catch (Exception e) { return e.toString(); }
    }
}
