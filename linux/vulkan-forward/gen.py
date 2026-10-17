#!/usr/bin/env python3
"""Stage B: pinned core 1.0–1.4 and explicit offscreen extension allowlist.
Fail generation on new dispatchable-handle parameters needing a custom adapter.
"""
import hashlib, pathlib, sys, xml.etree.ElementTree as ET
xml, out = map(pathlib.Path, sys.argv[1:])
assert hashlib.sha256(xml.read_bytes()).hexdigest() == '006807a47518e9ab23ecf7f4c95d936aab477a20d27a9a7ac23cedefb7210178'
r = ET.parse(xml).getroot()
core = {c.get('name') for f in r.findall('feature') if 'vulkan' in f.get('api','').split(',') and tuple(map(int,f.get('number').split('.'))) <= (1,4) for c in f.findall('./require/command')}
extensions=[x.strip() for x in pathlib.Path(__file__).with_name('extensions.txt').read_text().splitlines() if x.strip() and not x.startswith('#')]
ext_nodes={e.get('name'):e for e in r.findall('./extensions/extension')}
assert all(e in ext_nodes and ext_nodes[e].get('type')=='device' for e in extensions)
selected=set(core)
for e in extensions:
    selected.update(c.get('name') for c in ext_nodes[e].findall('./require/command'))
commands={}
for c in r.findall('./commands/command'):
    if c.get('api') and 'vulkan' not in c.get('api').split(','):continue
    commands[c.get('name') or c.findtext('proto/name')]=c
aliases={n:commands[n].get('alias') for n in selected if n in commands and commands[n].get('alias')}
for n,target in sorted(aliases.items()):
    while commands[target].get('alias'):target=commands[target].get('alias')
    selected.add(target)
    aliases[n]=target
manual = {'vkCreateInstance','vkDestroyInstance','vkEnumeratePhysicalDevices','vkEnumeratePhysicalDeviceGroups','vkEnumerateInstanceVersion','vkEnumerateInstanceExtensionProperties','vkEnumerateInstanceLayerProperties','vkEnumerateDeviceExtensionProperties','vkEnumerateDeviceLayerProperties','vkGetInstanceProcAddr','vkGetDeviceProcAddr','vkCreateDevice','vkDestroyDevice','vkGetDeviceQueue','vkGetDeviceQueue2','vkAllocateCommandBuffers','vkFreeCommandBuffers','vkQueueSubmit','vkQueueSubmit2','vkCmdExecuteCommands','vkDestroyCommandPool'}
queries=[]
for c in r.findall('./commands/command'):
    name=c.findtext('proto/name')
    if c.get('api') and 'vulkan' not in c.get('api').split(','):continue
    if name not in selected or name in manual: continue
    params=[p for p in c.findall('param') if not p.get('api') or 'vulkan' in p.get('api').split(',')]
    if not params or params[0].findtext('type') not in ('VkPhysicalDevice','VkDevice','VkQueue','VkCommandBuffer'):continue
    assert all(p.findtext('type') not in ('VkInstance','VkDevice','VkQueue','VkCommandBuffer','VkPhysicalDevice') for p in params[1:]), name
    queries.append((name,c,params))
out.mkdir(parents=True,exist_ok=True)
fields=''.join(f'    PFN_{n} {n};\n' for n,_,_ in queries)+''.join(f'    PFN_{n} {n};\n' for n in sorted(manual) if n not in ('vkGetDeviceProcAddr',) and n in commands and commands[n].findtext('param/type') in ('VkDevice','VkQueue','VkCommandBuffer'))
(out/'dispatch.h').write_text('/* Generated from pinned vk.xml. */\nstruct mv_dispatch {\n'+fields+'};\n')
lines=['/* Generated; no CPU-specific code. */']
for n,c,ps in queries:
    ret=c.findtext('proto/type'); first=ps[0].findtext('name'); kind=ps[0].findtext('type'); device=kind!='VkPhysicalDevice'; owner='h->device->dispatch' if device else 'h->instance->dispatch'
    decl=', '.join(''.join(p.itertext()) for p in ps)
    args=', '.join([f'({kind})h->android']+['0' if p.findtext('type')=='VkAllocationCallbacks' else p.findtext('name') for p in ps[1:]])
    failure='return;' if ret=='void' else 'return VK_ERROR_INITIALIZATION_FAILED;' if ret=='VkResult' else 'return 0;'
    lines += [f'static {ret} mv_{n}({decl}) {{',*['    (void)'+p.findtext('name')+';' for p in ps if p.findtext('type')=='VkAllocationCallbacks'],f'    struct mv_handle *h=(void *){first};',f'    struct mv_scope scope; if(!mv_enter(&scope)) {{ {failure} }}',f'    if(!{owner}.{n}) {{ switch_back(&scope); {failure} }}',f'    '+('' if ret=='void' else ret+' result=')+f'{owner}.{n}({args});','    switch_back(&scope);']
    if ret!='void':lines+=['    return result;']
    lines+=['}']
