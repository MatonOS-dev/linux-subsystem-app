package org.matonos.linux.store;

import java.io.*;
import java.util.*;
import java.util.zip.GZIPInputStream;
import javax.xml.parsers.SAXParserFactory;
import org.xml.sax.*;
import org.xml.sax.helpers.DefaultHandler;

/** AppStream XML, never the Flathub web catalogue. Android-independent for host tests. */
public final class AppStreamCatalogue {
    /** Streams the XML: Flathub's catalogue as a DOM exceeds an app heap. */
    public static List<Map<String,Object>> read(InputStream input, boolean gzip, String remote) throws Exception {
        SAXParserFactory factory=SAXParserFactory.newInstance();
        XMLReader reader=factory.newSAXParser().getXMLReader();
        reader.setEntityResolver((publicId,systemId)->{throw new SAXException("AppStream XML references external entity "+systemId);});
        Handler handler=new Handler(remote);reader.setContentHandler(handler);
        reader.setProperty("http://xml.org/sax/properties/lexical-handler",handler);  // rejects DOCTYPE
        reader.parse(new InputSource(gzip?new GZIPInputStream(input):input));
        return handler.result;
    }

    /** One component at a time; paths are element names below <component>. */
    private static final class Handler extends DefaultHandler implements org.xml.sax.ext.LexicalHandler {
        @Override public void startDTD(String name,String publicId,String systemId) throws SAXException {throw new SAXException("AppStream XML must not declare a DOCTYPE");}
        @Override public void endDTD() { }
        @Override public void startEntity(String name) { }
        @Override public void endEntity(String name) { }
        @Override public void startCDATA() { }
        @Override public void endCDATA() { }
        @Override public void comment(char[] ch,int start,int length) { }
        final List<Map<String,Object>> result=new ArrayList<>();
        private final String remote;
        private final ArrayDeque<String> path=new ArrayDeque<>();
        private final ArrayDeque<StringBuilder> texts=new ArrayDeque<>();
        private final ArrayDeque<Integer> orderedLists=new ArrayDeque<>();
        private Component current;
        Handler(String remote){this.remote=remote;}

