#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <math.h>
#include <map>
#include <stack>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <bcc/BPF.h>
#include <jvmti.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

using namespace std;

static bool writing_perf = false;
static FILE* out_cpu;
static FILE* out_thread;
static FILE* out_mem;
static FILE* out_perf;
static JNIEnv* jni = NULL;
static jvmtiEnv* jvmti = NULL;
static jrawMonitorID lock;

static int SAMPLE_TOP_N = 20;
static int COUNT_TOP_N = 0;
static int LAT_TOP_N = 0;
static int BPF_PERF_FREQ = 49;
static int SAMPLE_DURATION = 10;
static int TOP_DURATION = 5;
static int BP_SEQ = 1;
static string MON_CLASS = "";
static string MON_FIELD = "";
static string MON_FIELD_TYPE = "";
static string TUNE_CLASS = "";
static string TUNE_FIELD = "";
static string TUNE_FIELD_TYPE = "";
static string TUNE_MIN_MAX = "";
static int TUNE_ALGO = -1;		//1:*2  2:-0.05
static int MON_SIZE = -1;
static int ALLOC_COUNT = -1;
static ebpf::BPF bpf;
static map<int,string> BPF_TXT_MAP;
static map<int,string> BPF_FN_MAP;
static map<string,int> MEM_MAP;
static map<uint32_t,uint32_t> INT_FIELD_MAP;
static map<uint32_t,uint32_t> SIZE_MAP;

struct Frame {
    jlong samples;
    jlong bytes;
    map<jmethodID, Frame> children;
};
static map<string, Frame> root;

struct method_type {
    uint64_t    addr;
    uint64_t    ret;
    string      name;
    bool operator<(const method_type &m) const{
        return addr < m.addr;
    }
};
//for flame generation
struct stack_key_t {
  int pid;
  uint64_t kernel_ip;
  uint64_t kernel_ret_ip;
  int user_stack_id;
  int kernel_stack_id;
  char name[16];
};
//for top method 
struct method_key_t {
  int pid;
  uint64_t kernel_ip;
  int user_stack_id;
  int kernel_stack_id;
  uint64_t bp;
  uint64_t ret;
};
//for method latency
struct hist_key_t {
    uint64_t key;
    uint64_t slot;
};
//for thread sampling
struct thread_key_t {
    int pid;
    int tid;
    long state;
    char name[16];
};

string BPF_TXT_TRD = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct thread_key_t {
    u32 pid;
    u32 tid;
    long state;
    char name[TASK_COMM_LEN];
};
BPF_HASH(counts, struct thread_key_t);

int do_perf_event_thread(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    struct thread_key_t key = {.pid = tgid};
    key.tid = pid;
    key.state = 0;    //must be runnable
    bpf_get_current_comm(&key.name, sizeof(key.name));
    counts.increment(key);
    return 0;
}
)";
///////////////////////////////////////////
string BPF_TXT_FLM = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct stack_key_t {
    u32 pid;
    u64 kernel_ip;
    u64 kernel_ret_ip;
    int user_stack_id;
    int kernel_stack_id;
    char name[TASK_COMM_LEN];
};
BPF_HASH(counts, struct stack_key_t);
BPF_STACK_TRACE(stack_traces, 16384); //STACK_SIZE

int do_perf_event_flame(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    struct stack_key_t key = {.pid = tgid};
    bpf_get_current_comm(&key.name, sizeof(key.name));
    key.user_stack_id = stack_traces.get_stackid(&ctx->regs, BPF_F_USER_STACK);
    key.kernel_stack_id = stack_traces.get_stackid(&ctx->regs, 0);
    if (key.kernel_stack_id >= 0) {
        // populate extras to fix the kernel stack
        u64 ip = PT_REGS_IP(&ctx->regs);
        u64 page_offset;
        // if ip isn't sane, leave key ips as zero for later checking
#if defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE)
        // x64, 4.16, ..., 4.11, etc., but some earlier kernel didn't have it
        page_offset = __PAGE_OFFSET_BASE;
#elif defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE_L4)
        // x64, 4.17, and later
#if defined(CONFIG_DYNAMIC_MEMORY_LAYOUT) && defined(CONFIG_X86_5LEVEL)
        page_offset = __PAGE_OFFSET_BASE_L5;
#else
        page_offset = __PAGE_OFFSET_BASE_L4;
#endif
#else
        // earlier x86_64 kernels, e.g., 4.6, comes here
        // arm64, s390, powerpc, x86_32
        page_offset = PAGE_OFFSET;
#endif
        if (ip > page_offset) {
            key.kernel_ip = ip;
        }
    }
    counts.increment(key);
    return 0;
}
)";
string BPF_TXT_MTD = R"(
#include <linux/sched.h>
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf_perf_event.h>
struct method_key_t {
    u32 pid;
    u64 kernel_ip;
    int user_stack_id;
    int kernel_stack_id;
    u64 bp;
    u64 ret;
};
typedef struct hist_key {
    u64 key;
    u64 count;
} hist_key_t;

