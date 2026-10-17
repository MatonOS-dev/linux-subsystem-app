#!/usr/bin/env python3
"""Stage A: Vulkan core global/instance/physical queries; no extensions or WSI.
Fail generation on new dispatchable-handle parameters needing a custom adapter.
"""
import hashlib, pathlib, sys, xml.etree.ElementTree as ET
xml, out = map(pathlib.Path, sys.argv[1:])
assert hashlib.sha256(xml.read_bytes()).hexdigest() == '006807a47518e9ab23ecf7f4c95d936aab477a20d27a9a7ac23cedefb7210178'
r = ET.parse(xml).getroot()
core = {c.get('name') for f in r.findall('feature') if 'vulkan' in f.get('api','').split(',') and tuple(map(int,f.get('number').split('.'))) <= (1,4) for c in f.findall('./require/command')}
manual = {'vkCreateInstance','vkDestroyInstance','vkEnumeratePhysicalDevices','vkEnumeratePhysicalDeviceGroups','vkEnumerateInstanceVersion','vkEnumerateInstanceExtensionProperties','vkEnumerateInstanceLayerProperties','vkEnumerateDeviceExtensionProperties','vkEnumerateDeviceLayerProperties','vkGetInstanceProcAddr'}
queries=[]
for c in r.findall('./commands/command'):
    name=c.findtext('proto/name')
    if name not in core or name in manual: continue
    params=[p for p in c.findall('param') if not p.get('api') or 'vulkan' in p.get('api').split(',')]
    if not params or params[0].findtext('type')!='VkPhysicalDevice' or name=='vkCreateDevice':continue
    assert all(p.findtext('type') not in ('VkInstance','VkDevice','VkQueue','VkCommandBuffer','VkPhysicalDevice') for p in params[1:]), name
    queries.append((name,c,params))
out.mkdir(parents=True,exist_ok=True)
fields=''.join(f'    PFN_{n} {n};\n' for n,_,_ in queries)
(out/'dispatch.h').write_text('/* Generated from pinned vk.xml. */\nstruct mv_dispatch {\n'+fields+'};\n')
lines=['/* Generated; no CPU-specific code. */']
for n,c,ps in queries:
    ret=c.findtext('proto/type'); first=ps[0].findtext('name')
    decl=', '.join(''.join(p.itertext()) for p in ps)
    args=', '.join(['(VkPhysicalDevice)h->android']+[p.findtext('name') for p in ps[1:]])
    failure='return;' if ret=='void' else 'return VK_ERROR_INITIALIZATION_FAILED;'
    lines += [f'static {ret} mv_{n}({decl}) {{',f'    struct mv_handle *h=(void *){first};',f'    struct mv_scope scope; if(!mv_enter(&scope)) {{ {failure} }}',f'    if(!h->instance->dispatch.{n}) {{ switch_back(&scope); {failure} }}',f'    '+('' if ret=='void' else ret+' result=')+f'h->instance->dispatch.{n}({args});','    switch_back(&scope);']
    if ret!='void':lines+=['    return result;']
    lines+=['}']
lines+=['static void mv_load_dispatch(struct mv_instance *i) {']
for n,_,_ in queries: lines += [f'    i->dispatch.{n}=(PFN_{n})mv_gipa(i->base.android,"{n}");']
lines+=['}']
(out/'thunks.inc').write_text('\n'.join(lines)+'\n')
names=sorted(manual|{n for n,_,_ in queries})
(out/'lookup.inc').write_text('\n'.join(f'    if(!strcmp(name,"{n}")) return (PFN_vkVoidFunction)mv_{n};' for n in names)+'\n')
(out/'coverage.txt').write_text(f'{len(names)} core commands: {len(queries)} generated physical query thunks, {len(manual)} manual adapters\n0 extensions; no device creation, WSI, surfaces or swapchains\n'+'\n'.join(names)+'\n')
