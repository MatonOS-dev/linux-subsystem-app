import java.io.*;
import java.util.*;
import org.matonos.linuxhost.stubgen.OstreePacks;

public final class OstreeTimeHarness {
    public static void main(String[] args)throws Exception {
        File repo=new File(args[0]);String commit=args[1];
        OstreePacks.Command cli=(String... values)->{
            List<String> command=new ArrayList<>();command.add("ostree");command.add("--repo="+repo.getAbsolutePath());Collections.addAll(command,values);
            Process p=new ProcessBuilder(command).redirectErrorStream(true).start();ByteArrayOutputStream out=new ByteArrayOutputStream();
            try(InputStream in=p.getInputStream()){byte[] b=new byte[4096];int n;while((n=in.read(b))!=-1)out.write(b,0,n);}
            if(p.waitFor()!=0)throw new IOException(out.toString("UTF-8"));return out.toString("UTF-8");
        };
        OstreePacks.Xattrs xattrs=(file,name)->null;
        OstreePacks packs=OstreePacks.read(repo,commit,cli,xattrs);
        if(packs.commitTime<1700000000L||packs.commitTime>2100000000L)
            throw new AssertionError("commit timestamp is not Unix seconds: "+packs.commitTime);
        System.out.println("PASS OSTree commit timestamp seconds="+packs.commitTime);
    }
}
