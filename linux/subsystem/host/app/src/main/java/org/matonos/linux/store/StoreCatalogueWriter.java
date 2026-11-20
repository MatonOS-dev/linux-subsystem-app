package org.matonos.linux.store;
import java.io.IOException;
import java.io.Writer;

/** Streaming writer for a plugin's UTF-8 source catalogue. */
public final class StoreCatalogueWriter {
    public interface Rows { void writeRows(Writer out) throws Exception; }
    private StoreCatalogueWriter() { }
    public static void write(Writer out,String source,long generated,Rows rows)throws Exception {
        out.write("{\"format\":1,\"source\":");quote(out,source);out.write(",\"generated\":");out.write(Long.toString(generated));out.write(",\"apps\":[");
        rows.writeRows(out);out.write("]}");out.flush();
    }
    private static void quote(Writer out,String value)throws IOException {
        out.write('"');for(int i=0;i<value.length();i++){char c=value.charAt(i);switch(c){case '"':out.write("\\\"");break;case '\\':out.write("\\\\");break;case '\n':out.write("\\n");break;case '\r':out.write("\\r");break;case '\t':out.write("\\t");break;default:if(c<0x20)out.write(String.format("\\u%04x",(int)c));else out.write(c);}}out.write('"');
    }
}
