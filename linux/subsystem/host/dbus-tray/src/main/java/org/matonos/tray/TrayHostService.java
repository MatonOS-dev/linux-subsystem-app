package org.matonos.tray;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceControlViewHost;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.Arrays;
import java.util.List;

/** Bound Launcher3 interface to the shared tray item model. */
public final class TrayHostService extends Service {
  static final String ACTION_HOST = "org.matonos.tray.HOST";
  private static final RemoteCallbackList<ITrayListener> LISTENERS = new RemoteCallbackList<>();
  private static final Handler MAIN = new Handler(Looper.getMainLooper());
  private static volatile TrayHostService instance;
  private SurfaceControlViewHost pillHost;
  private PillView pillView;
  private boolean pillAttached;

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

        @Override
        public SurfaceControlViewHost.SurfacePackage attachPill(
            int displayId, IBinder hostToken, int heightPx, IPillCallback callback) {
          checkCaller();
          if (hostToken == null) {
            throw new IllegalArgumentException("attachPill hostToken is null package=org.matonos.tray");
          }
          if (callback == null) {
            throw new IllegalArgumentException("attachPill callback is null package=org.matonos.tray");
          }
          if (heightPx <= 0) {
            throw new IllegalArgumentException("attachPill heightPx must be positive: " + heightPx);
          }
          return onMain(() -> attachPillOnMain(displayId, hostToken, heightPx, callback));
        }

        @Override
        public void detachPill() {
          checkCaller();
          onMain(() -> {
            detachPillOnMain();
            return null;
          });
        }

        @Override
        public void setPillOrigin(int screenX, int screenY) {
          checkCaller();
          onMain(() -> {
            if (pillView == null) {
              throw new IllegalStateException("setPillOrigin without attached pill package=org.matonos.tray");
            }
            pillView.setScreenOrigin(screenX, screenY);
            return null;
          });
        }
      };

  @Override
  public IBinder onBind(Intent intent) {
    return binder;
  }

  @Override
  public void onCreate() {
    super.onCreate();
    instance = this;
  }

  @Override
  public void onDestroy() {
    onMain(() -> {
      detachPillOnMain();
      return null;
    });
    if (instance == this) instance = null;
    super.onDestroy();
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
    TrayHostService current = instance;
    if (current != null) {
      MAIN.post(() -> {
        if (current.pillView != null) current.pillView.refreshItems();
      });
    }
  }

  private SurfaceControlViewHost.SurfacePackage attachPillOnMain(
      int displayId, IBinder hostToken, int heightPx, IPillCallback callback) {
    detachPillOnMain();
    Display display = getSystemService(android.hardware.display.DisplayManager.class).getDisplay(displayId);
    if (display == null) {
      Log.e(TrayService.TAG, "Pill attach failed package=org.matonos.tray displayId=" + displayId + " heightPx=" + heightPx);
      throw new IllegalArgumentException("Unknown displayId=" + displayId + " package=org.matonos.tray");
    }
    try {
      android.content.Context displayContext = createDisplayContext(display);
      pillHost = new SurfaceControlViewHost(displayContext, display, hostToken);
      pillView = new PillView(displayContext, this, heightPx, callback);
      pillHost.setView(pillView, Math.max(1, pillView.desiredWidth()), pillView.desiredHeight());
      pillAttached = true;
      updatePillLayout(pillView.desiredWidth(), pillView.desiredHeight());
      return pillHost.getSurfacePackage();
    } catch (RuntimeException e) {
      Log.e(TrayService.TAG, "Pill attach failed package=org.matonos.tray displayId=" + displayId + " heightPx=" + heightPx, e);
      detachPillOnMain();
      throw e;
    }
  }

  private void detachPillOnMain() {
    if (pillHost != null) {
      try {
        pillHost.release();
      } catch (RuntimeException e) {
        Log.e(TrayService.TAG, "Pill SurfaceControlViewHost release failed package=org.matonos.tray", e);
      }
      pillHost = null;
      pillView = null;
      pillAttached = false;
    }
  }

  private static <T> T onMain(Callable<T> action) {
    if (Looper.myLooper() == Looper.getMainLooper()) {
      try {
        return action.call();
      } catch (RuntimeException e) {
        throw e;
      } catch (Exception e) {
        throw new IllegalStateException("Main-thread tray operation failed package=org.matonos.tray", e);
      }
    }
    FutureTask<T> task = new FutureTask<>(action);
    MAIN.post(task);
    try {
      return task.get(10, TimeUnit.SECONDS);
    } catch (Exception e) {
      Log.e(TrayService.TAG, "Main-thread tray operation timed out or failed package=org.matonos.tray", e);
      throw new IllegalStateException("Main-thread tray operation timed out or failed package=org.matonos.tray", e);
    }
  }

  void performPillAction(TrayItem item, Rect anchor, boolean openMenu) {
    if (item == null) {
      Log.e(TrayService.TAG, "Pill action ignored missing item package=org.matonos.tray");
      return;
    }
    launch(TrayService.key(item.attachId, item.service), anchor, openMenu);
  }

  void updatePillLayout(int widthPx, int heightPx) {
    if (!pillAttached || pillHost == null || pillView == null) return;
    try {
      pillHost.relayout(Math.max(1, widthPx), heightPx);
    } catch (RuntimeException e) {
      Log.e(TrayService.TAG, "Pill relayout failed package=org.matonos.tray widthPx=" + widthPx + " heightPx=" + heightPx, e);
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
    intent.addFlags(
        Intent.FLAG_ACTIVITY_NEW_TASK
            | Intent.FLAG_ACTIVITY_CLEAR_TOP
            | Intent.FLAG_ACTIVITY_SINGLE_TOP);
    if (openMenu) {
      intent.putExtra(TrayMenuActivity.EXTRA_OPEN_MENU, true);
    }
    if (anchor != null) {
      intent.setSourceBounds(anchor);
    }
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
