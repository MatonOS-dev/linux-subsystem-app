import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.matonos.linuxhost.stubgen.*;

/** Real HTTP pulls: the only base available to the second repo is A's packs. */
public final class PackSeedTest {
 static String run(String... args)throws Exception {
  Process p=new ProcessBuilder(args).redirectError(ProcessBuilder.Redirect.INHERIT).start();
  String text=new String(p.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(p.waitFor()!=0)throw new IOException(Arrays.toString(args));return text.trim();
 }
 static OstreePacks.Command cli(File repo){return args->{List<String> c=new ArrayList<>(Arrays.asList("ostree","--repo="+repo));Collections.addAll(c,args);return run(c.toArray(new String[0]));};}
 public static void main(String[] args)throws Exception {
  File root=new File(args[0]);root.mkdirs();File source=new File(root,"source"),tree=new File(root,"tree"),packs=new File(root,"packs"),scratch=new File(root,"scratch");tree.mkdirs();
  cli(source).run("init","--mode=bare-user");
  byte[] large=new byte[1024*1024];new Random(42).nextBytes(large);Files.write(new File(tree,"unchanged").toPath(),large);
  Files.writeString(new File(tree,"changed").toPath(),"before");Files.createSymbolicLink(new File(tree,"link").toPath(),Path.of("unchanged"));
  String a=OstreePacks.commitDeployed(tree,cli(source));OstreePacks old=OstreePacks.read(source,a,cli(source));old.write(packs);
  Files.writeString(new File(tree,"changed").toPath(),"after");String b=OstreePacks.commitDeployed(tree,cli(source));
  File archive=new File(root,"remote");cli(archive).run("init","--mode=archive");cli(archive).run("pull-local",source.toString(),b);cli(archive).run("refs","--create=matonos/private/deployed-files",b);cli(archive).run("summary","--update");
  // Serve actual archive objects; count content requests/bytes at the HTTP boundary.
  List<String> fetched=new ArrayList<>();long[] bytes={0};
  com.sun.net.httpserver.HttpServer server=com.sun.net.httpserver.HttpServer.create(new java.net.InetSocketAddress("127.0.0.1",0),0);
  server.createContext("/",exchange->{try{Path path=archive.toPath().resolve(exchange.getRequestURI().getPath().substring(1));
   if(!Files.isRegularFile(path)){exchange.sendResponseHeaders(404,-1);return;}
   byte[] data=Files.readAllBytes(path);synchronized(fetched){if(path.toString().endsWith(".filez")){fetched.add(path.toString());bytes[0]+=data.length;}}
   exchange.sendResponseHeaders(200,data.length);exchange.getResponseBody().write(data);
  }finally{exchange.close();}});server.start();
  try {
   cli(scratch).run("init","--mode=bare-user-only");List<ObjectPacks.Entry> index=ObjectPacks.readIndex(new File(packs,"objects.index"));
   PackSeed.seed(index,packs,scratch,()->false);
   cli(scratch).run("remote","add","--no-gpg-verify","test","http://127.0.0.1:"+server.getAddress().getPort());
   cli(scratch).run("pull","--disable-static-deltas","test","matonos/private/deployed-files");cli(scratch).run("fsck","--all");
   if(fetched.size()!=1||bytes[0]>4096)throw new AssertionError("Fetched "+fetched+" bytes="+bytes[0]);
   OstreePacks fresh=OstreePacks.read(scratch,b,cli(scratch));File next=new File(root,"next");
   ObjectPacks.update(fresh.entries,index,packs,next,0.5);ObjectPacks.writeIndex(new File(next,"objects.index"),fresh.entries);
   for(ObjectPacks.Entry e:fresh.entries)if(e.kind.equals("file")){
    try(RandomAccessFile in=new RandomAccessFile(new File(next,e.pack),"r")){in.seek(e.offset);byte[] actual=new byte[(int)e.length];in.readFully(actual);
     if(!Arrays.equals(actual,Files.readAllBytes(e.source.toPath())))throw new AssertionError(e.path);}
   }
   List<String> names=new ArrayList<>(),paths=new ArrayList<>();
   for(File pack:next.listFiles())if(pack.getName().startsWith("pack-")) {
    String name="pack_"+pack.getName().substring(5,69);File carrier=new File(root,name+".apk");
    ObjectPacks.carrier(pack,carrier,new byte[]{1});names.add(name);paths.add(carrier.getAbsolutePath());
   }
   String indexName="index_"+"0".repeat(64);File indexApk=new File(root,"index.apk");
   try(java.util.zip.ZipOutputStream zip=new java.util.zip.ZipOutputStream(new FileOutputStream(indexApk))) {
    zip.putNextEntry(new java.util.zip.ZipEntry("assets/matonos/objects.index"));zip.write(Files.readAllBytes(new File(next,"objects.index").toPath()));zip.closeEntry();
   }
   names.add(indexName);paths.add(indexApk.getAbsolutePath());
   List<String> launch=PackLaunch.arguments(indexApk,names.toArray(new String[0]),paths.toArray(new String[0]),new File(root,"launch"));
   for(ObjectPacks.Entry e:ObjectPacks.readIndex(new File(launch.get(2))))if(e.kind.equals("file")) {
    try(RandomAccessFile in=new RandomAccessFile(new File(launch.get(3),e.pack),"r")){in.seek(e.offset);byte[] data=new byte[(int)e.length];in.readFully(data);
     if(!Arrays.equals(data,Files.readAllBytes(new File(tree,e.path.substring(1)).toPath())))throw new AssertionError("Launch slice: "+e.path);}
   }
   System.out.println("PASS A packs -> B HTTP pull: "+fetched.size()+" content object, "+bytes[0]+" compressed bytes; 1 MiB unchanged file skipped; fsck and updated pack payloads valid; seed budget="+PackSeed.bytes(index,Files.getFileStore(root.toPath()).getBlockSize()));
  }finally{server.stop(0);}
 }
}
