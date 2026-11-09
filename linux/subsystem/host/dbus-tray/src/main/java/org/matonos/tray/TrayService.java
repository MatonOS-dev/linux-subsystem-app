package org.matonos.tray;

import android.app.Service;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructCmsghdr;
import android.system.StructMsghdr;
import android.util.Log;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import org.matonos.dbus.DBusMessage;
import org.matonos.dbus.DBusVariant;
import org.matonos.linux.IDBusService;

/** StatusNotifierWatcher provider; each attach owns an independent private bus. */
public final class TrayService extends Service {
  static final String TAG = "MatonTray", ACTION_ITEMS = "org.matonos.tray.ITEMS_CHANGED";
  static final AtomicLong NEXT_ATTACH = new AtomicLong(1);
  static final Map<String, TrayItem> ITEMS = new ConcurrentHashMap<>();
  static final String WATCH_PATH = "/StatusNotifierWatcher",
      WATCH_IFACE = "org.kde.StatusNotifierWatcher";
  private final Map<Long, Bus> buses = new ConcurrentHashMap<>();

  static String key(long attach, String service) {
    return attach + "\n" + service;
  }

  static void changed(TrayService s) {
    s.sendBroadcast(new Intent(ACTION_ITEMS).setPackage(s.getPackageName()));
    TrayHostService.modelChanged();
  }

  @Override
  public IBinder onBind(Intent intent) {
    return new IDBusService.Stub() {
      @Override
      public void attach(ParcelFileDescriptor socket) {
        int uid = Binder.getCallingUid();
        long id = NEXT_ATTACH.getAndIncrement();
        Bus b = new Bus(id, uid, socket);
        buses.put(id, b);
        b.start();
      }
    };
  }

  @Override
  public void onDestroy() {
    for (Bus b : buses.values()) b.close();
    super.onDestroy();
  }

  final class Bus extends Thread {
    final long id;
    final int uid;
    final ParcelFileDescriptor socket;
    final FileDescriptor fd;
    final FileOutputStream output;
    final AtomicInteger nextSerial = new AtomicInteger(1);
    final Map<Integer, CompletableFuture<DBusMessage.Message>> pending = new ConcurrentHashMap<>();
    final Map<String, TrayItem> items = new ConcurrentHashMap<>();
    final List<TrayItem> itemOrder = new CopyOnWriteArrayList<>();
    final Set<String> hosts = ConcurrentHashMap.newKeySet();
    volatile boolean closed;

    Bus(long id, int uid, ParcelFileDescriptor socket) {
      this.id = id;
      this.uid = uid;
      this.socket = socket;
      fd = socket.getFileDescriptor();
      output = new FileOutputStream(fd);
      setName("dbus-tray-" + uid + "-" + id);
    }

