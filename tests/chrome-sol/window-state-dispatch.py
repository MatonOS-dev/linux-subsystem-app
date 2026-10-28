#!/usr/bin/env python3
"""Run the actual activity state dispatch with counting Android stand-ins."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/StubActivity.java').read_text()
method = s[s.index('    private void applyState('):s.index('    private void confirmFullscreen(')]
method = method.replace('android.view.WindowInsetsController', 'Bars').replace('android.view.WindowInsets.Type.systemBars()', '0').replace('new android.os.OutcomeReceiver<Void, Throwable>()', 'new Receiver()')
code = '''
class StateTest {
 boolean destroyed; int window=1, requests, confirmations; Object runtime;
 static final int STATE_FULLSCREEN_ON=1, STATE_FULLSCREEN_OFF=2, STATE_MINIMIZE=3;
 static final int FULLSCREEN_MODE_REQUEST_ENTER=1,FULLSCREEN_MODE_REQUEST_EXIT=2;
 static class Bars { static final int BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE=1;
 void setSystemBarsBehavior(int x){} void hide(int x){} void show(int x){} }
 static class Receiver { public void onResult(Void x){} public void onError(Throwable x){} }
 static class View {void requestPointerCapture(){} void releasePointerCapture(){}} View view;
 static class NativeCompositor {static void nativeCaptureLost(int x){}}
 static class Window {Bars getInsetsController(){return new Bars();}}
 Window getWindow(){return new Window();} boolean hasWindowFocus(){return true;}
 boolean isInMultiWindowMode(){return true;} void moveTaskToBack(boolean b){}
 void confirmFullscreen(boolean b){confirmations++; applyState(6); applyState(7);}
 void requestFullscreenMode(int mode,Receiver cb){requests++;cb.onResult(null);}
'''
method = method.replace('org.matonos.linux.runtime.NativeCompositor', 'NativeCompositor')
code += method + '''
 public static void main(String[] args){ StateTest t=new StateTest();
 for(int i=0;i<10000;i++) for(int state:new int[]{6,7,16,17,31,0,99})t.applyState(state);
 if(t.requests!=0 || t.confirmations!=0)throw new AssertionError("Non-fullscreen state sent Binder callback");
 t.applyState(1);t.applyState(2);
 if(t.requests!=2 || t.confirmations!=2)throw new AssertionError("Fullscreen dispatch lost");
 System.out.println("window state: 70k non-fullscreen events, zero Binder callbacks"); }
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'StateTest.java';p.write_text(code)
 subprocess.run(['javac',str(p)],check=True)
 subprocess.run(['java','-cp',d,'StateTest'],check=True)
