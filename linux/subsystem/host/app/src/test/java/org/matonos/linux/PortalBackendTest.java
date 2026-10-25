package org.matonos.linux;

import java.util.*;
import java.io.FileDescriptor;
import static org.matonos.linux.PortalWire.*;

/** Standalone JVM tests: no Android runtime or Gradle dependency required. */
public final class PortalBackendTest {
    static void check(boolean ok){if(!ok)throw new AssertionError();}
    static int serial;
    static Message call(String owner,String iface,String method,String sig,Object... args) {
        Message m=new Message();m.type=1;m.serial=++serial;m.sender=owner;m.destination="org.freedesktop.portal.Desktop";
        m.path=PortalBackend.DESKTOP;m.iface="org.freedesktop.portal."+iface;m.member=method;m.signature=sig;m.body=Arrays.asList(args);
        byte[] wire=encode(m);return decode(wire);
    }
    static List<Object> opts(String token) {return dictionary(Collections.singletonMap("handle_token",new Variant("s",token)));}
    static byte[] hex(String text) {
        byte[] out=new byte[text.length()/2];
        for(int i=0;i<out.length;i++)out[i]=(byte)Integer.parseInt(text.substring(2*i,2*i+2),16);
        return out;
    }
    /** Decode bytes produced by an independent D-Bus implementation (dbus-java 5.2.2):
     * a method call Settings.Read("org.freedesktop.appearance","color-scheme"). */
    static void crossImplementationFixture() {
        Message m=decode(hex("6c0100013100000001000000a000000001016f001f0000002f6f72672f667265656465736b746f70"
            +"2f706f7274616c2f6465736b746f700007017300040000003a312e3500000000060173001e0000006f72672e66726565"
            +"6465736b746f702e706f7274616c2e4465736b746f700000020173001f0000006f72672e667265656465736b746f702e"
            +"706f7274616c2e53657474696e6773000301730004000000526561640000000008016700027373001a0000006f72672e66"
            +"7265656465736b746f702e617070656172616e636500000c000000636f6c6f722d736368656d6500"));
        check(m.type==1&&m.serial==1);
        check("/org/freedesktop/portal/desktop".equals(m.path));
        check(":1.5".equals(m.sender));
        check("org.freedesktop.portal.Desktop".equals(m.destination));
        check("org.freedesktop.portal.Settings".equals(m.iface));
        check("Read".equals(m.member));
        check("ss".equals(m.signature));
        check(m.body.equals(Arrays.asList("org.freedesktop.appearance","color-scheme")));
        check(Arrays.equals(encode(m),encode(decode(encode(m)))));
    }
    public static void main(String[] args) throws Exception {
        crossImplementationFixture();
        PortalTestPeer.Platform platform=new PortalTestPeer.Platform();PortalBackend b=new PortalBackend(platform);
        Message read=b.dispatch(call(":1.1","Settings","Read","ss","org.freedesktop.appearance","color-scheme")).get(0);
        Variant v=(Variant)decode(encode(read)).body.get(0);check(v.signature.equals("v")&&((Variant)v.value).value.equals(1));
        v=(Variant)b.dispatch(call(":1.1","Settings","ReadOne","ss","org.freedesktop.appearance","color-scheme")).get(0).body.get(0);check(v.signature.equals("u"));
        check(b.dispatch(call(":1.1","Settings","ReadOne","ss","missing","missing")).get(0).error.endsWith("NotFound"));
        Message all=b.dispatch(call(":1.1","Settings","ReadAll","as",Arrays.asList("org.freedesktop.*"))).get(0);
        check(((List<?>)decode(encode(all)).body.get(0)).size()==1);
        check(((List<?>)b.dispatch(call(":1.1","Settings","ReadAll","as",Collections.emptyList())).get(0).body.get(0)).size()==2);
        check(((List<?>)b.dispatch(call(":1.1","Settings","ReadAll","as",Arrays.asList(""))).get(0).body.get(0)).size()==2);
        check(b.dispatch(call(":1.1","Settings","Read","s","bad")).get(0).type==3);
        List<Message> inhibit=b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("first")));
        check(inhibit.size()==2&&platform.held&&platform.transitions==1);String handle=(String)inhibit.get(0).body.get(0);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("first"))).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",1,opts("logout"))).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","Inhibit","sua{sv}","",8,opts("bad/token"))).get(0).type==3);
        b.dispatch(call(":1.2","Inhibit","Inhibit","sua{sv}","",4,opts("other")));check(platform.transitions==1);
        Message close=call(":1.2","Request","Close","");close.path=handle;check(b.dispatch(close).get(0).type==3&&platform.held);
        close.sender=":1.1";check(b.dispatch(close).get(0).type==2&&platform.held);
        b.disconnected(":1.2");check(!platform.held&&platform.transitions==2);
        List<Message> monitor=b.dispatch(call(":1.1","Inhibit","CreateMonitor","sa{sv}","",opts("monitor")));
        check(monitor.size()==3&&!platform.held);Message signal=decode(encode(monitor.get(2)));String session=(String)signal.body.get(0);
        check(b.dispatch(call(":1.2","Inhibit","QueryEndResponse","o",session)).get(0).type==3);
        check(b.dispatch(call(":1.1","Inhibit","QueryEndResponse","o",session)).get(0).type==2);
        check(b.dispatch(call(":1.1","OpenURI","OpenURI","ssa{sv}","","https://example.com",opts("uri"))).get(1).body.get(0).equals(0));
        check(b.dispatch(call(":1.1","OpenURI","OpenURI","ssa{sv}","","fail:",opts("uri"))).get(1).body.get(0).equals(2));
        check(b.dispatch(call(":1.1","OpenURI","OpenFile","sha{sv}","",0,opts("file"))).size()==2);
        check(b.dispatch(call(":1.1","OpenURI","OpenDirectory","sha{sv}","",0,opts("dir"))).size()==2);
        Message sample=call(":1.1","Settings","ReadAll","as",Collections.emptyList());byte[] bytes=encode(sample);
        bytes[4]=(byte)255;try{decode(bytes);throw new AssertionError();}catch(IllegalArgumentException e){}
        Message fdCall=call(":1.1","OpenURI","OpenFile","sha{sv}","",0,opts("fd-check"));
        try {decode(encode(fdCall),Arrays.asList(FileDescriptor.in,FileDescriptor.out));throw new AssertionError();}
        catch(IllegalArgumentException expected){}
        byte[] frameHeader=java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(16).putInt(1).array();
        check(Arrays.equals(PortalWire.validateFrameHeader(frameHeader),new int[]{16,1}));
        frameHeader=java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(2*1024*1024).putInt(0).array();
        check(PortalWire.validateFrameHeader(frameHeader)[0]==2*1024*1024);
        frameHeader=java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(15).putInt(0).array();
        try{PortalWire.validateFrameHeader(frameHeader);throw new AssertionError();}catch(IllegalArgumentException expected){}
        frameHeader=java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(16).putInt(254).array();
        try{PortalWire.validateFrameHeader(frameHeader);throw new AssertionError();}catch(IllegalArgumentException expected){}
        frameHeader=java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(16).putInt(-1).array();
        try{PortalWire.validateFrameHeader(frameHeader);throw new AssertionError();}catch(IllegalArgumentException expected){}
        b.close();check(!platform.held);
        for(int i=0;i<300;i++)check(b.dispatch(call(":1.3","Inhibit","Inhibit","sua{sv}","",8,opts("many_"+i))).get(0).type==2);
        check(b.dispatch(call(":1.3","Inhibit","CreateMonitor","sa{sv}","",opts("many_monitor"))).get(0).type==2);
        b.close();check(!platform.held);
        System.out.println("PASS: Java settings, variants/arrays, caller ownership, inhibition lifetime, monitors, OpenURI responses, SCM_RIGHTS count mismatch and malformed frame/message lengths");
    }
}
