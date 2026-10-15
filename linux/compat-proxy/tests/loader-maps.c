#include <unistd.h>
/* Force a partial BSS page and several anonymous BSS pages. */
static volatile unsigned char bss[20001];
int main(void){
 for(unsigned i=0;i<sizeof(bss);i++)if(bss[i])return 2;
 char c;
 if(write(1,"ready\n",6)!=6)return 4;
 return read(0,&c,1)==1 && c=='q'?0:3;
}
