package org.matonos.tray;

import android.app.ActivityOptions;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.RemoteException;
import android.util.Log;
import java.util.Arrays;

/** Uses the foreground tray action to lend the normal Android BAL exemption. */
final class TrayWindowActivation {
  private static final String STUB_ACTIVITY = "org.matonos.linux.stub.StubActivity";
  private static final String EXTRA_TRAY_PLACEHOLDER = "org.matonos.linuxhost.TRAY_PLACEHOLDER";

  private TrayWindowActivation() {}

  static boolean bringAppForward(Context context, TrayItem item) {
    TrayService.Bus bus = item.bus;
    if (bus == null) {
      Log.e(TrayService.TAG, "Window activation lend failed service=" + item.service
          + " path=" + item.path + " reason=attached bus unavailable");
      return false;
    }
    String[] packages = context.getPackageManager().getPackagesForUid(bus.uid);
    if (packages == null || packages.length == 0) {
      Log.e(TrayService.TAG, "Window activation lend failed service=" + item.service
          + " path=" + item.path + " uid=" + bus.uid + " reason=no packages for D-Bus UID");
      return false;
    }
    final boolean liveWindow;
    try {
      if (bus.icons == null) {
        Log.e(TrayService.TAG, "Window activation decision failed service=" + item.service
            + " path=" + item.path + " uid=" + bus.uid + " packages=" + Arrays.toString(packages)
            + " reason=stub callback unavailable");
        return false;
      }
      liveWindow = bus.icons.hasLiveWindow();
      Log.i(TrayService.TAG, "Window activation decision service=" + item.service
          + " path=" + item.path + " uid=" + bus.uid + " packages=" + Arrays.toString(packages)
          + " hasLiveWindow=" + liveWindow);
    } catch (RemoteException | RuntimeException error) {
      Log.e(TrayService.TAG, "Window activation live-window query failed service=" + item.service
          + " path=" + item.path + " uid=" + bus.uid + " packages=" + Arrays.toString(packages), error);
      return false;
    }
    if (liveWindow) {
      Log.i(TrayService.TAG, "Window activation result=existing-window-kept service=" + item.service
          + " path=" + item.path + " uid=" + bus.uid + " packages=" + Arrays.toString(packages)
          + " activityStart=skipped");
      return true;
    }
    for (String packageName : packages) {
      ComponentName component = new ComponentName(packageName, STUB_ACTIVITY);
      try {
        context.getPackageManager().getActivityInfo(component, 0);
      } catch (PackageManager.NameNotFoundException absent) {
        continue;
      }
      Intent intent = new Intent().setComponent(component)
          .putExtra(EXTRA_TRAY_PLACEHOLDER, true)
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP
              | Intent.FLAG_ACTIVITY_SINGLE_TOP);
      PendingIntent launch = PendingIntent.getActivity(context, bus.uid, intent,
          PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
      ActivityOptions options = ActivityOptions.makeBasic()
          .setPendingIntentBackgroundActivityStartMode(
              ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED);
      try {
        Log.i(TrayService.TAG, "Lending foreground activity start service=" + item.service
            + " path=" + item.path + " uid=" + bus.uid + " package=" + packageName
            + " target=" + STUB_ACTIVITY);
        launch.send(context, 0, null, null, null, null, options.toBundle());
        Log.i(TrayService.TAG, "Lent foreground activity start sent service=" + item.service
            + " path=" + item.path + " uid=" + bus.uid + " package=" + packageName
            + " result=started");
        return true;
      } catch (PendingIntent.CanceledException | RuntimeException error) {
        Log.e(TrayService.TAG, "Lending foreground activity start failed service=" + item.service
            + " path=" + item.path + " uid=" + bus.uid + " package=" + packageName, error);
        return false;
      }
    }
    Log.e(TrayService.TAG, "Window activation lend failed service=" + item.service
        + " path=" + item.path + " uid=" + bus.uid + " reason=no exported StubActivity");
    return false;
  }
}