lines+=['static void mv_load_dispatch(struct mv_instance *i) {']
for n,_,ps in queries:
    if ps[0].findtext('type')=='VkPhysicalDevice': lines += [f'    i->dispatch.{n}=(PFN_{n})mv_gipa(i->base.android,"{n}");']
lines+=['}', 'static void mv_load_device(struct mv_device *d) {']
for n,ps in [(n,ps) for n,_,ps in queries]+[(n,commands[n].findall('param')) for n in sorted(manual) if n in commands]:
    if n!='vkGetDeviceProcAddr' and ps and ps[0].findtext('type') in ('VkDevice','VkQueue','VkCommandBuffer'): lines += [f'    d->dispatch.{n}=(PFN_{n})d->gdpa(d->base.android,"{n}");']
lines+=['}']
(out/'thunks.inc').write_text('\n'.join(lines)+'\n')
names=sorted(manual|{n for n,_,_ in queries})
assert core <= set(names), sorted(core-set(names))
aliases={n:t for n,t in aliases.items() if t in names}
for n,t in aliases.items():assert t in names
assert selected <= set(names)|set(aliases), sorted(selected-set(names)-set(aliases))

(out/'lookup.inc').write_text('\n'.join(f'    if(!strcmp(name,"{n}")) return (PFN_vkVoidFunction)mv_{n};' for n in names)+'\n'+''.join(f'    if(!strcmp(name,"{n}")) return (PFN_vkVoidFunction)mv_{t};\n' for n,t in sorted(aliases.items())))
(out/'coverage.txt').write_text(f'{len(names)} canonical commands ({len(set(names)&core)} core); {len(aliases)} extension aliases; {len(queries)} generated thunks; {len(manual)} manual adapters\n{len(extensions)} allowlisted device extensions; driver intersection only; zero WSI\n'+'\n'.join(sorted(set(names)|set(aliases)))+'\n')
(out/'extensions.inc').write_text('static const char *const mv_extensions[]={\n'+''.join('    "'+e+'",\n' for e in extensions)+'};\n')
# Generate input-chain validation. Only selected core/extension structures cross
# the boundary; function pointers and embedded dispatchable handles fail closed.
# VkDeviceGroupDeviceCreateInfo is copied/unwrapped by the manual create adapter.
types={t.get('name') or t.findtext('name'):t for t in r.findall('./types/type') if not t.get('api') or 'vulkan' in t.get('api').split(',')}
def canonical(t):
    while t in types and types[t].get('alias'):t=types[t].get('alias')
    return t
structs={n:t for n,t in types.items() if t.get('category')=='struct' and not t.get('alias') and not t.get('protect')}
def members(t):return [m for m in structs[t].findall('member') if not m.get('api') or 'vulkan' in m.get('api').split(',')]
selected_types={t.get('name') for f in r.findall('feature') if 'vulkan' in f.get('api','').split(',') and tuple(map(int,f.get('number').split('.'))) <= (1,4) for t in f.findall('./require/type')}
for e in extensions:selected_types.update(t.get('name') for t in ext_nodes[e].findall('./require/type'))
selected_types={canonical(t) for t in selected_types}
dispatchable={'VkInstance','VkPhysicalDevice','VkDevice','VkQueue','VkCommandBuffer'}
def dangerous(t,seen=None):
    if t in dispatchable or t.startswith('PFN_'):return True
    if t not in structs:return False
    seen=set() if seen is None else seen
    if t in seen:return False
    return any(dangerous(canonical(m.findtext('type')),seen|{t}) for m in members(t) if m.findtext('name')!='pNext')
for n,_,ps in queries:
    for p in ps:
        if 'const ' in ''.join(p.itertext()) and p.findtext('type')!='VkAllocationCallbacks':
            assert not dangerous(canonical(p.findtext('type'))), (n,p.findtext('type'))
chain={}
for n in selected_types:
    if n not in structs or not structs[n].get('structextends') or dangerous(n):continue
    st=next((m.get('values') for m in members(n) if m.findtext('name')=='sType'),None)
    if st:chain[st]=n
