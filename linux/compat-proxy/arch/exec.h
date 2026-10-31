/* SPDX-License-Identifier: Apache-2.0
 * Loader-family peer hooks. New architectures define their class, machine,
 * trusted sibling names and inherited gateways in the selected backend.
 * Return 0 for this class, 1 for its supported peer, -1 for invalid ELF.
 */
/* Return 0 for this loader, 1 for its peer, -1 for an unsupported pair. */
static inline int arch_exec_class(unsigned elf_class,unsigned machine) {
 if(elf_class==ARCH_ELF_CLASS && machine==ARCH_ELF_MACHINE)return 0;
 return elf_class==ARCH_PEER_ELF_CLASS && machine==ARCH_PEER_ELF_MACHINE?1:-1;
}
static inline int arch_handoff_class(const unsigned char *e){
 if(e[0]!=127||e[1]!='E'||e[2]!='L'||e[3]!='F')return 0;
 if(e[EI_DATA]!=ELFDATA2LSB||e[EI_VERSION]!=EV_CURRENT)return -1;
 unsigned machine=e[18]|(e[19]<<8);
 return arch_exec_class(e[EI_CLASS],machine);
}
static inline const char *arch_peer_loader(const char *base){
 /* Host fixture names never enable environment/path-based peer selection. */
 const char *a="linux-layer-v3",*b="linux-layer-v3-i386";
 unsigned i=0;while(base[i] && base[i]==a[i])i++;
 if(!base[i]&&!a[i])return ARCH_PEER_HOST_LOADER;
 i=0;while(base[i] && base[i]==b[i])i++;
 return !base[i]&&!b[i]?ARCH_PEER_HOST_LOADER:ARCH_PEER_LOADER;
}

/* Same-ABI kernel exec must enter the fixed raw gateway, not a newly ASLR'd
 * libc whose startup would trap before a SIGSYS handler exists. */
static inline const char *arch_reset_loader(const char *base){
 const char *prefix="libmatonos-v3-";unsigned i=0;
 while(prefix[i] && base[i]==prefix[i])i++;
 return !prefix[i]?ARCH_RESET_LOADER:ARCH_RESET_HOST_LOADER;
}
