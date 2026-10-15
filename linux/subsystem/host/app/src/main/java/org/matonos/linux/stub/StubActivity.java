package org.matonos.linux.stub;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;
import org.json.JSONObject;

/** Shared activity code runs in the generated app's own package and task. */
public final class StubActivity extends Activity implements SurfaceHolder.Callback, org.matonos.linux.PerAppRuntime.WindowHost {
    private volatile boolean foreground;
    private static final String WINDOW = "org.matonos.linuxhost.WINDOW_ID";
    /** After the last window closes, the session lives this long for a new
     * one: an app answering a close with a save dialog, switching modes
     * (Steam Big Picture) or going from splash to main window. Android lets
     * an app start activities shortly after its own finished. */
    private static final long LAST_WINDOW_GRACE_MS = 10_000;
    private static final android.os.Handler MAIN = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final Runnable END_SESSION = () -> {
        // killProcess bypasses Service.onDestroy: explicitly close native
        // runtime children before ending the per-app host.
        org.matonos.linux.runtime.NativeCompositor.nativeStop();
        android.os.Process.killProcess(android.os.Process.myPid());
    };

    /** Open an Android window for an app window that arrived while no
     * activity was hosting (e.g. after the app closed its last window). */
    public static void openWindow(android.content.Context context, int id, int width, int height) {
        context.startActivity(windowIntent(context, id), launchBounds(context, width, height));
    }
    private static Intent windowIntent(android.content.Context context, int id) {
        return new Intent().setClassName(context, StubActivity.class.getName())
                .putExtra(WINDOW, id)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
    }
    /** Freeform windows open at the size the app asked for, centred and
     * clamped to the display. Later resizes stay with Android and the user. */
    private static Bundle launchBounds(android.content.Context context, int width, int height) {
        if (width <= 0 || height <= 0) return null;
        android.graphics.Rect display = context.getSystemService(android.view.WindowManager.class)
                .getMaximumWindowMetrics().getBounds();
        int w = Math.min(width, display.width()), h = Math.min(height, display.height());
        int left = display.left + (display.width() - w) / 2, top = display.top + (display.height() - h) / 2;
        return android.app.ActivityOptions.makeBasic()
                .setLaunchBounds(new android.graphics.Rect(left, top, left + w, top + h)).toBundle();
    }

    /* Window id -> the activity showing it, for window-state requests. */
    private static final java.util.Map<Integer, StubActivity> SHOWING = new java.util.concurrent.ConcurrentHashMap<>();

