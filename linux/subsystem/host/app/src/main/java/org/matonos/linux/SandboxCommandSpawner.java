package org.matonos.linux;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Process;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** SCM_RIGHTS client for the proxy's resident sandbox snapshot. Never host exec. */
final class SandboxCommandSpawner implements HostCommandBackend.Spawner {
    private final File path;
    SandboxCommandSpawner(File directory){path=new File(directory,"host-command");}
    static byte[] request(byte[] cwd,List<byte[]> argv,Map<Integer,FileDescriptor> fds,Map<String,String> env,int flags) throws IOException {
        ByteArrayOutputStream body=new ByteArrayOutputStream();
        for(int target:fds.keySet())body.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(target).array());
        body.write(cwd);for(byte[] arg:argv)body.write(arg);
        for(Map.Entry<String,String> e:env.entrySet()){body.write((e.getKey()+"="+e.getValue()).getBytes(StandardCharsets.UTF_8));body.write(0);}
        if(body.size()>1024*1024||argv.size()>4096||env.size()>4096||fds.size()>16)throw new IOException("Command is too large");
        return ByteBuffer.allocate(24+body.size()).order(ByteOrder.LITTLE_ENDIAN).putInt(0x314d4348).putInt(body.size()).putInt(argv.size()).putInt(env.size()).putInt(fds.size()).putInt(flags).put(body.toByteArray()).array();
    }
    static int readInt(InputStream in) throws IOException {
        byte[] b=new byte[4];int at=0;while(at<4){int n=in.read(b,at,4-at);if(n<0)throw new EOFException("Sandbox supervisor disconnected");at+=n;}
        return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).getInt();
    }
    @Override public HostCommandBackend.Child spawn(byte[] cwd,List<byte[]> argv,Map<Integer,FileDescriptor> fds,Map<String,String> env,int flags) throws Exception {
        byte[] request=request(cwd,argv,fds,env,flags);LocalSocket socket=new LocalSocket();
        try {
            socket.connect(new LocalSocketAddress(path.getAbsolutePath(),LocalSocketAddress.Namespace.FILESYSTEM));
            if(socket.getPeerCredentials().getUid()!=Process.myUid())throw new IOException("Wrong sandbox UID");
            socket.setSoTimeout(5000);
            socket.setFileDescriptorsForSend(fds.isEmpty()?null:fds.values().toArray(new FileDescriptor[0]));
            try{socket.getOutputStream().write(request);}finally{socket.setFileDescriptorsForSend(null);}
            int pid=readInt(socket.getInputStream());if(pid<=0)throw new IOException("Sandbox spawn errno "+(-pid));
            socket.setSoTimeout(0);return new Child(socket,pid);
        }catch(Exception e){socket.close();throw e;}
    }
    @Override public void close(){
        try(LocalSocket socket=new LocalSocket()){
            socket.connect(new LocalSocketAddress(path.getAbsolutePath(),LocalSocketAddress.Namespace.FILESYSTEM));
            if(socket.getPeerCredentials().getUid()!=Process.myUid())return;
            socket.getOutputStream().write(ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(0x314d4348).putInt(0).putInt(0).putInt(0).putInt(0).putInt(4).array());
        }catch(Exception ignored){}
    }
    private static final class Child implements HostCommandBackend.Child {
        final LocalSocket socket; final int pid;
        Child(LocalSocket socket,int pid){this.socket=socket;this.pid=pid;}
        public int pid(){return pid;}
        public int waitStatus() throws IOException{return readInt(socket.getInputStream());}
        public synchronized void signal(int signal,boolean group) throws IOException {
            socket.getOutputStream().write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(signal).putInt(group?1:0).array());
        }
        public void close(){try{socket.close();}catch(IOException ignored){}}
    }
}
