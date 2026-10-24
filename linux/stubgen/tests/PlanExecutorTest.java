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

        // Concurrent jobs own separate executors/cancellation state.
        ExecutorService concurrentJobs=Executors.newFixedThreadPool(2);CountDownLatch peerStarted=new CountDownLatch(1);
        Future<PlanExecutor.Outcome<String>> cancelledJob=concurrentJobs.submit(()->PlanExecutor.execute(List.of(node("a")),1,n->{
            throw new InterruptedException("cancel A");}));
        Future<PlanExecutor.Outcome<String>> peerJob=concurrentJobs.submit(()->PlanExecutor.execute(List.of(node("b")),1,n->{
            peerStarted.countDown();Thread.sleep(50);return "B";}));
        peerStarted.await();PlanExecutor.Outcome<String> cancelled=cancelledJob.get(),peer=peerJob.get();concurrentJobs.shutdownNow();
        check(cancelled.failed.containsKey("a")&&peer.completed.get("b").equals("B"),"cancellation is isolated between jobs");
        System.out.println("PASS parallel plan order, cross-job in-flight dedup, cancellation isolation, dependant failure isolation");
    }
}
