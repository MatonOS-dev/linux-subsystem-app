package org.matonos.linux;
import java.io.*;
import org.matonos.linux.store.StoreCatalogueWriter;

public final class StoreCatalogueWriterTest {
    public static void main(String[] args)throws Exception{
        File output=new File(args[0]);int count=75_000;
        try(Writer writer=new BufferedWriter(new OutputStreamWriter(new FileOutputStream(output),java.nio.charset.StandardCharsets.UTF_8),64*1024)){
            StoreCatalogueWriter.write(writer,"org.matonos.linux",1730000000L,out->{
                for(int i=0;i<count;i++){if(i>0)out.write(',');out.write("{\"id\":\"app/org.example."+i+"\",\"name\":\"Synthetic "+i+"\",\"summary\":\"AppStream large fixture\",\"repo\":\"flathub\"}");}
            });
        }
        long rows=0;try(BufferedReader reader=new BufferedReader(new FileReader(output))){String line;while((line=reader.readLine())!=null){int at=0;while((at=line.indexOf("\"id\":",at))>=0){rows++;at+=5;}}}
        if(rows!=count)throw new AssertionError("expected "+count+" rows; got "+rows);
        if(output.length()<count*60L)throw new AssertionError("catalogue unexpectedly small "+output.length());
        System.out.println("StoreCatalogueWriterTest: PASS (streamed "+count+" rows, bytes="+output.length()+")");
    }
}
