int main(void) {
    static const char message[]="PASS static ELF\n";
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(1),"D"(1),"S"(message),"d"(sizeof(message)-1):"rcx","r11","memory");
    return result==(long)sizeof(message)-1?0:1;
}
