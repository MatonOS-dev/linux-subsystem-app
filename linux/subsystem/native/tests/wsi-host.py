"""Real lavapipe FIFO smoke against a minimal peer (not production wlroots).
Usage: python3 wsi-host.py --deps <compositor build> --out <out scratch>
       [--proxy <linux-layer-v3>] [--cube <vkcube-wayland>]
Returns 77 when the environment denies Wayland client creation.
"""
from pathlib import Path
import argparse,os,socket,subprocess,time
ap=argparse.ArgumentParser();ap.add_argument('--deps',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--proxy',type=Path);ap.add_argument('--cube',default='vkcube-wayland');args=ap.parse_args()
d=args.deps.resolve();b=args.out.resolve();b.mkdir(parents=True,exist_ok=True)
xml=d/'host/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
scanner=d/'host/bin/wayland-scanner'
for mode,name in [('server-header','xdg-shell-server-protocol.h'),('private-code','xdg-shell.c')]:
 subprocess.run([str(scanner),mode,str(xml),str(b/name)],check=True)
# Link the host library, never the Android .so in stage/.
subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(d/'stage/x86_64/include'),'-I'+str(b),str(Path(__file__).with_name('wsi-host-server.c')),str(b/'xdg-shell.c'),'-Wl,-l:libwayland-server.so.0','-o',str(b/'wsi-server')],check=True)
modes=[('native',[])];
if args.proxy:modes.append(('proxy',[str(args.proxy.resolve()),'--android-filter','--trace']))
for name,prefix in modes:
 a,c=socket.socketpair()
 env=dict(os.environ,WAYLAND_SOCKET=str(c.fileno()),WAYLAND_DEBUG='1',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json')
 serverlog=b/(name+'-server.log');clientlog=b/(name+'-vulkan.log')
 with serverlog.open('w') as log,clientlog.open('w') as out:
  p=subprocess.Popen([str(b/'wsi-server'),str(a.fileno())],pass_fds=[a.fileno()],env=dict(os.environ,WAYLAND_DEBUG='1'),stderr=log);a.close()
  try:
   time.sleep(.1)
   if p.poll() is not None:
    if 'Operation not permitted' in serverlog.read_text():
     print('SKIP: sandbox denied wl_client_create; no Vulkan presentation claim');raise SystemExit(77)
    raise RuntimeError(serverlog.read_text())
   result=subprocess.run(prefix+[args.cube,'--c','8','--present_mode','2'],pass_fds=[c.fileno()],env=env,stdout=out,stderr=out,timeout=30)
   assert result.returncode==0,clientlog.read_text()[-3000:]
  finally:
   c.close()
   if p.poll() is None:p.terminate()
   p.wait()
 text=serverlog.read_text();assert 'ACK ' in text and text.count('SHM present ')>=8,text
 print('PASS '+name+': configure ACK and eight lavapipe SHM FIFO presents')
