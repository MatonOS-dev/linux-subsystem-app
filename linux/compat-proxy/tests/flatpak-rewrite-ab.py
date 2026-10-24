#!/usr/bin/env python3
"""Host-only warm Flatpak A/B; supply an isolated pre-populated installation."""
import argparse,json,os,pathlib,signal,subprocess,time,selectors
p=argparse.ArgumentParser();p.add_argument('installation',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('--route',choices=['native','off','on','all'],default='all');p.add_argument('--patch-log',action='store_true');p.add_argument('--no-vulkan-driver',action='store_true');p.add_argument('--runs',type=int,default=3);p.add_argument('--operation',choices=['launch','deploy','pull-deploy'],default='launch');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
proxy=str(pathlib.Path(__file__).resolve().parents[1]/'build/linux-layer-v3')
env=os.environ.copy()
for key in ['DISPLAY','WAYLAND_DISPLAY','MATON_PROXY_REWRITE_LOG']:env.pop(key,None)
for key,sub in [('XDG_DATA_HOME','data'),('XDG_CONFIG_HOME','config'),('XDG_CACHE_HOME','cache')]:env[key]=str(a.installation.resolve()/sub)
base=['/usr/bin/flatpak']
if a.operation=='launch':
    base+=['run']
    if a.no_vulkan_driver:base+=['--env=VK_DRIVER_FILES=/nonexistent']
    base+=['--command=vulkaninfo','org.freedesktop.Platform.VulkanInfo']
else:
    base+=['install','--user','--noninteractive','--reinstall']
    if a.operation=='deploy':base+=['--no-pull']
    base+=['flathub','org.freedesktop.Platform.VulkanInfo//26.08']
for route in (['native','off','on'] if a.route=='all' else [a.route]):
    for run in range(1,a.runs+1):
        e=env.copy();e['MATON_PROXY_REWRITE']='0' if route=='off' else '1'
        if a.patch_log:e['MATON_PROXY_REWRITE_LOG']='1'
        cmd=base if route=='native' else [proxy,'--android-filter']+base
        logfile=a.output/f'{a.operation}-{route}-{run}.log'
        rss={};started=time.monotonic()
        with logfile.open('wb') as log:
            child=subprocess.Popen(cmd,env=e,stdout=log,stderr=log,start_new_session=True)
            selector=selectors.DefaultSelector();pidfd=os.pidfd_open(child.pid);selector.register(pidfd,selectors.EVENT_READ)
            while child.poll() is None:
                # Sample only this invocation's descendants (no unrelated apps/VMs).
                pending=[child.pid]
                while pending:
                    pid=pending.pop()
                    try:
                        pending.extend(int(x) for x in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split())
                        status=pathlib.Path(f'/proc/{pid}/status').read_text()
                        kb=next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
                        rss[str(pid)]=max(rss.get(str(pid),0),kb)
                    except (OSError,StopIteration,ValueError):pass
                if time.monotonic()-started>180:
                    os.killpg(child.pid,signal.SIGKILL);child.wait();break
                selector.select(.02)
            selector.close();os.close(pidfd)
        result=dict(operation=a.operation,route=route,run=run,seconds=round(time.monotonic()-started,4),exit=child.returncode,sampled_peak_rss_kb=rss)
        print(json.dumps(result),flush=True)
