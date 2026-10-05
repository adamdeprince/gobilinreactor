package dev.goblinlinux.zygoteprobe;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import java.io.File;
import java.io.RandomAccessFile;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class Research extends Instrumentation {
    private Bundle arguments;
    @Override public void onCreate(Bundle args) { arguments = args; super.onCreate(args); start(); }
    @Override public void onStart() {
        Context context = getTargetContext();
        String mode = arguments.getString("mode", "zygote");
        int children = Integer.parseInt(arguments.getString("children", "64"));
        int seconds = Integer.parseInt(arguments.getString("seconds", "180"));
        File report = new File(context.getFilesDir(), mode + "-report.txt");
        File data = new File(context.getFilesDir(), "research-data");
        CountDownLatch bound = new CountDownLatch(1);
        IBinder[] worker = new IBinder[1];
        ServiceConnection connection = new ServiceConnection() {
            public void onServiceConnected(ComponentName name, IBinder binder) { worker[0] = binder; bound.countDown(); }
            public void onServiceDisconnected(ComponentName name) { }
        };
        PowerManager.WakeLock wake = context.getSystemService(PowerManager.class)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "goblin:zygote-research");
        Bundle result = new Bundle();
        boolean ok = false, binding = false;
        try {
            wake.acquire((seconds + 60L) * 1000);
            try (RandomAccessFile file = new RandomAccessFile(data, "rw")) { file.setLength(65536); file.writeUTF("probe-data"); }
            Class<?> service = mode.equals("regular") ? RegularService.class : WorkerService.class;
            binding = context.bindService(new Intent(context, service), connection, Context.BIND_AUTO_CREATE);
            if (!binding || !bound.await(20, TimeUnit.SECONDS)) throw new IllegalStateException("service bind failed");
            try (ParcelFileDescriptor reportFd = ParcelFileDescriptor.open(report,
                         ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE | ParcelFileDescriptor.MODE_READ_WRITE);
                 ParcelFileDescriptor dataFd = ParcelFileDescriptor.open(data, ParcelFileDescriptor.MODE_READ_WRITE);
                 ServerSocket listener = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
                 Socket client = new Socket("127.0.0.1", listener.getLocalPort());
                 Socket accepted = listener.accept();
                 ParcelFileDescriptor networkFd = ParcelFileDescriptor.fromSocket(client)) {
                Parcel input = Parcel.obtain(), output = Parcel.obtain();
                try {
                    input.writeInterfaceToken("goblin.zygote.research");
                    input.writeString(data.getAbsolutePath()); input.writeInt(children); input.writeInt(seconds);
                    reportFd.writeToParcel(input, 0); dataFd.writeToParcel(input, 0); networkFd.writeToParcel(input, 0);
                    if (!worker[0].transact(IBinder.FIRST_CALL_TRANSACTION, input, output, 0)) throw new IllegalStateException("transaction rejected");
                    output.readException();
                    int status = output.readInt();
                    if (status != 0) throw new IllegalStateException("native helper status=" + status);
                    accepted.setSoTimeout(3000);
                    byte[] bytes = new byte[128];
                    int count = accepted.getInputStream().read(bytes);
                    if (count <= 0 || !new String(bytes, 0, count, StandardCharsets.UTF_8).contains("network-ok"))
                        throw new IllegalStateException("passed TCP socket did not work");
                    ok = true;
                } finally { input.recycle(); output.recycle(); }
            }
            result.putString("stream", new String(Files.readAllBytes(report.toPath()), StandardCharsets.UTF_8) + "PASS parent TCP verification\n");
        } catch (Throwable error) {
            String partial = "";
            try { partial = new String(Files.readAllBytes(report.toPath()), StandardCharsets.UTF_8); } catch (Exception ignored) { }
            result.putString("stream", partial + "FAIL " + error + "\n");
        } finally {
            if (binding) context.unbindService(connection);
            if (wake.isHeld()) wake.release();
        }
        finish(ok ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
    }
}