roots={canonical(p.findtext('type')) for _,_,ps in queries for p in ps if 'const ' in ''.join(p.itertext()) and p.findtext('type')!='VkAllocationCallbacks'}
roots.update(chain.values());roots.update(['VkDeviceCreateInfo','VkDeviceQueueInfo2','VkCommandBufferAllocateInfo','VkSubmitInfo','VkSubmitInfo2'])
needed=set()
def visit(n):
    n=canonical(n)
    if n not in structs or n in needed:return
    needed.add(n)
    for m in members(n):
        if m.findtext('name')!='pNext':visit(m.findtext('type'))
for n in roots:visit(n)
validation=['/* Generated input structure validation. */','static int mv_chain_ok(const void *chain);']
for n in sorted(needed):validation.append(f'static __attribute__((unused)) int mv_valid_{n}(const {n} *p);')
for n in sorted(needed):
    validation += [f'static __attribute__((unused)) int mv_valid_{n}(const {n} *p) {{','    if(!p)return 1;']
    if dangerous(n):validation+=['    return 0;','}'];continue
    for m in members(n):
        field=m.findtext('name');t=canonical(m.findtext('type'));decl=''.join(m.itertext())
        if field=='pNext':validation.append('    if(!mv_chain_ok(p->pNext))return 0;');continue
        if t not in needed:continue
        if '*' in decl:
            length=m.get('len')
            if length:
                assert length.isidentifier(),(n,field,length)
                validation.append(f'    if(p->{field})for(uint32_t j=0;j<p->{length};j++)if(!mv_valid_{t}(&p->{field}[j]))return 0;')
            else:validation.append(f'    if(!mv_valid_{t}(p->{field}))return 0;')
        elif '[' in decl:
            # No selected inline struct arrays contain pNext or dangerous data.
            assert not any(x.findtext('name')=='pNext' for x in members(t)),(n,field)
        else:validation.append(f'    if(!mv_valid_{t}(&p->{field}))return 0;')
    validation+=['    return 1;','}']
validation+=['static int mv_chain_ok(const void *chain) {','    if(!chain)return 1;','    const VkBaseInStructure *p=chain;','    switch(p->sType) {']
for st,n in sorted(chain.items()):validation.append(f'    case {st}:return mv_valid_{n}(chain);')
validation+=['    default:return 0;','    }','}']
# Clone chains for create-device, including the only selected input chain node
# containing dispatchable handles. Each node is owned and freed on app TLS.
validation+=['static size_t mv_chain_size(VkStructureType type) {','    switch(type) {']
for st,n in sorted(chain.items()):validation.append(f'    case {st}:return sizeof({n});')
validation+=['    case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:return sizeof(VkDeviceGroupDeviceCreateInfo);','    default:return 0;','    }','}']
(out/'validate.inc').write_text('\n'.join(validation)+'\n')
# Validate all reachable input structures before switching away from app TLS.
text=(out/'thunks.inc').read_text()
for n,c,ps in queries:
    ret=c.findtext('proto/type');failure='return;' if ret=='void' else 'return VK_ERROR_FEATURE_NOT_PRESENT;' if ret=='VkResult' else 'return 0;'
    checks=[]
    for p in ps:
        t=canonical(p.findtext('type'));name=p.findtext('name');decl=''.join(p.itertext())
        if t not in needed or 'const ' not in decl:continue
        length=p.get('len')
        if length:
            assert length.isidentifier(),(n,name,length)
            checks.append(f'    if({name})for(uint32_t j=0;j<{length};j++)if(!mv_valid_{t}(&{name}[j])) {{ {failure} }}')
        else:checks.append(f'    if(!mv_valid_{t}({name})) {{ {failure} }}')
    decl=', '.join(''.join(p.itertext()) for p in ps)
    signature=f'static {ret} mv_{n}({decl}) {{'
    text=text.replace(signature,signature+'\n'+'\n'.join(checks))
# Resolve alias spellings during initialization if the driver's canonical name
# is unavailable (extensions may predate promotion to core).
for alias,target in sorted(aliases.items()):
    needle=f'    d->dispatch.{target}=(PFN_{target})d->gdpa(d->base.android,"{target}");'
    # Append to the last existing canonical assignment once per alias.
    if needle in text:text=text.replace(needle,needle+f'\n    if(!d->dispatch.{target})d->dispatch.{target}=(PFN_{target})d->gdpa(d->base.android,"{alias}");')
(out/'thunks.inc').write_text(text)
