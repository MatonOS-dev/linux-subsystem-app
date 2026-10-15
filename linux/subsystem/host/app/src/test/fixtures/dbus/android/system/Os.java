package android.system;
import java.io.FileDescriptor;
import java.util.*;
public class Os {
    public static final List<FileDescriptor> closed=new ArrayList<>();
    public static void close(FileDescriptor fd){closed.add(fd);}
    public static void chmod(String path,int mode){}
}
