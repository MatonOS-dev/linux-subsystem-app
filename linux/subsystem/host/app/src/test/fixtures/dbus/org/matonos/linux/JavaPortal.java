package org.matonos.linux;
import android.content.Context;
import java.io.File;
import java.util.function.Consumer;
public class JavaPortal {
    public interface Launcher {}
    public static int closes;
    public JavaPortal(Context c,File f,Consumer<Boolean> changed,Launcher launcher){}
    public String secret(){return "secret";}
    public void setFlatpakDirectories(File userDir,File systemDir){}
    public void close(){closes++;}
}
