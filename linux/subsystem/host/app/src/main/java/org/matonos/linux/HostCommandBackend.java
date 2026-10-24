package org.matonos.linux;

import java.io.FileDescriptor;
import java.util.*;
import java.util.function.Consumer;
import static org.matonos.linux.PortalWire.*;

/** Flatpak Development v1. Every child belongs to this app's resident sandbox. */
final class HostCommandBackend implements AutoCloseable {
    static final String PATH="/org/freedesktop/Flatpak/Development";
    static final String IFACE="org.freedesktop.Flatpak.Development";
    interface Child extends AutoCloseable {
        int pid();
        int waitStatus() throws Exception;
        void signal(int signal,boolean group) throws Exception;
        void close();
    }
    interface Spawner extends AutoCloseable {
        default void close() {}
        Child spawn(byte[] cwd,List<byte[]> argv,Map<Integer,FileDescriptor> fds,Map<String,String> env,int flags) throws Exception;
    }
    private static final class Entry {
        final String owner; final Child child; final boolean watch;
        boolean started;
        Entry(String owner,Child child,boolean watch){this.owner=owner;this.child=child;this.watch=watch;}
    }
    private final Spawner spawner;
    private final Consumer<Message> output;
    private final byte[] flatpakExecutable;
    private final Map<Integer,Entry> children=new HashMap<>();
    private Child portal;
    private int serial;
    private boolean closed;
    HostCommandBackend(Spawner spawner,Consumer<Message> output){this(spawner,output,null);}
    HostCommandBackend(Spawner spawner,Consumer<Message> output,String flatpakExecutable){
        this.spawner=spawner;this.output=output;
        this.flatpakExecutable=flatpakExecutable==null?null:(flatpakExecutable+"\0").getBytes(java.nio.charset.StandardCharsets.UTF_8);
    }
    private Message reply(Message c,int type,String sig,Object... body){
        Message m=new Message();m.type=type;m.serial=++serial;m.sender=":1.0";m.destination=c.sender;
        m.replySerial=c.serial;m.signature=sig;m.body=Arrays.asList(body);return m;
    }
    private static void signature(Message c,String expected){if(!expected.equals(c.signature))throw new IllegalArgumentException("Expected "+expected);}
    static byte[] bytes(Object value){
        List<?> list=(List<?>)value;byte[] result=new byte[list.size()];
        if(result.length==0)throw new IllegalArgumentException("Missing NUL terminator");
        for(int i=0;i<result.length;i++){int n=((Number)list.get(i)).intValue();if(n<0||n>255||n==0&&i!=result.length-1)throw new IllegalArgumentException("Invalid byte string");result[i]=(byte)n;}
        if(result[result.length-1]!=0)throw new IllegalArgumentException("Missing NUL terminator");return result;
    }
    synchronized List<Message> dispatch(Message c){
        try {
            if(closed)throw new IllegalStateException("Application runtime closed");
            if(c.sender==null||!c.sender.matches(":[0-9]+\\.[0-9]+"))throw new IllegalArgumentException("Missing sender");
            if(!PATH.equals(c.path))throw new IllegalArgumentException("Unknown object");
            if("org.freedesktop.DBus.Introspectable".equals(c.iface)&&"Introspect".equals(c.member)){
                signature(c,"");return Collections.singletonList(reply(c,2,"s",XML));
            }
            if("org.freedesktop.DBus.Properties".equals(c.iface)){
                if("Get".equals(c.member)){signature(c,"ss");if(!IFACE.equals(c.body.get(0))||!"version".equals(c.body.get(1)))throw new IllegalArgumentException("Unknown property");return Collections.singletonList(reply(c,2,"v",new Variant("u",1)));}
                if("GetAll".equals(c.member)){signature(c,"s");if(!IFACE.equals(c.body.get(0)))throw new IllegalArgumentException("Unknown interface");return Collections.singletonList(reply(c,2,"a{sv}",dictionary(Collections.singletonMap("version",new Variant("u",1)))));}
            }
            if(!IFACE.equals(c.iface))return error(c,"org.freedesktop.DBus.Error.UnknownInterface","Unknown interface");
            if("HostCommand".equals(c.member)){
                signature(c,"ayaaya{uh}a{ss}u");int flags=(Integer)c.body.get(4);if((flags&~3)!=0)throw new IllegalArgumentException("Unsupported flags");
                byte[] cwd=bytes(c.body.get(0));List<byte[]> argv=new ArrayList<>();for(Object arg:(List<?>)c.body.get(1))argv.add(bytes(arg));
                if(argv.isEmpty()||argv.get(0).length<2)throw new IllegalArgumentException("Empty command");
                if(flatpakExecutable!=null&&Arrays.equals(argv.get(0),"flatpak\0".getBytes(java.nio.charset.StandardCharsets.UTF_8)))argv.set(0,flatpakExecutable.clone());
                Map<Integer,FileDescriptor> fds=new LinkedHashMap<>();
                for(Object pair:(List<?>)c.body.get(2)){List<?> p=(List<?>)pair;int target=(Integer)p.get(0),index=(Integer)p.get(1);
                    if(target<0||index<0||index>=c.fds.size()||fds.containsKey(target))throw new IllegalArgumentException("Invalid fd mapping");fds.put(target,c.fds.get(index));}
                Map<String,String> env=new LinkedHashMap<>();for(Object pair:(List<?>)c.body.get(3)){List<?> p=(List<?>)pair;String key=(String)p.get(0),value=(String)p.get(1);
                    if(key.isEmpty()||key.indexOf('=')>=0||key.indexOf(0)>=0||value.indexOf(0)>=0)throw new IllegalArgumentException("Invalid environment");env.put(key,value);}
                if(children.size()>=128) return error(c,"org.freedesktop.DBus.Error.LimitsExceeded","Too many commands");
                Child child=spawner.spawn(cwd,argv,fds,env,flags);
                if(child.pid()<=0||children.containsKey(child.pid())){child.close();throw new IllegalStateException("Invalid child PID");}
                children.put(child.pid(),new Entry(c.sender,child,(flags&2)!=0));
                return Collections.singletonList(reply(c,2,"u",child.pid()));
            }
            if("HostCommandSignal".equals(c.member)){
                signature(c,"uub");Entry e=children.get((Integer)c.body.get(0));
                if(e==null||!e.owner.equals(c.sender))return error(c,"org.freedesktop.DBus.Error.AccessDenied","Command does not belong to caller");
                int signal=(Integer)c.body.get(1);if(signal<0||signal>64)throw new IllegalArgumentException("Invalid signal");
                e.child.signal(signal,(Boolean)c.body.get(2));return Collections.singletonList(reply(c,2,""));
            }
            return error(c,"org.freedesktop.DBus.Error.UnknownMethod","Unknown operation");
        }catch(IllegalArgumentException e){return error(c,"org.freedesktop.DBus.Error.InvalidArgs",e.getMessage());}
        catch(Exception e){return error(c,"org.freedesktop.DBus.Error.Failed","Sandbox command failed: "+e.getMessage());}
    }
    /** Launch the upstream Flatpak portal inside the resident compat-proxy
     * filesystem. This keeps its /proc and bwrap view aligned with the app. */
    synchronized void startFlatpakPortal(List<byte[]> argv,Map<String,String> env) throws Exception {
        if(closed||portal!=null)return;
        Child child=spawner.spawn(new byte[]{0},argv,Collections.emptyMap(),env,0);
        if(child.pid()<=0){child.close();throw new IllegalStateException("Invalid portal PID");}
        portal=child;
        Thread watcher=new Thread(()->{
            try{int status=child.waitStatus();java.util.logging.Logger.getLogger("MatonPortal").info("Upstream Flatpak portal exited with wait status "+status);}
            catch(Exception error){java.util.logging.Logger.getLogger("MatonPortal").warning("Lost upstream Flatpak portal process: "+error);}
            finally{child.close();synchronized(HostCommandBackend.this){if(portal==child)portal=null;}}
        },"flatpak-portal");
        watcher.setDaemon(true);watcher.start();
    }
    private List<Message> error(Message c,String name,String reason){Message m=reply(c,3,"s",reason);m.error=name;return Collections.singletonList(m);}
    /** Start reaping only after the method reply has been written to the bus. */
    synchronized void replied(Message c){
        for(Entry e:children.values())if(!e.started){e.started=true;Thread worker=new Thread(()->reap(e),"sandbox-command");worker.setDaemon(true);worker.start();}
    }
    private void reap(Entry e){
        try {
            int status=e.child.waitStatus();Message m;
            synchronized(this){if(children.get(e.child.pid())!=e)return;children.remove(e.child.pid());
                m=new Message();m.type=4;m.serial=++serial;m.sender=":1.0";m.destination=e.owner;m.path=PATH;m.iface=IFACE;m.member="HostCommandExited";m.signature="uu";m.body=Arrays.asList(e.child.pid(),status);}
            output.accept(m);
        }catch(Exception ignored){}finally{e.child.close();synchronized(this){children.remove(e.child.pid(),e);}}
    }
    synchronized void disconnected(String owner){for(Entry e:children.values())if(e.watch&&e.owner.equals(owner))e.child.close();}
    @Override public synchronized void close(){closed=true;for(Entry e:children.values()){try{e.child.signal(9,true);}catch(Exception ignored){}e.child.close();}children.clear();if(portal!=null){try{portal.signal(9,true);}catch(Exception ignored){}portal.close();portal=null;}spawner.close();}
    static final String XML="<node><interface name='"+IFACE+"'><property name='version' type='u' access='read'/>"
        +"<method name='HostCommand'><arg type='ay' direction='in'/><arg type='aay' direction='in'/><arg type='a{uh}' direction='in'/><arg type='a{ss}' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='out'/></method>"
        +"<method name='HostCommandSignal'><arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='b' direction='in'/></method>"
        +"<signal name='HostCommandExited'><arg type='u'/><arg type='u'/></signal></interface></node>";
}
