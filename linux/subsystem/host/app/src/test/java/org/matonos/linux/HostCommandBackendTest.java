package org.matonos.linux;

import java.io.FileDescriptor;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import static org.matonos.linux.PortalWire.*;

/** Wire codec round trips plus ownership, reply-before-exit, and lifetimes. */
public final class HostCommandBackendTest {
    static final class Child implements HostCommandBackend.Child {
        final int pid; final CountDownLatch done=new CountDownLatch(1);
        int status,signal;boolean group,closed;
        Child(int pid){this.pid=pid;}
        public int pid(){return pid;}
        public int waitStatus() throws Exception{done.await();return status;}
        public void signal(int value,boolean all){signal=value;group=all;}
        public void close(){closed=true;done.countDown();}
        void exit(int value){status=value;done.countDown();}
    }
    static List<Object> bytes(String text){List<Object> result=new ArrayList<>();for(byte b:(text+"\0").getBytes(java.nio.charset.StandardCharsets.UTF_8))result.add(b&255);return result;}
    static byte[] term(String text){byte[] raw=text.getBytes(java.nio.charset.StandardCharsets.UTF_8);return java.util.Arrays.copyOf(raw,raw.length+1);}
    static Message call(String member,String signature,Object... body){Message m=new Message();m.type=1;m.serial=7;m.sender=":1.7";m.destination="org.freedesktop.Flatpak";m.path=HostCommandBackend.PATH;m.iface=HostCommandBackend.IFACE;m.member=member;m.signature=signature;m.body=Arrays.asList(body);return m;}
    static Message wire(Message m){m.fdCount=m.fds.size();return decode(encode(m),m.fds);}
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static Message command(int flags){return call("HostCommand","ayaaya{uh}a{ss}u",bytes("/app"),Arrays.asList(bytes("/usr/bin/sh"),bytes("-l")),Arrays.asList(Arrays.asList(0,0),Arrays.asList(1,0),Arrays.asList(2,0)),Arrays.asList(Arrays.asList("SHELL","/usr/bin/sh")),flags);}
    public static void main(String[] args) throws Exception {
        List<Child> launched=new ArrayList<>();BlockingQueue<Message> signals=new LinkedBlockingQueue<>();FileDescriptor tty=new FileDescriptor();
        HostCommandBackend backend=new HostCommandBackend((cwd,argv,fds,env,flags)->{
            check(new String(cwd).equals("/app\0"),"cwd");check(argv.size()==2,"argv");check(fds.size()==3&&fds.get(2)==tty,"fd capability indexes");check(env.get("SHELL").equals("/usr/bin/sh"),"env");
            Child child=new Child(42+launched.size());launched.add(child);return child;
        },signals::add);
        Message c=command(2);c.fds=Collections.singletonList(tty);Message decoded=wire(c);
        Message reply=wire(backend.dispatch(decoded).get(0));check(reply.type==2&&reply.body.get(0).equals(42),"real pid reply");
        launched.get(0).exit(7<<8);check(signals.poll(50,TimeUnit.MILLISECONDS)==null,"exit must follow reply");backend.replied(decoded);
        Message exit=wire(signals.poll(2,TimeUnit.SECONDS));check(exit.member.equals("HostCommandExited")&&exit.destination.equals(":1.7")&&exit.body.equals(Arrays.asList(42,7<<8)),"raw wait status signal");
        c=command(0);c.fds=Collections.singletonList(tty);backend.dispatch(wire(c));backend.replied(c);
        Message sig=call("HostCommandSignal","uub",43,15,true);sig.sender=":1.8";check(backend.dispatch(wire(sig)).get(0).error.endsWith("AccessDenied"),"foreign pid denied");sig.sender=":1.7";check(backend.dispatch(wire(sig)).get(0).type==2&&launched.get(1).signal==15&&launched.get(1).group,"group signal");
        backend.disconnected(":1.7");check(!launched.get(1).closed,"non-WATCH survives caller");
        c=command(2);c.fds=Collections.singletonList(tty);backend.dispatch(wire(c));backend.replied(c);backend.disconnected(":1.7");check(launched.get(2).closed,"WATCH kills on caller loss");
        c=command(4);c.fds=Collections.singletonList(tty);check(backend.dispatch(wire(c)).get(0).error.endsWith("InvalidArgs"),"flags rejected");
        c=command(0);c.fds=Collections.singletonList(tty);c.body.set(0,Arrays.asList(65));check(backend.dispatch(wire(c)).get(0).error.endsWith("InvalidArgs"),"unterminated byte string");
        c=command(0);check(backend.dispatch(wire(c)).get(0).error.endsWith("InvalidArgs"),"invalid fd handle");
        Message prop=call("Get","ss",HostCommandBackend.IFACE,"version");prop.iface="org.freedesktop.DBus.Properties";check(((Variant)wire(backend.dispatch(wire(prop)).get(0)).body.get(0)).value.equals(1),"version");
        backend.close();check(launched.get(1).closed&&launched.get(1).signal==9,"runtime closes command group");
        final String[] normalized={null};
        HostCommandBackend alias=new HostCommandBackend((cwd,argv,fds,env,flags)->{
            normalized[0]=new String(argv.get(0),java.nio.charset.StandardCharsets.UTF_8);
            Child child=new Child(99);child.exit(0);return child;
        },signals::add,"/native/libmatonos-app-flatpak.so");
        Message flatpak=call("HostCommand","ayaaya{uh}a{ss}u",bytes("/"),Arrays.asList(bytes("flatpak"),bytes("--version")),Collections.emptyList(),Collections.emptyList(),0);
        alias.dispatch(wire(flatpak));check("/native/libmatonos-app-flatpak.so\0".equals(normalized[0]),"flatpak command resolves to the packaged launcher");alias.close();
        AtomicReference<Map<String,String>> spawnEnv=new AtomicReference<>();
        HostCommandBackend spawn=new HostCommandBackend((cwd,argv,fds,env,flags)->{
            spawnEnv.set(new LinkedHashMap<>(env));Child child=new Child(100);child.exit(0);return child;
        },signals::add,"/native/libmatonos-app-flatpak.so");
        spawn.setFlatpakDirectories("/private/user-install","/private/system-install");
        Message portalSpawn=call("HostCommand","ayaaya{uh}a{ss}u",bytes("/"),
                Arrays.asList(bytes("/native/libmatonos-app-flatpak.so"),bytes("run")),Collections.emptyList(),
                Arrays.asList(Arrays.asList("FLATPAK_USER_DIR","/sandbox/missing"),Arrays.asList("KEEP","yes")),0);
        spawn.dispatch(wire(portalSpawn));
        check("/private/user-install".equals(spawnEnv.get().get("FLATPAK_USER_DIR"))&&
                "/private/system-install".equals(spawnEnv.get().get("FLATPAK_SYSTEM_DIR"))&&
                "yes".equals(spawnEnv.get().get("KEEP")),"portal Spawn restores private installation paths without losing env");
        spawn.close();
        System.out.println("PASS Development wire signature/fds, real pid, version, reply-before-exit, raw wait status, sender ownership, flags, WATCH_BUS, runtime cleanup");
        portalRestartRegression();
        System.out.println("PASS portal restarts across consecutive payload launches (each payload finds org.freedesktop.portal.Flatpak)");
    }

