package org.matonos.linux.stub;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

public final class GuiProcessTest {
    private static ProcessBuilder command(String[] prefix) {
        ArrayList<String> argv = new ArrayList<>(Arrays.asList(prefix));
        // The shell forks a child that reads inherited stdin, then waits for it.
        argv.addAll(Arrays.asList("/bin/sh", "-c", "(cat >/dev/null); printf stdin-eof"));
        return new ProcessBuilder(argv);
    }

    private static void completed(Process child) throws Exception {
        if (!child.waitFor(10, TimeUnit.SECONDS)) throw new AssertionError("stdin EOF never reached child");
        String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (child.exitValue() != 0 || !output.equals("stdin-eof"))
            throw new AssertionError("status=" + child.exitValue() + " output=" + output + " stderr=" +
                new String(child.getErrorStream().readAllBytes(), StandardCharsets.UTF_8));
    }

    public static void main(String[] args) throws Exception {
        Process control = command(args).start();
        try {
            if (control.waitFor(500, TimeUnit.MILLISECONDS))
                throw new AssertionError("control should block with its stdin writer open");
            control.getOutputStream().close();
            completed(control);
        } finally { control.destroyForcibly(); }
        Process child = GuiProcess.start(command(args));
        try { completed(child); } finally { child.destroyForcibly(); }
        System.out.println("PASS GUI launch delivers stdin EOF through fork/wait" +
            (args.length == 0 ? "" : " under proxy supervisor and Android filter"));
    }
}