BPF_HASH(counts, struct method_key_t);
BPF_STACK_TRACE(stack_traces, 16384);
BPF_TABLE("array", int, int, top_counter, 16);
BPF_TABLE("array", int, int, top_ret_counter, 16);
BPF_HASH(start, u32);
BPF_HISTOGRAM(dist, u64);

static int inc(int idx) {
    int *ptr = top_counter.lookup(&idx);
    if (ptr) ++(*ptr);
    return 0;
}
static int incr(int idx) {
    int *ptr = top_ret_counter.lookup(&idx);
    if (ptr) ++(*ptr);
    return 0;
}
//struct bpf_perf_event_data *ctx
int do_bp_count0(void *ctx){ return inc(0);}
int do_bp_count1(void *ctx){ return inc(1);}
int do_bp_count2(void *ctx){ return inc(2);}
int do_bp_count3(void *ctx){ return inc(3);}
int do_bp_count4(void *ctx){ return inc(4);}

int do_ret_count0(void *ctx){ return incr(0);}
int do_ret_count1(void *ctx){ return incr(1);}
int do_ret_count2(void *ctx){ return incr(2);}
int do_ret_count3(void *ctx){ return incr(3);}
int do_ret_count4(void *ctx){ return incr(4);}

int func_entry0(struct bpf_perf_event_data *ctx){
    u64 pid_tgid = bpf_get_current_pid_tgid();
    //u32 tgid = pid_tgid >> 32;
    u32 pid = pid_tgid;
    //u64 ip = PT_REGS_IP(&ctx->regs);
    //ipaddr.update(&pid, &ip);
    u64 ts = bpf_ktime_get_ns();
    start.update(&pid, &ts);
    return 0;
}
int func_return0(struct bpf_perf_event_data *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    //u32 tgid = pid_tgid >> 32;
    u32 pid = pid_tgid;
    u64 *tsp = start.lookup(&pid);
    if (tsp == 0) return 0;
    u64 delta = bpf_ktime_get_ns() - *tsp;
    start.delete(&pid);

    u64 key = bpf_log2l(delta);
    dist.increment(key);
    return 0;
} 

int do_perf_event_method(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;

    //struct task_struct *p = (struct task_struct*) bpf_get_current_task();
    //void* ptr = p->stack + THREAD_SIZE - TOP_OF_KERNEL_STACK_PADDING;
    //struct pt_regs* regs = ((struct pt_regs *)ptr) - 1;
    struct pt_regs* regs = &ctx->regs;
    struct method_key_t key = {.pid = tgid};
    key.bp = regs->bp;
    //key.ret = regs->r14;
    u64 ret = regs->bp+8;
    bpf_probe_read(&key.ret, sizeof(u64), (void *)ret);

    key.user_stack_id = stack_traces.get_stackid(&ctx->regs, BPF_F_USER_STACK);
    key.kernel_stack_id = stack_traces.get_stackid(&ctx->regs, 0);
    if (key.kernel_stack_id >= 0) {
        // populate extras to fix the kernel stack
        u64 ip = PT_REGS_IP(&ctx->regs);
        u64 page_offset;
        // if ip isn't sane, leave key ips as zero for later checking
#if defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE)
        // x64, 4.16, ..., 4.11, etc., but some earlier kernel didn't have it
        page_offset = __PAGE_OFFSET_BASE;
#elif defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE_L4)
        // x64, 4.17, and later
#if defined(CONFIG_DYNAMIC_MEMORY_LAYOUT) && defined(CONFIG_X86_5LEVEL)
        page_offset = __PAGE_OFFSET_BASE_L5;
#else
        page_offset = __PAGE_OFFSET_BASE_L4;
#endif
#else
        // earlier x86_64 kernels, e.g., 4.6, comes here
        // arm64, s390, powerpc, x86_32
        page_offset = PAGE_OFFSET;
#endif
        if (ip > page_offset) {
            key.kernel_ip = ip;
        }
    }
    counts.increment(key);
    return 0;
}
)";
static inline uint32_t log2(const uint32_t x) {
  uint32_t y;
  asm ( "\tbsr %1, %0\n"
      : "=r"(y)
      : "r" (x)
  );
  return y;
}
void genbpftextmap(){
    BPF_TXT_MAP[0]=BPF_TXT_FLM;
    BPF_TXT_MAP[1]=BPF_TXT_MTD;
    BPF_TXT_MAP[2]=BPF_TXT_TRD;
    BPF_FN_MAP[0]="do_perf_event_flame";
    BPF_FN_MAP[1]="do_perf_event_method";
    BPF_FN_MAP[2]="do_perf_event_thread";
}
string get_prof_func(int id){
    return BPF_FN_MAP[id];
}
string getbpftext(int id){
    return BPF_TXT_MAP[id];
}

