package org.matonos.linux;

final class PortalIntrospection {
    private static String arg(String type,String direction) {return "<arg type='"+type+"' direction='"+direction+"'/>";}
    private static String method(String name,String[] in,String[] out) {
        StringBuilder s=new StringBuilder("<method name='"+name+"'>");
        for(String t:in)s.append(arg(t,"in"));for(String t:out)s.append(arg(t,"out"));return s.append("</method>").toString();
    }
    private static String iface(String name,String methods) {return "<interface name='org.freedesktop.portal."+name+"'><property name='version' type='u' access='read'/>"+methods+"</interface>";}
    static String xml(boolean desktop,boolean session) {
        String empty[]=new String[0],handle[]={"o"},options[]={"s","a{sv}"};
        if(!desktop)return "<node>"+iface(session?"Session":"Request",method("Close",empty,empty)+(session?"<signal name='Closed'><arg type='a{sv}'/></signal>":"<signal name='Response'><arg type='u'/><arg type='a{sv}'/></signal>"))+"</node>";
        return "<node>"+iface("Settings",method("Read",new String[]{"s","s"},new String[]{"v"})+method("ReadOne",new String[]{"s","s"},new String[]{"v"})+method("ReadAll",new String[]{"as"},new String[]{"a{sa{sv}}"})+"<signal name='SettingChanged'><arg type='s'/><arg type='s'/><arg type='v'/></signal>")+
            iface("Inhibit",method("Inhibit",new String[]{"s","u","a{sv}"},handle)+method("CreateMonitor",options,handle)+method("QueryEndResponse",handle,empty)+"<signal name='StateChanged'><arg type='o'/><arg type='a{sv}'/></signal>")+
            iface("OpenURI",method("OpenURI",new String[]{"s","s","a{sv}"},handle)+method("OpenFile",new String[]{"s","h","a{sv}"},handle)+method("OpenDirectory",new String[]{"s","h","a{sv}"},handle))+"</node>";
    }
}
