package dev.goblinreactor.sentry;

import android.os.ParcelFileDescriptor;

/** One app-zygote isolated host for the whole UML machine and its workers. */
public final class KernelService extends HostedService {
    static { System.loadLibrary("goblinlauncher"); }
    private static native int spawn(String path, String[] args, int[] sources, int[] targets, int status);
    private static native void stop();
    @Override protected int launch(String[] args, int[] targets, ParcelFileDescriptor[] files, ParcelFileDescriptor status) {
        int[] sources = new int[files.length];
        for (int i = 0; i < files.length; ++i) sources[i] = files[i].getFd();
        return spawn(getApplicationInfo().nativeLibraryDir + "/libgoblinuml-kernel.so", args, sources, targets, status.getFd());
    }
    @Override protected void shutdown() { stop(); }
    @Override public boolean onUnbind(android.content.Intent intent) {
        stop(); android.os.Process.killProcess(android.os.Process.myPid()); return false;
    }
}
