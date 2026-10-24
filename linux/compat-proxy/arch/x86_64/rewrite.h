#include <limits.h>
/* Conservative decoder. An unrecognised instruction rejects the entire
 * function, including all its candidate windows. */
static uintptr_t arch_rewrite_table;
static int arch_rewrite_init(void) {
    if(arch_rewrite_table)return 1;
    extern void proxy_rewrite_entry_common(void),proxy_rewrite_fcntl_entry(void);
    const size_t bytes=512*32;
    long m=RAW(PROXY_SC_mmap,0,bytes,PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    if((unsigned long)m>=(unsigned long)-4095)return 0;
    unsigned char *p=(void *)(uintptr_t)m;
    uintptr_t common=(uintptr_t)proxy_rewrite_entry_common;
    for(unsigned i=0;i<512;i++){
        unsigned char *s=p+32*i;
        s[0]=0xf3;s[1]=0x0f;s[2]=0x1e;s[3]=0xfa;
        uintptr_t target=i==72?(uintptr_t)proxy_rewrite_fcntl_entry:common;
        s[4]=0x49;s[5]=0xbb;p_copy(s+6,&target,8);
        s[14]=0x41;s[15]=0xff;s[16]=0xe3;
    }
    if(RAW(PROXY_SC_mprotect,m,bytes,PROT_READ|PROT_EXEC,0,0,0)<0){
        RAW(PROXY_SC_munmap,m,bytes,0,0,0,0);return 0;
    }
    arch_rewrite_table=(uintptr_t)m;return 1;
}
/* Assembly reads this exported pointer without TLS or application registers. */
uintptr_t proxy_rewrite_table;
static int arch_rewrite_number(long nr){
    return nr==257 || nr==262 || nr==332 || nr==3 || nr==5 || nr==8 || nr==89 || nr==217 || nr==267 || nr==72;
}
struct rewrite_insn {size_t size;int64_t target;int stack_delta;unsigned nr,window,flow,stack_access,rax,syscall,number;};
static int arch_rewrite_decode(const unsigned char *p,size_t n,struct rewrite_insn *d){
    size_t i=0,imm=0;unsigned rex=0,op,modrm=0,operand16=0;
    *d=(struct rewrite_insn){.target=INT64_MIN,.rax=1};
    if(n>=4 && p[0]==0xf3 && p[1]==0x0f && p[2]==0x1e && p[3]==0xfa){d->size=4;return 1;}
    while(i<n && (p[i]==0x66 || p[i]==0x2e || p[i]==0x64)){
        if(p[i]==0x66)operand16=1;
        if(++i>3)return 0;
    }
    if(i<n && (p[i]&0xf0)==0x40)rex=p[i++];
    if(i>=n)return 0;
    op=p[i++];
    if(op==0xc3){d->flow=2;}
    else if(op==0x90){}
    else if(op>=0x50 && op<=0x57)d->stack_delta=8;
    else if(op>=0x58 && op<=0x5f)d->stack_delta=-8;
    else if(op==0xc9){d->flow=2;d->stack_delta=INT_MIN;}
    else if(op==0xe8){if(n-i<4)return 0;int32_t rel;p_copy(&rel,p+i,4);d->target=(int64_t)i+4+rel;d->flow=3;imm=4;}
    else if(op==0x68){if(n-i<4)return 0;d->stack_delta=8;imm=4;}
    else if(op==0x6a){if(i>=n)return 0;d->stack_delta=8;imm=1;}
    else if(op==0xeb || (op>=0x70 && op<=0x7f)){
        if(i>=n)return 0;
        d->target=(int8_t)p[i]+(int64_t)i+1;d->flow=op==0xeb?1:0;imm=1;
    }else if(op==0xe9){
        if(n-i<4)return 0;
        int32_t rel;p_copy(&rel,p+i,4);d->target=(int64_t)i+4+rel;d->flow=1;imm=4;
    }else if(op>=0xb8 && op<=0xbf){
        if(((op-0xb8)|((rex&1)?8:0))==4)return 0; /* rsp destination */
        imm=(rex&8)?8:(operand16?2:4);
        d->rax=((op-0xb8)|((rex&1)?8:0))==0;
    }else if(op==0x3d || op==0xa9)imm=operand16?2:4;
    else if(op==0xa8)imm=1;
    else if(op==0x0f){
        if(i>=n)return 0;
        op=p[i++];
        if(op==5){d->syscall=1;} /* syscall */
        else if(op>=0x40 && op<=0x4f)modrm=1; /* cmovcc */
        else if(op>=0x80 && op<=0x8f){
            if(n-i<4)return 0;
            int32_t rel;p_copy(&rel,p+i,4);d->target=(int64_t)i+4+rel;d->flow=0;imm=4;
        }else if(op==0x1f)modrm=1;
        else return 0;
    }else if(op==0x89 || op==0x8b || op==0x8d || op==0x63 || op==0x01 || op==0x03 ||
              op==0x21 || op==0x23 || op==0x29 || op==0x2b || op==0x09 || op==0x0b ||
              op==0x31 || op==0x33 || op==0x39 || op==0x3b || op==0x85 || op==0x84 ||
              op==0x88 || op==0x8a || op==0x80 || op==0x83 || op==0x81 ||
              op==0xc7 || op==0xf6 || op==0xf7){
        modrm=1;
        if(op==0x80 || op==0x83 || op==0xf6)imm=1;
        if(op==0x81 || op==0xc7)imm=operand16?2:4;
    }else return 0;
    if(modrm){
        if(i>=n)return 0;
        unsigned m=p[i++],mode=m>>6,reg=(m>>3)&7,rm=m&7;
        unsigned regx=reg|((rex&4)?8:0),rmx=rm|((rex&1)?8:0);
        d->rax=(regx==0 || rmx==0 || (mode!=3 && rm==4));
        int group_imm=(op==0x80 || op==0x81 || op==0x83 || op==0xf6 || op==0xf7 || op==0xc7);
        if(mode==3 && ((!group_imm && regx==4) || rmx==4)){
            /* Register moves involving rsp are allowed only when they save or
             * restore it through rbp. Other writes need a richer proof. */
            int save_rsp=(op==0x89 && regx==4 && rmx==5);
            int restore_rsp=(op==0x89 && regx==5 && rmx==4) || (op==0x8b && regx==4 && rmx==5);
            int adjust_rsp=(op==0x83 || op==0x81) && rmx==4 && (reg==0 || reg==5);
            if(!save_rsp && !restore_rsp && !adjust_rsp)return 0;
            if(restore_rsp)d->stack_delta=INT_MIN;
        }
        if(mode!=3 && regx==4 && op!=0x83 && op!=0x81)return 0;
        if(op==0xf6){if(reg==0)imm=1;else if(reg!=2 && reg!=3)return 0;}
        if(op==0xf7){if(reg==0)imm=operand16?2:4;else if(reg!=2 && reg!=3)return 0;}
        if(op==0xc7 && reg!=0)return 0;
        if(mode==3 && rm==4 && (op==0x83 || op==0x81)){
            int64_t amount;
            if(imm==1)amount=(int8_t)p[i];else if(imm==4){int32_t x;p_copy(&x,p+i,4);amount=x;}else return 0;
            if(amount<=0 || amount>INT_MAX || (reg!=0 && reg!=5))return 0;
            d->stack_delta=reg==5?(int)amount:-(int)amount;
        }
        if(mode!=3){
            if(rm==4){
                if(i>=n)return 0;
                unsigned sib=p[i++];
                if(((sib&7)==4 || (sib&7)==5) && mode!=0)d->stack_access=1;
                if(mode==0 && (sib&7)==5)i+=4;
            }else if(mode==0 && rm==5)i+=4;
            else if(rm==5)d->stack_access=1;
            if(mode==1)i++;else if(mode==2)i+=4;
        }
    }
    if(i>n || imm>n-i || i+imm>15)return 0;
    d->size=i+imm;
    /* Constant number load; the shared proof locates its syscall. */
    size_t off=0,w=0;
    if(n>=7 && p[0]==0xb8){off=1;w=7;}
    if(n>=9 && p[0]==0x48 && p[1]==0xc7 && p[2]==0xc0){off=3;w=9;}
    if(w){
        uint32_t nr;p_copy(&nr,p+off,4);
        if(arch_rewrite_number(nr)){d->nr=nr;d->number=1;}
    }
    return 1;
}
static uintptr_t arch_rewrite_entry(unsigned nr){return arch_rewrite_table+32*nr;}
static void arch_rewrite_patch(unsigned char *p,unsigned nr,unsigned window){
    uint32_t entry=(uint32_t)arch_rewrite_entry(nr);
    if(p[0]==0xb8)p_copy(p+1,&entry,4);
    else p_copy(p+3,&entry,4);
    p[window-2]=0xff;p[window-1]=0xd0;
}

static long rewrite_slow(long nr,proxy_arg_t *a){
    extern long proxy_rewrite_trap(long,proxy_arg_t *);
    return proxy_rewrite_trap(arch_encode_syscall(nr),a);
}

static int arch_rewrite_internal_site(uintptr_t pc){
    extern char proxy_rewrite_fcntl_after[];
    return pc==(uintptr_t)proxy_rewrite_fcntl_after;
}
