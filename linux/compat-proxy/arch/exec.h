/* SPDX-License-Identifier: Apache-2.0
 * Loader-family peer hooks. New architectures define their class, machine,
 * trusted sibling names and inherited gateways in the selected backend.
 * Return 0 for this class, 1 for its supported peer, -1 for invalid ELF.
 */
static inline int arch_handoff_class(const unsigned char *e){
 if(e[0]!=127||e[1]!='E'||e[2]!='L'||e[3]!='F')return 0;
 if(e[EI_DATA]!=ELFDATA2LSB||e[EI_VERSION]!=EV_CURRENT)return -1;
 unsigned machine=e[18]|(e[19]<<8);
 if(e[EI_CLASS]==ARCH_ELF_CLASS && machine==ARCH_ELF_MACHINE)return 0;
 return e[EI_CLASS]==ARCH_PEER_ELF_CLASS && machine==ARCH_PEER_ELF_MACHINE?1:-1;
}
static inline const char *arch_peer_loader(const char *base){
 /* Host fixture names never enable environment/path-based peer selection. */
 const char *a="linux-layer-v3",*b="linux-layer-v3-i386";
 unsigned i=0;while(base[i] && base[i]==a[i])i++;
 if(!base[i]&&!a[i])return ARCH_PEER_HOST_LOADER;
 i=0;while(base[i] && base[i]==b[i])i++;
 return !base[i]&&!b[i]?ARCH_PEER_HOST_LOADER:ARCH_PEER_LOADER;
}
