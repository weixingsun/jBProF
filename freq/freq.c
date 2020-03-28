#define _GNU_SOURCE
#include <numa.h>
#include <fcntl.h>
#include <cpufreq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
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

void print_bitmask(const struct bitmask *bm){
    printf("bitmask.size=%lu\n",bm->size);
    for(size_t i=0; i<bm->size; ++i){
	//printf("%d", numa_bitmask_isbitset(bm, i));
        if(numa_bitmask_isbitset(bm, i)){
	    printf("%ld,",i);
	}
    }
    printf("\n");
}
void print_array(int *arr, int len){
    printf("[");
    for(int i = 0; i < len; i++){
        printf("%d,",arr[i]);
    }
    printf("]\n");
}
void get_node_cpus(int node, struct bitmask *bm, int *cpus, int len){
    numa_node_to_cpus(node, bm);
    //printf("numa node %d ", i);
    //print_bitmask(bm);
    int j=0;
    for(size_t i=0; i<bm->size; i++){
        if(numa_bitmask_isbitset(bm, i)) {
            cpus[j]=i;
            j++;
        }
    }
    //print_array(cpus,len);
}
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
	int BATCH=8;
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
        printf("CPU Base Frequency [%lu - %lu]MHz \n",min/MHZ, max/MHZ );
	int n_cpus = numa_num_task_cpus();
        int n_numas = numa_max_node();
	int len = n_cpus / (n_numas+1);
	//printf("%d / %d = %d   MSR=%d\n", n_cpus, n_numas, len, MSR);
        struct bitmask *bm = numa_bitmask_alloc(n_cpus);
	for (int i=0; i<=n_numas; i++) {
		int cpus[len];
		memset( cpus, 0, len*sizeof(int) );
		get_node_cpus(i,bm,cpus,len);
		//printf(" %g GB\n", numa_node_size(i, 0) / (1024.*1024*1024.));
		for (int i=0;i<len;i++){
                    switch(MSR){
                        case 0: curr = cpu_api(cpus[i]); break;
                        case 1: curr = cpu_msr_aperf(cpus[i],max); break;
                        case 2: curr = cpu_msr_ce(cpus[i]); break;
                    }
                    printf("[%d] %lu   ",cpus[i], curr/MHZ );
                    //printf("C%d : ",cpus[i] );
                    if (i==len-1 || i+1 == len/2) printf("\n");
                }
	}
	printf("\n");
	numa_bitmask_free(bm);
}
