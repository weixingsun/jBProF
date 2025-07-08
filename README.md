# JVM eBPF profiler

    JVM profiling tool on linux: low overhead, robust, accurate. 
    Consistent with perf top/record
    Automatically resolve symbols
    Hardware breakpoint sampling, counting, timing, and perform rule-based actions dynamically 
    This project has been inactively maintained for a while
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
    "sample_duration=3;sample_method=9;log_file=method.log;monitor_duration=2;lat_top=1"
    "sample_duration=3;sample_method=9;log_file=method.log;monitor_duration=3;lat_name=sleep"
    
    Top methods for 3 seconds:
    samples	 method_addr	 method_name
     354	 7f9264d26310 -> 7f9264ec3514	 __pthread_mutex_unlock_usercnt
     231	 7f925c023c50 -> 7f925c023c50	 __pthread_mutex_unlock_usercnt
     168	 7f921fa3d2ec -> 7f92457ed3ab	 Interpreter
     110	 7f92640066d2 -> 7f9263ffe740	 __pthread_cond_timedwait
      69	 7f9264ec3440 -> 7f9264a0dea7	 JavaThread::sleep(long)
      66	 7f9236450219 -> 7f926545d4a5	 [UNKNOWN]
      42	 7f9264a0dcc0 -> 7f924d387764	 JVM_Sleep
      38	 7f92640066c2 -> 7f9263ffe730	 __pthread_cond_timedwait
      29	 7f9264ec1430 -> 7f9264ec349d	 java_lang_Thread::interrupted(oopDesc*)
      28	 7f924d3876c8 -> 7f924d38cdc8	 java.lang.Thread.sleep

    (2) latency for method: (7f9264ec3440 -> 7f9264a0dea7)	"JavaThread::sleep(long)"
    nsecs    	     count
    >1048576     	 21	 
    >2097152     	 2756	 

    (2) latency for method: (7f924d3876c8 -> 7f924d38cdc8)	"java.lang.Thread.sleep"
    nsecs    	     count
    >1048576     	 11	 
    >2097152     	 2752

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
        rule: java.util.HashMap.resize>1s	        Main$()V^IncreaseMapInitSize()
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
