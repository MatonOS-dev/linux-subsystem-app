package org.matonos.linux;

import android.net.LocalSocket;
import android.system.Os;
import org.matonos.linux.runtime.NativeBroker;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Runs with src/test/fixtures/dbus doubles, never with a VM or JNI broker. */
public final class DBusRuntimeFailureTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    static byte[] frame(int size,int count,int payload){
        return ByteBuffer.allocate(8+payload).order(ByteOrder.LITTLE_ENDIAN).putInt(size).putInt(count).array();
    }
    static LocalSocket socket(byte[] bytes){LocalSocket s=new LocalSocket();s.input=new ByteArrayInputStream(bytes);return s;}
    static void failure(byte[] bytes,boolean readFailure)throws Exception {
        LocalSocket s=socket(bytes);FileDescriptor fd=new FileDescriptor();s.ancillary=new FileDescriptor[]{fd};
        LocalSocketChannel c=new LocalSocketChannel(s);s.failRead=readFailure;
        try{c.read();throw new AssertionError();}catch(IOException|IllegalArgumentException expected){}
        check(Collections.frequency(Os.closed,fd)==1);
        c.close();
    }
    static void received()throws Exception {
        failure(new byte[3],true);failure(new byte[3],false);failure(frame(16,1,8),false);
        failure(frame(1,1,0),false);failure(frame(16,0,16),false);
        LocalSocket s=socket(frame(16,1,16));FileDescriptor fd=new FileDescriptor();s.ancillary=new FileDescriptor[]{fd};
        LocalSocketChannel c=new LocalSocketChannel(s);PortalChannel.Frame f=c.read();
        check(f.fds.size()==1&&!Os.closed.contains(fd));c.closeDescriptors(f.fds);c.close();
        check(Collections.frequency(Os.closed,fd)==1);
        s=socket(new byte[0]);fd=new FileDescriptor();s.ancillary=new FileDescriptor[]{fd};c=new LocalSocketChannel(s);
        check(c.read()==null&&Os.closed.contains(fd));c.close();
    }
    static void sent()throws Exception {
        LocalSocket s=socket(new byte[0]);LocalSocketChannel c=new LocalSocketChannel(s);
        c.write(new byte[16],Arrays.asList(new FileDescriptor()));c.write(new byte[16],Collections.emptyList());
        check(s.writes==2&&s.sent==1&&s.outbound==null&&s.bytes.size()==48);
        s.failWrite=true;
        try{c.write(new byte[16],Arrays.asList(new FileDescriptor()));throw new AssertionError();}catch(IOException expected){}
        check(s.outbound==null);c.close();
    }
    static void interrupted()throws Exception {
        Path directory=Files.createTempDirectory("dbus-interrupt-");
        Thread.currentThread().interrupt();
        try {
            new SessionBus(null,directory.toFile(),"app/test/x86_64/stable",0,b->{},null);
            throw new AssertionError();
        } catch(InterruptedException expected) {
            check(Thread.currentThread().isInterrupted());
            check(JavaPortal.closes==1&&NativeBroker.starts==1&&NativeBroker.stops==1);
            check(!Files.exists(directory.resolve("bus.policy")));
        } finally {Thread.interrupted();Files.delete(directory);}
    }
    public static void main(String[] args)throws Exception {
        received();sent();interrupted();
        System.out.println("PASS: received FD failure cleanup, exactly-once FD send and clear, interrupted startup cleanup");
    }
}
