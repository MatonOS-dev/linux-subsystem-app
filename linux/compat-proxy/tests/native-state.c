/* Inherited exemptions must stay occupied after both kernel ABI handoffs. */
#include "../proxy.c"
#define CHECK(x) do {if(!(x))return __LINE__;}while(0)
int main(int argc,char **argv,char **env) {
    CHECK(argc==4);
    proxy_trace=1;
    inherited_native=RESIDENT_ALLOC(sizeof(*inherited_native));CHECK(inherited_native);
    inherited_native[0]=(struct arch_ip_range){.begin=UINT64_C(0x730000000000),.end=UINT64_C(0x730000001000)};
    inherited_native_count=1;
    CHECK(proxy_native_prepare()==0);
    struct image image;CHECK(proxy_load(argv[1],argv+1,env,&image)==0);
    CHECK(proxy_install(1)==0);
    proxy_enter(image.entry,image.stack);
}
