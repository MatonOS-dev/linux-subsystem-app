package org.matonos.tray;

import android.os.Bundle;
import java.util.List;

oneway interface ITrayListener {
    void itemsChanged(in List<Bundle> items);
}
