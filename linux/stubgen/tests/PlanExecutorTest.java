package org.matonos.linuxhost.stubgen;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class PlanExecutorTest {
    static DependencyPlan.Node node(String ref,String... dependencies) {
        DependencyPlan.Node node=new DependencyPlan.Node(ref,"remote",null,false);
        node.dependencies.addAll(Arrays.asList(dependencies));
        node.stageDependencies.addAll(Arrays.asList(dependencies));return node;
    }
    static void check(boolean condition,String message) { if(!condition)throw new AssertionError(message); }
    public static void main(String[] args)throws Exception {
        DependencyPlan.Node runtime=node("runtime"),locale=node("locale"),gl=node("gl"),app=node("app","runtime","gl");
        List<String> order=Collections.synchronizedList(new ArrayList<>());
        PlanExecutor.Outcome<String> result=PlanExecutor.execute(List.of(runtime,locale,gl,app),3,n->{
            if(n.ref.equals("app"))check(order.contains("runtime")&&order.contains("gl"),"app ran before dependencies");
            order.add(n.ref);return n.ref;
        });
        check(result.completed.size()==4&&order.indexOf("runtime")<order.indexOf("app")&&order.indexOf("gl")<order.indexOf("app"),"dependency order");

        // Repair plans can list the app before refs discovered through an
        // installed runtime's related refs. Execution must resolve that graph,
        // not treat a later plan entry as a missing stage dependency.
        String appRef="app/org.mozilla.firefox/x86_64/stable";
        String runtimeRef="runtime/org.gnome.Platform/x86_64/49";
        String localeRef="runtime/org.gnome.Platform.Locale/x86_64/49";
        String glRef="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08";
        String codecsRef="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08-extra";
        DependencyPlan.Node repairApp=new DependencyPlan.Node(appRef,"remote",null,false);
        repairApp.stageDependencies.addAll(List.of(runtimeRef,glRef,codecsRef));
        DependencyPlan.Node repairRuntime=new DependencyPlan.Node(runtimeRef,"remote",null,true);
        DependencyPlan.Node localeNode=new DependencyPlan.Node(localeRef,"remote",null,false);
        localeNode.stageDependencies.add(runtimeRef);
        DependencyPlan.Node glNode=new DependencyPlan.Node(glRef,"remote",null,false);
        glNode.stageDependencies.add(runtimeRef);
        DependencyPlan.Node codecsNode=new DependencyPlan.Node(codecsRef,"remote",null,false);
        codecsNode.stageDependencies.add(runtimeRef);
        PlanExecutor.Outcome<String> repaired=PlanExecutor.execute(List.of(repairApp,repairRuntime,localeNode,glNode,codecsNode),4,n->n.ref);
        check(repaired.completed.containsKey(appRef)&&repaired.completed.size()==5&&repaired.skipped.isEmpty(),
                "repair graph with installed runtime and related extensions stages: "+repaired.skippedDependencies);

        DependencyPlan.Node base=node("base"),localeCommit=node("locale","base"),codecs=node("codecs"),glCommit=node("gl-independent");
        CountDownLatch rootsStarted=new CountDownLatch(3),releaseRoots=new CountDownLatch(1);
        AtomicBoolean baseFinished=new AtomicBoolean();AtomicInteger active=new AtomicInteger(),peak=new AtomicInteger();
        ExecutorService commitJobs=Executors.newSingleThreadExecutor();
        Future<PlanExecutor.Outcome<Void>> commitPlan=commitJobs.submit(()->PlanExecutor.execute(List.of(base,codecs,glCommit,localeCommit),3,n->{
            if(n.ref.equals("locale"))check(baseFinished.get(),"Locale commit ran before its runtime");
            int now=active.incrementAndGet();peak.accumulateAndGet(now,Math::max);
            if(!n.ref.equals("locale")) {
                rootsStarted.countDown();releaseRoots.await();
            }
            if(n.ref.equals("base"))baseFinished.set(true);
            active.decrementAndGet();return null;
        }));
        check(rootsStarted.await(2,TimeUnit.SECONDS),"independent commits started concurrently");
        releaseRoots.countDown();
        PlanExecutor.Outcome<Void> commitResult=commitPlan.get();commitJobs.shutdownNow();
        check(commitResult.completed.size()==4&&peak.get()>=2,"independent package commits overlap");
        check(baseFinished.get(),"runtime package commit completed");

        PlanExecutor.InFlight<String,String> shared=new PlanExecutor.InFlight<>();AtomicInteger pulls=new AtomicInteger();
        CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);ExecutorService jobs=Executors.newFixedThreadPool(2);
        Future<String> first=jobs.submit(()->shared.run("runtime@commit",()->{pulls.incrementAndGet();entered.countDown();release.await();return "deployment";}));
        entered.await();AtomicReference<String> secondResult=new AtomicReference<>();CountDownLatch joined=new CountDownLatch(1);
        Thread second=new Thread(()->{try{joined.countDown();secondResult.set(shared.run("runtime@commit",()->{pulls.incrementAndGet();return "duplicate";}));}
            catch(Exception error){throw new RuntimeException(error);}},"in-flight-waiter");second.start();joined.await();
        long waitUntil=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(second.getState()!=Thread.State.WAITING&&System.nanoTime()<waitUntil)Thread.sleep(1);
        check(second.getState()==Thread.State.WAITING,"second job joined in-flight work");release.countDown();
        check(first.get().equals("deployment"),"leader result");second.join();
        check("deployment".equals(secondResult.get())&&pulls.get()==1,"shared runtime fetched once");jobs.shutdownNow();

        DependencyPlan.Node bad=node("bad"),dependent=node("dependent","bad"),independent=node("independent");
        result=PlanExecutor.execute(List.of(bad,dependent,independent),2,n->{
            if(n.ref.equals("bad"))throw new IllegalStateException("one branch failed");
            if(n.ref.equals("independent"))return "ok";
            throw new AssertionError("dependent should be skipped");
        });
        check(result.failed.containsKey("bad")&&result.skipped.contains("dependent")&&result.completed.containsKey("independent"),"failure only aborts dependants");
        check(result.skippedDependencies.get("dependent").equals(List.of("bad")),"skipped dependency cause is retained");

        // Concurrent jobs own separate executors/cancellation state.
        ExecutorService concurrentJobs=Executors.newFixedThreadPool(2);CountDownLatch peerStarted=new CountDownLatch(1);
        Future<PlanExecutor.Outcome<String>> cancelledJob=concurrentJobs.submit(()->PlanExecutor.execute(List.of(node("a")),1,n->{
            throw new InterruptedException("cancel A");}));
        Future<PlanExecutor.Outcome<String>> peerJob=concurrentJobs.submit(()->PlanExecutor.execute(List.of(node("b")),1,n->{
            peerStarted.countDown();Thread.sleep(50);return "B";}));
        peerStarted.await();PlanExecutor.Outcome<String> cancelled=cancelledJob.get(),peer=peerJob.get();concurrentJobs.shutdownNow();
        check(cancelled.failed.containsKey("a")&&peer.completed.get("b").equals("B"),"cancellation is isolated between jobs");
        System.out.println("PASS dependency-ordered parallel commits, parallel plan order, cross-job in-flight dedup, cancellation isolation, dependant failure isolation");
    }
}
