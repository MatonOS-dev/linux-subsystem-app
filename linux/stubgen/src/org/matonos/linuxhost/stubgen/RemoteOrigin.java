package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/** Immutable origin carried in the signed deployment, never a global alias lookup. */
public final class RemoteOrigin {
    public final String name,url,key;
    public RemoteOrigin(String name,String url,String key) throws IOException {
        if(name==null||!name.matches("[A-Za-z0-9_][A-Za-z0-9_.-]*"))throw new IOException("Invalid remote name");
        if(url==null||key==null)throw new IOException("Invalid remote descriptor");
        if(name.equals("flathub")&&url.isEmpty()&&key.isEmpty()) {this.name=name;this.url=url;this.key=key;return;}
        if(name.equalsIgnoreCase("flathub"))throw new IOException("flathub is reserved for the image remote");
        try {URI parsed=new URI(url);if(!"https".equals(parsed.getScheme())||parsed.getHost()==null||parsed.getUserInfo()!=null||parsed.getFragment()!=null)throw new Exception();}
        catch(Exception invalid){throw new IOException("Remote URL must be HTTPS with a host",invalid);}
        try {if(Base64.getDecoder().decode(key).length==0)throw new Exception();}catch(Exception invalid){throw new IOException("Remote requires a base64 GPG key",invalid);}
        this.name=name;this.url=url;this.key=key;
    }
    /** Parse descriptor contents, never a caller path or an unauthenticated URL fetch. */
    public static RemoteOrigin flatpakrepo(String name,String text) throws IOException {
        if(text==null)throw new IOException("Invalid flatpakrepo");
        java.util.Map<String,String> fields=new java.util.HashMap<>();boolean inside=false;
        for(String line:text.split("\\r?\\n")) {
            line=line.trim();if(line.isEmpty()||line.startsWith("#"))continue;
            if(line.startsWith("[")){inside=line.equals("[Flatpak Repo]");continue;}
            if(!inside)continue;int equals=line.indexOf('=');if(equals<1)throw new IOException("Malformed flatpakrepo");
            String field=line.substring(0,equals);if(fields.put(field,line.substring(equals+1))!=null)throw new IOException("Duplicate flatpakrepo field");
        }
        return new RemoteOrigin(name,fields.get("Url"),fields.get("GPGKey"));
    }
    public static RemoteOrigin parse(String value) throws IOException {
        if("flathub".equals(value))return new RemoteOrigin(value,"","");
        if(value==null||!value.startsWith("origin:"))throw new IOException("Remote must be flathub or an encoded origin");
        try {String[] p=value.substring(7).split(":",-1);if(p.length!=3)throw new Exception();return new RemoteOrigin(decode(p[0]),decode(p[1]),decode(p[2]));}
        catch(Exception e){throw new IOException("Invalid remote descriptor",e);}
    }
    private static String decode(String s){return new String(Base64.getUrlDecoder().decode(s),StandardCharsets.UTF_8);}
    private static String encode(String s){return Base64.getUrlEncoder().withoutPadding().encodeToString(s.getBytes(StandardCharsets.UTF_8));}
    public String encode(){return url.isEmpty()?"flathub":"origin:"+encode(name)+":"+encode(url)+":"+encode(key);}
    public String display(){return name+" ("+(url.isEmpty()?"dl.flathub.org":URI.create(url).getHost())+")";}
    public String flatpakrepo(){return "[Flatpak Repo]\nVersion=1\nTitle="+name+"\nUrl="+url+"\nGPGKey="+key+"\n";}
}
