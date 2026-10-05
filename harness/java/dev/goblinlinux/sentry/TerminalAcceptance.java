package dev.goblinlinux.sentry;

import android.app.*;
import android.content.Intent;
import android.os.Bundle;
import android.view.*;
import android.view.inputmethod.*;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.function.BooleanSupplier;

/** Instrumentation for the debug harness, exercising the actual view and PTY. */
public final class TerminalAcceptance extends Instrumentation {
    private TerminalActivity activity;
    private Bundle arguments;
    private final StringBuilder report=new StringBuilder();
    private String root;
    private static final String UNICODE="café 界 e\u0301 🙂";
    @Override public void onCreate(Bundle args) { super.onCreate(args); arguments=args==null?new Bundle():args; start(); }
    private void pass(String message) { report.append("PASS: ").append(message).append('\n'); }
    private void require(boolean value,String message) {
        if(!value) throw new AssertionError(message);
    }
    private void waitFor(BooleanSupplier condition,String message) {
        long end=System.nanoTime()+30_000_000_000L;
        while(System.nanoTime()<end) {
            if(condition.getAsBoolean()) return;
            android.os.SystemClock.sleep(50);
        }
        throw new AssertionError(message+"; screen="+screen());
    }
    private <T extends View> T find(View view,Class<T> type) {
        if(type.isInstance(view)) return type.cast(view);
        if(view instanceof ViewGroup) {
            ViewGroup group=(ViewGroup)view;
            for(int i=0;i<group.getChildCount();i++) {
                T result=find(group.getChildAt(i),type); if(result!=null)return result;
            }
        }
        return null;
    }
    private KittyView terminal() { return find(activity.getWindow().getDecorView(),KittyView.class); }
    private static String quote(String text) { return "'"+text.replace("'","'\\''")+"'"; }
    private String screen() { return new String(KittyRuntime.dumpNative(),StandardCharsets.UTF_8); }
    private String read(String file) {
        return new String(SessionService.readGuestNative(root+"/"+file),StandardCharsets.UTF_8);
    }
    private void command(String text) {
        type(text); enter();
    }

