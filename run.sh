SRC=profiler.cpp
AGENT=profiler.so
if [ ! -f $SRC ]; then
    mv $AGENT $SRC
fi
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
    time $JAVA_HOME/bin/java $JIT -agentpath:./$AGT=$OPT Main $LOOP 
    #pid=`pgrep java`
    #echo "PID=$pid"
}
#java_build
cpp_build
if [ $? = 0 ]; then
    echo "build done"
    #run_with_agent $AGENT "sample_duration=5;sample_mem=mem.log"
    #run_and_attach $AGENT "sample_duration=3;frequency=49;sample_cpu=cpu.log"
    #./flamegraph.pl cpu.log > flame.svg

    #run_and_attach $AGENT "sample_duration=3;frequency=49;sample_thread=thread.log"

    #run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log"
    #run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;count_top=1"
    #run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;lat_top=2"

    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;mon_field=Main@loop@I"
    run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;mon_size=1"

    echo "rule of thumb: when top functions has HashMap.resize  -> bigger initial_capacity"
    #HashMap.DEFAULT_INITIAL_CAPACITY name#363 length=2, value#364 value=0x00000010 (16) flag=0x0018 (static final), type=I (int)
    #00000010 -> 16
    #00000040 -> 64
    #00000100 -> 256
    #00000400 -> 1024
    echo "rule of thumb:           .  .  .      HashMap.getNode -> smaller loadFactor "
    #HashMap.DEFAULT_LOAD_FACTOR name#366, length=2, value#92, value=0x3f400000 (0.75), flag=0x0018 (static final), type=F (float)
    #3f400000 -> 0.75
    #3f000000 -> 0.5
    #3e800000 -> 0.25
    #/usr/share/bcc/tools/funclatency -d 3 c:malloc
fi
