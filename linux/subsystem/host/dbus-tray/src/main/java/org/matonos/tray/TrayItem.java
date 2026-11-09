package org.matonos.tray;

import android.graphics.Bitmap;

import java.util.ArrayList;
import java.util.List;

/** Mutable snapshot of one StatusNotifierItem on one attached private bus. */
public final class TrayItem {
    public final long attachId;
    public final String service;
    public final String path;
    public final String owner;
    public volatile String id = "", title = "", status = "Active", iconName = "", toolTip = "", menuPath = "";
    public volatile Bitmap icon;
    public volatile List<MenuEntry> menu = new ArrayList<>();
    TrayService.Bus bus;
    TrayItem(long attachId, String service, String path, String owner, TrayService.Bus bus) {
        this.attachId = attachId; this.service = service; this.path = path; this.owner = owner; this.bus = bus;
    }
    public void activate(int x,int y){invoke("Activate",x,y);}
    public void secondaryActivate(int x,int y){invoke("SecondaryActivate",x,y);}
    public void contextMenu(int x,int y){invoke("ContextMenu",x,y);}
    public void scroll(int delta,String orientation){new Thread(()->{try{if(bus!=null)bus.invokeScroll(this,delta,orientation);}catch(Exception e){android.util.Log.e(TrayService.TAG,"Scroll failed service="+service+" path="+path,e);}},"tray-scroll").start();}
    private void invoke(String method,int x,int y){new Thread(()->{try{if(bus!=null)bus.invokeItem(this,method,x,y);}catch(Exception e){android.util.Log.e(TrayService.TAG,method+" failed service="+service+" path="+path,e);}},"tray-action").start();}
    public static final class MenuEntry {
        public final int id, depth;
        public final String label;
        public final boolean enabled, visible, separator;
        public final List<MenuEntry> children;
        MenuEntry(int id, String label, boolean enabled, boolean visible, boolean separator, List<MenuEntry> children) { this(id,label,enabled,visible,separator,children,0); }
        MenuEntry(int id, String label, boolean enabled, boolean visible, boolean separator, List<MenuEntry> children,int depth) {
            this.id=id; this.label=label; this.enabled=enabled; this.visible=visible; this.separator=separator; this.children=children; this.depth=depth;
        }
    }
}
