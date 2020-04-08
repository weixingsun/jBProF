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

    "sample_duration=3;sample_top=9;sample_mem=mem.log;mon_size=1"
    
    6:38:28	 Count 	 Method(Class) 
	 43 	 java.lang.Integer.toString(byte) 
	 47 	 java.lang.Integer.toString(java.lang.String) 
	 25 	 java.lang.Integer.valueOf(java.lang.Integer) 
	 64 	 java.util.HashMap.newNode(java.util.HashMap$Node) 
	 5 	 java.util.HashMap.resize(java.util.HashMap$Node[]) 

	 Size            Count 
	 >16             115 
	 >32             64 
	 >262144 	 1 
	 >2097152 	 1 
	 >4194304 	 1 
	 >8388608 	 1 
	 >16777216 	 1 
    6:38:29	 Count 	 Method(Class) 
	 15 	 java.lang.Integer.toString(byte) 
	 17 	 java.lang.Integer.toString(java.lang.String) 
	 13 	 java.lang.Integer.valueOf(java.lang.Integer) 
	 34 	 java.util.HashMap.newNode(java.util.HashMap$Node) 

	 Size 	 Count 
	 >16 	 45 
	 >32 	 34 

5. Tuning: [tune.log](https://github.com/jBProF/blob/master/tune.log)

       "sample_duration=3;sample_top=9;sample_method=method.log;tune_cfg=tune.cfg;tune_n=3;until=PROF%start"
       
       |***************************************|
        perf map: /tmp/perf-1314.map
        sample_duration=3
        sample_top=9
        sample_method=method.log
        tune: java.util.HashMap.resize	java.util.HashMap$I^DEFAULT_INITIAL_CAPACITY 	x2<1024
        tune: java.util.ArrayList.grow	java.util.ArrayList$I^DEFAULT_CAPACITY       	x2<2048
        tune: java.util.HashMap.getNode	java.util.HashMap$F^DEFAULT_LOAD_FACTOR      	-0.05>0.2
        tune_n=3
        until=PROF%start
       |***************************************|
       |************* sleep 0s **************|
       found text=start in PROF
       Start BPF
       attached fn:do_perf_event_method to pid:1314 perf event 
       BPF sampling 3 seconds
       sampled 329 methods
       count 	 bp     	 ret    	 addr       	 name
       138	 7f81277c0910	 7f811054c138	, 7f811054abcf	 Main.count
       126	 7f81277c0890	 7f811054af54	, 7f8110547ec9	 java.util.HashMap.resize
       103	 7f81277c0890	 7f811054af54	, 7f8110547e90	 java.util.HashMap.resize
       60	 7f81277c0890	 7f811054af54	, 7f8110547dcb	 java.util.HashMap.resize
       54	 7f81277c0910	 7f811054c138	, 7f811054abbf	 Main.count
       51	 7f81277c0910	 7f811054c138	, 7f811054aceb	 Main.count
       45	 7f81277c0910	 7f811054c138	, 7f811054a7ca	 Main.count
       39	 7f81277c0890	 7f811054af54	, 7f8110547e88	 java.util.HashMap.resize
       38	 7f81277c0910	 7f811054c138	, 7f811054a8c8	 Main.count
       java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 16 -> 32
       
       Initialized BPF(4)
       attached fn:do_perf_event_method to pid:1314 perf event 
       BPF sampling 3 seconds
       sampled 298 methods
       count 	 bp     	 ret    	 addr       	 name
       73	 7f81277c0780	 7f811054af54	, 7f8110547e90	 java.util.HashMap.resize
       62	 7f81277c0780	 7f811054af54	, 7f8110547ec9	 java.util.HashMap.resize
       59	 7f81277c0800	 7f811054c138	, 7f811054abcf	 Main.count
       40	 7f81277c0800	 7f811054c138	, 7f811054abbf	 Main.count
       34	 7f81277c0780	 7f811054af54	, 7f8110547dcb	 java.util.HashMap.resize
       33	 7f81277c0800	 7f811054c138	, 7f811054abcd	 Main.count
       30	 7f81277c0780	 7f811054af54	, 7f8110547e8d	 java.util.HashMap.resize
       28	 7f81277c0800	 7f811054c138	, 7f811054a8c8	 Main.count
       28	 7f81277c0800	 7f811054c138	, 7f811054a8e4	 Main.count
       java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 32 -> 64

       Initialized BPF(4)
       attached fn:do_perf_event_method to pid:1314 perf event 
       BPF sampling 3 seconds
       sampled 349 methods
       count 	 bp     	 ret    	 addr       	 name
       95	 7f81277c0990	 7f811054d7cc	, 7f811054abcf	 Main.count
       91	 7f81277c0910	 7f811054af54	, 7f8110547ec9	 java.util.HashMap.resize
       73	 7f81277c0910	 7f811054af54	, 7f8110547e90	 java.util.HashMap.resize
       45	 7f81277c0990	 7f811054d7cc	, 7f811054a9bc	 Main.count
       36	 7f81277c0990	 7f811054d7cc	, 7f811054aceb	 Main.count
       35	 7f81277c0910	 7f811054af54	, 7f8110547dcb	 java.util.HashMap.resize
       33	 7f81277c0990	 7f811054d7cc	, 7f811054a8c8	 Main.count
       30	 7f81277c0910	 7f811054af54	, 7f8110547e88	 java.util.HashMap.resize
       27	 7f81277c0990	 7f811054d7cc	, 7f811054abcd	 Main.count
       java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 64 -> 128
       Done.
    
Install:

    1.install BCC & dependencies
    2.install clang
    3.install JDK (13 tested)
    4.run.sh

TODO:
    
    1. remove duplicated method entries, currently BCC use IP as method entry.
    2. add feature to get instance level variable values, like ArrayList.size, HashMap.size
    3. add feature to trigger tuning when a method latency > threshold.
    4. add feature to tune performance based on bayesian optimization.
