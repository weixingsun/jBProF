gcc freq.c -static -lcpufreq -o freq
#ar rcs libfreq.a /usr/lib/libcpufreq.so
#gcc freq.o -Lfreq -o freq.bin
