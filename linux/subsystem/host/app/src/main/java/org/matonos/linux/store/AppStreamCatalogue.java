package org.matonos.linux.store;

import java.io.*;
import java.util.*;
import java.util.zip.GZIPInputStream;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.*;

/** AppStream XML, never the Flathub web catalogue. Android-independent for host tests. */
public final class AppStreamCatalogue {
    public static List<Map<String,Object>> read(InputStream input, boolean gzip, String remote) throws Exception {
        DocumentBuilderFactory factory=DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl",true);
        factory.setFeature("http://xml.org/sax/features/external-general-entities",false);
        factory.setFeature("http://xml.org/sax/features/external-parameter-entities",false);
        Document document=factory.newDocumentBuilder().parse(gzip?new GZIPInputStream(input):input);
        List<Map<String,Object>> result=new ArrayList<>();
        NodeList components=document.getElementsByTagName("component");
        for(int i=0;i<components.getLength();i++) {
            Element component=(Element)components.item(i);String ref="";
            for(Element bundle:children(component,"bundle"))if("flatpak".equals(bundle.getAttribute("type")))ref=bundle.getTextContent().trim();
            if(!ref.startsWith("app/"))continue;
            String[] parts=ref.split("/",-1);if(parts.length!=4)throw new IOException("Invalid AppStream Flatpak bundle: "+ref);
            Map<String,Object> row=new LinkedHashMap<>();
            row.put("id",parts[1]);row.put("ref",ref);row.put("origin",remote);
            row.put("name",text(component,"name"));row.put("summary",text(component,"summary"));
            row.put("description",text(component,"description"));row.put("license",text(component,"project_license"));
            String developer=text(component,"developer_name");Element dev=first(component,"developer");
            if(dev!=null)developer=text(dev,"name");row.put("developer",developer);
            String icon="";
            for(Element item:children(component,"icon")) {
                if("remote".equals(item.getAttribute("type"))) {icon=item.getTextContent().trim();break;}
                if("cached".equals(item.getAttribute("type")))icon="cached:"+item.getTextContent().trim();
            }
            row.put("icon",icon);
            List<String> screenshots=new ArrayList<>();Element shots=first(component,"screenshots");
            if(shots!=null)for(Element shot:children(shots,"screenshot"))for(Element img:children(shot,"image"))
                if(!"thumbnail".equals(img.getAttribute("type")))screenshots.add(img.getTextContent().trim());
            row.put("screenshots",screenshots);
            for(Element url:children(component,"url"))if("homepage".equals(url.getAttribute("type")))row.put("homepage",url.getTextContent().trim());
            Element releases=first(component,"releases");if(releases!=null) {Element release=first(releases,"release");if(release!=null)row.put("versionName",release.getAttribute("version"));}
            Element rating=first(component,"content_rating");
            if(rating!=null) {Map<String,Object> age=new LinkedHashMap<>();age.put("type",rating.getAttribute("type"));
                Map<String,String> attributes=new LinkedHashMap<>();for(Element attr:children(rating,"content_attribute"))attributes.put(attr.getAttribute("id"),attr.getTextContent().trim());
                age.put("attributes",attributes);row.put("ageRating",age);}
            result.add(row);
        }
        return result;
    }
    public static List<Map<String,Object>> search(List<Map<String,Object>> rows,String term) {
        String needle=term==null?"":term.toLowerCase(Locale.ROOT);List<Map<String,Object>> result=new ArrayList<>();
        for(Map<String,Object> row:rows)if((row.get("id")+" "+row.get("name")+" "+row.get("summary")+" "+row.get("developer")).toLowerCase(Locale.ROOT).contains(needle)) {
            Map<String,Object> listing=new LinkedHashMap<>();for(String key:Arrays.asList("id","packageName","name","summary","icon","developer","versionName","origin","installed"))if(row.containsKey(key))listing.put(key,row.get(key));result.add(listing);
        }
        return result;
    }
    private static List<Element> children(Element parent,String name) {
        List<Element> result=new ArrayList<>();for(Node node=parent.getFirstChild();node!=null;node=node.getNextSibling())if(node instanceof Element&&name.equals(node.getNodeName()))result.add((Element)node);return result;
    }
    private static Element first(Element parent,String name) {List<Element> values=children(parent,name);return values.isEmpty()?null:values.get(0);}
    private static String text(Element parent,String name) {
        // Prefer the unlocalized fallback, independent of XML order.
        for(Element child:children(parent,name))if(!child.hasAttributeNS("http://www.w3.org/XML/1998/namespace","lang"))return child.getTextContent().trim();
        Element child=first(parent,name);return child==null?"":child.getTextContent().trim();
    }
    private AppStreamCatalogue() { }
}
