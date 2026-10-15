package android.net;
import java.io.*;
import java.util.*;
/** Test double: Android retains descriptors across writes. */
public class LocalSocket {
    public InputStream input;
    public FileDescriptor[] ancillary, outbound;
    public boolean failWrite, failRead, failClose;
    public int writes, sent, clears;
    public final ByteArrayOutputStream bytes=new ByteArrayOutputStream();
    public InputStream getInputStream() { return new FilterInputStream(input) {
        public int read(byte[] b,int off,int len)throws IOException {
            if(failRead)throw new IOException("read failed");return super.read(b,off,Math.min(len,3));
        }
    }; }
    public OutputStream getOutputStream() {return new OutputStream(){
        public void write(int value)throws IOException{throw new AssertionError("Single byte write");}
        public void write(byte[] b,int off,int len)throws IOException {
            writes++;sent+=outbound==null?0:outbound.length;
            if(failWrite)throw new IOException("write failed");bytes.write(b,off,len);
        }
    };}
    public FileDescriptor[] getAncillaryFileDescriptors(){FileDescriptor[] f=ancillary;ancillary=null;return f;}
    public void setFileDescriptorsForSend(FileDescriptor[] f){outbound=f;if(f==null)clears++;}
    public void close()throws IOException{if(failClose)throw new IOException("close failed");}
}
