package org.matonos.linux;

import java.io.*;
import java.nio.*;
import java.util.*;
import static org.matonos.linux.PortalWire.*;

/** Real Java backend for C/Java interoperability tests, transported over pipes. */
public final class PortalTestPeer {
    static final class Platform implements PortalBackend.Platform {
        boolean held;
        int transitions;
        public Map<String,Map<String,Variant>> settings() {
            Map<String,Variant> a=new LinkedHashMap<>();a.put("color-scheme",new Variant("u",1));a.put("contrast",new Variant("u",0));
            a.put("accent-color",new Variant("(ddd)",Arrays.asList(0.1,0.2,0.3)));
            Map<String,Map<String,Variant>> s=new LinkedHashMap<>();s.put("org.freedesktop.appearance",a);
            s.put("org.gnome.desktop.wm.preferences",Collections.singletonMap("button-layout",new Variant("s",":")));return s;
        }
        public void hold(boolean next){if(held!=next)transitions++;held=next;}
        public boolean open(String owner,String method,Object target,Map<String,Variant> options){return !"fail:".equals(target);}
    }
    public static void main(String[] args) throws Exception {
        PortalBackend backend=new PortalBackend(new Platform());DataInputStream in=new DataInputStream(System.in);
        try {
            for(;;) {
                int size=Integer.reverseBytes(in.readInt()),fds=Integer.reverseBytes(in.readInt());
                if(size<16||size>MAX_MESSAGE||fds!=0)throw new IOException("Test frame");
                byte[] data=new byte[size];in.readFully(data);Message call=decode(data);
                if(call.type==4){backend.disconnected((String)call.body.get(0));continue;}
                for(Message reply:backend.dispatch(call)) {
                    byte[] bytes=encode(reply);System.out.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(bytes.length).putInt(0).array());System.out.write(bytes);
                }
                System.out.flush();
            }
        }catch(EOFException e){backend.close();}
    }
}
