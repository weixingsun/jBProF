# JVM eBPF profiler

    JVM profiling tool on linux: low overhead, robust, accurate. 
    Consistent with perf top/record
    Automatically resolve symbols
    Hardware breakpoint sampling, counting, timing, and perform rule-based actions dynamically 

Features: 

    Agent:   java -agentpath:./libprof.so=$OPTS
    Attach:  jcmd $pid JVMTI.agent_load ./libprof.so $OPTS
    Binary:  ./jbprof $pid `pwd`/libprof.so $OPT
    
    WHERE $OPTS equals to:

1.Flamegraph: [flame.svg](https://github.com/weixingsun/jBProF/blob/master/flame.svg)  [root]

    "sample_duration=3;frequency=49;log_file=cpu.log"
    ./flamegraph.pl cpu.log > flame.svg

2.Thread Sampling: [thread.log](https://github.com/weixingsun/jBProF/blob/master/thread.log)  [root]

    "sample_duration=3;frequency=49;sample_thread=4;log_file=thread.log"
    
    pid 	tid 	count	pct 	name
    8876	8879	790 	51.13	ParGC Thread#1
    8876	8878	742 	48.03	ParGC Thread#0
    8876	8880	8   	0.52	VM Thread
    8876	8877	5   	0.32	java

3.Method Sampling: [method.log](https://github.com/weixingsun/jBProF/blob/master/method.log)  [root]

    "sample_duration=3;sample_method=9;log_file=method.log"
    "sample_duration=3;sample_method=9;log_file=method.log;monitor_duration=1;count_top=3"
    "sample_duration=3;sample_method=9;log_file=method.log;monitor_duration=1;lat_top=1"
    
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

4.Memory sampling: [alloc.log](https://github.com/weixingsun/jBProF/blob/master/alloc.log)

    "sample_duration=5;sample_alloc=4;alloc_class_size=java.lang.String;sample_alloc_interval=10m;log_file=alloc.log"
     Counts	Method(Class)
     293	java.lang.Integer.toString(byte)
     305	java.lang.Integer.toString(java.lang.String)
     206	java.lang.Integer.valueOf(java.lang.Integer)
     374	java.util.HashMap.newNode(java.util.HashMap$Node)
     Class java.lang.String size:
     Counts	Size
     305	24

5.Tuning: [tune.log](https://github.com/weixingsun/jBProF/blob/master/tune.log)  [root]

     "sample_duration=3;sample_method=9;log_file=method.log;rule_cfg=tune.cfg;action_n=3;start_until=PROF%start"
       
     |***************************************|
        perf map: /tmp/perf-1314.map
        sample_duration=3
        sample_method=9
        log_file=method.log
        rule: java.util.HashMap.resize		java.util.HashMap$I^DEFAULT_INITIAL_CAPACITY 	x4<1024
        rule: java.util.HashMap.resize		Main$()V^IncreaseMapInitSize()
        rule: java.util.HashMap.resize>1s	Main$()V^IncreaseMapInitSize()
        rule: java.util.ArrayList.grow		java.util.ArrayList$I^DEFAULT_CAPACITY       	x2<2048
        rule: java.util.HashMap.getNode		java.util.HashMap$F^DEFAULT_LOAD_FACTOR      	-0.05>0.2
        action_n=3
        start_until=PROF%start
     |***************************************|
     |************* sleep 0s **************|
     found text=start in PROF
     Start BPF
     attached fn:do_perf_event_method to pid:1314 perf event 
     BPF sampling 3 seconds
     sampled 329 methods
     java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 16 -> 64
       
     Initialized BPF(4)
     attached fn:do_perf_event_method to pid:1314 perf event 
     BPF sampling 3 seconds
     sampled 298 methods
     java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 64 -> 256

     Initialized BPF(4)
     attached fn:do_perf_event_method to pid:1314 perf event 
     BPF sampling 3 seconds
     sampled 349 methods
     java/util/HashMap.DEFAULT_INITIAL_CAPACITY: 256 -> 1024
     Done.
    
Install:

    1.install BCC & dependencies
    2.install clang
    3.install JDK (OpenJDK 10,11,12,13,14 tested)
    4.run.sh

Todo || Issues:
    
    1. sometimes there may be duplicated method, it usually caused by JIT recompilation.
    2. add feature to get instance level variable values, like ArrayList.size, HashMap.size, from JVMTI api
    3. add feature to report on factors which selected by bayes.
