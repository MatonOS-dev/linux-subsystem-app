/* Conservative leaf-function decoder. An unrecognised instruction rejects the
 * entire symbol, including all its candidate windows. No speculative byte scan.
 * Stack operands/stack mutation and indirect control flow are deliberately out
 * of this first slice: this proves no red-zone use without data-flow guesses. */
static uintptr_t arch_rewrite_table;
static int arch_rewrite_init(void) {
    if(arch_rewrite_table)return 1;
    extern void proxy_rewrite_entry_common(void);
    const size_t bytes=512*32;
    long m=RAW(PROXY_SC_mmap,0,bytes,PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    if((unsigned long)m>=(unsigned long)-4095)return 0;
    unsigned char *p=(void *)(uintptr_t)m;
    uintptr_t common=(uintptr_t)proxy_rewrite_entry_common;
    for(unsigned i=0;i<512;i++){
        unsigned char *s=p+32*i;
        s[0]=0xf3;s[1]=0x0f;s[2]=0x1e;s[3]=0xfa;
        s[4]=0x49;s[5]=0xbb;p_copy(s+6,&common,8);
        s[14]=0x41;s[15]=0xff;s[16]=0xe3;
    }
    if(RAW(PROXY_SC_mprotect,m,bytes,PROT_READ|PROT_EXEC,0,0,0)<0){
        RAW(PROXY_SC_munmap,m,bytes,0,0,0,0);return 0;
    }
    arch_rewrite_table=(uintptr_t)m;return 1;
}
/* Assembly reads this exported pointer without TLS or application registers. */
uintptr_t proxy_rewrite_table;
struct rewrite_insn {size_t size;int64_t target;unsigned nr,window;};
static int arch_rewrite_decode(const unsigned char *p,size_t n,struct rewrite_insn *d){
    size_t i=0,imm=0;unsigned rex=0,op,modrm=0,operand16=0;
    *d=(struct rewrite_insn){.target=INT64_MIN};
    if(n>=4 && p[0]==0xf3 && p[1]==0x0f && p[2]==0x1e && p[3]==0xfa){d->size=4;return 1;}
    while(i<n && (p[i]==0x66 || p[i]==0x2e || p[i]==0x64)){
        if(p[i]==0x66)operand16=1;
        if(++i>3)return 0;
    }
    if(i<n && (p[i]&0xf0)==0x40)rex=p[i++];
    if(i>=n)return 0;
    op=p[i++];
    if(op==0xc3 || op==0x90){}
    else if(op==0xeb || (op>=0x70 && op<=0x7f)){
        if(i>=n)return 0;
        d->target=(int8_t)p[i]+(int64_t)i+1;imm=1;
    }else if(op==0xe9){
        if(n-i<4)return 0;
        int32_t rel;p_copy(&rel,p+i,4);d->target=(int64_t)i+4+rel;imm=4;
    }else if(op>=0xb8 && op<=0xbf){
        if(((op-0xb8)|((rex&1)?8:0))==4)return 0; /* rsp destination */
        imm=(rex&8)?8:(operand16?2:4);
    }else if(op==0x3d || op==0xa9)imm=operand16?2:4;
    else if(op==0x0f){
        if(i>=n)return 0;
        op=p[i++];
        if(op==5){} /* syscall */
        else if(op>=0x80 && op<=0x8f){
            if(n-i<4)return 0;
            int32_t rel;p_copy(&rel,p+i,4);d->target=(int64_t)i+4+rel;imm=4;
        }else if(op==0x1f)modrm=1;
        else return 0;
    }else if(op==0x89 || op==0x8b || op==0x8d || op==0x31 || op==0x33 ||
              op==0x39 || op==0x3b || op==0x85 || op==0x84 || op==0x88 ||
              op==0x8a || op==0x83 || op==0x81 || op==0xc7 || op==0xf7){
        modrm=1;
        if(op==0x83)imm=1;
        if(op==0x81 || op==0xc7)imm=operand16?2:4;
    }else return 0;
    if(modrm){
        if(i>=n)return 0;
        unsigned m=p[i++],mode=m>>6,reg=(m>>3)&7,rm=m&7;
        /* Reject every mention of rsp/esp (including register aliases). */
        if((reg==4 && !(rex&4)) || (mode==3 && rm==4 && !(rex&1)))return 0;
        if(op==0xf7){if(reg==0)imm=operand16?2:4;else if(reg!=2 && reg!=3)return 0;}
        if(op==0xc7 && reg!=0)return 0;
        if(mode!=3){
            if(rm==4){
                if(i>=n)return 0;
                unsigned sib=p[i++];
                if(((sib&7)==4 && !(rex&1)) || (((sib>>3)&7)==4 && (rex&2)))return 0;
                if(mode==0 && (sib&7)==5)i+=4;
            }else if(mode==0 && rm==5)i+=4;
            if(mode==1)i++;else if(mode==2)i+=4;
        }
    }
    if(i>n || imm>n-i || i+imm>15)return 0;
    d->size=i+imm;
    /* Only exact adjacent primary encodings; immediate is a native x86 ID. */
    size_t off=0,w=0;
    if(n>=7 && p[0]==0xb8){off=1;w=7;}
    if(n>=9 && p[0]==0x48 && p[1]==0xc7 && p[2]==0xc0){off=3;w=9;}
    if(w && p[w-2]==0x0f && p[w-1]==5){
        uint32_t nr;p_copy(&nr,p+off,4);
        if(nr==257 || nr==262 || nr==332 || nr==3 || nr==5 || nr==8 || nr==89 || nr==217 || nr==267 || nr==72){d->nr=nr;d->window=w;}
    }
    return 1;
}
static uintptr_t arch_rewrite_entry(unsigned nr){return arch_rewrite_table+32*nr;}
static void arch_rewrite_patch(unsigned char *p,unsigned nr,unsigned window){
    uint32_t entry=(uint32_t)arch_rewrite_entry(nr);
    p[0]=0xb8;p_copy(p+1,&entry,4);p[5]=0xff;p[6]=0xd0;
    for(unsigned i=7;i<window;i++)p[i]=0x90;
}

static long rewrite_slow(long nr,proxy_arg_t *a){
    extern long proxy_rewrite_trap(long,proxy_arg_t *);
    return proxy_rewrite_trap(arch_encode_syscall(nr),a);
}