    @Override
    public void run() {
      new Thread(
              () -> {
                try {
                  addMatch(
                      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
                } catch (Exception e) {
                  Log.e(
                      TAG,
                      "NameOwnerChanged AddMatch failed attach=" + id + " errno=" + errno(e),
                      e);
                }
              },
              "tray-owner-match-" + id)
          .start();
      try {
        while (!closed) {
          byte[] bytes = readPacket(fd);
          if (bytes == null) break;
          DBusMessage.Message m = DBusMessage.decode(bytes);
          if (m.type == 2 || m.type == 3) {
            CompletableFuture<DBusMessage.Message> f = pending.remove(m.replySerial);
            if (f != null) f.complete(m);
            else Log.e(TAG, "unexpected reply attach=" + id + " serial=" + m.replySerial);
          } else if (m.type == 1 || m.type == 4) handle(m);
          else Log.e(TAG, "unexpected D-Bus type attach=" + id + " type=" + m.type);
        }
      } catch (Exception e) {
        if (!closed)
          Log.e(TAG, "reader failed attach=" + id + " uid=" + uid + " errno=" + errno(e), e);
      } finally {
        close();
      }
    }

    DBusMessage.Message call(
        String dest, String path, String iface, String member, String sig, Object... args)
        throws Exception {
      DBusMessage.Message m = new DBusMessage.Message();
      m.type = 1;
      m.serial = nextSerial.getAndIncrement();
      m.destination = dest;
      m.path = path;
      m.iface = iface;
      m.member = member;
      m.signature = sig;
      Collections.addAll(m.body, args);
      CompletableFuture<DBusMessage.Message> f = new CompletableFuture<>();
      pending.put(m.serial, f);
      send(m);
      DBusMessage.Message r;
      try {
        r = f.get(15, TimeUnit.SECONDS);
      } catch (Exception e) {
        pending.remove(m.serial);
        throw new IOException(
            "D-Bus call failed attach="
                + id
                + " destination="
                + dest
                + " path="
                + path
                + " member="
                + member,
            e);
      }
      if (r.type == 3)
        throw new IOException(
            "D-Bus error="
                + r.error
                + " attach="
                + id
                + " service="
                + dest
                + " member="
                + member
                + " body="
                + r.body);
      return r;
    }

    void addMatch(String rule) throws Exception {
      call(
          "org.freedesktop.DBus",
          "/org/freedesktop/DBus",
          "org.freedesktop.DBus",
          "AddMatch",
          "s",
          rule);
    }

    void handle(DBusMessage.Message m) throws Exception {
      if (m.type == 4
          && "org.freedesktop.DBus".equals(m.iface)
          && "NameOwnerChanged".equals(m.member)
          && m.body.size() >= 3) {
        if (((String) m.body.get(2)).isEmpty()) removeOwner((String) m.body.get(0));
        return;
      }
      if (m.type == 4 && "org.kde.StatusNotifierItem".equals(m.iface) && isNew(m.member)) {
        refreshPath(m.path, m.sender);
        return;
      }
      if (m.type == 4
          && "com.canonical.dbusmenu".equals(m.iface)
          && ("LayoutUpdated".equals(m.member) || "ItemsPropertiesUpdated".equals(m.member))) {
        refreshMenu(m.path, m.sender);
        return;
      }
      if (m.type != 1) return;
      if (WATCH_PATH.equals(m.path) && WATCH_IFACE.equals(m.iface)) {
        watcher(m);
        return;
      }
      if ("org.freedesktop.DBus.Properties".equals(m.iface)
          && "GetAll".equals(m.member)
          && m.body.size() == 1) {
        String iface = (String) m.body.get(0);
        if (WATCH_IFACE.equals(iface)) {
          send(reply(m, "a{sv}", DBusMessage.dictionary(watcherProps())));
          return;
        }
      }
      if ("org.freedesktop.DBus.Properties".equals(m.iface)
          && "Get".equals(m.member)
          && m.body.size() == 2
          && WATCH_IFACE.equals(m.body.get(0))) {
        DBusVariant v = watcherProps().get((String) m.body.get(1));
        if (v == null) {
          send(error(m, "org.freedesktop.DBus.Error.UnknownProperty", "unknown property"));
          return;
        }
        send(reply(m, "v", v));
        return;
      }
      if ("org.kde.StatusNotifierItem".equals(m.iface)) {
        action(m);
        return;
      }
      if ("com.canonical.dbusmenu".equals(m.iface)) {
        menuMethod(m);
        return;
      }
      Log.e(
          TAG,
          "unhandled call attach="
              + id
              + " sender="
              + m.sender
              + " path="
              + m.path
              + " iface="
              + m.iface
              + " member="
              + m.member);
      send(error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unsupported watcher method"));
    }

    void watcher(DBusMessage.Message m) throws Exception {
      if ("RegisterStatusNotifierItem".equals(m.member)) {
        if (m.body.size() != 1 || !(m.body.get(0) instanceof String)) {
          send(error(m, "org.freedesktop.DBus.Error.InvalidArgs", "expected service string"));
          return;
        }
        String arg = (String) m.body.get(0),
            service = arg.startsWith("/") ? m.sender : arg,
            path = arg.startsWith("/") ? arg : "/StatusNotifierItem";
        if (arg.isEmpty()) {
          send(error(m, "org.freedesktop.DBus.Error.InvalidArgs", "empty service"));
          return;
        }
        if (service == null) {
          send(error(m, "org.freedesktop.DBus.Error.InvalidArgs", "missing sender"));
          return;
        }
        if (!items.containsKey(service)) {
          TrayItem i = new TrayItem(id, service, path, m.sender, this);
          items.put(service, i);
          itemOrder.add(i);
          ITEMS.put(key(id, service), i);
          new Thread(
                  () -> {
                    try {
                      addMatch(
                          "type='signal',sender='"
                              + m.sender
                              + "',path='"
                              + path
                              + "',interface='org.kde.StatusNotifierItem'");
                    } catch (Exception e) {
                      Log.e(
                          TAG,
                          "item AddMatch failed attach="
                              + id
                              + " service="
                              + service
                              + " path="
                              + path
                              + " errno="
                              + errno(e),
                          e);
                    }
                  },
                  "tray-item-match-" + id)
              .start();
          emit("StatusNotifierItemRegistered", "s", service + path);
          new Thread(() -> refresh(i), "tray-getall-" + id).start();
        }
        send(reply(m, "", null));
        changed(TrayService.this);
        return;
      }
      if ("RegisterStatusNotifierHost".equals(m.member)) {
        if (m.body.size() != 1 || !(m.body.get(0) instanceof String)) {
          send(error(m, "org.freedesktop.DBus.Error.InvalidArgs", "expected host service string"));
          return;
        }
        String host = (String) m.body.get(0);
        if (host.isEmpty()) {
          send(error(m, "org.freedesktop.DBus.Error.InvalidArgs", "empty host service"));
          return;
        }
        boolean first = hosts.add(host.startsWith("/") ? m.sender : host);
        send(reply(m, "", null));
        if (first) emit("StatusNotifierHostRegistered", "", null);
        return;
      }
      send(error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown watcher method"));
    }

    Map<String, DBusVariant> watcherProps() {
      Map<String, DBusVariant> p = new LinkedHashMap<>();
      List<Object> a = new ArrayList<>();
      for (TrayItem i : itemOrder) a.add(i.service + i.path);
      p.put("RegisteredStatusNotifierItems", new DBusVariant("as", a));
      p.put("IsStatusNotifierHostRegistered", new DBusVariant("b", true));
      p.put("ProtocolVersion", new DBusVariant("i", 0));
      return p;
    }

    boolean isNew(String s) {
      return Set.of(
              "NewTitle",
              "NewIcon",
              "NewAttentionIcon",
              "NewOverlayIcon",
              "NewToolTip",
              "NewStatus",
              "NewMenu")
          .contains(s);
    }

    void refreshPath(String path, String sender) {
      for (TrayItem i : itemOrder)
        if (path.equals(i.path) && (sender == null || sender.equals(i.owner)))
          new Thread(() -> refresh(i), "tray-refresh-" + id).start();
    }

    void refresh(TrayItem i) {
      try {
        DBusMessage.Message r =
            call(
                i.service,
                i.path,
                "org.freedesktop.DBus.Properties",
                "GetAll",
                "s",
                "org.kde.StatusNotifierItem");
        Map<String, DBusVariant> p = DBusMessage.dictionary(r.body.get(0));
        i.id = str(p, "Id");
        i.title = str(p, "Title");
        i.status = str(p, "Status");
        i.iconName = str(p, "IconName");
        i.toolTip = tooltip(p.get("ToolTip"));
        i.menuPath = str(p, "Menu");
        i.itemIsMenu = bool(p, "ItemIsMenu", false);
        i.icon = decodeIcon(p.get("IconPixmap"));
        if (!i.menuPath.isEmpty() && !"/".equals(i.menuPath)) {
          addMatch(
              "type='signal',sender='"
                  + i.owner
                  + "',path='"
                  + i.menuPath
                  + "',interface='com.canonical.dbusmenu'");
          refreshMenu(i.menuPath, i.owner);
        }
        changed(TrayService.this);
      } catch (Exception e) {
        Log.e(
            TAG,
            "GetAll failed attach="
                + id
                + " uid="
                + uid
                + " service="
                + i.service
                + " path="
                + i.path
                + " errno="
                + errno(e),
            e);
      }
    }

    String str(Map<String, DBusVariant> p, String k) {
      DBusVariant v = p.get(k);
      return v != null && v.value instanceof String ? (String) v.value : "";
    }

    boolean bool(Map<String, DBusVariant> p, String k, boolean d) {
      DBusVariant v = p.get(k);
      return v != null && v.value instanceof Boolean ? (Boolean) v.value : d;
    }

    String tooltip(DBusVariant v) {
      if (v != null && v.value instanceof List) {
        List<?> values = (List<?>) v.value;
        if (values.size() > 3 && values.get(3) instanceof String) return (String) values.get(3);
      }
      return "";
    }

    Bitmap decodeIcon(DBusVariant v) {
      try {
        if (v == null || !(v.value instanceof List) || ((List<?>) v.value).isEmpty()) return null;
        List<?> p = (List<?>) ((List<?>) v.value).get(0);
        int w = ((Number) p.get(0)).intValue(), h = ((Number) p.get(1)).intValue();
        List<?> a = (List<?>) p.get(2);
        if (w <= 0 || h <= 0 || (long) w * h * 4 > a.size())
          throw new IllegalArgumentException("invalid ARGB32 pixmap dimensions");
        int[] c = new int[w * h];
        for (int n = 0; n < c.length; n++)
          c[n] =
              (u8(a.get(n * 4)) << 24)
                  | (u8(a.get(n * 4 + 1)) << 16)
                  | (u8(a.get(n * 4 + 2)) << 8)
                  | u8(a.get(n * 4 + 3));
        return Bitmap.createBitmap(c, w, h, Bitmap.Config.ARGB_8888);
      } catch (Exception e) {
        Log.e(TAG, "IconPixmap parse failed attach=" + id + " errno=" + errno(e), e);
        return null;
      }
    }

    int u8(Object x) {
      return ((Number) x).intValue() & 255;
    }

    void refreshMenu(String path, String sender) {
      for (TrayItem i : itemOrder)
        if (path.equals(i.menuPath) && (sender == null || sender.equals(i.owner)))
          new Thread(() -> loadMenu(i), "tray-menu-" + id).start();
    }

    void loadMenu(TrayItem i) {
      try {
        DBusMessage.Message r =
            call(
                i.service,
                i.menuPath,
                "com.canonical.dbusmenu",
                "GetLayout",
                "iias",
                0,
                -1,
                Collections.emptyList());
        if (r.body.size() < 2) throw new IOException("GetLayout returned no layout");
        i.menu = parseLayout(r.body.get(1));
        changed(TrayService.this);
      } catch (Exception e) {
        Log.e(
            TAG,
            "GetLayout failed attach="
                + id
                + " service="
                + i.service
                + " path="
                + i.menuPath
                + " errno="
                + errno(e),
            e);
      }
    }

    List<TrayItem.MenuEntry> parseLayout(Object v) {
      List<?> root = (List<?>) v;
      if (root.size() < 3) throw new IllegalArgumentException("invalid dbusmenu layout");
      return parseChildren(root.get(2));
    }

    List<TrayItem.MenuEntry> parseChildren(Object v) {
      List<TrayItem.MenuEntry> out = new ArrayList<>();
      for (Object child : (List<?>) v) {
        Object x = child instanceof DBusVariant ? ((DBusVariant) child).value : child;
        if (!(x instanceof List)) continue;
        List<?> node = (List<?>) x;
        if (node.size() < 3) continue;
        int id = ((Number) node.get(0)).intValue();
        Map<String, DBusVariant> props = DBusMessage.dictionary(node.get(1));
        String type = propString(props.get("type"));
        out.add(
            new TrayItem.MenuEntry(
                id,
                propString(props.get("label")),
                propBool(props.get("enabled"), true),
                propBool(props.get("visible"), true),
                "separator".equals(type),
                propString(props.get("toggle-type")),
                propInt(props.get("toggle-state"), 0),
                decodeMenuIcon(props.get("icon-data")),
                parseChildren(node.get(2))));
      }
      return out;
    }

    String propString(DBusVariant v) {
      return v != null && v.value instanceof String ? (String) v.value : "";
    }

    boolean propBool(DBusVariant v, boolean d) {
      return v != null && v.value instanceof Boolean ? (Boolean) v.value : d;
    }

    int propInt(DBusVariant v, int d) {
      return v != null && v.value instanceof Number ? ((Number) v.value).intValue() : d;
    }

    Bitmap decodeMenuIcon(DBusVariant v) {
      try {
        if (v == null || !(v.value instanceof List)) return null;
        List<?> bytes = (List<?>) v.value;
        byte[] data = new byte[bytes.size()];
        for (int n = 0; n < data.length; n++) data[n] = (byte) u8(bytes.get(n));
        return BitmapFactory.decodeByteArray(data, 0, data.length);
      } catch (Exception e) {
        Log.e(TAG, "DBusMenu icon-data parse failed attach=" + id + " errno=" + errno(e), e);
        return null;
      }
    }

    void action(DBusMessage.Message m) throws Exception {
      Log.e(
          TAG,
          "unexpected client call to item path="
              + m.path
              + " sender="
              + m.sender
              + " member="
              + m.member);
      send(
          error(
              m,
              "org.freedesktop.DBus.Error.UnknownMethod",
              "item methods are called by the tray host"));
    }

    void invokeItem(TrayItem i, String member, int x, int y) throws Exception {
      call(i.service, i.path, "org.kde.StatusNotifierItem", member, "ii", x, y);
    }

    void invokeScroll(TrayItem i, int delta, String orientation) throws Exception {
      call(
          i.service,
          i.path,
          "org.kde.StatusNotifierItem",
          "Scroll",
          "is",
          delta,
          orientation == null ? "vertical" : orientation);
    }

    void menuEvent(TrayItem i, int menuId) throws Exception {
      call(
          i.service,
          i.menuPath,
          "com.canonical.dbusmenu",
          "Event",
          "isvu",
          menuId,
          "clicked",
          new DBusVariant("s", ""),
          0);
    }

    boolean menuAbout(TrayItem i, int menuId) throws Exception {
      DBusMessage.Message r =
          call(i.service, i.menuPath, "com.canonical.dbusmenu", "AboutToShow", "i", menuId);
      boolean updated = !r.body.isEmpty() && Boolean.TRUE.equals(r.body.get(0));
      if (updated) loadMenu(i);
      return updated;
    }

    TrayItem find(String path, String dest) {
      for (TrayItem i : items.values())
        if (i.path.equals(path) && (dest == null || dest.equals(i.service))) return i;
      return null;
    }

    void menuMethod(DBusMessage.Message m) throws Exception {
      TrayItem i = find(m.path, m.destination);
      if (i == null) {
        send(error(m, "org.freedesktop.DBus.Error.UnknownObject", "unknown menu"));
        return;
      }
      if ("Event".equals(m.member) && m.body.size() == 4) {
        call(
            i.service,
            i.menuPath,
            "com.canonical.dbusmenu",
            "Event",
            "isvu",
            m.body.get(0),
            m.body.get(1),
            m.body.get(2),
            m.body.get(3));
        send(reply(m, "", null));
        return;
      }
      if ("AboutToShow".equals(m.member) && m.body.size() == 1) {
        DBusMessage.Message r =
            call(
                i.service, i.menuPath, "com.canonical.dbusmenu", "AboutToShow", "i", m.body.get(0));
        send(reply(m, "b", r.body.isEmpty() ? false : r.body.get(0)));
        return;
      }
      send(error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unsupported dbusmenu method"));
    }

    void removeOwner(String owner) {
      boolean changed = false;
      for (TrayItem i : new ArrayList<>(items.values()))
        if (owner.equals(i.service) || owner.equals(i.owner)) {
          items.remove(i.service);
          itemOrder.remove(i);
          ITEMS.remove(key(id, i.service));
          emit("StatusNotifierItemUnregistered", "s", i.service + i.path);
          changed = true;
        }
      hosts.remove(owner);
      if (changed) changed(TrayService.this);
    }

    void emit(String member, String sig, Object arg) {
      try {
        DBusMessage.Message m = new DBusMessage.Message();
        m.type = 4;
        m.serial = nextSerial.getAndIncrement();
        m.path = WATCH_PATH;
        m.iface = WATCH_IFACE;
        m.member = member;
        m.signature = sig;
        if (arg != null) m.body.add(arg);
        send(m);
      } catch (Exception e) {
        Log.e(TAG, "signal failed attach=" + id + " member=" + member + " errno=" + errno(e), e);
      }
    }

    DBusMessage.Message reply(DBusMessage.Message req, String sig, Object val) {
      DBusMessage.Message r = new DBusMessage.Message();
      r.type = 2;
      r.replySerial = req.serial;
      r.signature = sig;
      if (val != null) r.body.add(val);
      return r;
    }

    DBusMessage.Message error(DBusMessage.Message req, String name, String text) {
      DBusMessage.Message r = reply(req, "s", text);
      r.type = 3;
      r.error = name;
      return r;
    }

    void send(DBusMessage.Message m) throws Exception {
      synchronized (output) {
        output.write(DBusMessage.encode(m));
        output.flush();
      }
    }

    void close() {
      if (closed) return;
      closed = true;
      for (TrayItem i : new ArrayList<>(items.values())) {
        ITEMS.remove(key(id, i.service));
        emit("StatusNotifierItemUnregistered", "s", i.service + i.path);
      }
      items.clear();
      itemOrder.clear();
      for (CompletableFuture<DBusMessage.Message> f : pending.values())
        f.completeExceptionally(new IOException("bus closed attach=" + id));
      pending.clear();
      try {
        socket.close();
      } catch (IOException e) {
        Log.e(TAG, "socket close failed attach=" + id + " fd=" + fd + " errno=" + errno(e), e);
      }
      buses.remove(id);
      changed(TrayService.this);
    }
  }

  static int errno(Exception e) {
    return e instanceof ErrnoException ? ((ErrnoException) e).errno : 0;
  }

  static byte[] readPacket(FileDescriptor fd) throws Exception {
    ByteBuffer probe = ByteBuffer.allocateDirect(1);
    StructMsghdr peek = new StructMsghdr(null, new ByteBuffer[] {probe}, new StructCmsghdr[0], 0);
    int size = Os.recvmsg(fd, peek, OsConstants.MSG_PEEK | OsConstants.MSG_TRUNC);
    if (size == 0) return null;
    if (size < 0) throw new IOException("recvmsg negative size");
    ByteBuffer packet = ByteBuffer.allocateDirect(size);
    StructMsghdr read = new StructMsghdr(null, new ByteBuffer[] {packet}, null, 0);
    int n = Os.recvmsg(fd, read, 0);
    if (n != size) throw new IOException("seqpacket size changed peek=" + size + " read=" + n);
    byte[] b = new byte[n];
    packet.position(0);
    packet.get(b);
    if (read.msg_control != null && read.msg_control.length != 0)
      throw new IOException("unexpected ancillary fds");
    if ((read.msg_flags & OsConstants.MSG_CTRUNC) != 0)
      throw new IOException("ancillary data truncated");
    return b;
  }
}
