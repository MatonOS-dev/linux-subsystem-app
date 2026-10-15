#!/usr/bin/env python3
"""Compile sources/classes in memory and run pure JVM and Android-double tests."""
from pathlib import Path
import subprocess
import json

root = Path(__file__).resolve().parent
main = root.parent / 'main/java/org/matonos/linux'
test = root / 'java/org/matonos/linux'
names = ['DBusSignature', 'DBusReader', 'DBusWriter', 'PortalWire', 'PortalChannel',
         'PeerConnection', 'PortalBackend', 'PortalIntrospection', 'LocalSocketChannel', 'SessionBus']
files = [main / (n + '.java') for n in names]
files += [test / (n + '.java') for n in ['PipeChannel', 'PortalTestPeer', 'PortalBackendTest',
                                       'PortalConnectionTest', 'DBusValidationTest']]
files += sorted((root / 'fixtures/dbus').rglob('*.java'))
script = '''import javax.tools.*;
import java.net.*;
import java.nio.file.*;
import java.io.*;
import java.util.*;
class Mem extends ForwardingJavaFileManager<JavaFileManager> { Map<String,ByteArrayOutputStream> bytes=new HashMap<>(); Mem(JavaFileManager m){super(m);} public JavaFileObject getJavaFileForOutput(JavaFileManager.Location l,String name,JavaFileObject.Kind k,FileObject sibling){return new SimpleJavaFileObject(URI.create("mem:///"+name.replace('.','/')+k.extension),k){public OutputStream openOutputStream(){var b=new ByteArrayOutputStream();bytes.put(name,b);return b;}};} ClassLoader loader(){return new ClassLoader(){protected Class<?> findClass(String n)throws ClassNotFoundException{var b=bytes.get(n);if(b==null)throw new ClassNotFoundException(n);byte[] v=b.toByteArray();return defineClass(n,v,0,v.length);}};} }
var compiler=ToolProvider.getSystemJavaCompiler();
var mem=new Mem(compiler.getStandardFileManager(null,null,null));
var sources=new ArrayList<JavaFileObject>();
'''
for i, f in enumerate(files):
    script += 'sources.add(new SimpleJavaFileObject(URI.create("string:///Source' + str(i) + '/' + f.name + '"),JavaFileObject.Kind.SOURCE){public CharSequence getCharContent(boolean ignore){return ' + json.dumps(f.read_text()) + ';}});\n'
script += '''if(!compiler.getTask(null,mem,null,List.of("-proc:none"),null,sources).call())Runtime.getRuntime().halt(1);
var loader=mem.loader();
'''
for name in ['PortalBackendTest', 'DBusValidationTest', 'DBusRuntimeFailureTest', 'PortalConnectionTest']:
    script += 'try{loader.loadClass("org.matonos.linux.' + name + '").getMethod("main",String[].class).invoke(null,(Object)new String[0]);}catch(Throwable t){t.printStackTrace();Runtime.getRuntime().halt(1);}\n'
script += 'Runtime.getRuntime().halt(0);\n'
result = subprocess.run(['jshell', '--feedback', 'concise', '--execution', 'local',
                         '-J-Djava.util.prefs.userRoot=/tmp/dbus-review-prefs'],
                        input=script, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
print(result.stdout)
raise SystemExit(result.returncode)