    /** The app asked for a window state (MATON_STATE_* in compositor_core.h). */
    public static void windowState(int id, int state) {
        StubActivity activity = SHOWING.get(id);
        if (activity != null) activity.runOnUiThread(() -> activity.applyState(state));
    }
    private static final int STATE_FULLSCREEN_ON = 1, STATE_FULLSCREEN_OFF = 2, STATE_MINIMIZE = 3;
    private void applyState(int state) {
        if (destroyed) return;
        if (state >= 16 && state <= 31) { beginGeometry(state - 16); return; }
        if (state == 4 || state == 5) {
            if (view != null) { if (state == 4 && hasWindowFocus()) view.requestPointerCapture(); else view.releasePointerCapture(); }
            if (state == 4 && !hasWindowFocus()) org.matonos.linux.runtime.NativeCompositor.nativeCaptureLost(window);
            return;
        }
        if (state == 6 || state == 7) { clientDecorated = state == 6; applyCaption(); return; }
        if (state == STATE_MINIMIZE) { moveTaskToBack(true); return; }
        boolean on = state == STATE_FULLSCREEN_ON;
        android.view.WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            if (on) {
                bars.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                bars.hide(android.view.WindowInsets.Type.systemBars());
            } else bars.show(android.view.WindowInsets.Type.systemBars());
        }
        if (!isInMultiWindowMode()) { confirmFullscreen(on); return; }
        // Desktop windowing: ask Android to make this freeform window fullscreen
        // (or restore it); tell the app only what actually happened.
        requestFullscreenMode(on ? FULLSCREEN_MODE_REQUEST_ENTER : FULLSCREEN_MODE_REQUEST_EXIT,
                new android.os.OutcomeReceiver<Void, Throwable>() {
                    @Override public void onResult(Void result) { confirmFullscreen(on); }
                    @Override public void onError(Throwable error) {
                        // The state did not change: a failed enter stays windowed.
                        if (on && bars != null) bars.show(android.view.WindowInsets.Type.systemBars());
                        confirmFullscreen(!on);
                    }
                });
    }
    private void confirmFullscreen(boolean on) {
        if (runtime != null && window != 0) runtime.setFullscreen(window, on);
    }
    private static final WindowLifetime WINDOWS = new WindowLifetime();
    private android.os.PowerManager.WakeLock portalWake;
    private boolean stopped;
    private String ref;
    private volatile int window;
    private boolean attached;
    private volatile boolean destroyed;
    private TextView status;
    // Captured mice expose deltas as X/Y on Android; some sources also
    // provide explicit RELATIVE axes. Captured touchpads use RELATIVE axes.
    private static float capturedDelta(MotionEvent event, int relative, int position) {
        float delta = event.getAxisValue(relative);
        return delta == 0 && event.isFromSource(android.view.InputDevice.SOURCE_MOUSE_RELATIVE)
                ? event.getAxisValue(position) : delta;
    }
    private LinuxWindowClient linuxWindow;
    private boolean geometryActive, geometryFramePending, pointerDown;
    private float pointerX, pointerY, geometryX, geometryY, geometryDx, geometryDy;
    private long pointerTime;
    private void beginGeometry(int edges) {
        if (destroyed || geometryActive || view == null || linuxWindow == null
                || !hasWindowFocus() || !pointerDown || view.hasPointerCapture()
                || android.os.SystemClock.uptimeMillis() - pointerTime > 1000) return;
        if (!linuxWindow.begin(edges)) return;
        geometryActive=true; geometryX=pointerX; geometryY=pointerY; geometryDx=geometryDy=0;
        // Clear the Linux seat's button/focus; subsequent motion belongs to Android.
        org.matonos.linux.runtime.NativeCompositor.nativeCaptureLost(window);
        view.requestPointerCapture();
    }
    private void geometryFrame() {
        if (geometryFramePending || view == null) return;
        geometryFramePending=true;
        view.postOnAnimation(() -> {
            geometryFramePending=false;
            if (geometryActive && !linuxWindow.update(geometryDx, geometryDy)) endGeometry();
        });
    }
    private void endGeometry() {
        if (!geometryActive) return;
        geometryActive=false;
        if (linuxWindow != null) { linuxWindow.update(geometryDx, geometryDy); linuxWindow.end(); }
        if (view != null && view.hasPointerCapture()) view.releasePointerCapture();
        pointerDown=false;
    }
    private boolean geometryMotion(MotionEvent event, boolean relative) {
        if (!geometryActive) return false;
        if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) { endGeometry(); return true; }
        if (relative) {
            geometryDx+=capturedDelta(event, MotionEvent.AXIS_RELATIVE_X, MotionEvent.AXIS_X);
            geometryDy+=capturedDelta(event, MotionEvent.AXIS_RELATIVE_Y, MotionEvent.AXIS_Y);
        } else {
            geometryDx=event.getRawX()-geometryX; geometryDy=event.getRawY()-geometryY;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_CANCEL
                || event.getActionMasked() == MotionEvent.ACTION_UP
                || (event.getButtonState() & MotionEvent.BUTTON_PRIMARY) == 0) endGeometry();
        else geometryFrame();
        return true;
    }
    private boolean clientDecorated;
    private void applyCaption() {
        android.view.WindowInsetsController bars = getWindow().getInsetsController();
        boolean custom = clientDecorated && isInMultiWindowMode();
        if (bars != null) bars.setSystemBarsAppearance(custom ? android.view.WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND : 0,
                android.view.WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND);
        getWindow().setDecorFitsSystemWindows(!custom);
    }
    @Override public void onMultiWindowModeChanged(boolean multi, android.content.res.Configuration config) {
        super.onMultiWindowModeChanged(multi, config); applyCaption();
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) endGeometry();
        if (!focus && window != 0) org.matonos.linux.runtime.NativeCompositor.nativeCaptureLost(window);
    }
    private SurfaceView view;
    private Surface surface;
    private volatile org.matonos.linux.PerAppRuntime runtime;
    @Override public boolean openUri(Intent intent) {
        java.util.concurrent.FutureTask<Boolean> task = new java.util.concurrent.FutureTask<>(() -> {
            if (destroyed || isFinishing()) return false;
            try { startActivity(intent); return true; }
            catch (android.content.ActivityNotFoundException | SecurityException error) { return false; }
        });
        runOnUiThread(task);
        try { return task.get(4, java.util.concurrent.TimeUnit.SECONDS); }
        catch (Exception error) { task.cancel(false); return false; }
    }
    @Override public void inhibitChanged(boolean active) {
        runOnUiThread(() -> { if (!destroyed) setInhibited(active); });
    }
    // Per-app compositor callbacks (in-process).
    @Override public void windowOpened(int id, int width, int height) {
        runOnUiThread(() -> {
            if (destroyed || window == id) return;
            // The launcher opened this activity before the app's size was
            // known, and an app cannot resize its own task: reopen the first
            // window at the requested size like any later one.
            Bundle bounds = window == 0 && isInMultiWindowMode() ? launchBounds(this, width, height) : null;
            if (bounds != null) { startActivity(windowIntent(this, id), bounds); finish(); }
            else if (window == 0) showWindow(id);
            // The runtime has exactly one window host at a time, so whichever
            // activity holds it opens further windows. The root activity may
            // already be gone (its window closed, e.g. an updater window).
            else startActivity(windowIntent(this, id), launchBounds(this, width, height));
        });
    }
    @Override public void windowClosed(int id) {
        runOnUiThread(() -> { if (!destroyed && window == id) finish(); });
    }
    private void launchLocally() {
        new Thread(() -> {
            try {
                runtime = StubService.runtime();
                for (int i=0; i<50 && runtime==null && !destroyed; i++) {
                    Thread.sleep(100); runtime=StubService.runtime();
                }
                if (destroyed) return;
                if (runtime==null) throw new IllegalStateException("The app runtime did not become ready");
                runtime.setWindowHost(this, window);
                runOnUiThread(() -> {
                    if (!destroyed && window!=0) { if(view==null)showWindow(window);else attachIfReady(); }
                });
                JSONObject result=V3Launcher.launch(this, ref);
                if (!result.optBoolean("ok")) { message(result.optString("error","Application could not start"));return; }
                message("Waiting for the application window…");
                int seconds=0;
                while (!destroyed) {
                    Thread.sleep(1000);
                    if (!V3Launcher.getLaunchStatus().optBoolean("ok")) {
                        runOnUiThread(() -> { if(!destroyed)finish(); }); break;
                    }
                    if (++seconds==30 && window==0) message("Application is running, but no window has appeared.");
                }
            } catch(Exception error) { failure("Cannot start application",error); }
        }, "flatpak-stub-launch").start();
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        WINDOWS.opened(this);
        MAIN.removeCallbacks(END_SESSION);
        status=new TextView(this);status.setPadding(32,32,32,32);
        status.setText("Starting application…");setContentView(status);
        window=state!=null?state.getInt(WINDOW,0):getIntent().getIntExtra(WINDOW,0);
        int minimum=1;
        try {
            ActivityInfo info=getPackageManager().getActivityInfo(getComponentName(),PackageManager.GET_META_DATA);
            if(info.metaData!=null){ref=info.metaData.getString(HostContract.META_FLATPAK_REF);minimum=info.metaData.getInt(HostContract.META_MIN_INTERFACE,1);}
        } catch(Exception e){failure("Cannot read application reference",e);return;}
        if(ref==null||ref.trim().isEmpty()){message("This launcher has no application reference.");return;}
        linuxWindow = new LinuxWindowClient(this, ref, () -> { endGeometry(); applyCaption(); });
        if(minimum>HostContract.getInterfaceVersion()){message("Update the Linux host to launch this application.");return;}
        // Start the per-app runtime (session bus + portal) in THIS process/UID.
        startRuntimeAndConnect();
    }
    private void startRuntimeAndConnect() {
        try {
            startForegroundService(new Intent(this, StubService.class).putExtra(HostContract.EXTRA_FLATPAK_REF, ref));
            launchLocally();
        } catch(Exception error) { failure("Cannot start the app runtime",error); }
    }
    // Runs on the activity thread, in the generated stub package's UID.
    private void setInhibited(boolean active) {
        if(active) {
            if(portalWake==null) {
                portalWake=getSystemService(android.os.PowerManager.class).newWakeLock(
                        android.os.PowerManager.PARTIAL_WAKE_LOCK,"MatonOS:JavaPortalInhibit");
                portalWake.setReferenceCounted(false);
            }
            // Generated stubs request WAKE_LOCK; an older stub without it keeps
            // only the screen-on flag instead of crashing.
            try {if(!portalWake.isHeld())portalWake.acquire();}
            catch(SecurityException e){android.util.Log.w("MatonStub","WAKE_LOCK not granted",e);}
            getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            if(portalWake!=null && portalWake.isHeld())portalWake.release();
            getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }
    private void showWindow(int id) {
        window=id;SHOWING.put(id,this);
        view=new SurfaceView(this) {
            @Override public void onPointerCaptureChange(boolean captured) {
                super.onPointerCaptureChange(captured);
                if (!captured) endGeometry();
                if (!captured) org.matonos.linux.runtime.NativeCompositor.nativeCaptureLost(window);
            }
            @Override public boolean onCapturedPointerEvent(MotionEvent event) {
                if (geometryMotion(event, true)) return true;
                org.matonos.linux.runtime.NativeCompositor.nativeRelative(window,
                        capturedDelta(event, MotionEvent.AXIS_RELATIVE_X, MotionEvent.AXIS_X), capturedDelta(event, MotionEvent.AXIS_RELATIVE_Y, MotionEvent.AXIS_Y),
                        event.getAxisValue(MotionEvent.AXIS_VSCROLL), event.getAxisValue(MotionEvent.AXIS_HSCROLL),
                        event.getButtonState(), event.getEventTime()*1000000L);
                return true;
            }
        };view.getHolder().addCallback(this);
        view.setFocusableInTouchMode(true);
        view.setOnTouchListener((v,event)->motion(event));
        view.setOnGenericMotionListener((v,event)->motion(event));
        setContentView(view);view.requestFocus();
    }
    private void message(String text) {
        runOnUiThread(()->{if(!destroyed&&window==0)status.setText(text);});
    }
    private void failure(String operation,Exception error) {
        android.util.Log.e("MatonFlatpakStub",operation+" for "+ref,error);
        String detail=error.getMessage()!=null?error.getMessage():error.getClass().getSimpleName();
        runOnUiThread(()->{
            if(destroyed)return;
            status.setText(operation+": "+detail);
            if(view!=null)setContentView(status);
        });
    }
    @Override protected void onStart() {
        super.onStart(); stopped=false; reportStopped();
    }
    @Override protected void onStop() {
        stopped=true; reportStopped(); super.onStop();
    }
    private void reportStopped() {
        if(runtime!=null && window!=0)try{runtime.stopped(window,stopped);}catch(Exception ignored){}
    }
    private void attachIfReady() {
        if(attached||runtime==null||surface==null||!surface.isValid()||window==0)return;
        try{runtime.attach(window,surface,view.getWidth(),view.getHeight());attached=true;reportStopped();}
        catch(Exception e){failure("Cannot display application window",e);}
    }
    public void surfaceCreated(SurfaceHolder holder){surface=holder.getSurface();attachIfReady();}
    public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){
        surface=holder.getSurface();attachIfReady();
        try{if(attached&&runtime!=null)runtime.resize(window,width,height);}
        catch(Exception e){failure("Cannot resize application window",e);}
    }
    public void surfaceDestroyed(SurfaceHolder holder){
        try{if(attached&&runtime!=null)runtime.detach(window);}catch(Exception ignored){}
        attached=false;surface=null;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(runtime==null||window==0)return super.dispatchKeyEvent(event);
        try{runtime.key(window,event.getKeyCode(),event.getScanCode(),event.getAction(),event.getMetaState(),event.getEventTime()*1000000L);}
        catch(Exception e){failure("Cannot send keyboard input",e);}
        return true;
    }
    private boolean motion(MotionEvent event){
        if(runtime==null||window==0)return false;
        if (geometryMotion(event, false)) return true;
        pointerX=event.getRawX(); pointerY=event.getRawY(); pointerTime=event.getEventTime();
        pointerDown=(event.getButtonState() & MotionEvent.BUTTON_PRIMARY) != 0;
        try{runtime.motion(window,event.getX(),event.getY(),event.getAxisValue(MotionEvent.AXIS_VSCROLL),event.getAxisValue(MotionEvent.AXIS_HSCROLL),event.getActionMasked(),org.matonos.linux.PointerInput.buttons(event.getActionMasked(),event.getButtonState(),event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)),event.getEventTime()*1000000L);}
        catch(Exception e){failure("Cannot send pointer input",e);}
        return true;
    }
    @Override protected void onSaveInstanceState(Bundle state){state.putInt(WINDOW,window);super.onSaveInstanceState(state);}
    @Override protected void onResume() { super.onResume(); foreground = true; }
    @Override protected void onPause() { endGeometry(); foreground = false; super.onPause(); }
    @Override protected void onDestroy(){
        endGeometry();
        if (linuxWindow != null) linuxWindow.close();
        destroyed=true;
        if(window!=0)SHOWING.remove(window,this);
        if(runtime!=null)runtime.clearWindowHost(this);
        setInhibited(false);
        if(runtime!=null){
            try{if(attached&&runtime!=null)runtime.detach(window);}catch(Exception ignored){}
            if(runtime!=null&&isFinishing()&&window!=0)runtime.closeWindow(window);
        }
        super.onDestroy();
        if(WINDOWS.destroyed(this,isFinishing(),isChangingConfigurations())) {
            // The runtime and native callbacks are owned by this process.
            // Ending the last Android window must end that whole session:
            // The loader parent-death signal and Android app cgroup end
            // the Linux tree with the stub. Reopening gets a fresh process.
            // A grace period lets the app open a new window first (cancelled
            // when one opens); the close request already went to the app.
            MAIN.postDelayed(END_SESSION,LAST_WINDOW_GRACE_MS);
        }
    }
}
