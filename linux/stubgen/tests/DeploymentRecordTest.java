import org.matonos.linuxhost.stubgen.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public final class DeploymentRecordTest {
    interface Checked {void run() throws Exception;}
    static void rejects(Checked check) throws Exception {
        try{check.run();throw new AssertionError("Accepted malformed deployment");}catch(IOException expected){}
    }
    public static void main(String[] args) throws Exception {
        Path scratch=Files.createTempDirectory(Path.of(args[0]),"deployment-");
        byte[] object=new byte[]{3,2,1};
        String commit=ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(object));
        DeploymentRecord record=new DeploymentRecord("app/org.test.App/x86_64/stable",commit,"flathub",
                "[Application]\nname=org.test.App\n".getBytes(),new byte[]{1,2,3},object);
        RemoteOrigin custom=new RemoteOrigin("test","https://repo.example/repo","AQID");
        DeploymentRecord customRecord=new DeploymentRecord(record.ref,commit,custom.encode(),record.metadata,record.deploy,object);
        if(!DeploymentRecord.decode(customRecord.encode()).remote.equals(custom.encode()))throw new AssertionError("Origin not preserved");
        if(!RemoteOrigin.parse(custom.encode()).url.equals(custom.url))throw new AssertionError();
        if(!RemoteOrigin.flatpakrepo("test",custom.flatpakrepo()).encode().equals(custom.encode()))throw new AssertionError();
        rejects(()->new RemoteOrigin("FlAtHuB",custom.url,custom.key));
        rejects(()->new RemoteOrigin("flathub",custom.url,custom.key));
        rejects(()->new RemoteOrigin("test","file:///repo",custom.key));
        rejects(()->new RemoteOrigin("test",custom.url,""));
        rejects(()->new RemoteOrigin("test",custom.url,"AQID\\nUrl=https://evil.example"));
        rejects(()->RemoteOrigin.flatpakrepo("test",custom.flatpakrepo()+"Url=https://evil.example\n"));
        DeploymentRecord bound=new DeploymentRecord(record.ref,commit,custom.encode(),record.metadata,record.deploy,object,
                List.of(new DeploymentRecord.Dependency("runtime/org.test.Platform/x86_64/stable",custom.encode(),"/usr")),"/app/extensions/test");
        DeploymentRecord restored=DeploymentRecord.decode(bound.encode());
        if(restored.dependencies.size()!=1||!restored.dependencies.get(0).remote.equals(custom.encode())||!restored.dependencies.get(0).mount.equals("/usr")||!restored.mount.equals(bound.mount))throw new AssertionError("Dependency origin/mount lost");
        rejects(()->new DeploymentRecord.Dependency("runtime/org.test.Platform/x86_64/stable",custom.encode(),"/usr/../escape"));
        rejects(()->new DeploymentRecord(record.ref,commit,custom.encode(),record.metadata,record.deploy,object,
                List.of(new DeploymentRecord.Dependency(record.ref,custom.encode(),"/app"))));
        for(String old:List.of("MTDEP1","MTDEP2")) {
            ByteArrayOutputStream legacy=new ByteArrayOutputStream();new DataOutputStream(legacy).writeUTF(old);
            rejects(()->DeploymentRecord.decode(legacy.toByteArray()));
        }
        byte[] bytes=record.encode();
        DeploymentRecord decoded=DeploymentRecord.decode(bytes);
        if(!decoded.ref.equals(record.ref)||!Arrays.equals(record.deploy,decoded.deploy))throw new AssertionError();
        decoded.materialize(scratch.toFile(),"/app");
        Path version=scratch.resolve(record.ref).resolve(commit);
        if(!Files.readSymbolicLink(version.resolve("files")).toString().equals("/app"))throw new AssertionError();
        if(!Files.readSymbolicLink(version.getParent().resolve("active")).toString().equals(commit))throw new AssertionError();
        if(!Arrays.equals(Files.readAllBytes(version.resolve("metadata")),record.metadata))throw new AssertionError();
        rejects(()->DeploymentRecord.decode(Arrays.copyOf(bytes,bytes.length-1)));
        rejects(()->DeploymentRecord.decode(Arrays.copyOf(bytes,bytes.length+1)));
        rejects(()->new DeploymentRecord("app/../x86_64/stable",commit,"flathub",new byte[]{1},new byte[]{1},object));
        rejects(()->new DeploymentRecord(record.ref,"not-a-checksum","flathub",new byte[]{1},new byte[]{1},object));
        rejects(()->new DeploymentRecord(record.ref,commit,"untrusted",new byte[]{1},new byte[]{1},object));
        rejects(()->record.materialize(scratch.toFile(),"/usr/../escape"));
        Path objects=scratch.resolve("objects");Files.createDirectories(objects);
        Files.write(objects.resolve("objects.index"),new byte[]{1});Files.write(objects.resolve("objects.ostree"),new byte[]{2});
        String before=StubGenerator.indexSplitName(objects.toFile());
        Files.write(objects.resolve("deployment.bin"),bytes);
        String after=StubGenerator.indexSplitName(objects.toFile());
        if(before.equals(after))throw new AssertionError("Deployment excluded from index identity");
        System.out.println("PASS deployment record round-trip, disposable Flatpak lookup tree, malformed input rejection and index identity");
    }
}
