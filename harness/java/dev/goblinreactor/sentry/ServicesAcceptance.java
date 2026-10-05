package dev.goblinreactor.sentry;

import android.app.*;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.*;
import android.content.pm.ApplicationInfo;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/** Installed separately from the release; exercises the real app and guest. */
public final class ServicesAcceptance extends Instrumentation {
    private Bundle arguments;
    private final StringBuilder report = new StringBuilder();
    private final String token = UUID.randomUUID().toString().replace("-", "");
    private File directory;
    private static byte[] bytes(String value) { return value.getBytes(StandardCharsets.UTF_8); }
    private static String text(byte[] value) { return new String(value, StandardCharsets.UTF_8); }
    private static String quote(String value) { return "'" + value.replace("'", "'\"'\"'") + "'"; }
    private void require(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    private void pass(String message) { report.append("PASS: ").append(message).append('\n'); }
    private void waitFor(BooleanSupplier condition, String message) {
        long until = SystemClock.elapsedRealtime() + 180000;
        while (SystemClock.elapsedRealtime() < until) { if (condition.getAsBoolean()) return; SystemClock.sleep(100); }
        throw new AssertionError(message + "; state=" + SessionService.stateNative());
    }
    private byte[] consume(InputStream stream) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(); byte[] chunk = new byte[8192]; int n;
        while ((n = stream.read(chunk)) >= 0) out.write(chunk, 0, n); return out.toByteArray();
    }
    private String execute(String command) throws Exception {
        return execute(command, 180);
    }
    private String execute(String command, int seconds) throws Exception {
        java.lang.Process child = new ProcessBuilder(getTargetContext().getApplicationInfo().nativeLibraryDir + "/libgoblinuml-ctl.so",
            new File(directory, "control.sock").toString(), "exec", command).redirectErrorStream(true).start();
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        Thread reader = new Thread(() -> { try { output.write(consume(child.getInputStream())); } catch (IOException ignored) { } }); reader.start();
        if (!child.waitFor(seconds, TimeUnit.SECONDS)) { child.destroyForcibly(); throw new AssertionError("Guest command timed out: " + command); }
        reader.join(); String result = text(output.toByteArray()).replace("\r", "");
        require(child.exitValue() == 0, "Guest command failed: " + command + "\n" + result); return result;
    }
    private void startLinux() {
        runOnMainSync(() -> getTargetContext().startForegroundService(new Intent(getTargetContext(), SessionService.class)));
        waitFor(() -> "Running".equals(SessionService.stateNative()), "Linux boot");
    }
    private void ready() throws Exception {
        startLinux(); require(execute("cat /proc/1/comm").trim().equals("systemd"), "systemd must be PID 1");
    }
    private void maintenance(String action, File file, String previous) throws Exception {
        File result = new File(getTargetContext().getFilesDir(), "maintenance-result.txt"); Files.deleteIfExists(result.toPath());
        Intent intent = new Intent(getTargetContext(), MaintenanceService.class).setAction(action);
        if (file != null) intent.setData(Uri.fromFile(file)); if (previous != null) intent.putExtra("disk", previous);
        runOnMainSync(() -> getTargetContext().startForegroundService(intent));
        long until = SystemClock.elapsedRealtime() + 900000;
        while (!result.exists() && SystemClock.elapsedRealtime() < until) SystemClock.sleep(100);
        require(result.exists(), "Maintenance did not finish: " + action);
        String message = text(Files.readAllBytes(result.toPath())); require(!message.contains("failed"), message);
        waitFor(() -> SessionService.runningNative() && (SessionService.rescueNative() || "Running".equals(SessionService.stateNative())), "Restart after " + action);
    }
    private void send(Socket socket, String value) throws Exception {
        byte[] expected = bytes(value), received = new byte[expected.length]; socket.setSoTimeout(10000);
        socket.getOutputStream().write(expected); new DataInputStream(socket.getInputStream()).readFully(received);
        require(Arrays.equals(expected, received), "TCP payload mismatch");
    }
    private void tcp(String address, int port) throws Exception { try (Socket socket = new Socket()) { socket.connect(new InetSocketAddress(address, port), 10000); send(socket, token); } }
    private void udp(String address, int port) throws Exception {
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setSoTimeout(10000); byte[] value = bytes(token);
            socket.send(new DatagramPacket(value, value.length, InetAddress.getByName(address), port));
            byte[] result = new byte[100]; DatagramPacket packet = new DatagramPacket(result, result.length); socket.receive(packet);
            require(text(Arrays.copyOf(result, packet.getLength())).equals(token), "UDP payload mismatch");
        }
    }
    private void ports(String value) { String error = NetworkSettings.applyNative(value); require(error.isEmpty(), error); }
    private void network() throws Exception {
        ready(); require(execute("systemctl is-system-running").trim().equals("running"), "Failed system services");
        String service = "goblin-network-check-" + token;
        File savedFile = new File(directory, "ports.conf"); String saved = savedFile.exists() ? text(Files.readAllBytes(savedFile.toPath())) : "";
        String server = "import socket,threading,select\n" +
            "def client(c):\n try:\n  while True:\n   b=c.recv(65536)\n   if not b: break\n   c.sendall(b)\n finally: c.close()\n" +
            "ss=[]\nfor family,addr in [(socket.AF_INET,'0.0.0.0'),(socket.AF_INET6,'::')]:\n for kind in [socket.SOCK_STREAM,socket.SOCK_DGRAM]:\n" +
            "  s=socket.socket(family,kind); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)\n" +
            "  if family==socket.AF_INET6:s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,1)\n" +
            "  s.bind((addr,38080))\n  if kind==socket.SOCK_STREAM:s.listen()\n  ss.append(s)\n" +
            "while True:\n for s in select.select(ss,[],[])[0]:\n  if s.type==socket.SOCK_STREAM:threading.Thread(target=client,args=(s.accept()[0],),daemon=True).start()\n  else:\n   data,peer=s.recvfrom(65536);s.sendto(data,peer)\n";
        try {
            execute("systemd-run --quiet --unit=" + service + " /usr/bin/python3 -u -c " + quote(server));
            execute("python3 -c " + quote("import socket,time\nfor i in range(100):\n try:\n  s=socket.create_connection(('127.0.0.1',38080),.2);s.close();break\n except OSError:time.sleep(.1)\nelse:raise Exception('server did not start')"));
            String local = "tcp 38081 38080 local\nudp 38081 38080 local\n"; ports(local);
            tcp("127.0.0.1", 38081); tcp("::1", 38081); udp("127.0.0.1", 38081); udp("::1", 38081);
            pass("Android localhost reaches systemd-managed TCP and UDP servers over IPv4 and IPv6");
            try (Socket live = new Socket("127.0.0.1", 38081)) {
                send(live, "before"); ports(local + "tcp 38082 38080 lan\nudp 38082 38080 lan\n"); send(live, "after");
                tcp("127.0.0.1", 38082); tcp("::1", 38082); udp("127.0.0.1", 38082); udp("::1", 38082);
                try (ServerSocket occupied = new ServerSocket(0, 5, InetAddress.getByName("127.0.0.1"))) {
                    String error = NetworkSettings.applyNative("tcp " + occupied.getLocalPort() + " 38080 local\n");
                    require(!error.isEmpty(), "Occupied port incorrectly acknowledged");
                    tcp("127.0.0.1", 38081); send(live, "rollback");
                }
            }
            pass("Live changes preserve open TCP connections; conflicting ports retain the previous rules");
            StringBuilder many = new StringBuilder();
            for (int port = 39000; port < 39130; ++port) many.append("tcp ").append(port).append(" 38080 local\n");
            ports(many.toString()); tcp("127.0.0.1", 39129); tcp("::1", 39129);
            pass("260 IPv4/IPv6 listener rules work beyond the removed 255-rule limit");
            execute("getent ahosts apt.goblinreactor.com >/dev/null; python3 -c " + quote(
                "import socket,struct\nq=struct.pack('!6H',42,256,1,0,0,0)+b'\\x03deb\\x06debian\\x03org\\0'+struct.pack('!2H',1,1)\n" +
                "for typ in (socket.SOCK_DGRAM,socket.SOCK_STREAM):\n s=socket.socket(socket.AF_INET,typ);s.settimeout(15);s.connect(('10.0.2.3',53))\n" +
                " if typ==socket.SOCK_DGRAM:s.send(q);r=s.recv(65535)\n else:\n  s.sendall(struct.pack('!H',len(q))+q);f=s.makefile('rb');n=struct.unpack('!H',f.read(2))[0];r=f.read(n)\n" +
                " assert len(r)>=12 and r[:2]==q[:2] and r[3]&15==0 and struct.unpack('!H',r[6:8])[0]>0,r\n s.close()"));
            pass("Guest UDP and TCP DNS queries resolve through Android's resolver");
            int terminal = SessionService.openTerminalNative("goblin", false); require(terminal > 0, "Open user terminal");
            waitFor(() -> SessionService.terminalStateNative(terminal) == 1, "User terminal ready");
            SystemClock.sleep(1500);
            require(execute("loginctl list-sessions --no-legend --no-pager").contains("goblin"), "Debian PAM/logind session missing");
            execute("systemctl is-active user@1000.service; test -d /run/user/1000");
            SessionService.closeTerminalNative(terminal);
            execute("systemctl is-active " + service); ports(local); tcp("127.0.0.1", 38081);
            pass("Debian user sessions start a user service manager; server services survive terminal closure");
        } finally {
            ports(saved); execute("systemctl stop " + service + "; systemctl reset-failed " + service + " 2>/dev/null || true");
        }
    }
    private void recovery() throws Exception {
        ready(); String probe = "/home/goblin/.backup-check-" + token;
        File backup = new File(getTargetContext().getCacheDir(), "linux-" + token + ".goblin.gz");
        Set<String> original = new HashSet<>(Arrays.asList(directory.list()));
        try {
            execute("printf before > " + probe + "; sync"); maintenance("backup", backup, null);
            require(backup.length() > 0, "Empty backup");
            execute("printf after > " + probe + "; sync"); maintenance("restore", backup, null);
            require(execute("cat " + probe).equals("before"), "Restored disk content differs");
            String previous = null;
            for (String name : directory.list()) if (name.startsWith("rootfs.before-restore.") && !original.contains(name)) previous = name;
            require(previous != null, "Previous disk not retained"); maintenance("previous", null, previous);
            require(execute("cat " + probe).equals("after"), "Previous disk recovery differs");
            pass("Android backup, restore and previous-disk recovery retain the expected files and restart Linux");
            maintenance("rescue", null, null); require(SessionService.rescueNative(), "Not in rescue mode");
            execute("test -b /dev/ubda; test -x /sbin/e2fsck; test -x /sbin/resize2fs; mount -o ro /dev/ubda /mnt; cat /mnt" + probe + "; umount /mnt");
            maintenance("normal", null, null); require(execute("cat " + probe).equals("after"), "Rescue changed user data");
            pass("Rescue boots independently of Debian and can inspect the retained ext4 disk read-only");
        } finally {
            if (SessionService.rescueNative()) maintenance("normal", null, null);
            execute("rm -f " + probe + "; sync"); backup.delete();
            for (String name : directory.list()) if (name.startsWith("rootfs.before-restore.") && !original.contains(name)) new File(directory, name).delete();
        }
    }
    private void transport() throws Exception {
        ready(); String path = "/tmp/goblin-transport-" + token;
        Random random = new Random(0x474c4d55L);
        try {
            // Larger than the TTY receive queue, followed by another request:
            // a throttled serial input must resume without a new epoll edge.
            for (int round = 0; round < 16; ++round) {
                byte[] payload = new byte[16384 + round * 3072]; random.nextBytes(payload);
                String encoded = Base64.getEncoder().encodeToString(payload);
                execute("umask 077; printf %s " + quote(encoded) + " | base64 -d > " + path);
                require(execute("wc -c < " + path).trim().equals(Integer.toString(payload.length)), "Request after input backpressure");
                require(Arrays.equals(payload, SessionService.readGuestNative(path)), "Binary serial round trip " + round);
            }
            pass("Sixteen large binary transfers preserve every byte and resume serial input after backpressure");
        } finally { execute("rm -f " + path); }
    }
    private void upgrade(boolean verify) throws Exception {
        ready(); File record = new File(getTargetContext().getFilesDir(), "release-upgrade-check");
        String probe = "/home/goblin/.release-upgrade-check";
        String snapshot = "set -e; stat -c '%i:%u:%g:%a' " + probe + "; sha256sum " + probe + " /etc/passwd /etc/group /etc/shadow /etc/hostname /etc/hosts /etc/resolv.conf /etc/sudoers /etc/sudoers.d/90-goblin /etc/apt/sources.list.d/goblinreactor.sources /home/goblin/.bashrc /home/goblin/.profile; dpkg-query -W -f='${Package} ${Version} ${db:Status-Status}\\n'";
        if (!verify) {
            execute("test ! -e " + probe + "; printf %s " + token + " > " + probe + "; chown goblin:goblin " + probe + "; chmod 600 " + probe + "; sync");
            Files.write(record.toPath(), bytes(execute(snapshot))); pass("Recorded Linux files, metadata and package versions before APK replacement");
        } else {
            require((getTargetContext().getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0, "Release is debuggable");
            require(text(Files.readAllBytes(record.toPath())).equals(execute(snapshot)), "APK upgrade changed Linux data/configuration/packages");
            execute("rm " + probe + "; test -z \"$(dpkg --audit)\"; sync"); record.delete();
            pass("Signed release upgrade preserves Linux data, ownership, configuration and package versions");
        }
    }
    private void soak() throws Exception {
        int seconds = Integer.parseInt(arguments.getString("seconds", "600")); require(seconds > 0, "Positive test duration required");
        File shared = new File(directory, "share/parallel-check-" + token);
        String binary = "/tmp/parallel-check-" + token;
        try {
            require(shared.getParentFile().isDirectory() || shared.getParentFile().mkdirs(), "Test boot assets directory");
            try (InputStream source = getContext().getAssets().open("uml-parallel-test")) { Files.write(shared.toPath(), consume(source)); }
            ready();
            execute("cp /run/goblin-host/" + shared.getName() + " " + binary + "; chmod 700 " + binary);
            String mask = text(Files.readAllBytes(new File("/sys/devices/system/cpu/present").toPath())).trim();
            int cpus = 0; for (String part : mask.split(",")) { String[] range = part.split("-"); cpus += range.length == 1 ? 1 : Integer.parseInt(range[1]) - Integer.parseInt(range[0]) + 1; }
            require(Integer.parseInt(execute("getconf _NPROCESSORS_ONLN").trim()) == cpus, "Hardware CPU count differs");
            String output = execute("set -e; end=$(( $(date +%s) + " + seconds + " )); n=0; while [ $(date +%s) -lt $end ]; do " + binary + "; n=$((n+1)); done; printf 'Completed %s native multicore iterations\\n' \"$n\"; test -z \"$(dpkg --audit)\"; test \"$(systemctl is-system-running)\" = running; sync", seconds + 180);
            report.append(output); pass("Sustained native multicore, TLS, mapping coherence, migration and fork/exec workload for at least " + seconds + " seconds on " + cpus + " CPUs");
        } finally {
            try { execute("rm -f " + binary); } catch (Throwable error) { report.append("Cleanup: ").append(error).append('\n'); }
            shared.delete();
        }
    }
    private void unplugged() throws Exception {
        ready(); int seconds = Integer.parseInt(arguments.getString("seconds", "300")); require(seconds > 0, "Positive duration required");
        int waitSeconds = Integer.parseInt(arguments.getString("wait-seconds", "900")); require(waitSeconds > 0, "Positive setup deadline required");
        PowerManager power = getTargetContext().getSystemService(PowerManager.class);
        boolean original = SessionService.keepAwake(getTargetContext());
        String unit = "goblin-battery-check-" + token, heartbeat = "/tmp/" + unit;
        File status = new File(getTargetContext().getFilesDir(), "unplugged-status.txt");
        try {
            runOnMainSync(() -> SessionService.setKeepAwake(getTargetContext(), true));
            execute("systemd-run --quiet --unit=" + unit + " /bin/sh -c " + quote("i=0; while :; do i=$((i+1)); printf '%s' $i > " + heartbeat + "; sleep 1; done"));
            execute("while [ ! -s " + heartbeat + " ]; do sleep .1; done");
            String boot = execute("cat /proc/sys/kernel/random/boot_id").trim();
            report.append("Android battery-optimization exemption: ").append(power.isIgnoringBatteryOptimizations(getTargetContext().getPackageName())).append('\n');
            batteryStatus(status, "READY: unplug USB and turn the screen off for " + seconds + " seconds");
            long deadline = SystemClock.elapsedRealtime() + waitSeconds * 1000L, start = 0; int before = 0, samples = 0;
            while (SystemClock.elapsedRealtime() < deadline) {
                Intent battery = getTargetContext().registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                boolean unplugged = battery != null && battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) == 0;
                if (unplugged && !power.isInteractive()) {
                    if (start == 0) {
                        start = SystemClock.elapsedRealtime(); deadline = start + (seconds + 60L) * 1000L;
                        before = Integer.parseInt(execute("cat " + heartbeat).trim());
                        batteryStatus(status, "RUNNING: actual battery power, screen off");
                    }
                    ++samples;
                    if (SystemClock.elapsedRealtime() - start >= seconds * 1000L) break;
                } else if (start != 0) throw new AssertionError("Screen woke or external power returned before the test finished");
                SystemClock.sleep(1000);
            }
            require(start != 0 && SystemClock.elapsedRealtime() - start >= seconds * 1000L, "No complete unplugged screen-off interval");
            int after = Integer.parseInt(execute("cat " + heartbeat).trim());
            require(after >= before + seconds / 2, "Guest heartbeat stalled on battery power");
            require(execute("cat /proc/sys/kernel/random/boot_id").trim().equals(boot), "Linux restarted during the battery test");
            pass("Actual battery power and screen-off operation for " + seconds + " seconds; " + samples + " Android power samples; guest heartbeat advanced " + (after-before));
            pass("The same Linux instance survived the complete battery interval");
            batteryStatus(status, "PASS: reconnect USB to collect the report");
        } finally {
            try { execute("systemctl stop " + unit + "; rm -f " + heartbeat); }
            finally { runOnMainSync(() -> SessionService.setKeepAwake(getTargetContext(), original)); }
        }
    }
    private void batteryStatus(File file, String message) throws IOException {
        Files.write(file.toPath(), bytes(message));
        android.util.Log.i("goblin-battery", token + " " + message);
    }
    @Override public void onCreate(Bundle args) { super.onCreate(args); arguments = args == null ? new Bundle() : args; start(); }
    @Override public void onStart() {
        boolean ok = false; String mode = arguments.getString("mode", "network");
        directory = new File(getTargetContext().getFilesDir(), "uml");
        try {
            if (mode.equals("network")) network();
            else if (mode.equals("transport")) transport();
            else if (mode.equals("recovery")) recovery();
            else if (mode.equals("upgrade-record")) upgrade(false);
            else if (mode.equals("upgrade-verify")) upgrade(true);
            else if (mode.equals("soak")) soak();
            else if (mode.equals("unplugged")) unplugged();
            else if (mode.equals("collect-unplugged")) report.append(text(Files.readAllBytes(new File(getTargetContext().getFilesDir(), "services-unplugged-report.txt").toPath())));
            else if (mode.equals("exec")) { ready(); report.append(execute(arguments.getString("command"))); }
            else throw new IllegalArgumentException("Unknown mode: " + mode);
            // Android force-stops the target when instrumentation finishes.
            // Commit completed setup commands before that abrupt VM teardown;
            // crash-consistency tests inject their power loss explicitly.
            if ("Running".equals(SessionService.stateNative())) execute("sync");
            ok = true;
        } catch (Throwable error) { report.append("FAIL: ").append(error).append('\n'); StringWriter stack = new StringWriter(); error.printStackTrace(new PrintWriter(stack)); report.append(stack); if (mode.equals("unplugged")) android.util.Log.e("goblin-battery", token + " FAIL: " + error); }
        report.append(ok ? "GOBLIN SERVICES PASS\n" : "GOBLIN SERVICES FAIL\n");
        try { Files.write(new File(getTargetContext().getFilesDir(), "services-" + mode + "-report.txt").toPath(), bytes(report.toString())); } catch (Exception ignored) { }
        Bundle result = new Bundle(); result.putString("stream", report.toString()); finish(ok ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
    }
}
