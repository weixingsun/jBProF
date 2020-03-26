#include <fcntl.h>
#include <cpufreq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static int GHZ = 1024*1024;
static int MHZ = 1024;
#define AZ(m, e) if ((e) < 0) {    \
	perror(m);               \
	return 1;                  \
}
#define rdmsr(id, var) \
	e = pread(fd, (void *)&var, 0x08, id); \
	AZ("While reading IA32_MPERF",e);
	//printf("IA32_MPERF (0xE7) is %Ld\n", (unsigned long long)var);

char* itos(int x){
    int length = snprintf( NULL, 0, "%d", x );
    char* str = malloc( length + 1 );
    snprintf( str, length + 1, "%d", x );
    return str;
}
char* merge3(char* s1, char* s2, char* s3){
    int n = strlen(s1)+strlen(s2)+strlen(s3);
    char *s4=malloc(n);
    snprintf(s4,n,"%s%s%s", s1,s2,s3);
}
char* gen_msr_path(int index){
        char *path1 = "/dev/cpu/";
        char *path2 = "/msr";
        char *num = itos(index);
	int n = strlen(path1)+strlen(path2)+strlen(num)+1;
        char *path=malloc(n);
        snprintf(path, n, "%s%d%s", path1,index,path2);
	//printf("N=%d  PATH1=%s  PATH2=%s  NUM=%s\n", n, path1, path2, num);
	free(num);
	return path;
}
unsigned long cpu_msr_ce(int index){
	char *path = gen_msr_path(index);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		//perror("Failed to open CE MSR");
		printf("Failed to open CE MSR %s\n", path);
		return 0;
	}
	//printf("Successfully opened %s\n", path);
	unsigned long freq;
	int e = pread(fd, (void *)&freq, 0x8, 0xCE);
	AZ("While reading 0xCE", e);
	//printf("Value returned for pread at offset 0xCE is %lx\n", freq);
	unsigned long h = (freq >> 8)&0xFF ;
	//printf("Value= %lx freq= %hhdG  shift= %lx \n",freq, (int)((freq >> 8) & 0xFF), h );

	if (close(fd) < 0) {
		perror("While closing 0xCE MSR FD");
		return -1;
	}
	free(path);
	return h*1024*100;
}
unsigned long cpu_msr_aperf(int index, unsigned long max){
	u_int64_t aperf0, mperf0, aperf1, mperf1;
	char *path = gen_msr_path(index);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		//perror("Failed to open MSR");
		printf("MSR Error %s\n", path);
		return 0;
	}
	
	int e;
	rdmsr(0xE7, mperf0);
	rdmsr(0xE8, aperf0);
	
	usleep(10*1000);

	rdmsr(0xE7, mperf1);
	rdmsr(0xE8, aperf1);

	if (close(fd) < 0) {
		perror("While closing FD");
		return 0;
	}
	free(path);
	return max * (aperf1-aperf0) / (mperf1-mperf0);
}
unsigned long cpu_api(int index){
        return cpufreq_get_freq_kernel(index);
        //printf("CPU %d Frequency  %lu \n",index, curr/MHZ );
}
int main(int argc, char* argv[]){
	int MSR=0;
	int BATCH=4;
	int opt;

        while ((opt = getopt(argc, argv, "abk")) != -1) {
            switch (opt) {
            case 'k': MSR = 0; printf("Read Linux API. "); break;
            case 'a': MSR = 1; printf("Read MSR 0xe8. ");  break;
            case 'b': MSR = 2; printf("Read MSR 0xce. ");  break;
            default:
                fprintf(stderr, "Usage: %s [-a/-b/-k] \n", argv[0]);
                fprintf(stderr, "      -k : read linux kernel api [default]\n");
                fprintf(stderr, "      -a : read real clock. msr aperf (0xe8)\n");
                fprintf(stderr, "      -b : read base clock. msr info (0xce) \n");
                exit(-1);
            }
        }

	unsigned long min, max, curr ;
	if (cpufreq_get_hardware_limits(0, &min, &max)) {
                fprintf(stderr, "Could not get max frequency (P0), try load cpufreq driver\n");
        }
        printf("CPU Frequency [%lu - %lu]MHz \n",min/MHZ, max/MHZ );
        int nprocs = get_nprocs();
        for (int i=0; i<nprocs; ++i) {
		switch(MSR){
			case 0: curr = cpu_api(i); break;
			case 1: curr = cpu_msr_aperf(i,max); break;
			case 2: curr = cpu_msr_ce(i); break;
		}
		printf("CPU %d : %lu \t",i, curr/MHZ );
		if ((i+1) % BATCH == 0)	printf("\n");
	}
}
