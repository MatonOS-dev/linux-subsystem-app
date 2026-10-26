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
 static OstreePacks.Xattrs xattrs(){return (file,name)->{
  String key=name.startsWith("user.")?name.substring(5):name;
  try {
   java.nio.file.attribute.UserDefinedFileAttributeView view=Files.getFileAttributeView(file.toPath(),java.nio.file.attribute.UserDefinedFileAttributeView.class);
   if(view==null||!view.list().contains(key))return null;
   java.nio.ByteBuffer value=java.nio.ByteBuffer.allocate(view.size(key));view.read(key,value);return Arrays.copyOf(value.array(),value.position());
  } catch(NoSuchFileException missing) { return null; }
 };}
 static OstreePacks read(File repo,String rev)throws Exception{return OstreePacks.read(repo,rev,cli(repo),xattrs());}
 static void verifyImageSeed(File root)throws Exception {
  File image=new File(root,"mounted-image/24.08/Locale/fr");image.mkdirs();
  byte[] payload="locale payload from installed image".getBytes(java.nio.charset.StandardCharsets.UTF_8);
  File source=new File(image,"messages.mo");Files.write(source.toPath(),payload);
  String checksum=ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(payload));
  ObjectPacks.Entry entry=new ObjectPacks.Entry("/Locale/fr/messages.mo",checksum,"file",0100644);
  entry.length=payload.length;entry.uid=0;entry.gid=0;entry.xattrs=new byte[0];
  File repo=new File(root,"image-seed-repo");cli(repo).run("init","--mode=bare-user-only");
  PackSeed.seedFromImage(Collections.singletonList(entry),new File(root,"mounted-image"),"24.08",repo,()->false);
  File object=new File(repo,"objects/"+checksum.substring(0,2)+"/"+checksum.substring(2)+".file");
  if(!Arrays.equals(payload,Files.readAllBytes(object.toPath())))throw new AssertionError("EROFS image update seeding failed");
  byte[] original="original index /files prefix".getBytes(java.nio.charset.StandardCharsets.UTF_8);
  Files.write(new File(image,"original.mo").toPath(),original);
  String originalChecksum=ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(original));
  ObjectPacks.Entry originalEntry=new ObjectPacks.Entry("/files/Locale/fr/original.mo",originalChecksum,"file",0100644);
  originalEntry.length=original.length;originalEntry.uid=0;originalEntry.gid=0;
  PackSeed.seedFromImage(Collections.singletonList(originalEntry),new File(root,"mounted-image"),"24.08","/files",repo,()->false);
  File originalObject=new File(repo,"objects/"+originalChecksum.substring(0,2)+"/"+originalChecksum.substring(2)+".file");
  if(!Arrays.equals(original,Files.readAllBytes(originalObject.toPath())))throw new AssertionError("EROFS original-index path mapping failed");
 }
 static List<String> mount(File packs,File root)throws Exception {
  root.mkdirs();File carriers=new File(root,"carriers");carriers.mkdirs();
  for(File pack:packs.listFiles())if(pack.getName().matches("pack-[0-9a-f]{64}\\.zip"))
   Files.copy(pack.toPath(),new File(carriers,pack.getName()).toPath());
  File index=new File(root,"objects.index");Files.copy(new File(packs,"objects.index").toPath(),index.toPath());
  return Arrays.asList("--pack-mount","/app",index.getAbsolutePath(),carriers.getAbsolutePath());
 }
 public static void main(String[] args)throws Exception {
  File root=new File(args[0]);root.mkdirs();File source=new File(root,"source"),tree=new File(root,"tree"),packs=new File(root,"packs"),scratch=new File(root,"scratch");tree.mkdirs();File files=new File(tree,"files");files.mkdirs();Files.writeString(new File(tree,"metadata").toPath(),"[Application]\nname=org.test.App\n");
  verifyImageSeed(root);
  cli(source).run("init","--mode=bare-user");
  byte[] large=new byte[1024*1024];new Random(42).nextBytes(large);Files.write(new File(files,"unchanged").toPath(),large);
  Files.write(new File(files,"changed").toPath(),large);Files.createSymbolicLink(new File(files,"link").toPath(),Path.of("unchanged"));
  String a=OstreePacks.commitDeployed(tree,cli(source));OstreePacks old=read(source,a);old.write(packs);
  String deployed=OstreePacks.commitDeployed(files,cli(source));
  OstreePacks deployedPacks=read(source,deployed);
  for(ObjectPacks.Entry e:deployedPacks.entries)if(!e.kind.equals("dir")) {
   ObjectPacks.Entry original=old.entries.stream().filter(o->o.path.equals("/files"+e.path)).findFirst().orElseThrow();
   if(!e.checksum.equals(original.checksum))throw new AssertionError("Noncanonical deployed identity: "+e.path);
  }
  old.writeOriginal(new File(root,"original.ostree"));
  DeploymentRecord record=new DeploymentRecord("app/org.test.App/x86_64/stable",a,"flathub",new byte[]{1},new byte[]{1},Files.readAllBytes(new File(source,"objects/"+a.substring(0,2)+"/"+a.substring(2)+".commit").toPath()));
  large[1000]^=1;Files.write(new File(files,"changed").toPath(),large);String b=OstreePacks.commitDeployed(tree,cli(source));
  File archive=new File(root,"remote");cli(archive).run("init","--mode=archive");cli(archive).run("pull-local",source.toString(),b);cli(archive).run("refs","--create=matonos/private/deployed-files",b);cli(archive).run("pull-local",source.toString(),a);cli(archive).run("static-delta","generate","--from="+a,"--to="+b);cli(archive).run("summary","--update");
  // Serve actual archive objects; count content requests/bytes at the HTTP boundary.
  List<String> fetched=new ArrayList<>();long[] bytes={0},allObjects={0,0},delta={0,0};
  com.sun.net.httpserver.HttpServer server=com.sun.net.httpserver.HttpServer.create(new java.net.InetSocketAddress("127.0.0.1",0),0);
  server.createContext("/",exchange->{try{Path path=archive.toPath().resolve(exchange.getRequestURI().getPath().substring(1));
   if(!Files.isRegularFile(path)){exchange.sendResponseHeaders(404,-1);return;}
   byte[] data=Files.readAllBytes(path);synchronized(fetched){if(path.toString().contains("/deltas/")){delta[0]++;delta[1]+=data.length;}if(path.toString().contains("/objects/")){allObjects[0]++;allObjects[1]+=data.length;}if(path.toString().endsWith(".filez")){fetched.add(path.toString());bytes[0]+=data.length;}}
   exchange.sendResponseHeaders(200,data.length);exchange.getResponseBody().write(data);
  }finally{exchange.close();}});server.start();
  try {
   cli(scratch).run("init","--mode=bare-user-only");List<ObjectPacks.Entry> index=ObjectPacks.readIndex(new File(packs,"objects.index"));
   List<String> base=mount(packs,new File(root,"installed-carriers"));
   File baseCarriers=new File(base.get(3));
   for(File packed:packs.listFiles())Files.delete(packed.toPath());Files.delete(packs.toPath());
   PackSeed.seed(index,baseCarriers,scratch,()->false);
   // Missing objects must reject a metadata-only approximation, without poisoning fallback.
   File incomplete=new File(root,"incomplete");cli(incomplete).run("init","--mode=bare-user-only");
   try {OstreePacks.restoreOriginal(new FileInputStream(new File(root,"original.ostree")),incomplete,record.commit,record.commitObject,cli(incomplete));throw new AssertionError("Accepted missing file objects");}
   catch(IOException expected) {if(new File(incomplete,"objects/"+a.substring(0,2)+"/"+a.substring(2)+".commit").exists())throw new AssertionError("Failed base retained");}
   Files.createDirectories(new File(scratch,"state").toPath());
   Path partial=new File(scratch,"state/"+a+".commitpartial").toPath();Files.write(partial,new byte[0]);
   try {OstreePacks.restoreOriginal(new FileInputStream(new File(root,"original.ostree")),scratch,record.commit,record.commitObject,cli(scratch));throw new AssertionError("Accepted partial base");}catch(IOException expected){}
   Files.delete(partial);
   OstreePacks.restoreOriginal(new FileInputStream(new File(root,"original.ostree")),scratch,record.commit,record.commitObject,cli(scratch));
   cli(scratch).run("remote","add","--no-gpg-verify","test","http://127.0.0.1:"+server.getAddress().getPort());
   cli(scratch).run("pull","test","matonos/private/deployed-files");cli(scratch).run("fsck","--all");
   if(delta[0]==0||delta[1]>128*1024||!fetched.isEmpty()||!cli(scratch).run("rev-parse","test:matonos/private/deployed-files").equals(b))throw new AssertionError("Fetched "+fetched+" bytes="+bytes[0]);
   long deltaSize;
   try(java.util.stream.Stream<Path> paths=Files.walk(new File(archive,"deltas").toPath())) {deltaSize=paths.filter(Files::isRegularFile).mapToLong(p->{try{return Files.size(p);}catch(IOException e){throw new java.io.UncheckedIOException(e);}}).sum();}
   if(delta[1]!=deltaSize)throw new AssertionError("Delta HTTP bytes differ: "+delta[1]+" != "+deltaSize);
   OstreePacks fresh=read(scratch,b);File next=new File(root,"next");
   ObjectPacks.update(fresh.entries,index,baseCarriers,next,0.5);ObjectPacks.writeIndex(new File(next,"objects.index"),fresh.entries);
   for(ObjectPacks.Entry e:fresh.entries)if(e.kind.equals("file")){
    try(RandomAccessFile in=new RandomAccessFile(new File(next,e.pack),"r")){in.seek(e.offset);byte[] actual=new byte[(int)e.length];in.readFully(actual);
     if(!Arrays.equals(actual,Files.readAllBytes(e.source.toPath())))throw new AssertionError(e.path);}
   }
   List<String> launch=mount(next,new File(root,"new-carriers"));
   for(ObjectPacks.Entry e:ObjectPacks.readIndex(new File(launch.get(2))))if(e.kind.equals("file")) {
    try(RandomAccessFile in=new RandomAccessFile(new File(launch.get(3),e.pack),"r")){in.seek(e.offset);byte[] data=new byte[(int)e.length];in.readFully(data);
     if(!Arrays.equals(data,Files.readAllBytes(new File(tree,e.path.substring(1)).toPath())))throw new AssertionError("Launch slice: "+e.path);}
   }
   System.out.println("PASS v3.5 A packs -> B HTTP delta pull: delta requests="+delta[0]+", delta bytes="+delta[1]+"; "+fetched.size()+" content object, "+bytes[0]+" compressed bytes; total object requests="+allObjects[0]+", object bytes="+allObjects[1]+"; 1 MiB unchanged file skipped; fsck and updated pack payloads valid; seed budget="+PackSeed.bytes(index,Files.getFileStore(root.toPath()).getBlockSize()));
   PackSeed.seed(index,baseCarriers,incomplete,()->false);
   cli(incomplete).run("remote","add","--no-gpg-verify","test","http://127.0.0.1:"+server.getAddress().getPort());
   cli(incomplete).run("pull","--disable-static-deltas","test","matonos/private/deployed-files");cli(incomplete).run("fsck","--all");
   if(fetched.size()!=1||bytes[0]<1024*1024||!cli(incomplete).run("rev-parse","test:matonos/private/deployed-files").equals(b))throw new AssertionError("Per-object fallback failed");
   System.out.println("PASS missing/partial bases rejected; ordinary fallback fetched "+bytes[0]+" content bytes and equals B");
  }finally{server.stop(0);}
 }
}
