package org.matonos.linux.store;
import java.io.IOException;
import java.util.concurrent.*;
/** Launch-time repair asks the store to use its recorded source and remote. */
public final class StoreRepair {
    public static final String ACTION="org.matonos.store.STORE";
    public interface Result {void result(boolean ok,String error);}
    public interface Store {void update(String id,Result result)throws Exception;}
    private final String app,dependency;
    private final CompletableFuture<Void> completed=new CompletableFuture<>();
    public StoreRepair(String app,String dependency){this.app=app;this.dependency=dependency;}
    public void request(Store store){
        try{store.update(app,(ok,error)->{if(ok)completed.complete(null);else failed("Store refused repair: "+error);});}
        catch(Exception error){failed("Store repair request failed: "+error);}
    }
    public void failed(String error){completed.completeExceptionally(new IOException(error+" app="+app+" dependency="+dependency));}
    public void await(long timeout,TimeUnit unit)throws Exception {
        try{completed.get(timeout,unit);}catch(ExecutionException error){throw (Exception)error.getCause();}
    }
}
