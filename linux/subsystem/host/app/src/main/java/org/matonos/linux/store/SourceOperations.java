package org.matonos.linux.store;

import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BooleanSupplier;

/** Work-independent keep-alive table. Terminal delivery wins once, even during cancellation. */
public final class SourceOperations {
    private final ConcurrentHashMap<String,Operation> table=new ConcurrentHashMap<>();
    public Operation start(String id) {
        if(id==null||id.isBlank())throw new IllegalArgumentException("Operation ID required");
        Operation op=new Operation(id);
        if(table.putIfAbsent(id,op)!=null)throw new IllegalArgumentException("Operation already running: "+id);
        return op;
    }
    public boolean running(String id){return id!=null&&table.containsKey(id);}
    public void cancel(String id){Operation op=id==null?null:table.get(id);if(op!=null)op.cancel();}
    public final class Operation implements BooleanSupplier {
        private final String id;
        private volatile boolean cancelled;
        private boolean finished,terminalClaimed;
        public volatile Thread worker;
        private final java.util.Set<Process> processes=new java.util.HashSet<>();
        private final java.util.Set<Runnable> cancelActions=new java.util.HashSet<>();
        public synchronized void onCancel(Runnable action){if(cancelled)action.run();else cancelActions.add(action);}
        public synchronized void removeCancel(Runnable action){cancelActions.remove(action);}
        private Operation(String id){this.id=id;}
        public boolean cancelled(){return cancelled;}
        @Override public boolean getAsBoolean(){return cancelled;}
        public void check(){if(cancelled)throw new java.util.concurrent.CancellationException("cancelled");}
        public synchronized void process(Process value){processes.add(value);if(cancelled)value.destroyForcibly();}
        public synchronized void release(Process value){processes.remove(value);}
        private synchronized void cancel(){
            if(finished||terminalClaimed)return;
            cancelled=true;
            for(Process process:processes)process.destroyForcibly();
            if(worker!=null)worker.interrupt();
            for(Runnable action:cancelActions)action.run();
            cancelActions.clear();
        }
        public synchronized boolean claimTerminal(){if(finished||terminalClaimed)return false;terminalClaimed=true;return true;}
        public synchronized boolean finish(){if(finished)return false;finished=true;table.remove(id,this);return true;}
    }
}