static jlong get_time(jvmtiEnv* jvmti) {
    jlong current_time;
    jvmti->GetTime(&current_time);
    return current_time;
}
static string decode_object_sig(string class_sig){
    // rm first 'L' and last ';'
    class_sig.erase(0, 1);
    class_sig.pop_back();
    //class_sig.substr(1,class_sig.size()-2);
    // Replace '/' with '.'
    replace( class_sig.begin(), class_sig.end(), '/', '.' );
    return class_sig;
}
static string decode_class_signature(string class_sig) {
    switch (class_sig[1]) {
        case 'B': return "byte";
        case 'C': return "char";
        case 'D': return "double";
        case 'F': return "float";
        case 'I': return "int";
        case 'J': return "long";
        case 'S': return "short";
        case 'Z': return "boolean";
        //case 'L': return decode_object_sig(class_sig);
        case '[': return decode_class_signature(class_sig.erase(0,1))+"[][]";
    }
    switch (class_sig[0]){
        case '[': return decode_object_sig(class_sig.erase(0,1))+"[]";
        case 'L': return decode_object_sig(class_sig);
    }
    return "Unknown";
}
void jvmti_free(char* ptr){
    if (ptr != NULL) jvmti->Deallocate((unsigned char*) ptr);
}
static string get_method_name(jmethodID method) {
    //cout<<"get_method_name()"<<endl;
    jclass method_class;
    char* class_sig = NULL;
    char* method_name = NULL;
    if (jvmti->GetMethodDeclaringClass(method, &method_class) == 0 &&
        jvmti->GetClassSignature(method_class, &class_sig, NULL) == 0 &&
        jvmti->GetMethodName(method, &method_name, NULL, NULL) == 0) {
        string result = decode_class_signature(string(class_sig)) + "." + method_name;
        jvmti_free(method_name);
        jvmti_free(class_sig);
        return result;
    } else {
        return "(NONAME)";
    }
}

string getCallerMethodName(jthread thread){
    int DEPTH = 1;
    jvmtiFrameInfo frames[DEPTH];
    jint count;
    if( jvmti->GetStackTrace(thread, 0, DEPTH, frames, &count) ==0 ){
        jmethodID method = frames[0].method;
	return get_method_name(method);
    }else{
        return "(NONAME)";
    }
}
/*
 +---+---------+
 | Z | boolean |
 | B | byte    |
 | C | char    |
 | S | short   |
 | I | int     |
 | J | long    |
 | F | float   |
 | D | double  |
 | Ljava.lang/String; | String |
 +-------------+
*/
float get_float(JNIEnv* jni, jobject o, string field_name){
    jclass cls = jni->GetObjectClass(o);
    jfieldID f = jni->GetFieldID(cls, field_name.c_str(), "F");
    return jni->GetFloatField(cls,f);
}
int get_int(JNIEnv* jni, jobject o, string field_name){
    jclass cls = jni->GetObjectClass(o);
    jfieldID f = jni->GetFieldID(cls, field_name.c_str(), "I");
    return jni->GetIntField(cls,f);
}

int get_static_int(JNIEnv* jni, jclass cls, string field_name){
    //jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "I");
    return jni->GetStaticIntField(cls,f);
}
void set_static_int(JNIEnv* jni, jclass cls, string field_name, int value){
    //jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "I");
    jni->SetStaticIntField(cls,f,value);
}
float get_static_float(JNIEnv* jni, jclass cls, string field_name){
    //jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "F");
    return jni->GetStaticFloatField(cls,f);
}
void set_static_float(JNIEnv* jni, jclass cls, string field_name, float value){
    //jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "F");
    jni->SetStaticFloatField(cls,f,value);
}
void int_map_inc(map<uint32_t,uint32_t> m, int v){
    uint32_t key = log2(v);
    jvmti->RawMonitorEnter(lock);
    m[key]++;
    jvmti->RawMonitorExit(lock);
}
vector<string> str_2_vec(string str, char sep){
    istringstream ss(str);
    vector<string> kv;
    string sub;
    while (getline(ss,sub,sep)){
        kv.push_back(sub);
    }
    return kv;
}
float tune_float(float v, int algo, string MIN_MAX){
    vector<string> vs= str_2_vec(MIN_MAX,',');
    float MIN = stof(vs[0]);
    float MAX = stof(vs[1]);
    switch(algo){
        case 1: return (v<MAX?v*2:MAX);
	case 2: return (v>MIN?v-0.05f:MIN);
    }
    return 0;
}
int tune_int(int v, int algo, string MIN_MAX){
    vector<string> vs= str_2_vec(MIN_MAX,',');
    float MIN = stoi(vs[0]);
    float MAX = stoi(vs[1]);
    switch(algo){
        case 1: return (v<MAX?v*2:MAX);
	case 2: return (v>MIN?v-1:MIN);
    }
    return 0;
}
void JNICALL SampledObjectAlloc(jvmtiEnv* jvmti, JNIEnv* env, jthread thread, jobject object, jclass klass, jlong size) {
    char* class_sig = NULL;
    if (jvmti->GetClassSignature(klass, &class_sig, NULL) ==0 ){
        string class_name = decode_class_signature(string(class_sig));
        if(ALLOC_COUNT>0){
            string method_name = getCallerMethodName(thread);
            string result = method_name+"("+class_name + ")";
            jvmti->RawMonitorEnter(lock);
            MEM_MAP[result]++;
            jvmti->RawMonitorExit(lock);
	}
	if(MON_SIZE > 0) int_map_inc(SIZE_MAP,size);

        if(MON_CLASS.length()>0 && MON_FIELD.length() > 0 && MON_CLASS == class_name ) { //class match
            if (MON_FIELD_TYPE == "I"){
                cout<<"SampleAlloc() "<<MON_CLASS<<"."<<MON_FIELD<<endl;
                int v = get_float(env,object,MON_FIELD);
                cout<<"SampleAlloc() "<<MON_CLASS<<"."<<MON_FIELD<<"="<<v<<endl;
                //int_map_inc(INT_FIELD_MAP, v);
            }else if (MON_FIELD_TYPE == "F"){
                float v = get_float(jni, klass, MON_FIELD);
                cout<<"SampleAlloc() "<<MON_CLASS<<"."<<MON_FIELD<<"="<<v<<endl;
            }
        }
        if(TUNE_CLASS.length()>0 && TUNE_ALGO>0 && TUNE_CLASS == class_name) {
            if (TUNE_FIELD_TYPE == "I"){
                int v = get_static_int(env,klass,TUNE_FIELD);
                set_static_int(env,klass,TUNE_FIELD,tune_int(v,TUNE_ALGO,TUNE_MIN_MAX));
                cout<<"SampleAlloc() tune -> "<<TUNE_CLASS<<"."<<TUNE_FIELD<<"="<<v<<endl;
            }else if (TUNE_FIELD_TYPE == "F"){
                float v = get_static_float(env,klass,TUNE_FIELD);
                set_static_float(env,klass,TUNE_FIELD,tune_float(v,TUNE_ALGO,TUNE_MIN_MAX));
                cout<<"SampleAlloc "<<"("<<class_name<<") tune -> "<<TUNE_CLASS<<"."<<TUNE_FIELD<<"="<<v<<endl;
            }
        }
    }
    jvmti_free(class_sig);
}

