package org.matonos.linux;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.util.Log;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/** Subsystem-owned D-Bus provider discovery, first-seen order, and handler choice. */
public final class DBusProviderSettings extends ContentProvider {
    static final String ACTION = "org.matonos.dbus.SERVICE";
    static final String URI_TEXT = "content://org.matonos.linux.dbus-settings/providers";
    static final Uri URI = Uri.parse(URI_TEXT);
    private static final String TAG = "MatonDBusSettings";
    private static final String READ = "org.matonos.permission.READ_DBUS_PROVIDER_SETTINGS";
    private static final String WRITE = "org.matonos.permission.WRITE_DBUS_PROVIDER_SETTINGS";
    private static final String PREFS = "dbus-provider-settings";
    private static final String ORDER_PREFIX = "order:";
    private static final String CHOICE_PREFIX = "choice:";

    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return "vnd.android.cursor.item/vnd.maton.dbus-providers"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) { return null; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("D-Bus provider settings are call-only"); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException("D-Bus provider settings are call-only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException("D-Bus provider settings are call-only"); }

    @Override public Bundle call(String method, String arg, Bundle extras) {
        if ("getProviders".equals(method) || "refreshProviders".equals(method)) {
            requireReadCaller();
            try {
                String json = refreshAndSerialize();
                Bundle result = new Bundle();
                result.putString("providers", json);
                return result;
            } catch (Exception error) {
                Log.e(TAG, "Provider discovery failed uid=" + Binder.getCallingUid(), error);
                throw new IllegalStateException("Provider discovery failed", error);
            }
        }
        if ("setChoice".equals(method)) {
            require(WRITE);
            if (arg == null || extras == null) throw new IllegalArgumentException("Missing D-Bus name or provider id");
            String name = extras.getString("name");
            if (name == null || arg.isEmpty()) throw new IllegalArgumentException("Empty D-Bus name or provider id");
            android.content.SharedPreferences prefs = getContext().getSharedPreferences(PREFS, 0);
            try {
                JSONArray rows = new JSONArray(refreshAndSerialize());
                boolean foundName = false, foundChoice = false;
                for (int i = 0; i < rows.length(); i++) {
                    JSONObject row = rows.getJSONObject(i);
                    if (!name.equals(row.getString("name"))) continue;
                    foundName = true;
                    JSONArray claimants = row.getJSONArray("claimants");
                    for (int j = 0; j < claimants.length(); j++)
                        if (arg.equals(claimants.getJSONObject(j).getString("id"))) foundChoice = true;
                }
                if (!foundName || !foundChoice) throw new IllegalArgumentException("Provider does not claim D-Bus name=" + name + " id=" + arg);
            } catch (RuntimeException error) { throw error; }
            catch (Exception error) { throw new IllegalStateException("Cannot validate D-Bus handler choice name=" + name, error); }
            String old = prefs.getString(CHOICE_PREFIX + name, null);
            if (arg.equals(old)) return Bundle.EMPTY;
            if (!prefs.edit().putString(CHOICE_PREFIX + name, arg).commit())
                throw new IllegalStateException("Could not persist D-Bus handler choice name=" + name + " id=" + arg);
            Log.i(TAG, "D-Bus handler choice changed uid=" + Binder.getCallingUid() + " name=" + name
                    + " old=" + old + " new=" + arg);
            getContext().getContentResolver().notifyChange(URI, null);
            return Bundle.EMPTY;
        }
        throw new IllegalArgumentException("Unknown D-Bus provider settings method=" + method);
    }

    private void require(String permission) {
        getContext().enforceCallingPermission(permission, "D-Bus provider settings requires " + permission);
    }

    private void requireReadCaller() {
        // Generated stubs use per-app signing keys, so they cannot hold this subsystem signature permission.
        // Their loader and Flatpak-ref manifest markers identify the read-only session-bus clients.
        if (getContext().checkCallingPermission(READ) == PackageManager.PERMISSION_GRANTED) return;
        int uid = Binder.getCallingUid();
        String[] packages = getContext().getPackageManager().getPackagesForUid(uid);
        if (packages != null) for (String packageName : packages) {
            try {
                android.content.pm.ApplicationInfo app = getContext().getPackageManager().getApplicationInfo(
                        packageName, PackageManager.GET_META_DATA);
                String ref = app.metaData == null ? null : app.metaData.getString("org.matonos.linuxhost.FLATPAK_REF");
                if (app.uid == uid && "org.matonos.linux.stub.StubLoader".equals(app.appComponentFactory)
                        && ref != null && ref.matches("app/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")) return;
            } catch (PackageManager.NameNotFoundException error) {
                Log.e(TAG, "Cannot verify D-Bus settings caller uid=" + uid + " package=" + packageName, error);
            }
        }
        throw new SecurityException("D-Bus provider settings read denied uid=" + uid
                + " packages=" + java.util.Arrays.toString(packages));
    }

    private String refreshAndSerialize() throws Exception {
        android.content.SharedPreferences prefs = getContext().getSharedPreferences(PREFS, 0);
        PackageManager pm = getContext().getPackageManager();
        List<ResolveInfo> services = pm.queryIntentServices(new Intent(ACTION), PackageManager.GET_META_DATA);
        Collections.sort(services, (a, b) -> componentId(a).compareTo(componentId(b)));
        LinkedHashMap<String, List<Claimant>> available = new LinkedHashMap<>();
        for (ResolveInfo ri : services) {
            if (ri.serviceInfo == null) continue;
            String names = ri.serviceInfo.metaData == null ? null : ri.serviceInfo.metaData.getString("org.matonos.dbus.NAMES");
            if (names == null || names.trim().isEmpty()) {
                Log.e(TAG, "Provider has no names metadata component=" + componentId(ri));
                continue;
            }
            String id = componentId(ri);
            CharSequence label = ri.loadLabel(pm);
            for (String raw : names.split(",")) {
                String name = raw.trim();
                if (!validWellKnownName(name)) {
                    Log.e(TAG, "Ignoring invalid D-Bus provider name component=" + id + " name=" + name);
                    continue;
                }
                List<Claimant> list = available.get(name);
                if (list == null) { list = new ArrayList<>(); available.put(name, list); }
                boolean duplicate = false;
                for (Claimant c : list) if (c.id.equals(id)) duplicate = true;
                if (!duplicate) list.add(new Claimant(id, ri.serviceInfo.packageName, label == null ? ri.serviceInfo.packageName : label.toString()));
            }
        }
        boolean changed = false;
        JSONArray rows = new JSONArray();
        for (String name : available.keySet()) {
            List<Claimant> now = available.get(name);
            JSONArray persistedOrder;
            try { persistedOrder = new JSONArray(prefs.getString(ORDER_PREFIX + name, "[]")); }
            catch (Exception error) { Log.e(TAG, "Invalid stored claimant order name=" + name, error); persistedOrder = new JSONArray(); }
            List<String> order = new ArrayList<>();
            for (int i = 0; i < persistedOrder.length(); i++) {
                String id = persistedOrder.optString(i, "");
                if (contains(now, id) && !order.contains(id)) order.add(id);
            }
            for (Claimant c : now) if (!order.contains(c.id)) order.add(c.id);
            JSONArray savedOrder = new JSONArray(); for (String id : order) savedOrder.put(id);
            String orderString = savedOrder.toString();
            if (!orderString.equals(prefs.getString(ORDER_PREFIX + name, null))) { prefs.edit().putString(ORDER_PREFIX + name, orderString).apply(); changed = true; }
            JSONArray claimants = new JSONArray();
            for (String id : order) for (Claimant c : now) if (id.equals(c.id)) claimants.put(c.json());
            String chosen = prefs.getString(CHOICE_PREFIX + name, null);
            if (!contains(now, chosen)) {
                String fallback = order.isEmpty() ? null : order.get(0);
                if (chosen != null) Log.w(TAG, "Chosen D-Bus provider disappeared; falling back name=" + name + " removed=" + chosen + " fallback=" + fallback);
                if (fallback == null) prefs.edit().remove(CHOICE_PREFIX + name).apply();
                else prefs.edit().putString(CHOICE_PREFIX + name, fallback).apply();
                chosen = fallback;
                changed = true;
            }
            JSONObject row = new JSONObject(); row.put("name", name); row.put("selected", chosen); row.put("claimants", claimants); rows.put(row);
        }
        // Names with no remaining claimant are removed from exposed state and their stale selection is logged.
        for (String key : prefs.getAll().keySet()) if (key.startsWith(CHOICE_PREFIX)) {
            String name = key.substring(CHOICE_PREFIX.length());
            if (!available.containsKey(name)) {
                Log.i(TAG, "Removing settings for D-Bus name with no providers name=" + name);
                prefs.edit().remove(key).remove(ORDER_PREFIX + name).apply(); changed = true;
            }
        }
        if (changed) getContext().getContentResolver().notifyChange(URI, null);
        return rows.toString();
    }

    private static String componentId(ResolveInfo ri) {
        if (ri.serviceInfo == null) return "";
        return new android.content.ComponentName(ri.serviceInfo.packageName, ri.serviceInfo.name).flattenToString();
    }
    private static boolean contains(List<Claimant> list, String id) {
        if (id == null) return false;
        for (Claimant c : list) if (c.id.equals(id)) return true;
        return false;
    }
    private static boolean validWellKnownName(String name) {
        if (name.length() < 3 || name.length() > 255 || name.charAt(0) == ':' || name.startsWith(".") || name.endsWith(".")) return false;
        String[] parts = name.split("\\.", -1);
        if (parts.length < 2) return false;
        for (String part : parts) {
            if (part.isEmpty() || !asciiLetter(part.charAt(0)) && part.charAt(0) != '_') return false;
            for (int i = 1; i < part.length(); i++) {
                char c = part.charAt(i);
                if (!(asciiLetter(c) || c >= '0' && c <= '9' || c == '_' || c == '-')) return false;
            }
        }
        return true;
    }

    private static boolean asciiLetter(char c) { return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z'; }

    private static final class Claimant {
        final String id, packageName, label;
        Claimant(String id, String packageName, String label) { this.id = id; this.packageName = packageName; this.label = label; }
        JSONObject json() throws Exception { JSONObject row = new JSONObject(); row.put("id", id); row.put("package", packageName); row.put("label", label); return row; }
    }
}
