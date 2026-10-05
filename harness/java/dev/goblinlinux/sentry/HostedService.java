package dev.goblinlinux.sentry;

import android.app.Service;
import android.content.Intent;
import android.os.*;

/** Private descriptor handoff shared by the two Android-managed hosts. */
abstract class HostedService extends Service {
    protected abstract int launch(String[] args, int[] targets, ParcelFileDescriptor[] files, ParcelFileDescriptor status) throws Exception;
    protected abstract void shutdown();
    private boolean started;
    private final Binder binder = new Binder() {
        @Override protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != ManagedLinux.START && code != ManagedLinux.STOP) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(ManagedLinux.PROTOCOL);
            if (code == ManagedLinux.STOP) { shutdown(); reply.writeNoException(); return true; }
            String[] args = data.createStringArray(); int[] targets = data.createIntArray();
            ParcelFileDescriptor[] files = data.createTypedArray(ParcelFileDescriptor.CREATOR);
            ParcelFileDescriptor status = ParcelFileDescriptor.CREATOR.createFromParcel(data);
            try {
                if (started) throw new IllegalStateException("Linux host already started");
                if (files == null || targets == null || files.length != targets.length) throw new IllegalArgumentException("Invalid descriptor handoff");
                int pid = launch(args, targets, files, status);
                if (pid <= 0) throw new IllegalStateException("Linux host launch failed");
                started = true; reply.writeNoException(); reply.writeInt(pid);
            } catch (Exception e) { reply.writeException(e); }
            finally {
                try {
                    if (files != null) for (ParcelFileDescriptor file : files) if (file != null) file.close();
                    status.close();
                } catch (java.io.IOException ignored) {}
            }
            return true;
        }
    };
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public boolean onUnbind(Intent intent) { shutdown(); return false; }
    @Override public void onDestroy() { shutdown(); super.onDestroy(); }
}
