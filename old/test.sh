SRC=profiler.cpp
AGENT=profiler.so
if [ ! -f $SRC ]; then
    mv $AGENT $SRC
fi
rm -rf $AGENT log thread.log cpu.log mem.log hs_err* jcmd.log /tmp/perf*  #flame.svg
kill -9 `ps -ef|grep java|grep -v grep |awk '{print $2}'`
LOOP="1000 200000"
JIT="-Xmx400m -Xms10m -XX:+UseParallelOldGC -XX:ParallelGCThreads=1 -XX:+PreserveFramePointer" # -XX:+DTraceMethodProbes" #-XX:+ExtendedDTraceProbes

JAVA_HOME=/home/sun/jbb/jdk13
java_build(){
    $JAVA_HOME/bin/javac Main.java
}
cpp_build(){
  BCC=/home/sun/jbb/bcc
  BCC_INC="-I$BCC/src/cc -I$BCC/src/cc/api -I$BCC/src/cc/libbpf/include/uapi"
  CC=clang
  CPP=g++
  OS=linux
  JAVA_INC="-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS"
  #export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib/
  #export LIBRARY_PATH=$LIBRARY_PATH:/usr/local/lib/
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
    echo "test0 flame"
    run_and_attach $AGENT "sample_duration=5;frequency=49;flame=cpu.log"
    ./flamegraph.pl cpu.log > flame.svg
    ###########run_with_agent $AGENT "sample_duration=5;sample_mem=mem.log"

    echo "test1 thread"
    run_and_attach $AGENT "sample_duration=5;frequency=49;sample_thread=thread.log"

    echo "test2 method"
    run_and_attach $AGENT "sample_duration=5;sample_top=9;sample_method=method.log"

    echo "test3 method lat"
    #run_and_attach $AGENT "sample_duration=3;sample_top=9;sample_method=method.log;monitor_duration=1;count_top=1"
    run_and_attach $AGENT "sample_duration=5;sample_top=9;sample_method=method.log;monitor_duration=1;lat_top=2"

    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;count_alloc=1"
    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;mon_size=1"

    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;mon_field=java.util.HashMap@loadFactor@F"
    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;mon_field=java.util.HashMap@DEFAULT_LOAD_FACTOR@F"

    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;tune_field=java.util.HashMap@DEFAULT_INITIAL_CAPACITY@I@1@2,499"  #1:*2
    #run_with_agent $AGENT "sample_duration=10;sample_mem=mem.log;tune_field=java.util.HashMap@DEFAULT_LOAD_FACTOR@F@2@0.2,0.8"  #2:-0.5 (0.2, 0.8)

    #run_with_agent $AGENT "sample_duration=5;sample_top=9;sample_method=method.log;tune_fields=tune.cfg"
    #echo "rule : when HashMap.resize  -> + initial_capacity"
    #echo "rule :      HashMap.getNode -> - loadFactor "

    #HashMap.DEFAULT_INITIAL_CAPACITY name#363 length=2, value#364 value=0x00000010 (16) flag=0x0018 (static final), type=I (int)
    #00000010 -> 16
    #00000040 -> 64
    #00000100 -> 256
    #00000400 -> 1024
    #HashMap.DEFAULT_LOAD_FACTOR name#366, length=2, value#92, value=0x3f400000 (0.75), flag=0x0018 (static final), type=F (float)
    #3f400000 -> 0.75
    #3f000000 -> 0.5
    #3e800000 -> 0.25
    #/usr/share/bcc/tools/funclatency -d 3 c:malloc
fi
