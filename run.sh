
AGENT=profiler.so
rm -rf $AGENT log thread.log cpu.log mem.log hs_err* jcmd.log /tmp/perf*  #flame.svg
kill -9 `ps -ef|grep java|grep -v grep |awk '{print $2}'`
LOOP=3000000
JIT="-Xmx300m -Xms300m -XX:+UseParallelOldGC -XX:ParallelGCThreads=1 -XX:+PreserveFramePointer" # -XX:+DTraceMethodProbes" #-XX:+ExtendedDTraceProbes

JAVA_HOME=/home/sun/jbb/jdk13
java_build(){
    $JAVA_HOME/bin/javac Main.java
}
cpp_build(){
  BCC=/home/sun/perf_tuning_results/jvm/bcc
  BCC_INC="-I$BCC/src/cc -I$BCC/src/cc/api -I$BCC/src/cc/libbpf/include/uapi"
  CC=clang
  CPP=clang++
  OS=linux
  JAVA_INC="-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS"
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib/
  export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/lib/
  LIB="-shared -lbcc -lstdc++"
  #LIB="-shared -lbcc -lstdc++ "
  OPTS="-O3 -fPIC $JAVA_INC $BCC_INC $LIB"
  #echo "$CPP -o $AGENT profiler.cpp $OPTS"
        $CPP -o $AGENT profiler.cpp $OPTS
}

run_and_attach(){
    AGT=$1
    OPT=$2
    time $JAVA_HOME/bin/java $JIT Main $LOOP &
    sleep 1
    pid=`pgrep java`
    #/usr/share/bcc/tools/tplist -p $pid
    #echo "$JAVA_HOME/bin/jcmd $pid JVMTI.agent_load ./$AGT $OPT"
    $JAVA_HOME/bin/jcmd $pid JVMTI.agent_load ./$AGT "\"$OPT\"" > jcmd.log 2>&1
    #python method.py -F 99 -p $pid -f 3 > profile.out
}

run_with_agent(){
    AGT=$1
    OPT=$2
    #-XX:+EnableJVMCI -XX:+UseJVMCICompiler -XX:-TieredCompilation -XX:+PrintCompilation -XX:+UnlockExperimentalVMOptions 
    echo "$JAVA_HOME/bin/java $JIT -agentpath:./$AGT=$OPT Main $LOOP"
    time $JAVA_HOME/bin/java $JIT -agentpath:./$AGT=$OPT Main $LOOP > java.log 2>&1 &
    pid=`pgrep java`
    echo "PID=$pid"
}
java_build
cpp_build
if [ $? = 0 ]; then
    echo "build done"
    #run_with_agent $AGENT "sample_duration=5;sample_mem=mem.log"
    #run_and_attach $AGENT "sample_duration=3;frequency=49;sample_cpu=cpu.log"
    #./flamegraph.pl cpu.log > flame.svg

    #run_and_attach $AGENT "sample_duration=3;frequency=49;sample_thread=thread.log"

    #run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log"
    run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;monitor_top=2"
    #grep Main.loop /tmp/perf-*.map
    #perf top
fi
