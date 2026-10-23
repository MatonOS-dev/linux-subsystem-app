package org.matonos.linux.stub;

import java.util.Arrays;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.assertEquals;

public final class LoaderDnsCommandTest {
    @Test public void addsConfiguredResolverForProxyInterception() {
        assertEquals("127.0.0.53", LoaderCommand.resolverAddress());
        List<String> command = new java.util.ArrayList<>();
        LoaderCommand.addDns(command, LoaderCommand.resolverAddress());
        assertEquals(Arrays.asList("--dns-forwarder", "127.0.0.53:53"), command);
    }
}
