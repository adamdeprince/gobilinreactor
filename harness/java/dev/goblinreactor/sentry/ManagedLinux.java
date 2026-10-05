package dev.goblinreactor.sentry;

import android.content.*;
import android.os.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** The foreground session owns these bindings, independently of terminal views. */
final class ManagedLinux {
    static final String PROTOCOL = "dev.goblinreactor.sentry.HostedLinux";
    static final int START = IBinder.FIRST_CALL_TRANSACTION;
    static final int STOP = START + 1;
    private static Context context;
    private static final Handle[] handles = new Handle[2];
    static synchronized void initialize(Context value) { context = value.getApplicationContext(); }

    private static final class Handle implements ServiceConnection {
        final CountDownLatch connected = new CountDownLatch(1), died = new CountDownLatch(1);
        volatile IBinder binder;
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            binder = service;
            try { service.linkToDeath(died::countDown, 0); }
            catch (RemoteException e) { died.countDown(); }
            connected.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) { died.countDown(); }
        @Override public void onBindingDied(ComponentName name) { connected.countDown(); died.countDown(); }
        @Override public void onNullBinding(ComponentName name) { connected.countDown(); died.countDown(); }
    }

    // Called from the native setup worker, never from Android's main thread.
    static synchronized int start(boolean kernel, String[] args, int[] sources, int[] targets, int status) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper()) throw new IllegalStateException("Linux startup needs a worker thread");
        int slot = kernel ? 0 : 1;
        if (context == null || handles[slot] != null) throw new IllegalStateException("Linux service already bound or uninitialized");
        Handle handle = new Handle();
        Intent intent = new Intent(context, kernel ? KernelService.class : NetworkService.class);
        if (!context.bindService(intent, handle, Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT))
            throw new IllegalStateException("Cannot bind Linux service");
        handles[slot] = handle;
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        ParcelFileDescriptor[] descriptors = new ParcelFileDescriptor[sources.length];
        try (ParcelFileDescriptor exit = ParcelFileDescriptor.fromFd(status)) {
            if (!handle.connected.await(30, TimeUnit.SECONDS) || handle.binder == null || handle.died.getCount() == 0)
                throw new IllegalStateException("Linux service did not connect");
            for (int i = 0; i < sources.length; ++i) descriptors[i] = ParcelFileDescriptor.fromFd(sources[i]);
            data.writeInterfaceToken(PROTOCOL);
            data.writeStringArray(args); data.writeIntArray(targets);
            data.writeTypedArray(descriptors, 0); exit.writeToParcel(data, 0);
            if (!handle.binder.transact(START, data, reply, 0)) throw new IllegalStateException("Linux service rejected startup");
            reply.readException();
            return reply.readInt();
        } catch (Exception e) {
            stop(kernel);
            throw e;
        } finally {
            for (ParcelFileDescriptor fd : descriptors) if (fd != null) fd.close();
            data.recycle(); reply.recycle();
        }
    }

    static synchronized void stop(boolean kernel) {
        int slot = kernel ? 0 : 1;
        Handle handle = handles[slot];
        if (handle == null) return;
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            if (handle.binder != null && handle.died.getCount() != 0) {
                data.writeInterfaceToken(PROTOCOL);
                handle.binder.transact(STOP, data, reply, 0);
                reply.readException();
            }
        } catch (RemoteException ignored) {
            // The network service exits its own process when asked to stop.
        } finally {
            context.unbindService(handle); handles[slot] = null;
            data.recycle(); reply.recycle();
        }
        if (handle.binder != null) {
            try {
                if (!handle.died.await(30, TimeUnit.SECONDS)) throw new IllegalStateException("Linux host did not terminate");
            } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IllegalStateException(e); }
        }
    }
}