tm* get_time(){
    time_t theTime = time(NULL);
    tm* now = localtime(&theTime);
    return now;
}
void PrintMemoryMap(int signum){
    //cout<<"sampled alloc objects: map.size="<<MEM_MAP.size()<<endl;
    if(out_mem != NULL){
        tm* now = get_time();
        //if(MON_CLASS.length()>0){
            fprintf(out_mem, "%d:%d:%d\t Count \t Method(Class) \n", now->tm_hour, now->tm_min, now->tm_sec);
            for(auto elem : MEM_MAP){
                //cout << "\t" <<elem.second << "\t" << elem.first << endl;
                fprintf(out_mem, "\t %d \t %s \n", elem.second, elem.first.c_str() );
            }
            MEM_MAP.clear();
	//}
        if(MON_FIELD.length()>0){
	    //cout<<"sampled field: map.size="<<INT_FIELD_MAP.size()<<endl;
            fprintf(out_mem, "\n\t Size \t Count   %s\n", MON_FIELD.c_str() );
            for(auto elem : INT_FIELD_MAP){
                fprintf(out_mem, "\t >%ld \t %d \n", (long)exp2(elem.first), elem.second );
            }
            INT_FIELD_MAP.clear();
        }
        if(MON_SIZE>0){
	    //cout<<"sampled size_map: map.size="<<SIZE_MAP.size()<<endl;
            fprintf(out_mem, "\n\t Size \t Count   Alloc\n" );
            for(auto elem : SIZE_MAP){
                fprintf(out_mem, "\t >%ld \t %d \n", (long)exp2(elem.first), elem.second );
            }
            SIZE_MAP.clear();
        }
        fflush(out_mem);
    }

    //test_field_value();
}
void SetupTimer(int duration, int interval, __sighandler_t timer_handler){
    struct sigaction sa;
    itimerval timer;
    /* Install timer_handler as the signal handler for SIGVTALRM. */
    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = timer_handler;
    sigaction (SIGVTALRM, &sa, NULL);

    //start after 1 second
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = interval;
    timer.it_interval.tv_usec = 0;
    setitimer (ITIMER_VIRTUAL, &timer, NULL);
}

void JNICALL VMDeath(jvmtiEnv* jvmti, JNIEnv* env) {
    //trace(jvmti_env, "VM exited");
}

void JNICALL GarbageCollectionStart(jvmtiEnv *jvmti) {
    //trace(jvmti_env, "GC started");
}

