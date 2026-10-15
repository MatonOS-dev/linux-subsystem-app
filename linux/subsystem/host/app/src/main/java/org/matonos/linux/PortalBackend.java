package org.matonos.linux;

import java.util.*;
import static org.matonos.linux.PortalWire.*;

/** Portal semantics and caller-scoped request state. All calls run on one channel thread. */
final class PortalBackend {
    static final String DESKTOP="/org/freedesktop/portal/desktop", PREFIX="org.freedesktop.portal.";
    interface Platform {
        Map<String,Map<String,Variant>> settings();
        void hold(boolean active) throws Exception;
        boolean open(String owner,String method, Object target, Map<String,Variant> options) throws Exception;
    }
    private static final class Handle {
        final String owner;
        final boolean hold, session;
        Handle(String owner,boolean hold,boolean session) {this.owner=owner;this.hold=hold;this.session=session;}
    }
    private final Platform platform;
    private final Map<String,Handle> handles=new LinkedHashMap<>();
    private int sequence;
    PortalBackend(Platform platform) {this.platform=platform;}
    private static final class Failure extends RuntimeException {
        final String name;
        Failure(String name,String message) {super(message);this.name=name;}
    }
    private static Failure invalid(String text) {return new Failure("org.freedesktop.DBus.Error.InvalidArgs",text);}
    private static void signature(Message call,String expected) {if(!expected.equals(call.signature))throw invalid("Expected signature "+expected);}
    private Message response(Message call,int type,String sig,Object... body) {
        Message m=new Message();m.type=type;m.serial=++sequence;if(m.serial==0)m.serial=++sequence;
        m.destination=call.sender;m.sender=":1.0";m.replySerial=call.serial;m.signature=sig;m.body=Arrays.asList(body);return m;
    }
    private Message signal(Message call,String path,String iface,String member,String sig,Object... body) {
        Message m=response(call,4,sig,body);m.replySerial=0;m.path=path;m.iface=iface;m.member=member;return m;
    }
    private static Variant uint(int v) {return new Variant("u",v);}
    private static int version(String iface) {
        if((PREFIX+"Settings").equals(iface))return 2;
        if((PREFIX+"Inhibit").equals(iface))return 3;
        if((PREFIX+"OpenURI").equals(iface))return 3;
        if((PREFIX+"Session").equals(iface))return 1;
        throw new Failure("org.freedesktop.DBus.Error.UnknownInterface","Unknown portal interface");
    }
    private String handle(Message call,String kind,Map<String,Variant> opts,String key) {
        Variant v=opts.get(key);String token=v==null?"maton_"+(++sequence):"s".equals(v.signature)?(String)v.value:null;
        if(token==null || !token.matches("[A-Za-z0-9_]+"))throw invalid("Invalid handle token");
        String path=DESKTOP+"/"+kind+"/"+call.sender.substring(1).replace('.','_')+"/"+token;
        if(handles.containsKey(path))throw invalid("Handle token already in use");
        if(handles.size()>=256)throw new Failure("org.freedesktop.DBus.Error.LimitsExceeded","Too many portal objects");
        return path;
    }
    private void updateHold() throws Exception {platform.hold(handles.values().stream().anyMatch(h->h.hold));}
    synchronized void disconnected(String owner) throws Exception {handles.values().removeIf(h->h.owner.equals(owner));updateHold();}
    synchronized void close() throws Exception {handles.clear();platform.hold(false);}
    synchronized List<Message> dispatch(Message call) {
        try {return invoke(call);}
        catch(Failure e) {Message m=response(call,3,"s",e.getMessage());m.error=e.name;return Collections.singletonList(m);}
        catch(Exception e) {Message m=response(call,3,"s","Portal operation failed");m.error="org.freedesktop.portal.Error.Failed";return Collections.singletonList(m);}
    }
    private List<Message> invoke(Message c) throws Exception {
        if(c.sender==null||!c.sender.matches(":[0-9]+\\.[0-9]+"))throw invalid("Missing authenticated sender");
        List<Message> out=new ArrayList<>();
        boolean desktop=DESKTOP.equals(c.path);
        Handle object=handles.get(c.path);
        if(!desktop && (object==null || !object.owner.equals(c.sender)))
            throw new Failure("org.freedesktop.DBus.Error.AccessDenied","Unknown portal object for caller");
        if("org.freedesktop.DBus.Properties".equals(c.iface)) {
            if("Get".equals(c.member)) {
                signature(c,"ss");String iface=(String)c.body.get(0);
                if(!desktop && (!object.session || !(PREFIX+"Session").equals(iface)))throw invalid("Invalid object interface");
                if(!"version".equals(c.body.get(1)))throw new Failure("org.freedesktop.DBus.Error.UnknownProperty","Unknown property");
                out.add(response(c,2,"v",uint(version(iface))));return out;
            }
            if("GetAll".equals(c.member)) {
                signature(c,"s");String iface=(String)c.body.get(0);
                if(!desktop && (!object.session || !(PREFIX+"Session").equals(iface)))throw invalid("Invalid object interface");
                out.add(response(c,2,"a{sv}",dictionary(Collections.singletonMap("version",uint(version(iface))))));return out;
            }
        }
        if("org.freedesktop.DBus.Introspectable".equals(c.iface)&&"Introspect".equals(c.member)) {
            signature(c,"");out.add(response(c,2,"s",PortalIntrospection.xml(desktop,object!=null&&object.session)));return out;
        }
        if(!desktop && "Close".equals(c.member) && (PREFIX+(object.session?"Session":"Request")).equals(c.iface)) {
            signature(c,"");handles.remove(c.path);updateHold();out.add(response(c,2,""));
            if(object.session)out.add(signal(c,c.path,PREFIX+"Session","Closed","a{sv}",Collections.emptyList()));
            return out;
        }
        if(desktop && (PREFIX+"Settings").equals(c.iface)) {
            Map<String,Map<String,Variant>> values=platform.settings();
            if("Read".equals(c.member)||"ReadOne".equals(c.member)) {
                signature(c,"ss");Map<String,Variant> ns=values.get(c.body.get(0));Variant v=ns==null?null:ns.get(c.body.get(1));
                if(v==null)throw new Failure(PREFIX+"Error.NotFound","Setting is unavailable");
                out.add(response(c,2,"v","Read".equals(c.member)?new Variant("v",v):v));return out;
            }
            if("ReadAll".equals(c.member)) {
                signature(c,"as");List<?> patterns=(List<?>)c.body.get(0);List<Object> result=new ArrayList<>();
                values.forEach((ns,entries)-> {
                    if(patterns.isEmpty()||patterns.stream().anyMatch(p->((String)p).isEmpty()||glob((String)p,ns)))result.add(Arrays.asList(ns,dictionary(entries)));
                });out.add(response(c,2,"a{sa{sv}}",result));return out;
            }
        }
        if(desktop && (PREFIX+"Inhibit").equals(c.iface)) {
            if("QueryEndResponse".equals(c.member)) {
                signature(c,"o");Handle h=handles.get(c.body.get(0));
                if(h==null||!h.session||!h.owner.equals(c.sender))throw new Failure("org.freedesktop.DBus.Error.AccessDenied","Unknown monitoring session");
                out.add(response(c,2,""));return out;
            }
            boolean monitor="CreateMonitor".equals(c.member);
            if(monitor||"Inhibit".equals(c.member)) {
                signature(c,monitor?"sa{sv}":"sua{sv}");int flags=monitor?0:(Integer)c.body.get(1);
                if(!monitor && (flags==0 || (flags&~12)!=0))throw new Failure("org.freedesktop.DBus.Error.NotSupported","Only suspend and idle inhibition are supported");
                if(monitor&&handles.size()>254)throw new Failure("org.freedesktop.DBus.Error.LimitsExceeded","Too many portal objects");
                Map<String,Variant> opts=dictionary(c.body.get(monitor?1:2));
                String request=handle(c,"request",opts,"handle_token");
                String session=monitor?handle(c,"session",opts,"session_handle_token"):null;
                handles.put(request,new Handle(c.sender,!monitor,false));
                if(session!=null)handles.put(session,new Handle(c.sender,false,true));
                try {updateHold();}catch(Exception e) {handles.remove(request);if(session!=null)handles.remove(session);throw e;}
                out.add(response(c,2,"o",request));
                out.add(signal(c,request,PREFIX+"Request","Response","ua{sv}",0,session==null?Collections.emptyList():dictionary(Collections.singletonMap("session_handle",new Variant("s",session)))));
                if(monitor) {
                    handles.remove(request);
                    Map<String,Variant> state=new LinkedHashMap<>();state.put("screensaver-active",new Variant("b",false));state.put("session-state",uint(1));
                    out.add(signal(c,DESKTOP,PREFIX+"Inhibit","StateChanged","oa{sv}",session,dictionary(state)));
                }
                return out;
            }
        }
        if(desktop && (PREFIX+"OpenURI").equals(c.iface) && Arrays.asList("OpenURI","OpenFile","OpenDirectory").contains(c.member)) {
            signature(c,"OpenURI".equals(c.member)?"ssa{sv}":"sha{sv}");
            Map<String,Variant> opts=dictionary(c.body.get(2));String request=handle(c,"request",opts,"handle_token");
            boolean success=platform.open(c.sender,c.member,c.body.get(1),opts);
            out.add(response(c,2,"o",request));out.add(signal(c,request,PREFIX+"Request","Response","ua{sv}",success?0:2,Collections.emptyList()));return out;
        }
        throw new Failure("org.freedesktop.DBus.Error.UnknownMethod","Unsupported portal operation");
    }
    static boolean glob(String pattern,String text) {
        // Settings.ReadAll supports globbing only in trailing sections.
        // Scan once: literals after a '*' are not a supported namespace pattern.
        int at=0;
        while(at<pattern.length() && pattern.charAt(at)!='*') {
            if(at>=text.length() || pattern.charAt(at)!=text.charAt(at))return false;
            at++;
        }
        if(at==pattern.length())return at==text.length();
        while(at<pattern.length())if(pattern.charAt(at++)!='*')return false;
        return true;
    }
}
