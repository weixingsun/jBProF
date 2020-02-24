
AGENT=profiler.so
rm -rf $AGENT log cpu.log mem.log hs_err* jcmd.log flame.svg /tmp/perf*
LOOP=3000000
JIT="-Xmx300m -Xms300m -XX:+UseParallelOldGC -XX:ParallelGCThreads=1 -XX:+PreserveFramePointer"

cpp_build(){
  BCC=/home/sun/perf_tuning_results/jvm/bcc
  BCC_INC="-I$BCC/src/cc -I$BCC/src/cc/api -I$BCC/src/cc/libbpf/include/uapi"
  CC=clang
  CPP=clang++
  OS=linux
  JAVA_HOME=/home/sun/jbb/jdk13
  JAVA_INC="-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS"
  OPTS="-O3 -fPIC -lbcc -lstdc++ -shared $JAVA_INC $BCC_INC"
  #echo "$CPP $OPTS -o $AGENT profiler.cpp"
  $CPP $OPTS -o $AGENT profiler.cpp
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
cpp_build
if [ $? = 0 ]; then
    echo "build done"
    #run_with_agent $AGENT "duration=5;sample_mem=mem.log;sample_cpu=cpu.log"
    run_and_attach $AGENT "duration=3;sample_mem=mem.log;sample_cpu=cpu.log"
    #FlameGraph/flamegraph.pl profile.out > flame.svg
    gojvmti/FlameGraph/flamegraph.pl cpu.log > flame.svg
fi
