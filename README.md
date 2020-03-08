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

3.Method Sampling: [method.log](https://github.com/weixingsun/jBProF/blob/master/method.log)

    "sample_duration=3;sample_top=9;sample_method=method.log"
    "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;monitor_top=4"

Install:

    1.install BCC & dependencies
    2.install clang
    3.install JDK (13 tested)
    4.run.sh
