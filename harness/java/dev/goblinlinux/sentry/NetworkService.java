package dev.goblinlinux.sentry;

import android.os.ParcelFileDescriptor;
import android.os.Process;

/** Socket creation remains in the ordinary app UID, in a managed process. */
public final class NetworkService extends HostedService {
    static { System.loadLibrary("goblinuml-netservice"); }
    private static native void run(String[] args, int packets, int log, int status);
    @Override protected int launch(String[] args, int[] targets, ParcelFileDescriptor[] files, ParcelFileDescriptor status) throws Exception {
        if (files.length != 2) throw new IllegalArgumentException("Expected network and log descriptors");
        // Own duplicates until the native event loop exits the service process.
        final ParcelFileDescriptor packets = ParcelFileDescriptor.dup(files[0].getFileDescriptor());
        final ParcelFileDescriptor log = ParcelFileDescriptor.dup(files[1].getFileDescriptor());
        final ParcelFileDescriptor exit = ParcelFileDescriptor.dup(status.getFileDescriptor());
        new Thread(() -> run(args, packets.getFd(), log.getFd(), exit.getFd()), "Linux network").start();
        return Process.myPid();
    }
    @Override protected void shutdown() { Process.killProcess(Process.myPid()); }
}
