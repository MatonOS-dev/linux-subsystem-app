#!/usr/bin/env python3
"""Exercise the activity ownership handoff used by late fixed X11 hints."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/StubActivity.java').read_text()
a=s.index('    public static void resizeWindow(');b=s.index('    /** The app asked for a window state',a)
method=s[a:b]
code='''
class StubActivity {
 boolean destroyed,attached=true,finishing,multi=true;int window=3,starts,finishes;
 Object surface=new Object();View view=new View();Runtime runtime=new Runtime();
 static final String WINDOW="window";
 static final java.util.Map<Integer,StubActivity> SHOWING=new java.util.HashMap<>();
 static class Main {void post(Runnable r){r.run();}} static Main MAIN=new Main();
 static class Runtime {int detached;void detach(int id){detached=id;}}
 static class Holder {boolean removed;void removeCallback(Object x){removed=true;}}
 static class View {Holder h=new Holder();Holder getHolder(){return h;}}
 static class Intent {boolean removed;void removeExtra(String s){removed=true;}}
 static class Bundle {int width,height;Bundle(int w,int h){width=w;height=h;}}
 Intent intent=new Intent();Intent getIntent(){return intent;}
 static Bundle launchBounds(StubActivity a,int w,int h){return w>0&&h>0?new Bundle(w,h):null;}
 static Intent windowIntent(StubActivity a,int id){if(id!=3)throw new AssertionError();return new Intent();}
 void startActivity(Intent i,Bundle b){if(b.width!=700||b.height!=330)throw new AssertionError();starts++;}
 boolean isInMultiWindowMode(){return multi;}boolean isFinishing(){return finishing;}
 void finish(){finishes++;if(window!=0||attached)throw new AssertionError("handoff would close/detach replacement window");}
'''+method+'''
 public static void main(String[] args){
 StubActivity a=new StubActivity();View old=a.view;SHOWING.put(3,a);
 resizeWindow(3,700,330);
 if(a.starts!=1||a.finishes!=1||a.runtime.detached!=3||a.surface!=null||!old.h.removed||!a.intent.removed||SHOWING.containsKey(3))throw new AssertionError("incomplete handoff");
 for(int state=0;state<4;state++){
 a=new StubActivity();SHOWING.put(3,a);
 if(state==0)a.destroyed=true;else if(state==1)a.finishing=true;else if(state==2)a.multi=false;
 resizeWindow(3,state==3?0:700,330);
 if(a.starts!=0||a.finishes!=0||a.window!=3||!a.attached)throw new AssertionError("invalid resize changed host");
 }
 System.out.println("PASS fixed-hint task resize keeps Linux window; old surface detached before replacement; inactive/invalid requests ignored");
 }
}
'''
with tempfile.TemporaryDirectory(prefix='maton-fixed-size-') as d:
    p=Path(d)/'StubActivity.java';p.write_text(code)
    subprocess.run(['javac',str(p)],check=True)
    subprocess.run(['java','-cp',d,'StubActivity'],check=True)
