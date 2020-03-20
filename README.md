# JVM eBPF profiler

    JVM profiling tool on linux: low overhead, robust, accurate. 
    Consistent with perf top/record
    Automatically resolve symbols
    Hardware breakpoint based sampling, measuring

Features: 

    Agent:   java -agentpath:./profiler.so=$OPTS
    Attach:  jcmd $pid JVMTI.agent_load ./profiler.so $OPTS
    
    WHERE $OPTS equals to:

1.Flamegraph: [flame.svg](https://github.com/weixingsun/jBProF/blob/master/flame.svg)  [root]

    "sample_duration=3;frequency=49;sample_cpu=cpu.log"
    ./flamegraph.pl cpu.log > flame.svg

2.Thread Sampling: [thread.log](https://github.com/weixingsun/jBProF/blob/master/thread.log)  [root]

    "sample_duration=3;frequency=49;sample_thread=thread.log"
    
    pid 	tid 	count	pct 	name
    8876	8880	8   	0.52	VM Thread
    8876	8879	790 	51.13	ParGC Thread#1
    8876	8877	5   	0.32	java
    8876	8878	742 	48.03	ParGC Thread#0

3.Method Sampling: [method.log](https://github.com/weixingsun/jBProF/blob/master/method.log)  [root]

    "sample_duration=3;sample_top=9;sample_method=method.log"
    "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;count_top=3"
    "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;lat_top=1"
    
    Top methods for 3 seconds:
    samples	 method_addr	 method_name
    176	 7f4a37600458	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    158	 7f4a376561f1	 ParCompactionManager::follow_marking_stacks()
    151	 7f4a376003f8	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    141	 7f4a3765f9e5	 ParallelCompactData::add_obj(HeapWordImpl**, unsigned long)
    46	 7f4a37600445	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    39	 7f4a376563e8	 ParCompactionManager::follow_marking_stacks()
    23	 7f4a3766414b	 ParallelCompactData::calc_new_pointer(HeapWordImpl**, ParCompactionManager*)
    18	 7f4a37600415	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    16	 7f4a3765620a	 ParCompactionManager::follow_marking_stacks()

    (9) latency for method: (7f4a37600458 -> 7f4a3765722a)	"ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)"
    nsecs           count
    >4096           79888	 
    >8192           806	 
    >16384          151	 
    >32768          36	 
    >65536          49	 
    >131072         33	 
    >262144         12	 
    >524288         1	 
    >1048576        2	 

4.Memory sampling: [mem.log](https://github.com/weixingsun/jBProF/blob/master/mem.log) 
    6:38:28	 Count 	 Method(Class) 
	 43 	 java.lang.Integer.toString(byte) 
	 47 	 java.lang.Integer.toString(java.lang.String) 
	 25 	 java.lang.Integer.valueOf(java.lang.Integer) 
	 64 	 java.util.HashMap.newNode(java.util.HashMap$Node) 
	 5 	     java.util.HashMap.resize(java.util.HashMap$Node[]) 

	 Size 	 Count 
	 >16 	 115 
	 >32 	 64 
	 >262144 	 1 
	 >2097152 	 1 
	 >4194304 	 1 
	 >8388608 	 1 
	 >16777216 	 1 
    6:38:30	 Count 	 Method(Class) 
	 15 	 java.lang.Integer.toString(byte) 
	 17 	 java.lang.Integer.toString(java.lang.String) 
	 13 	 java.lang.Integer.valueOf(java.lang.Integer) 
	 34 	 java.util.HashMap.newNode(java.util.HashMap$Node) 

	 Size 	 Count 
	 >16 	 45 
	 >32 	 34 
    6:38:31	 Count 	 Method(Class) 
	 Size 	 Count 

Install:

    1.install BCC & dependencies
    2.install clang
    3.install JDK (13 tested)
    4.run.sh