void JNICALL GarbageCollectionFinish(jvmtiEnv *jvmti_env) {
    //trace(jvmti_env, "GC finished");
}
void JNICALL CompiledMethodLoad(jvmtiEnv *jvmti, jmethodID method, jint code_size,
     const void* code_addr, jint map_length, const jvmtiAddrLocationMap* map, const void* compile_info){
    if (out_perf!=NULL && !writing_perf){
	string method_name = get_method_name(method);
	//fprintf(stdout, "method_name:  %s \n", method_name );
	fprintf(out_perf, "%lx %x %s\n", (unsigned long) code_addr, code_size, method_name.c_str());
    }
}
void JNICALL CompiledMethodUnload(jvmtiEnv *jvmti_env, jmethodID method, const void* code_addr){

}
void JNICALL DynamicCodeGenerated(jvmtiEnv *jvmti, const char* method_name, const void* code_addr, jint code_size) {
    if (out_perf!=NULL && !writing_perf){
        fprintf(out_perf, "%lx %x %s\n", (unsigned long) code_addr, code_size, method_name);
    }
}
// replace_string(s,'/','_')
string replace_string(string text, char t, char s){
    replace(text.begin(), text.end(), t, s);
    return text;
}
void JNICALL ClassFileLoadHook(jvmtiEnv *jvmti, JNIEnv* jni, jclass class_being_redefined, jobject loader,
		const char* name, jobject protection_domain, 
		jint class_data_len, const unsigned char* class_data,
		jint* new_class_data_len, unsigned char** new_class_data){
    if(replace_string(string(name),'/','.')==MON_CLASS){
        cout<<"ClassFileLoadHook: ["<< class_data_len<<"]"<<name<<endl;
	string file_name = replace_string(string(name),'/','_')+".class";
	FILE *file = fopen(file_name.c_str(), "wb");
        jvmti->RawMonitorEnter(lock);
	fwrite(class_data,1,class_data_len, file);
	fclose(file);
        jvmti->RawMonitorExit(lock);
    }
}
/////////////////////////////////////////////////////////////////////////
bool str_replace(string& str, const string& from, const string& to) {
    size_t start_pos = str.find(from);
    if(start_pos == std::string::npos)
        return false;
    str.replace(start_pos, from.length(), to);
    return true;
}

perf_event_attr AttachBreakPoint(uint64_t addr, const string& fn, int seq){
    string fn_seq = fn+to_string(seq);
    fprintf(stdout, "attach to breakpoint: addr=%lx fn=%s \n", addr, fn_seq.c_str() );
    struct perf_event_attr attr = {};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.bp_len = HW_BREAKPOINT_LEN_8; //for exec
    attr.size = sizeof(struct perf_event_attr);
    attr.bp_addr = addr;
    //attr.config = 0;
    attr.config = BP_SEQ;
    BP_SEQ++;
    attr.bp_type = HW_BREAKPOINT_EMPTY;
    attr.bp_type |= HW_BREAKPOINT_X;  //HW_BREAKPOINT_R/HW_BREAKPOINT_W conflict with HW_BREAKPOINT_X
    attr.sample_period = 1;    // Trigger for every event
    attr.precise_ip = 2;        //request sync delivery
    attr.wakeup_events = 1;
    //attr.inherit = 1;
    int pid=-1;
    int cpu=-1;
    int group=-1;
    auto att_r = bpf.attach_perf_event_raw(&attr,fn_seq,pid,cpu,group);
    if(att_r.code()!=0) cerr << att_r.msg() << endl;
    return attr;
}

