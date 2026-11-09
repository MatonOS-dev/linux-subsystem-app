package org.matonos.tray;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.util.Log;
import java.util.Arrays;
import java.util.List;

/** Bound Launcher3 interface to the shared tray item model. */
public final class TrayHostService extends Service {
  static final String ACTION_HOST = "org.matonos.tray.HOST";
  private static final RemoteCallbackList<ITrayListener> LISTENERS = new RemoteCallbackList<>();

  private final ITrayHost.Stub binder =
      new ITrayHost.Stub() {
        @Override
        public void register(ITrayListener listener) {
          checkCaller();
          if (listener == null) {
            throw new IllegalArgumentException("register listener is null");
          }
          synchronized (LISTENERS) {
            LISTENERS.register(listener);
          }
          sendItems(listener, TrayHostItems.snapshot());
        }

        @Override
        public void unregister(ITrayListener listener) {
          checkCaller();
          if (listener == null) {
            throw new IllegalArgumentException("unregister listener is null");
          }
          synchronized (LISTENERS) {
            LISTENERS.unregister(listener);
          }
        }

        @Override
        public void activate(String key, Rect anchor) {
          checkCaller();
          launch(key, anchor, false);
        }

        @Override
        public void openMenu(String key, Rect anchor) {
          checkCaller();
          launch(key, anchor, true);
        }
      };

  @Override
  public IBinder onBind(Intent intent) {
    return binder;
  }

  static void modelChanged() {
    List<android.os.Bundle> items = TrayHostItems.snapshot();
    int count;
    synchronized (LISTENERS) {
      count = LISTENERS.beginBroadcast();
      try {
        for (int i = 0; i < count; i++) {
          ITrayListener listener = LISTENERS.getBroadcastItem(i);
          try {
            listener.itemsChanged(items);
          } catch (RemoteException e) {
            Log.e(TrayService.TAG, "Tray listener update failed package=org.matonos.tray", e);
          }
        }
      } finally {
        LISTENERS.finishBroadcast();
      }
    }
  }

  private void sendItems(ITrayListener listener, List<android.os.Bundle> items) {
    try {
      listener.itemsChanged(items);
    } catch (RemoteException e) {
      Log.e(
          TrayService.TAG,
          "Initial tray listener update failed package=org.matonos.tray listener=" + listener,
          e);
    }
  }

  private void checkCaller() {
    int uid = Binder.getCallingUid();
    String[] packages = getPackageManager().getPackagesForUid(uid);
    String packageList = packages == null ? "[]" : Arrays.toString(packages);
    if (uid == Process.myUid()) {
      return;
    }
    if (packages != null && isHomeRoleHolder(packages)) {
      return;
    }
    throw new SecurityException("Tray host call denied uid=" + uid + " package=" + packageList);
  }

  private boolean isHomeRoleHolder(String[] packages) {
    Intent home = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME);
    android.content.pm.ResolveInfo resolved = getPackageManager().resolveActivity(home, PackageManager.MATCH_DEFAULT_ONLY);
    if (resolved != null && resolved.activityInfo != null) {
      for (String packageName : packages) {
        if (resolved.activityInfo.packageName.equals(packageName)) {
          return true;
        }
      }
    }
    return false;
  }

  private void launch(String key, Rect anchor, boolean openMenu) {
    TrayItem item = TrayService.ITEMS.get(key);
    if (item == null) {
      Log.e(TrayService.TAG, "Tray action ignored unknown key package=org.matonos.tray key=" + key);
      return;
    }
    Intent intent = new Intent(this, TrayMenuActivity.class)
        .putExtra(TrayMenuActivity.EXTRA_ITEM_KEY, key);
    if (openMenu) {
      intent.putExtra(TrayMenuActivity.EXTRA_OPEN_MENU, true);
    }
    if (anchor != null) {
      intent.setSourceBounds(anchor);
    }
    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    try {
      startActivity(intent);
    } catch (RuntimeException e) {
      Log.e(
          TrayService.TAG,
          "Tray activity start failed package=org.matonos.tray service="
              + item.service
              + " path="
              + item.path
              + " key="
              + key,
          e);
    }
  }
}
