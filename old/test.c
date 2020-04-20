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
#define print_entry( pre ){ \
    unsigned long *rbp, *rbp2; \
    asm("movq %%rbp, %0": "=rm"(rbp)); \
    rbp2=rbp; \
    unsigned long ret = rbp2[1]; \
    unsigned char *ins = (unsigned char*)ret-5; \
    int* off = (int*)ret-1; \
    unsigned long offset = *(off); \
    unsigned long entry = ret+offset; \
    printf("%s( %02x:   ret: %lx   off: %p   offset: %lx   entry: %lx )\n", pre, *ins, ret, off, offset, entry ); \
}
    //rbp2=(unsigned long*)*rbp; // caller
    //printf("%s(  rbp: %p rbp2: %p ret: %lx)\n", pre, rbp, rbp2, ret); \
    //printf("%s( %02x entry: %lx )", pre, *((unsigned char*)rbp2[1]-5), *((int*)rbp2[1]-1) + rbp2[1]);
    //printf(" -> ret: %lx \n", rbp2[1]);

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