void StartBPF(int id) {
    //cout << "StartBPF(" << id << ")" << endl;
    //ebpf::BPF bpf;
    int pid = getpid();
    const string PID = "(tgid=="+to_string(pid)+")";
    string BPF_TXT = getbpftext(id);
    str_replace(BPF_TXT, "PID", PID);
    //cout << "BPF:" << endl << BPF_TXT << endl;
    auto init_r = bpf.init(BPF_TXT);
    if (init_r.code() != 0) {
        cerr << init_r.msg() << endl;
    }
    int pid2=-1;
    string fn = get_prof_func(id);
    auto att_r = bpf.attach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES, fn, BPF_PERF_FREQ, 0, pid2);
    if (att_r.code() != 0) {
        cerr << "failed to attach fn:" << fn <<  " pid:" << pid << " err:" << att_r.msg() << endl;
    }else{
        cout << "attached fn:"<<fn <<" to pid:" << pid << " perf event "<< endl;
    }
    cout << "BPF sampling " << SAMPLE_DURATION << " seconds" << endl;
}
void StopBPF(){
    sleep(SAMPLE_DURATION);
    if (out_perf!=NULL){
        writing_perf=true;
        fclose(out_perf);
        writing_perf=false;
        out_perf=NULL;
    }
    bpf.detach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES);
}
void PrintThread(){
    auto table = bpf.get_hash_table<thread_key_t, uint64_t>("counts").get_table_offline();
    /*
    sort( table.begin(), table.end(),
      [](pair<thread_key_t, uint64_t> a, pair<thread_key_t, uint64_t> b) {
        return a.second < b.second;
      }
    );
    */
    //-1 unrunnable, 0 runnable, >0 stopped
    int total_samples = 0;
    fprintf(out_thread, "pid\ttid\tcount\tpct\tname\n");
    for (auto it : table) {
	total_samples+=it.second;
    }
    for (auto it : table) {
	fprintf(out_thread, "%d\t%d\t%ld\t%0.2f\t%s\n", it.first.pid,it.first.tid,it.second, (float)100*it.second/total_samples, it.first.name );
    }

    fclose(out_thread);
}
template <typename A, typename B> multimap<B, A> flip_map(map<A,B> & src) {
    multimap<B,A> dst;
    for(typename map<A, B>::const_iterator it = src.begin(); it != src.end(); ++it)
        dst.insert(pair<B, A>(it->second, it->first));
    return dst;
}
void PrintTopMethodCount(method_type methods[], int n){
    //auto cnt = bpf.get_array_table<unsigned long long>("top_counter");
    ebpf::BPFArrayTable<int> cnt = bpf.get_array_table<int>("top_counter");
    ebpf::StatusTuple res(0);
    fprintf(out_cpu, "Monitoring Methods:\ncount\t method_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
        if (res.code()!=0) cerr<<res.msg()<<endl;
        else fprintf(out_cpu, "%d\t %lx\t %s\n", value, methods[i].addr, methods[i].name.c_str() );
    }
}
void PrintTopMethodRetCount(method_type methods[], int n){
    ebpf::BPFArrayTable<int> cnt = bpf.get_array_table<int>("top_ret_counter");
    ebpf::StatusTuple res(0);
    fprintf(out_cpu, "Monitoring Methods:\ncount\t method_ret_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
	if (res.code()!=0) cerr<<res.msg()<<endl;
	else fprintf(out_cpu, "%d\t %lx\t %s\n", value, methods[i].ret, methods[i].name.c_str() );
    }
}
void DetachBreakPoint(struct perf_event_attr* attr){
    cout<<"detached bp"<<endl;
    bpf.detach_perf_event_raw(attr);
}
void CountMethods(method_type methods[], int n){
    perf_event_attr peas[2*n];
    for (int i=0; i<n; i++){
        if (i>n-1) break;
	int j=2*i;
        peas[j]  =AttachBreakPoint(methods[i].addr, "do_bp_count", i);
        peas[j+1]=AttachBreakPoint(methods[i].ret, "do_ret_count", i);
    }
    cout<<"counting "<< n <<" methods for "<<TOP_DURATION<<" second"<<endl;
    sleep(TOP_DURATION);

    for (perf_event_attr attr : peas){
        DetachBreakPoint(&attr);
    }
    PrintTopMethodCount(methods,n);
    PrintTopMethodRetCount(methods,n);
}
void LatencyMethod(method_type method){
    perf_event_attr peas[2];
    peas[0]  =AttachBreakPoint(method.addr, "func_entry", 0);
    peas[1]=AttachBreakPoint(method.ret, "func_return", 0);
    cout<<"latency measuring for "<<TOP_DURATION<<" second"<<endl;
    sleep(TOP_DURATION);
    DetachBreakPoint(&peas[0]);
    DetachBreakPoint(&peas[1]);
    auto dist = bpf.get_hash_table<uint64_t, uint64_t>("dist").get_table_offline();
    sort( dist.begin(), dist.end(),
      [](pair<uint64_t, uint64_t> a, pair<uint64_t, uint64_t> b) {
        return a.first < b.first;
      }
    );
    fprintf(out_cpu, "\n(%ld) latency for method: (%lx -> %lx)\t\"%s\"\n", dist.size(), method.addr, method.ret, method.name.c_str() );
    fprintf(out_cpu, "nsecs    \t count\n"); // distribution\n");
    for (auto it=dist.begin(); it!=dist.end();it++) {
        fprintf(out_cpu, ">%ld     \t %ld\t \n", (long)exp2(it->first), it->second );
    }
    dist.clear();
}

void PrintTopMethods(int n){
    auto table = bpf.get_hash_table<method_key_t, uint64_t>("counts").get_table_offline();
    auto stacks = bpf.get_stack_table("stack_traces");
    //cout<<"sampled "<< table.size() << " methods"<<endl;
    sort( table.begin(), table.end(),
      [](pair<method_key_t, uint64_t> a, pair<method_key_t, uint64_t> b) {
        return a.second > b.second;
      }
    );
    //fprintf(stdout, "count \t bp     \t ret    \t addr       \t name\n");
    map<method_type, int> mout;
    for (auto it : table) {
        uint64_t method_addr;
	string   method_name;
        if (it.first.kernel_stack_id >= 0) {
            method_addr = *stacks.get_stack_addr(it.first.kernel_stack_id).begin();
            method_name = *stacks.get_stack_symbol(it.first.kernel_stack_id, -1).begin()+"[k]";
        }else if(it.first.user_stack_id >= 0) {
            method_addr = *stacks.get_stack_addr(it.first.user_stack_id).begin();
            method_name = *stacks.get_stack_symbol(it.first.user_stack_id, it.first.pid).begin();
        }
        struct method_type method = {.addr=method_addr, .ret=it.first.ret, .name=method_name};
	auto p = mout.find(method);
	if ( p==mout.end() ){
	    mout.insert(pair<method_type,int>(method, (int)it.second));
	}else{
	    (*p).second += it.second;    //merge_method from different callers
	}
	if( mout.size() >n ) break;
        //fprintf(stdout,   "%ld\t %lx\t %lx\t, %lx\t %s\n", it.second, it.first.bp, it.first.ret, method_addr, method_name.c_str());
        //fprintf(out_cpu, "%ld\t %d\t  %d\t  %lx\t %s\n", it.second, it.first.user_stack_id, it.first.kernel_stack_id, method_addr, method_name.c_str());
    }
    fprintf(out_cpu, "samples\t method_addr\t method_name\n");

    multimap<int, method_type> rmap = flip_map(mout);
    //cout<<"flip method done, printing ("<< rmap.size()<<")"<<endl;
    int i=0;
    method_type methods[n];
    for (multimap<int,method_type>::const_reverse_iterator it = rmap.rbegin(); it!=rmap.rend(); ++it){
        fprintf(out_cpu, "%d\t %lx\t %s\n", it->first, it->second.addr, it->second.name.c_str() );
	if (i<n) methods[i++]=it->second;
    }
    //cout<<"method array done, printing ("<< n<<")"<<endl;
    if(COUNT_TOP_N>0){
        cout<<"start count monitoring..."<<endl;
        CountMethods(methods, COUNT_TOP_N);
    }
    if(LAT_TOP_N>0){
        cout<<"start latency measuring..."<<endl;
        for (int i=0; i<LAT_TOP_N; i++){
            LatencyMethod(methods[i]);
        }
    }
    fclose(out_cpu);
}
void PrintFlame(){
    auto table = bpf.get_hash_table<stack_key_t, uint64_t>("counts").get_table_offline();
    sort( table.begin(), table.end(),
      [](pair<stack_key_t, uint64_t> a, pair<stack_key_t, uint64_t> b) {
        return a.second < b.second;
      }
    );
    auto stacks = bpf.get_stack_table("stack_traces");
    stack<string> stack_traces;
    for (auto it : table) {
        //cout << "PID:" << it.first.pid << it.first.name << endl;
        if (it.first.kernel_stack_id >= 0) {
            auto syms = stacks.get_stack_symbol(it.first.kernel_stack_id, -1);
            for (auto sym : syms) {
                //fprintf(out_cpu, "%s;", sym.c_str());  //need to be reversed
                stack_traces.push(sym);
            }
        }
        if (it.first.user_stack_id >= 0) {
            auto syms = stacks.get_stack_symbol(it.first.user_stack_id, it.first.pid);
            for (auto sym : syms){
                //fprintf(out_cpu, "%s;", sym.c_str()); //need to be reversed
                stack_traces.push(sym);
            }
        }
        fprintf(out_cpu, "%s;", it.first.name);
        while (!stack_traces.empty()){
            fprintf(out_cpu, "%s", stack_traces.top().c_str());
            stack_traces.pop();
	    if (!stack_traces.empty()){
	        fprintf(out_cpu, ";");
	    }
        }
        fprintf(out_cpu, "      %ld\n", it.second);
    }
    fclose(out_cpu);
}

