package org.matonos.linux;

import java.util.*;
import static org.matonos.linux.PortalWire.*;

/** Standalone regression tests for the Java D-Bus review findings. */
public final class DBusValidationTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static void reject(Runnable action) {
        try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Malformed input accepted");
    }
    static Message call() {
        Message m=new Message();m.type=1;m.serial=1;m.path="/test";m.member="Test";
        return m;
    }
    static void u32(byte[] bytes,int at,int value) {
        for(int i=0;i<4;i++)bytes[at+i]=(byte)(value>>(8*i));
    }
    static byte[] fields(int type,int[] ids,String[] signatures,Object... values) {
        DBusWriter w=new DBusWriter();w.u8('l');w.u8(type);w.u8(0);w.u8(1);w.i32(0);w.i32(1);w.i32(0);
        for(int i=0;i<ids.length;i++){w.align(8);w.u8(ids[i]);w.variant(signatures[i],values[i]);}
        w.patchU32(12,w.size()-16);w.align(8);return w.toBytes();
    }
    static void signaturesAndBounds() {
        for(String sig:Arrays.asList("()","a()","z","a","{sv}","a{vv}","a{s}","a{sss}","(u","u".repeat(256),"a".repeat(33)+"u"))
            reject(()->DBusSignature.split(sig));
        reject(()->new DBusReader(new byte[8]).value("()",0));
        DBusReader reader=new DBusReader(new byte[]{1,0,0,0,0,0,0,0});
        reject(()->reader.value("au",0));
        check(reader.position()==4);
        DBusReader nested=new DBusReader(new byte[]{4,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0});
        reject(()->nested.value("aau",0));
        for(boolean little:Arrays.asList(true,false)) {
            DBusReader q=new DBusReader(new byte[]{-1,-1});q.littleEndian=little;
            check(q.value("q",0).equals(65535));
            DBusReader n=new DBusReader(new byte[]{-1,-1});n.littleEndian=little;
            check(n.value("n",0).equals(-1));
        }
        DBusWriter w=new DBusWriter();w.value("g","u".repeat(255));check(w.toBytes()[0]==(byte)255);
        reject(()->new DBusWriter().value("g","u".repeat(256)));
        reject(()->new DBusWriter().value("g","z"));
        reject(()->new DBusWriter().variant("uu",1));
        reject(()->new DBusWriter().variant("",1));
        reject(()->new DBusWriter().value("()",Collections.emptyList()));
        reject(()->new DBusReader(new byte[]{2,'u','u',0,0,0,0,0}).value("v",0));
    }
    static void wire() {
        check(decode(encode(call())).member.equals("Test"));
        for(int type=1;type<=4;type++) {
            Message m=new Message();m.serial=1;m.type=type;reject(()->decode(encode(m)));
        }
        reject(()->decode(fields(1,new int[]{1,3,3},new String[]{"o","s","s"},"/test","Test","Test")));
        reject(()->decode(fields(1,new int[]{1,3},new String[]{"s","s"},"/test","Test")));
        reject(()->decode(fields(1,new int[]{0},new String[]{"u"},1)));
        byte[] crossing=encode(call());u32(crossing,12,1);reject(()->decode(crossing));
        byte[] extra=Arrays.copyOf(encode(call()),encode(call()).length+4);u32(extra,4,4);reject(()->decode(extra));
        byte[] padding=encode(call());padding[30]=1;reject(()->decode(padding));
        byte[] outerPadding=fields(2,new int[]{5},new String[]{"u"},1);
        // This field ends at 24; add an unknown byte field to leave final padding.
        outerPadding=fields(2,new int[]{5,42},new String[]{"u","y"},1,1);
        outerPadding[outerPadding.length-1]=1;byte[] badPadding=outerPadding;reject(()->decode(badPadding));
        Message text=call();text.signature="s";text.body=Arrays.asList("ok");
        byte[] utf8=encode(text);utf8[utf8.length-3]=(byte)0xc0;reject(()->decode(utf8));
        Message bool=call();bool.signature="b";bool.body=Arrays.asList(true);
        byte[] booleanBytes=encode(bool);booleanBytes[booleanBytes.length-4]=2;reject(()->decode(booleanBytes));
        byte[] bounded=fields(2,new int[]{5},new String[]{"u"},1);u32(bounded,12,4);reject(()->decode(bounded));
        for(int type=2;type<=4;type++) {
            Message m=call();m.type=type;m.replySerial=1;m.error="test.Error";m.iface="test.Interface";
            check(decode(encode(m)).type==type);
        }
    }
    static void glob() {
        check(PortalBackend.glob("org.gnome."+"*".repeat(100000),"org.gnome.desktop.wm.preferences"));
        check(!PortalBackend.glob("*".repeat(100000)+"Z","org.gnome.desktop.wm.preferences"));
        check(!PortalBackend.glob("*"+"a".repeat(100000)+"b","a".repeat(200000)));
        check(PortalBackend.glob("org.freedesktop.*","org.freedesktop.appearance"));
        check(!PortalBackend.glob("org.freedesktop.*","org.gnome.desktop"));
        check(PortalBackend.glob("[x].?","[x].?"));
        check(!PortalBackend.glob("[x].?","[x].z"));
        check(PortalBackend.glob("*","*a"));check(PortalBackend.glob("*",""));check(!PortalBackend.glob("?",""));
        check(PortalBackend.glob("exact","exact"));check(!PortalBackend.glob("exact","exact.more"));
    }
    static void noReply() {
        for(int flags:new int[]{0,1}) {
            Message m=call();m.flags=flags;
            class Channel implements PortalChannel {
                boolean read,closed;int writes,cleaned;
                public Frame read(){if(read)return null;read=true;return new Frame(encode(m),Collections.emptyList());}
                public void write(byte[] bytes,List<java.io.FileDescriptor> fds){writes++;}
                public void closeDescriptors(List<java.io.FileDescriptor> fds){cleaned++;}
                public void close(){closed=true;}
            }
            Channel c=new Channel();int[] calls={0};
            new PeerConnection(c,new PeerConnection.Handler(){
                public List<Message> methodCall(Message call){calls[0]++;Message ret=call();ret.type=2;ret.replySerial=1;
                    Message error=call();error.type=3;error.replySerial=1;error.error="test.Error";
                    Message signal=call();signal.type=4;signal.iface="test.Interface";return Arrays.asList(ret,error,signal);}
                public void signal(Message signal){}
            }).serve();
            check(calls[0]==1 && c.writes==(flags==0?3:1) && c.cleaned==1 && c.closed);
        }
    }
    public static void main(String[] args) {
        glob();signaturesAndBounds();wire();noReply();
        System.out.println("PASS: D-Bus review glob, signature/array bounds, wire validation, UINT16, writer, NO_REPLY_EXPECTED");
    }
}
