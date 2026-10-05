package dev.goblinlinux.zygoteprobe;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

public class WorkerService extends Service {
    static { System.loadLibrary("zygoteprobe"); }
    private static native int launch(String executable, String dataPath, int report, int data,
                                     int network, int children, int seconds);
    @Override public IBinder onBind(Intent intent) {
        return new Binder() {
            @Override protected boolean onTransact(int code, Parcel input, Parcel output, int flags)
                    throws RemoteException {
                if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, input, output, flags);
                input.enforceInterface("goblin.zygote.research");
                String dataPath = input.readString();
                int children = input.readInt(), seconds = input.readInt();
                try (ParcelFileDescriptor report = ParcelFileDescriptor.CREATOR.createFromParcel(input);
                     ParcelFileDescriptor data = ParcelFileDescriptor.CREATOR.createFromParcel(input);
                     ParcelFileDescriptor network = ParcelFileDescriptor.CREATOR.createFromParcel(input)) {
                    int result = launch(getApplicationInfo().nativeLibraryDir + "/libzygotechild.so",
                            dataPath, report.getFd(), data.getFd(), network.getFd(), children, seconds);
                    output.writeNoException();
                    output.writeInt(result);
                } catch (Exception e) {
                    output.writeException(new IllegalStateException(e));
                }
                return true;
            }
        };
    }
}
