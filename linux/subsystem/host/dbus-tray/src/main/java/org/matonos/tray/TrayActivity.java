package org.matonos.tray;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/** Plain debug listing for StatusNotifierItems and their D-Bus menus. */
public final class TrayActivity extends Activity {
    private final Handler main=new Handler(Looper.getMainLooper());
    private LinearLayout rows;
    private final BroadcastReceiver updates=new BroadcastReceiver(){@Override public void onReceive(Context c,Intent i){render();}};
    @Override protected void onCreate(Bundle state){super.onCreate(state);ScrollView scroll=new ScrollView(this);rows=new LinearLayout(this);rows.setOrientation(LinearLayout.VERTICAL);rows.setPadding(12,12,12,12);scroll.addView(rows);setContentView(scroll);registerReceiver(updates,new IntentFilter(TrayService.ACTION_ITEMS),Context.RECEIVER_NOT_EXPORTED);render();}
    @Override protected void onDestroy(){unregisterReceiver(updates);super.onDestroy();}
    private void render(){if(rows==null)return;rows.removeAllViews();List<TrayItem> items=new ArrayList<>(TrayService.ITEMS.values());if(items.isEmpty()){TextView empty=new TextView(this);empty.setText("No StatusNotifierItems registered");empty.setPadding(16,16,16,16);rows.addView(empty);return;}for(TrayItem item:items){LinearLayout row=new LinearLayout(this);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(8,8,8,8);ImageView icon=new ImageView(this);if(item.icon!=null)icon.setImageBitmap(item.icon);else { icon.setContentDescription(item.iconName); icon.setScaleType(ImageView.ScaleType.CENTER); }row.addView(icon,new LinearLayout.LayoutParams(48,48));if(item.icon==null&&!item.iconName.isEmpty()){TextView iconName=new TextView(this);iconName.setText(item.iconName);iconName.setTextSize(10);row.addView(iconName);}TextView text=new TextView(this);text.setTypeface(null,Typeface.BOLD);text.setText(item.title.isEmpty()?item.id:item.title);text.setPadding(12,0,0,0);row.addView(text,new LinearLayout.LayoutParams(0,LinearLayout.LayoutParams.WRAP_CONTENT,1));TextView status=new TextView(this);status.setText(item.status);row.addView(status);row.setOnClickListener(v->showMenu(item));rows.addView(row);View divider=new View(this);divider.setBackgroundColor(0xff888888);rows.addView(divider,new LinearLayout.LayoutParams(-1,1));}}
    private void showMenu(TrayItem item){new Thread(()->{try{if(item.bus!=null&&!item.menuPath.isEmpty())item.bus.menuAbout(item,0);}catch(Exception e){Log.e(TrayService.TAG,"AboutToShow failed service="+item.service+" path="+item.menuPath,e);}main.post(()->displayMenu(item));},"tray-about-to-show").start();}
    private void displayMenu(TrayItem item){List<TrayItem.MenuEntry> entries=new ArrayList<>();flatten(item.menu,0,entries);List<TrayItem.MenuEntry> selectable=new ArrayList<>();List<String> labels=new ArrayList<>();for(TrayItem.MenuEntry e:entries){if(!e.visible)continue;labels.add((e.separator?"────────": "  ".repeat(e.depth)+e.label));selectable.add(e);}if(selectable.isEmpty()){labels.add("No menu entries");new AlertDialog.Builder(this).setTitle(item.title).setItems(labels.toArray(new String[0]),null).setPositiveButton("Close",null).show();return;}new AlertDialog.Builder(this).setTitle(item.title).setItems(labels.toArray(new String[0]),(d,which)->{TrayItem.MenuEntry e=selectable.get(which);if(!e.enabled||e.separator||!e.children.isEmpty())return;new Thread(()->{try{if(item.bus!=null)item.bus.menuEvent(item,e.id);}catch(Exception ex){Log.e(TrayService.TAG,"menu Event failed service="+item.service+" id="+e.id,ex);}},"tray-menu-click").start();}).setNegativeButton("Close",null).show();}
    private void flatten(List<TrayItem.MenuEntry> in,int depth,List<TrayItem.MenuEntry> out){for(TrayItem.MenuEntry e:in){out.add(new TrayItem.MenuEntry(e.id,e.label,e.enabled,e.visible,e.separator,e.children,depth));flatten(e.children,depth+1,out);}}
}
