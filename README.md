# JVM eBPF profiler

A JVM profiling tool on linux: low overhead, robust, accurate

Features:

Agent:   java -agentpath:./profiler.so=$OPTS

Attach:  jcmd $pid JVMTI.agent_load ./profiler.so $OPTS

WHERE $OPTS equals to:

1.Flamegraph: [flame.svg](https://github.com/weixingsun/jBProF/blob/master/flame.svg)

    "sample_duration=3;frequency=49;sample_cpu=cpu.log"
    ./flamegraph.pl cpu.log > flame.svg

2.Thread Sampling: [thread.log](https://github.com/weixingsun/jBProF/blob/master/thread.log)

    "sample_duration=3;frequency=49;sample_thread=thread.log"
    
    pid		tid		count	pct		name
    8876	8880	8		0.52	VM Thread
    8876	8879	790		51.13	ParGC Thread#1
    8876	8877	5		0.32	java
    8876	8878	742		48.03	ParGC Thread#0

3.Method Sampling: [method.log](https://github.com/weixingsun/jBProF/blob/master/method.log)

    "sample_duration=3;sample_top=9;sample_method=method.log"
    "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;monitor_top=4"
    
    samples	 method_addr	 method_name
    240 	 7f01f15631f1	 ParCompactionManager::follow_marking_stacks()
    236		 7f01f150d3f8	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    204		 7f01f156c9e5	 ParallelCompactData::add_obj(HeapWordImpl**, unsigned long)
    203		 7f01f150d458	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    42		 7f01f15633e8	 ParCompactionManager::follow_marking_stacks()
    26		 7f01f156c282	 UpdateOnlyClosure::do_addr(HeapWordImpl**, unsigned long)
    24		 7f01f150d470	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    17		 7f01f15641d8	 ParCompactionManager::follow_contents(oopDesc*)
    Monitoring Top Methods:
    count	 method_addr	 method_name
    89285	 7f01f15631f1	 ParCompactionManager::follow_marking_stacks()
    89493	 7f01f150d3f8	 ParMarkBitMap::mark_obj(HeapWordImpl**, unsigned long)
    88977	 7f01f156c9e5	 ParallelCompactData::add_obj(HeapWordImpl**, unsigned long)


Install:

    1.install BCC & dependencies
    2.install clang
    3.install JDK (13 tested)
    4.run.sh
