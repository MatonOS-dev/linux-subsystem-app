#!/usr/bin/env python3
"""Run the real client-window-close callback through bootstrap replacement."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2]
s=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/StubActivity.java').read_text()
a=s.index('    @Override public void windowClosed(');b=s.index('    private void launchLocally(',a)
method=s[a:b].replace('@Override ', '')
code='''
class ReplacementTest {
 boolean destroyed,foreground=true,attached=true;int window=3,finishes;
 Object surface=new Object();View view=new View();Status status=new Status();Runtime runtime=new Runtime();
 static final String WINDOW="window";
 static final java.util.Map<Integer,Object> SHOWING=new java.util.HashMap<>();
 static class Runtime {boolean windows;int detached;boolean hasWindows(){return windows;}void detach(int id){detached=id;}}
 static class Holder {void removeCallback(Object x){}}
 static class View {Holder getHolder(){return new Holder();}}
 static class Status {void setText(String s){}}
 static class Intent {void removeExtra(String s){}}
 Intent getIntent(){return new Intent();} void setContentView(Object v){} void finish(){finishes++;}
 void runOnUiThread(Runnable r){r.run();}
'''+method+'''
 public static void main(String[] args){
  ReplacementTest t=new ReplacementTest();SHOWING.put(3,t);t.windowClosed(3);
  if(t.finishes!=0||t.window!=0||t.attached||t.surface!=null||t.view!=null||t.runtime.detached!=3||SHOWING.containsKey(3))throw new AssertionError("last visible bootstrap must wait");
  t=new ReplacementTest();t.runtime.windows=true;t.windowClosed(3);if(t.finishes!=1)throw new AssertionError("other windows remain");
  t=new ReplacementTest();t.foreground=false;t.windowClosed(3);if(t.finishes!=1)throw new AssertionError("background close");
  t=new ReplacementTest();t.destroyed=true;t.windowClosed(3);if(t.finishes!=0||t.window!=3)throw new AssertionError("destroyed activity");
  t=new ReplacementTest();t.windowClosed(4);if(t.finishes!=0||t.window!=3)throw new AssertionError("another window");
  System.out.println("PASS visible bootstrap waits for replacement; background/multiple/destroyed windows unchanged");
 }
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'ReplacementTest.java';p.write_text(code)
    subprocess.run(['javac',str(p)],check=True)
    subprocess.run(['java','-cp',d,'ReplacementTest'],check=True)
