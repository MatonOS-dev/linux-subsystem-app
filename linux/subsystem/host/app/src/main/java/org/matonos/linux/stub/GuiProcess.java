package org.matonos.linux.stub;

import java.io.IOException;

/** A GUI launch has no terminal or input producer. Close its pipe to deliver EOF. */
final class GuiProcess {
    private GuiProcess() {}

    static Process start(ProcessBuilder builder) throws IOException {
        Process child = builder.start();
        try {
            child.getOutputStream().close();
        } catch (IOException failure) {
            child.destroy();
            throw failure;
        }
        return child;
    }
}