void PrintBPF(int id){
    switch(id){
        case 1:
            PrintFlame();
	    break;
        case 2:
	    PrintTopMethods(SAMPLE_TOP_N);
	    break;
        case 3:
            PrintThread();
	    break;
    }
}
string get_key(string kv, string sep){
    int i = kv.find(sep);
    return kv.substr(0,i);
}
string get_value(string kv, string sep){
    int i = kv.find(sep);
    return kv.substr(i+1);
}
bool str_contains(string s, string k){
    return s.find(k)==0;
}
///////////////////////////////////////////
void VMInit(jvmtiEnv *jvmti, JNIEnv* jnienv, jthread thread) {
    //oop_access_barrier
    //set_static_field_float(jni, "java.util.HashMap", "DEFAULT_LOAD_FACTOR", "F", 0.75f);
    //float load_factor = get_static_field_float(jni, "java.util.HashMap", "DEFAULT_LOAD_FACTOR", "F" );
    //float load_factor = get_static_field_float(jni, "java.util.HashMap", "loadFactor", "F" );
    //cout<<"java.util.HashMap.loadFactor ="<<load_factor<<endl;
    jni = jnienv;
    //jvmti->RawMonitorEnter(lock);
    string CLASS = "";
    if(MON_CLASS.length() >0 ){
        CLASS = MON_CLASS;
    }else if(TUNE_CLASS.length()>0){
        CLASS = TUNE_CLASS;
    }
    jclass obj = jni->FindClass(replace_string(CLASS,'.','/').c_str());
    jvmti->RetransformClasses(1, &obj);
    //jvmti->RawMonitorExit(lock);
    cout<<"VMInit() Load Class : "<<CLASS<<endl;
}
void registerCapa(int mode){
    jvmtiCapabilities capa = {0};
    if(mode>1){
        capa.can_tag_objects = 1;
        capa.can_generate_all_class_hook_events = 1;
        capa.can_generate_compiled_method_load_events = 1;
    }else if(mode==0){
        capa.can_generate_sampled_object_alloc_events = 1;
        capa.can_generate_garbage_collection_events = 1;
        //capa.can_generate_object_free_events = 1;
        //capa.can_generate_vm_object_alloc_events = 1;
	capa.can_redefine_classes = 1;
	capa.can_redefine_any_class = 1;
	capa.can_retransform_classes = 1;
	capa.can_retransform_any_class = 1;
    }
    jvmti->AddCapabilities(&capa);
}
void registerCall(int mode){
    jvmtiEventCallbacks call = {0};
    if(mode>1){
        call.VMInit = VMInit;
        call.CompiledMethodLoad = CompiledMethodLoad;
        call.CompiledMethodUnload = CompiledMethodUnload;
        call.DynamicCodeGenerated = DynamicCodeGenerated;
    }else if(mode==0){
        call.VMInit = VMInit;
        call.SampledObjectAlloc = SampledObjectAlloc;
        //call.GarbageCollectionStart = GarbageCollectionStart;
        call.ClassFileLoadHook = ClassFileLoadHook;
    }
    jvmti->SetEventCallbacks(&call, sizeof(call));
}
void disableAllEvents(){
	jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_VM_INIT, NULL);
	jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
	jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);
	jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);
}
void enableEvent(int mode){
    if(mode>1){
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
        //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);

        jvmti->GenerateEvents(JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
        jvmti->GenerateEvents(JVMTI_EVENT_COMPILED_METHOD_LOAD);
    }else if(mode==0){
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);
        //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, NULL);
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    }
}
void RegisterMemoryAlloc(){
    registerCapa(0);
    registerCall(0);
    enableEvent(0);
    jvmti->SetHeapSamplingInterval(1024*1024); //1m
}
void gen_perf_file(){
    registerCapa(2);
    registerCall(2);
    enableEvent(2);
}
int do_single_options(string k, string v, jvmtiEnv* jvmti){
    if (k.compare("sample_duration") == 0){
        SAMPLE_DURATION=stoi(v);
    }else if(k.compare("top_duration")==0){
        TOP_DURATION=stoi(v);
    }else if(k.compare("frequency")==0){
        BPF_PERF_FREQ=stoi(v);
    }else if(k.compare("sample_top")==0){
        SAMPLE_TOP_N=stoi(v);
    }else if(k.compare("count_top")==0){
        COUNT_TOP_N=stoi(v);
    }else if(k.compare("lat_top")==0){
        LAT_TOP_N=stoi(v);
    }else if(k.compare("sample_mem")==0){
        out_mem = fopen(v.c_str(), "w");
        RegisterMemoryAlloc();
        return 0;
    }else if(k.compare("count_alloc")==0){
        ALLOC_COUNT = stoi(v);
    }else if(k.compare("mon_size")==0){
        MON_SIZE = stoi(v);
    }else if(k.compare("mon_field")==0){
        vector<string> vs= str_2_vec(v,'@');
        MON_CLASS = vs[0];
        MON_FIELD = vs[1];
        MON_FIELD_TYPE = vs[2];
        //cout << "class=" << MON_CLASS << "   field=" << MON_FIELD <<  "   type=" << MON_FIELD_TYPE << endl;
        return 0;
    }else if(k.compare("tune_field")==0){
        cout << "tune_field= " << v << endl;
        vector<string> vs= str_2_vec(v,'@');
        cout << "size = " << vs.size() << endl;
        TUNE_CLASS = vs[0];
        TUNE_FIELD = vs[1];
        TUNE_FIELD_TYPE = vs[2];
        TUNE_ALGO = stoi(vs[3]);
        TUNE_MIN_MAX = vs[4];
        cout << "class=" << TUNE_CLASS << "   field=" << TUNE_FIELD <<  "   type=" << TUNE_FIELD_TYPE << "   algo="<<TUNE_ALGO <<"   minmax="<<TUNE_MIN_MAX<< endl;
        return 0;
    }else if(k.compare("flame")==0){
        gen_perf_file();
        out_cpu = fopen(v.c_str(), "w");
        return 1;
    }else if(k.compare("sample_method")==0){
        gen_perf_file();
        out_cpu = fopen(v.c_str(), "w");
        return 2;
    }else if(k.compare("sample_thread")==0){
        out_thread = fopen(v.c_str(), "w");
        return 3;
    }
    return -1;
}
void closeAllFiles() {
    //fclose(out_mem);
    //fclose(out_cpu);
    //fclose(out_thread);
    //fclose(out_perf);
}
void InitFile() {
    out_mem = stderr;
    out_cpu = stdout;
    out_thread = stdout;
    string pid = to_string(getpid());
    string path = "/tmp/perf-"+pid+".map";
    cout << "perf map: "<< path<< endl;
    out_perf = fopen(path.c_str(),"w");
}
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    cout << "|***************************************|"<< endl;
    InitFile();
    genbpftextmap();
    vm->GetEnv((void**) &jvmti, JVMTI_VERSION_1_0);
    //vm->GetEnv((void **) &jni, JNI_VERSION_1_6);
    jvmti->CreateRawMonitor("jvmti_lock", &lock);
    
    int id = -1;
    if (options != NULL) {
        vector<string> vkv = str_2_vec(string(options),';');
        for (string kv : vkv){
            string k = get_key(kv, "=");
            string v = get_value(kv, "=");
            cout << k << "=" << v << endl;
            int i = do_single_options(k,v,jvmti);
            if (i>-1) id=i;
        }
    }
    cout << "|***************************************|"<< endl;
    if (id>0){ // BPF enabled
        StartBPF(id);
        StopBPF();
        PrintBPF(id);
    }else if (id==0){  // sample memory alloc
        SetupTimer(SAMPLE_DURATION, 1, &PrintMemoryMap);
	cout<<"sampling memory alloc..."<<endl;
	//sleep(DURATION);
        //fclose(out_mem);
        //out_mem=NULL;
    }
    cout << "Done."<< endl;
    return 0;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm){
    cout<<"Agent Unload."<<endl;
    disableAllEvents();
    closeAllFiles();
}
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (jvmti != NULL) {
        return 0;
    }
    return Agent_OnLoad(vm, options, reserved);
}
