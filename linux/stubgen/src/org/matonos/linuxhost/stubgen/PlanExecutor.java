package org.matonos.linuxhost.stubgen;

import java.util.*;
import java.util.concurrent.*;

/** Runs independent plan nodes concurrently while preserving dependency order. */
public final class PlanExecutor {
    private static final class DependencyFailed extends Exception { }
    public interface Task<T> { T run(DependencyPlan.Node node) throws Exception; }
    public static final class Outcome<T> {
        public final Map<String,T> completed;
        public final Map<String,Throwable> failed;
        public final Set<String> skipped;
        Outcome(Map<String,T> completed,Map<String,Throwable> failed,Set<String> skipped) {
            this.completed=Collections.unmodifiableMap(completed);
            this.failed=Collections.unmodifiableMap(failed);
            this.skipped=Collections.unmodifiableSet(skipped);
        }
    }
    private PlanExecutor() { }

    /** Nodes with failed dependencies are skipped; unrelated branches still run. */
    public static <T> Outcome<T> execute(List<DependencyPlan.Node> plan,int workers,Task<T> task)throws InterruptedException {
        ForkJoinPool pool=new ForkJoinPool(Math.max(1,workers));
        try { return execute(plan,pool,task); }
        finally { pool.shutdownNow(); }
    }

    /** Uses one shared bounded pool for ref tasks and their nested layer builds. */
    public static <T> Outcome<T> execute(List<DependencyPlan.Node> plan,ForkJoinPool pool,Task<T> task)throws InterruptedException {
        Map<String,Future<T>> futures=new LinkedHashMap<>();
        Map<String,T> completed=new LinkedHashMap<>();Map<String,Throwable> failed=new LinkedHashMap<>();Set<String> skipped=new LinkedHashSet<>();
        for(DependencyPlan.Node node:plan) {
                List<Future<T>> dependencies=new ArrayList<>();boolean missing=false;
                for(String ref:node.stageDependencies) {
                    Future<T> dependency=futures.get(ref);
                    if(dependency==null) { if(!node.reused)missing=true; }
                    else dependencies.add(dependency);
                }
                if(missing) { skipped.add(node.ref);continue; }
                futures.put(node.ref,pool.submit(()->{
                    for(Future<T> dependency:dependencies)try { dependency.get(); }
                    catch(ExecutionException failure) { throw new DependencyFailed(); }
                    return task.run(node);
                }));
            }
        for(Map.Entry<String,Future<T>> entry:futures.entrySet())try { completed.put(entry.getKey(),entry.getValue().get()); }
            catch(ExecutionException error) {
                Throwable cause=error.getCause();
                boolean dependencyFailed=false;
                for(Throwable nested=cause;nested!=null;nested=nested.getCause())if(nested instanceof DependencyFailed){dependencyFailed=true;break;}
                if(dependencyFailed)skipped.add(entry.getKey());else failed.put(entry.getKey(),cause);
                // A dependent Future.get() wraps the original error. Keep the ref-level cause useful.
            }
        for(DependencyPlan.Node node:plan)if(futures.containsKey(node.ref)&&!completed.containsKey(node.ref)&&!failed.containsKey(node.ref))skipped.add(node.ref);
        return new Outcome<>(completed,failed,skipped);
    }

    /** Shared in-flight work is removed on completion; failures are never cached. */
    public static final class InFlight<K,V> {
        private final ConcurrentHashMap<K,CompletableFuture<V>> work=new ConcurrentHashMap<>();
        public V run(K key,Callable<V> action)throws Exception {
            CompletableFuture<V> mine=new CompletableFuture<>();CompletableFuture<V> shared=work.putIfAbsent(key,mine);
            if(shared==null) {
                shared=mine;
                try { mine.complete(action.call()); }
                catch(Throwable error) { mine.completeExceptionally(error); }
                finally { work.remove(key,mine); }
            }
            try { return shared.get(); }
            catch(ExecutionException error) {
                Throwable cause=error.getCause();if(cause instanceof Exception)throw (Exception)cause;
                if(cause instanceof Error)throw (Error)cause;throw new RuntimeException(cause);
            } catch(InterruptedException error) { Thread.currentThread().interrupt();throw error; }
        }
    }
}