    private void type(String value) {
        runOnMainSync(() -> {
            KittyView view=terminal(); view.requestFocus();
            InputConnection input=view.onCreateInputConnection(new EditorInfo());
            require(input.commitText(value,1),"IME commit rejected");
        });
    }
    private void key(int code) {
        runOnMainSync(() -> require(terminal().onKeyDown(code,new KeyEvent(KeyEvent.ACTION_DOWN,code)),"key rejected"));
    }
    private Button button(View view, String text) {
        if (view instanceof Button && ((Button)view).getText().toString().equals(text)) return (Button)view;
        if (view instanceof ViewGroup) for (int i=0;i<((ViewGroup)view).getChildCount();i++) {
            Button b=button(((ViewGroup)view).getChildAt(i),text); if(b!=null)return b;
        }
        return null;
    }
    private void press(String label) {
        runOnMainSync(() -> { Button b=button(activity.getWindow().getDecorView(),label); require(b!=null,"missing key "+label); b.performClick(); });
    }
    private void select(int id) {
        runOnMainSync(() -> { SessionService.selectNative(id); terminal().terminalChanged(); });
        waitFor(() -> SessionService.selectedNative()==id&&SessionService.terminalStateNative(id)==1,"terminal selection");
    }
    private int open(String user, boolean create) {
        int id=SessionService.openTerminalNative(user,create); require(id>0,"open terminal");
        waitFor(() -> SessionService.terminalStateNative(id)==1,"new terminal for "+user);
        select(id);
        // A PTY exists before Debian login finishes configuring it. Sending
        // commands before the prompt races login's normal input flush.
        waitFor(() -> screen().contains(user+"@goblin:")&&screen().trim().endsWith(user.equals("root")?"#":"$"),"login prompt for "+user);
        return id;
    }
    private void enter() { key(KeyEvent.KEYCODE_ENTER); }
    private void pinch(float factor) {
        final long start=android.os.SystemClock.uptimeMillis();
        final float x=terminal().getWidth()/2f, y=terminal().getHeight()/2f;
        // Use the screen's width: a fixed pixel span can stay below Android's
        // physical minimum pinch distance on a high-density display.
        final float span=terminal().getWidth()*0.85f/Math.max(1f,factor);
        for(int step=0;step<14;step++) {
            final int index=step;
            runOnMainSync(() -> {
                int count=index==0||index==13?1:2;
                MotionEvent.PointerProperties[] properties=new MotionEvent.PointerProperties[count];
                MotionEvent.PointerCoords[] points=new MotionEvent.PointerCoords[count];
                float spread=span*(index<2?1f:1f+(factor-1f)*Math.min(1f,(index-1)/10f));
                for(int i=0;i<count;i++) {
                    properties[i]=new MotionEvent.PointerProperties();properties[i].id=i;properties[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
                    points[i]=new MotionEvent.PointerCoords();points[i].x=x+(i==0?-1:1)*spread/2f;points[i].y=y;points[i].pressure=1;points[i].size=1;
                }
                int action=index==0?MotionEvent.ACTION_DOWN:index==1?MotionEvent.ACTION_POINTER_DOWN|(1<<8):index==12?MotionEvent.ACTION_POINTER_UP|(1<<8):index==13?MotionEvent.ACTION_UP:MotionEvent.ACTION_MOVE;
                MotionEvent event=MotionEvent.obtain(start,android.os.SystemClock.uptimeMillis(),action,count,properties,points,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
                terminal().dispatchTouchEvent(event);event.recycle();
            });
            android.os.SystemClock.sleep(25);
        }
        waitForIdleSync();
    }

    @Override public void onStart() {
        Bundle result=new Bundle(); boolean ok=false;
        android.content.SharedPreferences preferences=getTargetContext().getSharedPreferences("terminal",android.content.Context.MODE_PRIVATE);
        boolean hadFont=preferences.contains("font-size")||arguments.containsKey("restore-font-size");
        float originalFont=Float.parseFloat(arguments.getString("restore-font-size",Float.toString(preferences.getFloat("font-size",12f))));
        File sentinel=new File(getTargetContext().getFilesDir(),"kitty-graphics-acceptance");
        try {
            root="/home/goblin/goblin-kitty-"+UUID.randomUUID().toString().substring(0,12);
            activity=(TerminalActivity)startActivitySync(new Intent(getTargetContext(),TerminalActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitFor(() -> SessionService.stateNative().equals("Running")&&screen().contains("goblin:"),"Bash prompt");
            waitFor(() -> terminal().renderedFrames>2,"visible GLES terminal frames");
            pass("upstream kitty screen displays the guest Bash prompt");
            require(find(activity.getWindow().getDecorView(),EditText.class)==null,"Command entry remains");
            require(button(activity.getWindow().getDecorView(),"Start")==null&&button(activity.getWindow().getDecorView(),"Stop")==null,"Start/Stop remain");
            for(String label:new String[]{"Esc","Ctrl","Alt","Tab","←","↓","↑","→"}) require(button(activity.getWindow().getDecorView(),label)!=null,"missing extra key");
            final int original=SessionService.selectedNative();
            command("mkdir "+root+"; GOBLIN_KITTY_PROOF=alive; sleep 120 & echo $! > "+root+"/job; printf ready > "+root+"/ready");
            waitFor(() -> read("ready").equals("ready"),"shell setup");
            command("test $(id -un) = goblin && test $(id -u) != 0 && test \"$HOME:$PWD\" = /home/goblin:/home/goblin && test ! -w /etc/passwd && printf regular > "+root+"/identity");
            waitFor(() -> read("identity").equals("regular"),"regular user and home");
            pass("goblin starts in /home/goblin with a real unprivileged UID and protected system files");
            command("sudo -n id -u > "+root+"/sudo-root; sudo -n -u nobody id -u > "+root+"/sudo-nobody; sudo -n -i sh -c 'test \"$HOME\" = /root && id -u' > "+root+"/sudo-login; id -u > "+root+"/sudo-parent");
            waitFor(() -> read("sudo-root").equals("0\n")&&read("sudo-nobody").equals("65534\n")&&read("sudo-login").equals("0\n")&&!read("sudo-parent").equals("0\n")&&!read("sudo-parent").isEmpty(),"deployed passwordless sudo");
            pass("packaged sudo elevates commands without a password, supports login shells and leaves the terminal unprivileged");
            command("printf '%s\\n' $$ > "+root+"/sudo-shell-before");
            waitFor(() -> !read("sudo-shell-before").isEmpty(),"original sudo caller");
            command("sudo su -");
            waitFor(() -> screen().trim().endsWith("root@goblin:~#"),"interactive sudo su login prompt");
            command("printf '%s:%s:%s' \"$(id -u)\" \"$HOME\" \"$PWD\" > "+root+"/su-login");
            waitFor(() -> read("su-login").equals("0:/root:/root"),"root login shell identity and home");
            command("exit");
            waitFor(() -> screen().trim().endsWith("goblin@goblin:~$"),"sudo su returns to goblin");
            command("test $(id -u) != 0 && test \"$GOBLIN_KITTY_PROOF\" = alive && printf '%s\\n' $$ > "+root+"/sudo-shell-after");
            waitFor(() -> read("sudo-shell-after").equals(read("sudo-shell-before")),"sudo su preserves the original shell");
            pass("sudo su - opens an interactive root login shell; exit returns to the same unprivileged terminal");
            // Exercise Android's editor connection, including a supplementary
            // plane code point and a combining character, rather than adb text.
            type("printf '%s\\n' "+quote(UNICODE)+" > "+root+"/unicode"); enter();
            waitFor(() -> read("unicode").equals(UNICODE+"\n"),"Unicode IME to PTY");
            command("printf '\\033[2J\\033[H'; cat "+root+"/unicode");
            waitFor(() -> screen().contains(UNICODE),"Unicode screen cells");
            pass("IME commits accented, wide, combining and supplementary Unicode through Bash");
            // Pre-edit must not leak into the PTY or duplicate committed text.
            runOnMainSync(() -> {
                InputConnection input=terminal().onCreateInputConnection(new EditorInfo());
                input.setComposingText("discarded",1);
                input.setComposingText("replacement",1);
                input.commitText("printf composed > "+root+"/composition",1);
            });
            enter();waitFor(() -> read("composition").equals("composed"),"IME composition");
            pass("IME composition replaces pre-edit text before committing");
            command("python3 -c "+quote("print('\\n'.join('KITTY_LINE_%03d café 界' % i for i in range(1,161)))")+" > "+root+"/pages; less -R "+root+"/pages");
            waitFor(() -> screen().contains("KITTY_LINE_001"),"less first page");
            type("G");waitFor(() -> screen().contains("KITTY_LINE_160"),"less navigation");
            type("q");
            waitFor(() -> screen().trim().endsWith("goblin@goblin:~$"),"Bash returns after less");
            command("printf less_done > "+root+"/less");
            waitFor(() -> read("less").equals("less_done"),"less exit");
            pass("less paints, navigates and restores the alternate screen");
            command("vim.tiny -u NONE -N "+root+"/edited");
            waitFor(() -> screen().contains("[New File]")||screen().contains("[New]"),"Vim starts");
            type("i");type(UNICODE);
            waitFor(() -> screen().startsWith(UNICODE),"Vim receives inserted text");
            key(KeyEvent.KEYCODE_ESCAPE);type(":wq");
            waitFor(() -> screen().trim().endsWith(":wq"),"Vim command mode ready");enter();
            waitFor(() -> read("edited").equals(UNICODE+"\n"),"Vim writes Unicode");
            waitFor(() -> screen().trim().endsWith("goblin@goblin:~$"),"Bash returns after Vim");
            pass("Vim accepts direct input, Escape and commands, and saves Unicode");
            command("stty size > "+root+"/size-before");waitFor(() -> !read("size-before").isEmpty(),"initial PTY size");
            final float beforeFont=terminal().fontSize();
            pinch(beforeFont<24f?1.5f:0.7f);
            waitFor(() -> Math.abs(terminal().fontSize()-beforeFont)>1f,"pinch changes text size");
            final float zoomed=terminal().fontSize();
            command("stty size > "+root+"/size-zoomed");
            waitFor(() -> !read("size-zoomed").isEmpty()&&!read("size-zoomed").equals(read("size-before")),"pinch resizes guest PTY");
            pinch(beforeFont/zoomed);
            waitFor(() -> Math.abs(terminal().fontSize()-zoomed)>1f,"reverse pinch changes text size");
            final float savedFont=terminal().fontSize();
            report.append("Pinch font points: ").append(beforeFont).append(" -> ").append(zoomed).append(" -> ").append(savedFont)
                .append("; PTY: ").append(read("size-before").trim()).append(" -> ").append(read("size-zoomed").trim()).append('\n');
            pass("two-finger pinch zooms in and out and updates the guest terminal dimensions");
            runOnMainSync(() -> {
                KittyView view=terminal();
                ViewGroup.LayoutParams lp=view.getLayoutParams();
                lp.height=Math.max(100,view.getHeight()/2);view.setLayoutParams(lp);
            });
            android.os.SystemClock.sleep(600);
            command("stty size > "+root+"/size-after");
            waitFor(() -> !read("size-after").isEmpty()&&!read("size-after").equals(read("size-before")),"resized PTY");
            pass("Android surface resize changes guest PTY dimensions");
            ActivityMonitor monitor=addMonitor(TerminalActivity.class.getName(),null,false);
            runOnMainSync(() -> activity.recreate());
            activity=(TerminalActivity)monitor.waitForActivityWithTimeout(10000);
            removeMonitor(monitor);require(activity!=null,"activity recreation");
            waitFor(() -> terminal()!=null&&SessionService.stateNative().equals("Running"),"recreated terminal");
            require(terminal().fontSize()==savedFont,"font size lost during activity recreation");
            pass("pinch text size persists when the terminal activity is recreated");
            command("kill -0 $(cat "+root+"/job) && printf $GOBLIN_KITTY_PROOF > "+root+"/reconnect");
            waitFor(() -> read("reconnect").equals("alive"),"reconnected shell and job");
            pass("activity recreation preserves the kitty screen, shell and background job");
            Files.write(sentinel.toPath(),"broker sentinel".getBytes(StandardCharsets.UTF_8));
            String script="import os,tty,termios,select,base64,time\n"+
                "fd=os.open('/dev/tty',os.O_RDWR); old=termios.tcgetattr(fd); tty.setraw(fd)\n"+
                "try:\n"+
                " os.write(fd,b'\\x1b_Ga=T,t=t,i=42;'+base64.b64encode("+quote(sentinel.getAbsolutePath())+".encode())+b'\\x1b\\\\')\n"+
                " response=b''; end=time.monotonic()+3\n"+
                " while time.monotonic()<end and b'\\x1b\\\\' not in response:\n"+
                "  if select.select([fd],[],[],0.1)[0]: response+=os.read(fd,4096)\n"+
                " open('"+root+"/graphics','wb').write(response)\n"+
                "finally: termios.tcsetattr(fd,termios.TCSANOW,old); os.close(fd)\n";
            command("python3 -c "+quote(script));
            waitFor(() -> read("graphics").contains("ENOTSUP"),"graphics transport denied");
            require(new String(Files.readAllBytes(sentinel.toPath()),StandardCharsets.UTF_8).equals("broker sentinel"),"terminal output touched broker file");
            pass("kitty file graphics requests are rejected without reading or unlinking broker paths");
            String colors="import os,tty,termios,select,time\nold=termios.tcgetattr(0); tty.setraw(0)\ntry:\n"+
                " os.write(1,b'\\x1b]10;?\\x07\\x1b]11;?\\x07'); response=b''; end=time.monotonic()+3\n"+
                " while time.monotonic()<end and response.count(b'\\x1b\\\\')<2:\n"+
                "  if select.select([0],[],[],0.1)[0]: response+=os.read(0,4096)\n"+
                " open('"+root+"/colors','wb').write(response)\nfinally: termios.tcsetattr(0,termios.TCSANOW,old)";
            command("python3 -c "+quote(colors));
            waitFor(() -> read("colors").contains("10;rgb:ffff/ffff/ffff")&&read("colors").contains("11;rgb:0000/0000/0000"),"terminal color query replies");
            pass("terminal reports its white foreground and black background to guest applications");
            String raw="import os,tty,termios\nold=termios.tcgetattr(0); tty.setraw(0)\ntry:\n open('"+root+"/keys-ready','w').write('ready')\n data=b''\n while len(data)<17: data+=os.read(0,17-len(data))\n open('"+root+"/keys','w').write(data.hex())\nfinally: termios.tcsetattr(0,termios.TCSANOW,old)";
            command("python3 -c "+quote(raw));waitFor(() -> read("keys-ready").equals("ready"),"raw input ready");
            press("Ctrl");type("x");press("Alt");type("x");press("←");press("→");press("↑");press("↓");press("Esc");press("Tab");
            waitFor(() -> read("keys").equals("181b781b5b441b5b431b5b411b5b421b09"),"modifier and arrow bytes");
            require(!terminal().modifierSelected(4)&&!terminal().modifierSelected(2),"modifiers remained latched");
            pass("Ctrl and Alt apply once; Escape, Tab and all four arrows reach the PTY");
            command("printf '%s\\n' $$ > "+root+"/first-pid");waitFor(() -> !read("first-pid").isEmpty(),"first shell pid");
            int second=open("goblin",false);
            command("printf '%s\\n' $$ > "+root+"/second-pid; printf second > "+root+"/second");
            waitFor(() -> read("second").equals("second"),"second shell input");
            require(!read("first-pid").equals(read("second-pid")),"shared shell PID");
            select(original);command("printf $GOBLIN_KITTY_PROOF > "+root+"/switch");
            waitFor(() -> read("switch").equals("alive"),"independent shell survives switching");
            String server="import os,http.server,signal\nsignal.signal(signal.SIGHUP,signal.SIG_IGN)\nif os.fork(): os._exit(0)\nos.setsid()\nserver=http.server.HTTPServer(('127.0.0.1',0),http.server.SimpleHTTPRequestHandler)\nopen('"+root+"/server-pid','w').write(str(os.getpid()))\nopen('"+root+"/port','w').write(str(server.server_port))\nserver.serve_forever()";
            command("python3 -c "+quote(server)+" </dev/null >"+root+"/server-log 2>&1");
            waitFor(() -> !read("port").isEmpty(),"detached HTTP server");
            command("kill $(cat "+root+"/job); wait; exit");
            waitFor(() -> SessionService.terminalStateNative(original)==2,"first shell exits");
            select(second);
            command("python3 -c "+quote("import urllib.request; assert urllib.request.urlopen('http://127.0.0.1:"+read("port")+"/').status==200; open('"+root+"/http','w').write('alive')"));
            waitFor(() -> read("http").equals("alive"),"server survives original terminal exit");
            pass("independent terminals share Linux processes; a detached HTTP server survives its terminal exiting");
            command("exit");waitFor(() -> SessionService.terminalStateNative(second)==2,"last terminal exits");
            waitFor(() -> activity.isFinishing(),"exiting the last terminal closes its activity");
            require(SessionService.runningNative(),"terminal exit stopped Linux");
            activity=(TerminalActivity)startActivitySync(new Intent(getTargetContext(),TerminalActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitFor(() -> SessionService.selectedNative()>second&&screen().contains("goblin@goblin:~$"),"new terminal reconnects to Linux");
            command("kill -0 $(cat "+root+"/server-pid) && kill $(cat "+root+"/server-pid) && printf survivor > "+root+"/survivor");
            waitFor(() -> read("survivor").equals("survivor"),"server survives with no terminals open");
            pass("closing every terminal ends its shells while Linux and detached services remain available");
            String account="goblintest"+UUID.randomUUID().toString().substring(0,8);
            int regular=SessionService.selectedNative();int created=open(account,true);
            command("test $(id -un) = "+account+" && test \"$HOME:$PWD\" = /home/"+account+":/home/"+account+" && test $(id -u) != 1000 && test ! -r "+root+"/identity && ! sudo -n true && printf created > ~/.goblin-acceptance");
            waitFor(() -> new String(SessionService.readGuestNative("/home/"+account+"/.goblin-acceptance"),StandardCharsets.UTF_8).equals("created"),"new account identity, home and isolation");
            command("exit");waitFor(() -> SessionService.terminalStateNative(created)==2,"created account exits");
            int admin=open("root",false);
            // systemd keeps the user manager alive briefly after the last
            // session exits. Stop this disposable account's slice explicitly.
            command("systemctl stop user-$(id -u "+account+").slice && userdel --remove "+account+" && test ! -d /home/"+account+" && printf admin > "+root+"/admin");
            waitFor(() -> read("admin").equals("admin"),"administrative terminal");
            command("exit");waitFor(() -> SessionService.terminalStateNative(admin)==2,"admin exits");select(regular);
            pass("account creation gives a separate home and UID; root administration is an explicit terminal");
            int[] many=new int[12];
            for(int i=0;i<many.length;i++) many[i]=open("goblin",false);
            require(SessionService.terminalsNative().split("\n").length>=13,"terminal count was capped");
            select(regular);
            for(int id:many) { SessionService.closeTerminalNative(id); waitFor(() -> SessionService.terminalStateNative(id)==2,"extra terminal exits"); SessionService.releaseTerminalNative(id); }
            pass("more than eight simultaneous terminals; no Goblin terminal-count limit");
            command("rm -rf "+root+" && printf '\\033[2J\\033[H'; printf '%s\\n' 'Goblin · kitty on Android' "+quote(UNICODE)+" 'Bash, less, Vim, input, resize and reconnect passed.'");
            waitFor(() -> screen().startsWith("Goblin · kitty on Android\n")&&screen().trim().endsWith("goblin@goblin:~$")
                    ,"final screen and fixture cleanup");
            long frames=terminal().renderedFrames;
            waitFor(() -> terminal().renderedFrames>=frames+2,"render final screen");
            pass("terminal returns to Bash after full-screen application checks");
            android.graphics.Bitmap screenshot=getUiAutomation().takeScreenshot();
            require(screenshot!=null,"Android screenshot");
            try(FileOutputStream output=new FileOutputStream(new File(getTargetContext().getFilesDir(),"terminal-acceptance.png"))) {
                require(screenshot.compress(android.graphics.Bitmap.CompressFormat.PNG,100,output),"encode screenshot");
            }
            screenshot.recycle();
            report.append("KITTY ACCEPTANCE PASS\n");ok=true;
        } catch(Throwable error) {
            report.append("FAILED: ").append(error).append('\n').append(screen()).append('\n');
        } finally {
            // Reverse pinch is rounded to quarter points; restore the exact
            // preference rather than letting repeated tests drift the user's size.
            android.content.SharedPreferences.Editor editor=preferences.edit();
            if(hadFont) editor.putFloat("font-size",originalFont); else editor.remove("font-size");
            editor.commit();
            sentinel.delete();
            try { Files.write(new File(getTargetContext().getFilesDir(),"terminal-acceptance-report.txt").toPath(),report.toString().getBytes(StandardCharsets.UTF_8)); }
            catch(IOException error) { report.append(error);ok=false; }
            result.putString("stream",report.toString());
            finish(ok?Activity.RESULT_OK:Activity.RESULT_CANCELED,result);
        }
    }
}
