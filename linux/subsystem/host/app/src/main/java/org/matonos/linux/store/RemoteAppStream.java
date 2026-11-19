package org.matonos.linux.store;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Loads each remote independently so an unavailable AppStream index cannot hide other sources. */
public final class RemoteAppStream {
    public interface Refresher { void refresh(String remote) throws Exception; }
    public interface Reader { List<Map<String,Object>> read(String remote) throws Exception; }
    public interface Failure { void log(String remote,String stage,Exception error); }

    public static List<Map<String,Object>> load(Iterable<String> remotes,Map<String,Boolean> broken,
            Refresher refresher,Reader reader,Failure failure) {
        List<Map<String,Object>> result=new ArrayList<>();
        for(String remote:remotes) {
            boolean refreshFailed=false;
            try { refresher.refresh(remote); }
            catch(Exception error) { refreshFailed=true;broken.put(remote,true);failure.log(remote,"refresh",error); }
            try {
                List<Map<String,Object>> rows=reader.read(remote);
                if(rows==null)throw new IllegalStateException("AppStream reader returned null");
                result.addAll(rows);
                broken.put(remote,refreshFailed);
            } catch(Exception error) {
                broken.put(remote,true);failure.log(remote,"read",error);
            }
        }
        return result;
    }

    private RemoteAppStream() { }
}
