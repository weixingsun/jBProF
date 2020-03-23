#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cpufreq.h>
#include <sys/sysinfo.h>

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

unsigned long cpu_msr(int index, unsigned long max){
	u_int64_t aperf0, mperf0, aperf1, mperf1;
	int ret;
	char *path = "/dev/cpu/1/msr\0";
	int e, fd;
	//unsigned long long freq;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("Failed to open MSR");
		printf("Failed to open %s\n", path);
		return 0;
	}
	//printf("Successfully opened %s\n", path);
	//e = pread(fd, (void *)&freq, 0x8, 0xCE);
	//AZ("While reading 0xCE", e);
	//printf("Value returned for pread at offset 0xCE is %Lx\n", freq);
	//printf("Current frequency is %hhd\n", (int)((freq >> 8) & 0xFF));
	
	rdmsr(0xE7, mperf0);
	rdmsr(0xE8, aperf0);
	
	usleep(10*1000);

	rdmsr(0xE7, mperf1);
	rdmsr(0xE8, aperf1);

	//printf("CPU%d Frequency %lu MHz\n",index, max * (aperf1-aperf0) / (mperf1-mperf0)/MHZ );
	
	e = close(fd);
	if (e < 0) {
		perror("While closing FD");
		return 0;
	}
	return max * (aperf1-aperf0) / (mperf1-mperf0);
}
unsigned long cpu_api(int index){
        return cpufreq_get_freq_kernel(index);
        //printf("CPU %d Frequency  %lu \n",index, curr/MHZ );
}
int main(){
	unsigned long min, max, curr_msr, curr_api;
	if (cpufreq_get_hardware_limits(0, &min, &max)) {
                fprintf(stderr, "Could not get max frequency (P0), try load cpufreq driver\n");
        }
        printf("CPU Frequency [%lu - %lu]GHz\n",min/MHZ, max/MHZ );
        int nprocs = get_nprocs();
        for (int i=0; i<nprocs; ++i) {
		curr_msr = cpu_msr(i,max); 
		//curr_api = cpu_api(i);
		printf("CPU %d Frequency  %lu \n",i, curr_msr/MHZ );
	}
}