        @Override public void startElement(String uri,String local,String qName,Attributes attrs) {
            if(current==null) {if("component".equals(qName)){current=new Component();path.clear();}return;}
            String parentPath=String.join("/",path);path.addLast(qName);texts.addLast(new StringBuilder());
            if(qName.equals("ol"))orderedLists.addLast(0);
            if(String.join("/",path).startsWith("description/")) {
                switch(qName) {case "p": case "ul": case "ol": case "li": case "em": case "code":
                    if(qName.equals("p")||qName.equals("li"))mdBreak();
                    if(qName.equals("li")){if(parentPath.endsWith("/ol")&&!orderedLists.isEmpty()){int next=orderedLists.removeLast()+1;orderedLists.addLast(next);current.descriptionMarkdown.append(next).append(". ");}else current.descriptionMarkdown.append("- ");}
                    if(qName.equals("em"))current.descriptionMarkdown.append('*');
                    if(qName.equals("code"))current.descriptionMarkdown.append('`');
                }
            }
            String at=String.join("/",path);
            if(at.equals("bundle"))current.bundleType=attrs.getValue("type");
            else if(at.equals("icon"))current.iconType=attrs.getValue("type");
            else if(at.equals("screenshots/screenshot/image"))current.imageType=attrs.getValue("type");
            else if(at.equals("url"))current.urlType=attrs.getValue("type");
            else if(at.equals("releases/release")&&current.version==null)current.version=attrs.getValue("version");
            else if(at.equals("content_rating")&&current.rating==null)current.rating=attrs.getValue("type");
            else if(at.equals("content_rating/content_attribute"))current.attributeId=attrs.getValue("id");
            if(path.size()==1)current.lang=attrs.getValue("xml:lang");
        }
        @Override public void characters(char[] ch,int start,int length) {
            for(StringBuilder text:texts)text.append(ch,start,length);
            if(current!=null&&String.join("/",path).startsWith("description/"))for(int i=start;i<start+length;i++){char c=ch[i];if("\\`*_{}[]<>#+-.!|".indexOf(c)>=0)current.descriptionMarkdown.append('\\');current.descriptionMarkdown.append(c);}
        }
        @Override public void endElement(String uri,String local,String qName) throws SAXException {
            if(current==null)return;
            if(path.isEmpty()) {finish();current=null;return;}
            String at=String.join("/",path);String text=texts.removeLast().toString().trim();
            if(at.startsWith("description/")) {
                if(qName.equals("em"))current.descriptionMarkdown.append('*');
                if(qName.equals("code"))current.descriptionMarkdown.append('`');
                if(qName.equals("p")||qName.equals("li"))mdBreak();
            }
            if(qName.equals("ol")&&!orderedLists.isEmpty())orderedLists.removeLast();
            path.removeLast();
            Component c=current;
            switch(at) {
                case "bundle": if("flatpak".equals(c.bundleType))c.ref=text; break;
                case "name": case "summary": case "description": case "project_license": case "developer_name":
                    c.field(at,text,c.lang==null); break;
                case "developer/name": if(c.developerName==null)c.developerName=text; break;
                case "icon":
                    if(c.remoteIcon==null&&"remote".equals(c.iconType))c.remoteIcon=text;
                    else if("cached".equals(c.iconType))c.cachedIcon="cached:"+text;
                    break;
                case "screenshots/screenshot/image": if(!"thumbnail".equals(c.imageType))c.screenshots.add(text); break;
                case "url": if("homepage".equals(c.urlType))c.homepage=text; break;
                case "content_rating/content_attribute": c.attributes.put(c.attributeId,text); break;
                default: break;
            }
        }
        private void mdBreak(){if(current.descriptionMarkdown.length()>0&&current.descriptionMarkdown.charAt(current.descriptionMarkdown.length()-1)!='\n')current.descriptionMarkdown.append("\n\n");}
        private void finish() throws SAXException {
            Component c=current;
            if(c.ref==null||!c.ref.startsWith("app/"))return;
            String[] parts=c.ref.split("/",-1);if(parts.length!=4)throw new SAXException("Invalid AppStream Flatpak bundle: "+c.ref);
            Map<String,Object> row=new LinkedHashMap<>();
            row.put("id",parts[1]);row.put("ref",c.ref);row.put("origin",remote);
            row.put("name",c.get("name"));row.put("summary",c.get("summary"));
            row.put("description",c.get("description"));row.put("descriptionMarkdown",c.descriptionMarkdown.toString().replaceAll("\\n{3,}","\\n\\n").trim());row.put("license",c.get("project_license"));
            row.put("developer",c.developerName!=null?c.developerName:c.get("developer_name"));
            row.put("icon",c.remoteIcon!=null?c.remoteIcon:c.cachedIcon);
            row.put("screenshots",c.screenshots);
            if(c.homepage!=null)row.put("homepage",c.homepage);
            if(c.version!=null)row.put("versionName",c.version);
            if(c.rating!=null){Map<String,Object> age=new LinkedHashMap<>();age.put("type",c.rating);age.put("attributes",c.attributes);row.put("ageRating",age);}
            result.add(row);
        }
    }

    private static final class Component {
        String ref,bundleType,iconType,imageType,urlType,attributeId,lang,version,rating,developerName,remoteIcon,homepage;
        String cachedIcon="";
        final List<String> screenshots=new ArrayList<>();
        final StringBuilder descriptionMarkdown=new StringBuilder();
        final Map<String,String> attributes=new LinkedHashMap<>();
        // Unlocalized value wins regardless of XML order; else the first one.
        private final Map<String,String> plain=new HashMap<>(),firstSeen=new HashMap<>();
        void field(String name,String value,boolean unlocalized){
            if(unlocalized)plain.putIfAbsent(name,value);
            firstSeen.putIfAbsent(name,value);
        }
        String get(String name){String v=plain.get(name);if(v==null)v=firstSeen.get(name);return v==null?"":v;}
    }

    public static List<Map<String,Object>> search(List<Map<String,Object>> rows,String term) {
        String needle=term==null?"":term.toLowerCase(Locale.ROOT);List<Map<String,Object>> result=new ArrayList<>();
        for(Map<String,Object> row:rows)if((row.get("id")+" "+row.get("name")+" "+row.get("summary")+" "+row.get("developer")).toLowerCase(Locale.ROOT).contains(needle)) {
            Map<String,Object> listing=new LinkedHashMap<>();for(String key:Arrays.asList("id","packageName","name","summary","icon","developer","versionName","origin","installed","added","badges"))if(row.containsKey(key))listing.put(key,row.get(key));result.add(listing);
        }
        return result;
    }
    private AppStreamCatalogue() { }
}
