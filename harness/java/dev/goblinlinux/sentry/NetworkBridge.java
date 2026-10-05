package dev.goblinlinux.sentry;

import android.net.DnsResolver;
import java.util.concurrent.Executor;

/** Uses Android's active resolver policy, including VPN and Private DNS. */
final class NetworkBridge {
    private static final Executor CALLBACK = Runnable::run;
    private static native void answerNative(long generation, int id, byte[] response);
    static void resolve(long generation, int id, byte[] query) {
        try {
            DnsResolver.getInstance().rawQuery(null, query, DnsResolver.FLAG_EMPTY,
                    CALLBACK, null, new DnsResolver.Callback<byte[]>() {
                @Override public void onAnswer(byte[] answer, int rcode) { answerNative(generation, id, answer); }
                @Override public void onError(DnsResolver.DnsException error) { answerNative(generation, id, new byte[0]); }
            });
        } catch (RuntimeException error) { answerNative(generation, id, new byte[0]); }
    }
}