    /** Two (and three) consecutive payload launches in one runtime must each
     *  reach the flatpak portal: when a payload tree ends it takes the
     *  host-command server and the portal child down with it, and the stub
     *  must restart the portal for the next payload instead of leaving the
     *  session bus without org.freedesktop.portal.Flatpak (Steam round 19). */
    private static void portalRestartRegression() throws Exception {
        BlockingQueue<Message> signals=new LinkedBlockingQueue<>();
        List<Child> spawned=Collections.synchronizedList(new ArrayList<>());
        HostCommandBackend backend=new HostCommandBackend((cwd,argv,fds,env,flags)->{
            check(cwd.length==1&&cwd[0]==0,"portal cwd must be empty");
            check(fds.isEmpty(),"portal spawn carries no descriptors");
            Child child=new Child(200+spawned.size());
            spawned.add(child);
            return child;
        },signals::add);
        List<byte[]> portalArgv=Arrays.<byte[]>asList(term("/native/libmatonos-flatpak.so"),
                term("portal"),term("--no-idle-exit"),term("--verbose"));
        backend.startFlatpakPortal(portalArgv,Collections.emptyMap());
        check(spawned.size()==1,"first payload launch starts the portal");
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        spawned.get(0).exit(0); // first payload's tree ends, portal dies with it
        while(spawned.size()<2&&System.nanoTime()<deadline)Thread.sleep(10);
        check(spawned.size()>=2,"second payload launch restarts the portal");
        check(spawned.get(1).pid()!=spawned.get(0).pid(),"restart uses a fresh portal process");
        deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        spawned.get(1).exit(0); // second payload's tree ends too
        while(spawned.size()<3&&System.nanoTime()<deadline)Thread.sleep(10);
        check(spawned.size()>=3,"third payload launch restarts the portal again");
        backend.close();
        int count=spawned.size();
        deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(System.nanoTime()<deadline)Thread.sleep(20);
        check(spawned.size()==count,"no portal restart after the runtime is closed");
    }
}
