package org.matonos.linux;

import android.app.Activity;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import org.matonos.linux.runtime.NativeCompositor;

/** Standalone test window: its compositor belongs to this activity's app. */
public final class WindowActivity extends Activity implements SurfaceHolder.Callback {
    static final String EXTRA_START_DEMO = "start_demo";
    private int id=1;
    private SurfaceView view;
    private boolean ready, attached, demoLaunched;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        view=new SurfaceView(this);view.setFocusableInTouchMode(true);
        view.getHolder().addCallback(this);setContentView(view);
        ready=NativeCompositor.nativeStart("wayland-0",new java.io.File(getFilesDir(),"demo-runtime").getAbsolutePath(),this);
        if(!ready)finish();
    }
    @SuppressWarnings("unused") private void onNativeToplevel(int session,int window,int width,int height) {
        runOnUiThread(() -> {if(attached)NativeCompositor.nativeDetach(id);id=window;attached=false;attach();});
    }
    @SuppressWarnings("unused") private void onNativeToplevelClosed(int window) {
        if(window==id)runOnUiThread(this::finish);
    }
    private void attach() {
        if(!ready||attached||!view.getHolder().getSurface().isValid())return;
        NativeCompositor.nativeAttach(id,view.getHolder().getSurface(),view.getWidth(),view.getHeight());
        attached=true;view.requestFocus();
        if(!demoLaunched&&getIntent().getBooleanExtra(EXTRA_START_DEMO,false)) {
            demoLaunched=true;NativeCompositor.nativeLaunchDemo();
        }
    }
    @Override public void surfaceCreated(SurfaceHolder holder) {attach();}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) {
        attach();if(attached)NativeCompositor.nativeResize(id,width,height);
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if(attached)NativeCompositor.nativeDetach(id);attached=false;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if(ready)NativeCompositor.nativeKey(id,event.getKeyCode(),event.getScanCode(),event.getAction(),event.getMetaState(),event.getEventTime()*1000000L);
        return true;
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if(ready)NativeCompositor.nativeMotion(id,event.getX(),event.getY(),event.getAxisValue(MotionEvent.AXIS_VSCROLL),event.getAxisValue(MotionEvent.AXIS_HSCROLL),event.getActionMasked(),PointerInput.buttons(event.getActionMasked(),event.getButtonState(),event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)),event.getEventTime()*1000000L);
        return true;
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {return onTouchEvent(event);}
    @Override protected void onDestroy() {
        if(ready)NativeCompositor.nativeStop();ready=false;super.onDestroy();
    }
}
