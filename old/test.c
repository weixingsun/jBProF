#include "stdio.h"
/*
main.entry:  fa=0x104d17e10 fb=0x104d17db0 fc=0x104d17cd0 
	fa.entry: 0x104d17e10
	( e8 entry: 104d17e10 ) -> ret: 104d17eb8 
		fb.entry: 0x104d17db0
		( e8 entry: 104d17db0 ) -> ret: 104d17e44 
			fc.entry: 0x104d17cd0
			( e8 entry: 104d17cd0 ) -> ret: 104d17de4 
			fc.ret: 0x104d17cff
		fb.ret: 0x104d17de4
	fa.ret: 0x104d17e44
main.fa.ret: 0x104d17eb8
*/  
static inline void print_entry(char* pre){
    unsigned long* rbp;
    asm("movq %%rbp, %0": "=rm"(rbp));
    rbp=(unsigned long*)*rbp;
    printf("%s( %02x entry: %lx )", pre,
        *((unsigned char*)rbp[1]-5), *((int*)rbp[1]-1) + rbp[1]);
    printf(" -> ret: %lx \n", rbp[1]);
}

void fc(){
    printf("\t\t\tfc.entry: %p\n", fc);
    print_entry("\t\t\t");
retc:
    printf("\t\t\tfc.ret: %p\n", &&retc);
}
void fb(){
    printf("\t\tfb.entry: %p\n", fb);
    print_entry("\t\t");
    fc();
retb:
    printf("\t\tfb.ret: %p\n", &&retb);
}
void fa(){
    printf("\tfa.entry: %p\n", fa);
    print_entry("\t");
    fb();
reta:
    printf("\tfa.ret: %p\n", &&reta);
}
int main()
{
    printf("main.entry:  fa=%p fb=%p fc=%p \n", fa, fb, fc);
    fa();
ret:
    printf("main.fa.ret: %p\n", &&ret);
    return 0;
}
