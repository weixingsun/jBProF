#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <math.h>
#include <map>
#include <stack>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <BPF.h>
#include <jvmti.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <dlfcn.h>
#include <iomanip>
//#include <linux/kallsyms.h>
//const char *kallsyms_lookup(unsigned long addr, unsigned long *symbolsize, unsigned long *ofset, char **modname, char *namebuf)
//void print_symbol(const char *fmt, unsigned long addr)

using namespace std;

static bool ATTACH = false;
static int  PID = -1;
static bool writing_perf = false;
static FILE* out;
static FILE* out_perf;
static JNIEnv* jni = NULL;
static jvmtiEnv* jvmti = NULL;
static jrawMonitorID tree_lock;

static bool UNTIL = true;
static string UNTIL_TEXT;
static string LAT_NAME;
static bool BPF_INIT = false;
static int SAMPLE_ALLOC_N = 20;
static int SAMPLE_TOP_N = 20;
static int COUNT_TOP_N = 0;
static int LAT_TOP_N = 0;
static int BPF_PERF_FREQ = 49;
static int WAIT = 0;
static int DURATION = 10;
static int MON_DURATION = 5;
static int BP_SEQ = 1;
static int TUNING_N = 1;
static ebpf::BPF bpf;
static map<int,string> BPF_TXT_MAP;
static map<int,string> BPF_FN_MAP;
static map<string,long> MEM_MAP;
static vector<string> TUNE_RULES;

static bool ALLOC_SIZE_CLASS_NAME_HAS_QUOTE=false;
static long SAMPLE_ALLOC_INTERVAL = 0;
static string ALLOC_SIZE_CLASS_NAME;
static map<int,long> ALLOC_SIZE_MAP;
static map<unsigned long,string> SYM_MAP;

struct PartialMatch {
    string s;
    PartialMatch(const string& str) : s(str) {}
    bool operator()(const string& in){
        return in.find(s) != string::npos;
    }
};
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
  //uint64_t offset;
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
        //if (ip > page_offset) {
        //    key.kernel_ip = ip;
        //}
    }
    counts.increment(key);
    return 0;
}
)";

//#define PT_REGS_IP(ctx)		((ctx)->ip)
//#define PT_REGS_FP(ctx)       ((ctx)->bp)
//#define PT_REGS_SP(ctx)		((ctx)->sp)
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
    //u64 offset;
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
    //u64 p_ret = regs->bp+8;   //caller.next_ip  //32bit
    //u64 p_bp = regs->bp+4;    //caller.bp       //32bit
    //u64 p_ret = regs->bp+16;  //caller.next_ip  //64bit

    struct pt_regs* regs = &ctx->regs;
    struct method_key_t key = {.pid = tgid};
    u64 p_ret = regs->bp+8;  //caller.next_ip  //32bit
    bpf_probe_read(&key.ret, sizeof(u64), (void *)p_ret);
    if (key.ret>0){
        u32 offset = 0;  // 32bits
        bpf_probe_read(&offset, sizeof(offset), (void *)(key.ret-4) );
        key.bp = key.ret + (int)offset;
        key.user_stack_id = stack_traces.get_stackid(&ctx->regs, BPF_F_USER_STACK);
        key.kernel_stack_id = stack_traces.get_stackid(&ctx->regs, 0);
        if (key.kernel_stack_id >= 0) {
            // populate extras to fix the kernel stack
            u64 ip = PT_REGS_IP(&ctx->regs);
            u64 page_offset;
            // if ip isn't sane, leave key ips as zero for later checking
            #if defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE)  // x64 (4.11 ... 4.16)
                page_offset = __PAGE_OFFSET_BASE;
            #elif defined(CONFIG_X86_64) && defined(__PAGE_OFFSET_BASE_L4)  // x64 ( > 4.17 )
                #if defined(CONFIG_DYNAMIC_MEMORY_LAYOUT) && defined(CONFIG_X86_5LEVEL)
                    page_offset = __PAGE_OFFSET_BASE_L5;
                #else
                    page_offset = __PAGE_OFFSET_BASE_L4;
                #endif
            #else   // x64 (4.6 + arm64, s390, powerpc, x86_32 )
                page_offset = PAGE_OFFSET;
            #endif
            if (ip > page_offset) {
                key.kernel_ip = ip;
            }
        }
        counts.increment(key);
    }else{   // when if ret==0
    }
    return 0;
}
)";
void gen_bpf_map(){
    BPF_TXT_MAP[0]=BPF_TXT_FLM;
    BPF_TXT_MAP[1]=BPF_TXT_TRD;
    BPF_TXT_MAP[2]=BPF_TXT_MTD;
    BPF_FN_MAP[0]="do_perf_event_flame";
    BPF_FN_MAP[1]="do_perf_event_thread";
    BPF_FN_MAP[2]="do_perf_event_method";
}
string get_prof_func(unsigned long id){
    int index = log2(id);
    return BPF_FN_MAP[index];
}
string get_bpf_text(unsigned long id){
    int index = log2(id);
    return BPF_TXT_MAP[index];
}
inline bool str_ends_with(string const & value, string const & ending){
    if (ending.size() > value.size()) return false;
    return equal(ending.rbegin(), ending.rend(), value.rbegin());
}
static jlong get_time(jvmtiEnv* jvmti) {
    jlong current_time;
    jvmti->GetTime(&current_time);
    return current_time;
}
static void msleep(int N){
    struct timespec ts = {0, N*1000*1000};
    nanosleep(&ts, NULL);
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
    }
    // rm first 'L' and last ';'
    class_sig.erase(0, 1);
    class_sig.pop_back();
    //class_sig.substr(1,class_sig.size()-2);
    // Replace '/' with '.'
    replace( class_sig.begin(), class_sig.end(), '/', '.' );
    return class_sig;
}
void jvmti_free(char* ptr){
    if (ptr != NULL) jvmti->Deallocate((unsigned char*) ptr);
}
static string get_method_name(jmethodID method) {
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
        //return decode_class_signature(string(class_sig)) + "." + method_name;
    } else {
        return "(NONAME)";
    }
}
string getClassName(jclass klass){
    string name = "";
    char* class_sig = NULL;
    if (jvmti->GetClassSignature(klass, &class_sig, NULL) ==0 ){
        name = string(class_sig);
    }
    jvmti_free(class_sig);
    return name;
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

string replace_string(string text, char t, char s){
    replace(text.begin(), text.end(), t, s);
    return text;
}
int get_static_int(JNIEnv* jni, string cls_name, string field_name){
    jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "I");
    return jni->GetStaticIntField(cls,f);
}
void set_static_int(JNIEnv* jni, string cls_name, string field_name, int value){
    jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "I");
    jni->SetStaticIntField(cls,f,value);
}
float get_static_float(JNIEnv* jni, string cls_name, string field_name){
    jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "F");
    return jni->GetStaticFloatField(cls,f);
}
void set_static_float(JNIEnv* jni, string cls_name, string field_name, float value){
    jclass cls = jni->FindClass(cls_name.c_str());
    jfieldID f = jni->GetStaticFieldID(cls, field_name.c_str(), "F");
    jni->SetStaticFloatField(cls,f,value);
}
void exec_static_void(JNIEnv* jni, string cls_name, string method_name, string mod){
    jclass cls = jni->FindClass(cls_name.c_str());
    if(!cls) cout<<"class not found"<<endl;
    jmethodID m = jni->GetStaticMethodID(cls, method_name.c_str(), mod.c_str());
    if(!m) cout<<"method not found"<<endl;
    jni->CallStaticVoidMethod(cls, m, 1);
}
bool string_contains(string str, string c){
    if (str.find(c) != string::npos) return true;
    else return false;
}
bool BothAreSpaces(char lhs, char rhs ) { return (lhs == rhs) && (lhs == ' '); }
void vec_remove(vector<string> vs, string s){
    //vs.erase(remove(vs.begin(), vs.end(), s), vs.end());
    auto pos = find(vs.begin(), vs.end(), s);
    if( pos != vs.end() ) vs.erase(pos);
}
vector<string> str_2_vec( string str, char sep){
    string::iterator new_end = unique(str.begin(), str.end(), BothAreSpaces);
    str.erase(new_end, str.end());

    vector<string> kv;
    istringstream ss(str);
    string sub;
    while (getline(ss,sub,sep)){
	string::iterator end_pos = remove(sub.begin(), sub.end(), ' ');
	sub.erase(end_pos, sub.end());
        kv.push_back(sub);
    }
    return kv;
}

float tune_float(float v, string algo, string max){
    float MAX = stof(max);
    char s = algo[0];
    algo.erase(0,1);
    float av = stof(algo);
    switch(s){
        case '*': return (v*av<MAX?v*av:MAX);
        case '/': return (v/av>MAX?v/av:MAX);
        case '+': return (v+av<MAX?v+av:MAX);
        case '-': return (v-av>MAX?v-av:MAX);
    }
    return 0;
}
int tune_int(int v, string algo, string max){
    int MAX = stoi(max);
    char s = algo[0];
    algo.erase(0,1);
    int av = stoi(algo);
    switch(s){
        case '*': return (v*av<MAX?v*av:MAX);
        case '/': return (v/av>MAX?v/av:MAX);
        case '+': return (v+av<MAX?v+av:MAX);
        case '-': return (v-av>MAX?v-av:MAX);
    }
    return 0;
}
void tune_static(JNIEnv* env, string cls_name, string field_name, string field_type, string max, string algo){
    if(field_type=="I"){
        int v = get_static_int(env,cls_name,field_name);
        int v2 = tune_int(v,algo,max);
        set_static_int(env,cls_name,field_name, v2);
        cout<<"Tune: "<<cls_name<<"."<<field_name<<": "<<v<<" -> "<<v2<<endl<<endl;
        fprintf(out, "Tune: %s.%s: %d -> %d \n", cls_name.c_str(), field_name.c_str(), v, v2);
    }else if(field_type=="F"){
        float v = get_static_float(env,cls_name,field_name);
        float v2 = tune_float(v,algo,max);
        set_static_float(env,cls_name,field_name,v2);
        cout<<"Tune: "<<cls_name<<"."<<field_name<<": "<<v<<" -> "<<v2<<endl<<endl;
        fprintf(out, "Tune: %s.%s: %f -> %f \n", cls_name.c_str(), field_name.c_str(), v, v2);
    }
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
void JNICALL SampledObjectAlloc(jvmtiEnv* jvmti, JNIEnv* env, jthread thread,
                                jobject object, jclass klass, jlong size) {
    string class_name = getClassName(klass);
    string method_name = getCallerMethodName(thread);
    if(ALLOC_SIZE_CLASS_NAME.size()>0){
        string key_size = "";
        //if(ALLOC_SIZE_CLASS_NAME.find('(') != string::npos){
        if(ALLOC_SIZE_CLASS_NAME_HAS_QUOTE){
            key_size = method_name+"("+ decode_class_signature(class_name) + ")";
        }else{
            key_size = decode_class_signature(class_name);
        }
        if(ALLOC_SIZE_CLASS_NAME == key_size){
            ALLOC_SIZE_MAP[size]++;
        }
    }
    string result = method_name+"("+decode_class_signature(class_name) + ")";
    jvmti->RawMonitorEnter(tree_lock);  //maybe cpp mutex could improve perf ?
    MEM_MAP[result]++;
    jvmti->RawMonitorExit(tree_lock);
}

void JNICALL VMDeath(jvmtiEnv* jvmti, JNIEnv* env) {
}

void VMInit(jvmtiEnv *jvmti, JNIEnv* env, jthread thread) {
    //jclass cls = jni->FindClass("java/util/HashMap");
    //jvmti->RetransformClasses(1, &cls);
}
void JNICALL GarbageCollectionStart(jvmtiEnv *jvmti) {
}

void JNICALL GarbageCollectionFinish(jvmtiEnv *jvmti_env) {
    //trace(jvmti_env, "GC finished");
}
void JNICALL CompiledMethodLoad(jvmtiEnv *jvmti, jmethodID method, jint code_size,
		const void* code_addr, jint map_length, const jvmtiAddrLocationMap* map, const void* compile_info){
    if (out_perf!=NULL && !writing_perf){
	string method_name = get_method_name(method);
	//fprintf(stdout, "method_name:  %s \n", method_name );
    unsigned long key = (unsigned long) code_addr;
	fprintf(out_perf, "%lx %x %s\n", key, code_size, method_name.c_str());
    SYM_MAP[key] = method_name;
    //cout<<"CompiledMethodLoad: "<<(--SYM_MAP.end())->second<<endl;
    }
}
void JNICALL CompiledMethodUnload(jvmtiEnv *jvmti_env, jmethodID method, const void* code_addr){

}
void JNICALL DynamicCodeGenerated(jvmtiEnv *jvmti, const char* method_name, const void* code_addr, jint code_size) {
    if (out_perf!=NULL && !writing_perf){
        unsigned long key = (unsigned long) code_addr;
        fprintf(out_perf, "%lx %x %s\n", key, code_size, method_name);
        SYM_MAP[key] = std::string(method_name);
        //cout<<"DynamicCodeGenerated: "<<SYM_MAP.size()<<endl;
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

void StartBPF(unsigned long id) {
    if (!BPF_INIT){
        cout << "Start BPF(" << id << ")" << endl;
        const string spid = "(tgid=="+to_string(PID)+")";
        string BPF_TXT = get_bpf_text(id);
        str_replace(BPF_TXT, "PID", spid);
        //cout << "BPF:" << endl << BPF_TXT << endl;
        auto init_r = bpf.init(BPF_TXT);
        if (init_r.code() != 0)  cerr << init_r.msg() << endl;
	BPF_INIT=true;
    }else{
        cout << "Initialized BPF(" << id << ")" << endl;
    }
    int pid2=-1;
    string fn = get_prof_func(id);
    auto att_r = bpf.attach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES, fn, BPF_PERF_FREQ, 0, pid2);
    if (att_r.code() != 0) {
        cerr << "failed to attach fn:" << fn <<  " pid:" << PID << " err:" << att_r.msg() << endl;
    }else{
        cout << "attached fn:"<<fn <<" to pid:" << PID << " perf event "<< endl;
    }
    cout << "BPF sampling " << DURATION << " seconds" << endl;
}
void StopAlloc(){
    cout << "StopAlloc -----------> " << DURATION << " seconds" << endl;
    sleep(DURATION);
    if (out_perf!=NULL) fflush(out_perf);
}
void StopBPF(){
    sleep(DURATION);
    if (out_perf!=NULL){
        //writing_perf=true;
        fflush(out_perf);
        //writing_perf=false;
        //out_perf=NULL;
    }
    bpf.detach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES);
}
map<string,long> PrintThread(int N){
    auto table = bpf.get_hash_table<thread_key_t, uint64_t>("counts").get_table_offline();
    
    sort( table.begin(), table.end(),
      [](pair<thread_key_t, uint64_t> a, pair<thread_key_t, uint64_t> b) {
        return a.second > b.second;
      }
    );
    
    //-1 unrunnable, 0 runnable, >0 stopped
    int total_samples = 0;
    fprintf(out, "pid\ttid\tcount\tpct\tname\n");
    for (auto it : table) {
	total_samples+=it.second;
    }
    map<string,long> msl;
    int i = 0;
    for (auto it : table) {
        if (++i>N) break;
	fprintf(out, "%d\t%d\t%ld\t%0.2f\t%s\n", it.first.pid,it.first.tid,it.second, (float)100*it.second/total_samples, it.first.name );
	msl.insert({it.first.name, 100*it.second/total_samples});
    }
    fclose(out);
    return msl;
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
    fprintf(out, "Monitoring Methods:\ncount\t method_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
        if (res.code()!=0) cerr<<res.msg()<<endl;
        else fprintf(out, "%d\t %lx\t %s\n", value, methods[i].addr, methods[i].name.c_str() );
    }
}
void PrintTopMethodRetCount(method_type methods[], int n){
    ebpf::BPFArrayTable<int> cnt = bpf.get_array_table<int>("top_ret_counter");
    ebpf::StatusTuple res(0);
    fprintf(out, "Monitoring Methods:\ncount\t method_ret_addr\t method_name\n");
    for (int i=0; i<n; i++){
        if (i>n+1) break;
        int value;
        res = cnt.get_value(i, value);
	if (res.code()!=0) cerr<<res.msg()<<endl;
	else fprintf(out, "%d\t %lx\t %s\n", value, methods[i].ret, methods[i].name.c_str() );
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
    cout<<"counting "<< n <<" methods for "<<MON_DURATION<<" second"<<endl;
    sleep(MON_DURATION);

    for (perf_event_attr attr : peas){
        DetachBreakPoint(&attr);
    }
    PrintTopMethodCount(methods,n);
    PrintTopMethodRetCount(methods,n);
}
long LatencyMethod(method_type method){
    perf_event_attr peas[2];
    peas[0]=AttachBreakPoint(method.addr, "func_entry", 0);
    peas[1]=AttachBreakPoint(method.ret, "func_return", 0);
    cout<<"latency measuring for "<<MON_DURATION<<" second"<<endl;
    sleep(MON_DURATION);
    DetachBreakPoint(&peas[0]);
    DetachBreakPoint(&peas[1]);
    auto t = bpf.get_hash_table<uint64_t, uint64_t>("dist");
    auto dist = t.get_table_offline();
    cout<<"method latency measured in "<<dist.size() << " scales"<<endl;
    sort( dist.begin(), dist.end(),
      [](pair<uint64_t, uint64_t> a, pair<uint64_t, uint64_t> b) {
        return a.first < b.first;
      }
    );
    fprintf(out, "\n(%ld) latency for method: (%lx -> %lx)\t\"%s\"\n", dist.size(), method.addr, method.ret, method.name.c_str() );
    fprintf(out, "nsecs    \t count\n"); // distribution\n");
    long lat = 0;
    for (auto it=dist.begin(); it!=dist.end();it++) {
        lat = exp2(it->first);
        fprintf(out, ">%ld     \t %ld\t \n", lat, it->second );
    }
    fflush(out);
    t.clear_table_non_atomic();
    //dist.clear();
    return lat;
}

map<string,long> PrintTopMethods(int N){
    auto t = bpf.get_hash_table<method_key_t, uint64_t>("counts");
    auto table = t.get_table_offline();
    auto stacks = bpf.get_stack_table("stack_traces");
    cout<<"sampled "<< table.size() << " methods"<<endl;
    sort( table.begin(), table.end(),
      [](pair<method_key_t, uint64_t> a, pair<method_key_t, uint64_t> b) {
        return a.second > b.second;
      }
    );
    //fprintf(stdout, "count \t entry   \t ret   \t name\n");
    map<method_type, int> mout;		//addr ret name  count
    for (auto it : table) {
        //uint64_t method_addr;
        string   method_name;
        if ( SYM_MAP.find(it.first.bp) == SYM_MAP.end() ) {
            if (it.first.kernel_stack_id >= 0) {
                //method_addr = *stacks.get_stack_addr(it.first.kernel_stack_id).begin();
                method_name = *stacks.get_stack_symbol(it.first.kernel_stack_id, -1).begin()+"[k]";
            }
            if(it.first.user_stack_id >= 0) {
                //method_addr = *stacks.get_stack_addr(it.first.user_stack_id).begin();
                method_name = *stacks.get_stack_symbol(it.first.user_stack_id, it.first.pid).begin();
            }
        }else{
            method_name = SYM_MAP[it.first.bp]+"[MAP]";
        }
	    //cout<<"method="<<method_name<<"    kid="<<it.first.kernel_stack_id<<"    uid="<<it.first.user_stack_id<<"    bp="<<std::hex<<it.first.bp<<"    ret="<<std::hex<<it.first.ret<<endl;    //////////

        //struct method_type method = {.addr=method_addr, .ret=it.first.ret, .name=method_name};
        struct method_type method = {.addr=it.first.bp, .ret=it.first.ret, .name=method_name};
        auto p = mout.find(method);   // use method_name to remove duplicated rows
        if ( p==mout.end() ){
            mout.insert(pair<method_type,int>(method, (int)it.second));
        }else{
            (*p).second += it.second;    //merge_method from different callers
        }
        if( mout.size() >N ) break;
        //fprintf(stdout,   "%ld\t %lx\t %lx\t %lx\t %s\n", it.second, it.first.bp, it.first.offset, it.first.ret, method_name.c_str());
        //fprintf(out_cpu, "%ld\t %d\t  %d\t  %lx\t %s\n", it.second, it.first.user_stack_id, it.first.kernel_stack_id, method_addr, method_name.c_str());
    }
    fprintf(out, "samples\t method_addr\t method_name\n");

    multimap<int, method_type> rmap = flip_map(mout);
    //cout<<"flip method done, printing ("<< rmap.size()<<")"<<endl;
    int i=0;
    method_type methods[N];
    map<string,long> msl;
    for (multimap<int,method_type>::const_reverse_iterator it = rmap.rbegin(); it!=rmap.rend(); ++it){
        fprintf(out, "%d\t %lx -> %lx\t %s\n", it->first, it->second.addr, it->second.ret, it->second.name.c_str() );
        msl.insert({it->second.name, it->first});
        if (i<N) methods[i++]=it->second;
    }
    //cout<<"method array done, printing ("<< n<<")"<<endl;
    if(COUNT_TOP_N>0){
        cout<<"start count monitoring..."<<endl;
        CountMethods(methods, COUNT_TOP_N);
    }
    if(LAT_TOP_N>0){
        cout<<"start latency measuring..."<<endl;
	    msl.clear();
        for (int i=0; i<LAT_TOP_N; i++){
            long maxLat = LatencyMethod(methods[i]);
            //msl.erase( methods[i].name);
            //cout<<"method "<<methods[i].name<<" removed --------------"<<endl;
            msl.insert({methods[i].name, maxLat});
            //cout<<"method "<<methods[i].name<<" = "<<maxLat<<" added --------------"<<endl;
        }
    }
    if(LAT_NAME.size()>0){
        cout<<"LAT_NAME="<<LAT_NAME<<endl;
        for (int i=0; i<N; i++){
            if (methods[i].name.find(LAT_NAME) != std::string::npos) {
                cout<<"Monitor method: "<<methods[i].name<<endl;
                long maxLat = LatencyMethod(methods[i]);
            }
        }
    }
    fflush(out);
    t.clear_table_non_atomic();
    return msl;
}
map<string,long> PrintFlame(){
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
        //fprintf(out, "%s;", string(it.first.name).c_str());
        fprintf(out, "%s;", it.first.name);
        while (!stack_traces.empty()){
            fprintf(out, "%s", stack_traces.top().c_str());
            stack_traces.pop();
            if (!stack_traces.empty()){
                fprintf(out, ";");
            }
        }
        fprintf(out, "      %ld\n", it.second);
    }
    fclose(out);
    map<string,long> vs;
    return vs;
}

map<string,long> PrintAlloc(int N){
    int i = 0;
    for (auto it : MEM_MAP) {
        if (++i>N) break;
        fprintf(out, "%ld\t%s\n", it.second, it.first.c_str());
    }
    if(ALLOC_SIZE_CLASS_NAME.size()>0){
        fprintf(out, "Class %s size:\nCounts\tSize\n", ALLOC_SIZE_CLASS_NAME.c_str());
        for (auto it : ALLOC_SIZE_MAP) {
            fprintf(out, "%ld\t%d\n", it.second, it.first);
        }
    }
    fclose(out);
    return MEM_MAP;
}
map<string,long> PrintBPF(unsigned long id){
    map<string,long> msl;
    switch((int)log2(id)){
        case 0: return PrintFlame();
        case 1: return PrintThread(SAMPLE_TOP_N);
        case 2: return PrintTopMethods(SAMPLE_TOP_N);
    }
    return msl;
}
vector<string> parse_options(string str, char sep){
    istringstream ss(str);
    vector<string> kv;
    string sub;
    while (getline(ss,sub,sep)){
        kv.push_back(sub);
    }
    return kv;
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
void registerCapa(jvmtiEnv* jvmti, int alloc){
    jvmtiCapabilities capa = {0};
    capa.can_tag_objects = 1;
    capa.can_generate_all_class_hook_events = 1;
    capa.can_generate_compiled_method_load_events = 1;
    if(alloc>0) capa.can_generate_sampled_object_alloc_events = 1;
    //capa.can_generate_garbage_collection_events = 1;
    //capa.can_generate_object_free_events = 1;
    //capa.can_generate_vm_object_alloc_events = 1;
    jvmti->AddCapabilities(&capa);
}
void registerCall(jvmtiEnv* jvmti, int alloc){
    jvmtiEventCallbacks call = {0};
    //call.GarbageCollectionStart = GarbageCollectionStart;
    //call.GarbageCollectionFinish = GarbageCollectionFinish;
    //call.VMInit = VMInit;
    call.CompiledMethodLoad = CompiledMethodLoad;
    call.CompiledMethodUnload = CompiledMethodUnload;
    call.DynamicCodeGenerated = DynamicCodeGenerated;
    if(alloc>0) call.SampledObjectAlloc = SampledObjectAlloc;
    jvmti->SetEventCallbacks(&call, sizeof(call));
}
void disableAllEvents(){
    //jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_GARBAGE_COLLECTION_START, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_VM_INIT, NULL);
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);
}
void enableEvent(jvmtiEnv* jvmti, int alloc){
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_START, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, NULL);
    //jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);
    if(alloc>0) jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);

    jvmti->GenerateEvents(JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
    jvmti->GenerateEvents(JVMTI_EVENT_COMPILED_METHOD_LOAD);
}
void setup_jvmti(int alloc){
    registerCapa(jvmti,alloc);
    registerCall(jvmti,alloc);
    enableEvent(jvmti,alloc);
    if(SAMPLE_ALLOC_INTERVAL>0) jvmti->SetHeapSamplingInterval(SAMPLE_ALLOC_INTERVAL);
    else jvmti->SetHeapSamplingInterval(10*1024*1024); //default 10m
}
void read_cfg(string filename){
    //java.util.HashMap.resize		java.util.HashMap$I^DEFAULT_INITIAL_CAPACITY    *2<2048
    //java.util.HashMap.resize		Main$increaseMap()
    ifstream in(filename.c_str());
    if(!in) cerr<<"Error open cfg file:"<<filename<<endl;
    string str;
    while(getline(in,str)){
        if(str.size()>0) {
            TUNE_RULES.push_back(str);
            cout<<"rule: "<<str<<endl;
        }
    }
    in.close();
}
// 1s  -> 1000ms
// 1ms -> 1000us
// 1us -> 1000ns
long get_num_from_str(string str){
    if( str_ends_with(str,"ns") ){
        str.pop_back();str.pop_back();
        return stol(str);
    }else if( str_ends_with(str,"us") ){
        str.pop_back();str.pop_back();
        return stol(str)*1000;
    }else if( str_ends_with(str,"ms") ){
        str.pop_back();str.pop_back();
        return stol(str)*1000*1000;
    }else if( str_ends_with(str,"s") ){
        str.pop_back();
        return stol(str)*1000*1000*1000;
    }else if( str_ends_with(str,"k") ){
        str.pop_back();
        return stol(str)*1024;
    }else if( str_ends_with(str,"m") ){
        str.pop_back();
        return stol(str)*1024*1024;
    }else if( str_ends_with(str,"g") ){
        str.pop_back();
        return stol(str)*1024*1024*1024;
    }
    return 0;
}
int do_single_options(string k, string v){
    if (k.compare("sample_duration") == 0){
        DURATION=stoi(v);
    }else if(k.compare("monitor_duration")==0){
        MON_DURATION=stoi(v);
    }else if(k.compare("frequency")==0){
        BPF_PERF_FREQ=stoi(v);
    }else if(k.compare("log_file")==0){
        out = fopen(v.c_str(), "w");
    }else if(k.compare("alloc_class_size")==0){
	ALLOC_SIZE_CLASS_NAME = v;
        if(v.find('(') != string::npos){
            ALLOC_SIZE_CLASS_NAME_HAS_QUOTE=true;
        }
    }else if(k.compare("sample_alloc_interval")==0){
        SAMPLE_ALLOC_INTERVAL=get_num_from_str(v);
    }else if(k.compare("sample_alloc")==0){
        setup_jvmti(1);
        SAMPLE_ALLOC_N=stoi(v);
        return 0;
    }else if(k.compare("flame")==0){
        setup_jvmti(0);
        out = fopen(v.c_str(), "w");
        return 1;
    }else if(k.compare("sample_thread")==0){
        SAMPLE_TOP_N=stoi(v);
        return 2;
    }else if(k.compare("sample_top")==0){
        SAMPLE_TOP_N=stoi(v);
        setup_jvmti(0);
        return 4;
    }else if(k.compare("lat_name")==0){
        LAT_NAME=v;
    }else if(k.compare("lat_top")==0){
        LAT_TOP_N=stoi(v);
    }else if(k.compare("count_top")==0){
        COUNT_TOP_N=stoi(v);
    }else if(k.compare("config_rules")==0){
        read_cfg(v);
    }else if(k.compare("action_n")==0){
        TUNING_N=stoi(v);
    }else if(k.compare("wait")==0){
        WAIT=stoi(v);
    }else if(k.compare("start_until")==0){
        UNTIL=false;
	    UNTIL_TEXT=v;
    }
    return -1;
}
void InitFile() {
    out = stdout;
    string spid = to_string(PID);
    string path = "/tmp/perf-"+spid+".map";
    cout << "perf map: "<< path<< endl;
    out_perf = fopen(path.c_str(),"w");
}
template<typename K, typename V>
void print_map(map<K,V> const &m){
    for (auto const& pair: m) {
        std::cout << "{" << pair.first << ": " << pair.second << "}\n";
    }
}
void print_vector(vector<string> v){
    cout<<"print vector: ---------------"<<endl;
    for(string s : v){
        cout<<"    "<<s<<endl;
    }
}
void modify_field(vector<string> field){
    vector<string> cls_vec = str_2_vec(field[1],'$');
    string class_name = replace_string(cls_vec[0], '.', '/');
    vector<string> fld_vec = str_2_vec(cls_vec[1],'^');
    string field_name = fld_vec[1];
    string field_type = fld_vec[0];
    string f1 = field[2];
    string algo ;
    string max ;
    if( string_contains(f1,">") ){
        vector<string> fv = str_2_vec(field[2],'>');
        algo = fv[0];
        max = fv[1];
    }else{
        vector<string> fv = str_2_vec(field[2],'<');
        algo = fv[0];
        max = fv[1];
    }
    //cout<<"----------cls='"<< class_name<<"' f='"<< field_name<<"' t='"<< field_type<<"' a='"<< algo<<"' m='"<< max<<"'"<<endl;
    tune_static(jni, class_name, field_name, field_type, max, algo);
}
void exec_method(string method){
    cout<<"executing method: "<<method<<endl;
    vector<string> cls_vec = str_2_vec(method,'$');
    string class_name = replace_string(cls_vec[0], '.', '/');
    vector<string> mtd_vec = str_2_vec(cls_vec[1],'^');
    string method_name = mtd_vec[1];
    method_name.erase(method_name.length()-2);
    string mod = mtd_vec[0];
    //cout<<"class: "<<class_name<<"  method: "<<method_name<<"  mod:"<<mod<<endl;
    exec_static_void(jni,class_name,method_name,mod);
}
vector<string> merge_vec(vector<string> v1, vector<string> v2){
    v1.insert( v1.end(), v2.begin(), v2.end() );
    return v1;
}
vector<string> parse_cond(string s){
    vector<string> vs;
    if( s[0] == 'M' ) {
        vs.push_back("M");
	s.erase(0,2);
    }else if( s[0] == 'T' ) {
        vs.push_back("T");
	s.erase(0,2);
    }
    if( s.find(">") != string::npos ) {
        vs=merge_vec(vs, str_2_vec(s,'>'));
        vs.push_back(">");
    }else if( s.find("<") != string::npos ) {
        vs=merge_vec(vs, str_2_vec(s,'<'));
        vs.push_back("<");
    }else{  //no > <
        vs.push_back(s);
    }
    return vs;
}
void tune_all_fields(vector<string> TUNE_OPTIONS, map<string,long> results){
    if(TUNE_OPTIONS.size()>0){
        for(string line : TUNE_OPTIONS){
            vector<string> field = str_2_vec(line,'\t');
            //M:java.util.HashMap.resize	java.util.HashMap$I^DEFAULT_INITIAL_CAPACITY    *2<2048
            //M:java.util.HashMap.resize	Main$IncreaseInitMapSize()
            //M:java.util.HashMap.resize>1s	Main$IncreaseInitMapSize()
	    //T:java>99
            vector<string> vec_cond = parse_cond(field[0]);
            print_vector(vec_cond);
	    map<string,long>::iterator it = results.find(vec_cond[1]);
	    if( it != results.end()) {
                long num = it->second;
                if(vec_cond.size()>3 && num>0 ){
		    long threshold = get_num_from_str(vec_cond[2]);
		    //cout<<"condition check : ------> "<<num<<" --- "<<threshold<<endl;
		    if( vec_cond[3] == ">" && num < threshold ) continue;
		    else if(vec_cond[3] == "<" && num > threshold) continue;
		    cout<<"condition check success: ------> "<<num<<" : "<<threshold<<endl;
		}
                //print_map(results);
                if( str_ends_with (field[1], "()") ) exec_method(field[1]);
                else modify_field(field);
            }
        }
    }
}
//return -1 if found
//return 1 if not found, sleep
int file_search(string fn, string& text){
    string line;
    int pos;
    ifstream file;
    file.open(fn);
    while(file.good()){
        getline(file,line);
        pos=line.find(text);
	if(pos!=string::npos){
            cout<<"found text="<<text<<endl;
            return -3;
        }
    }
    file.close();
    //cout<<"text not found="<<text<<endl;
    return 3;
}
void wait_until(string UNTIL_TEXT){
    //cout<<"wait_until() text="<<UNTIL_TEXT<<endl;
    if(!UNTIL){
        vector<string> until_vec = str_2_vec(UNTIL_TEXT,'%');
        string fn = until_vec[0];
        string text = until_vec[1];
        while(!UNTIL){
            int t = file_search(fn, text);
            if (t>0) sleep( t );
            else UNTIL=true;
        }
    }
}
void getJNI(JavaVM* vm){
#ifdef ANDROID
    vm->AttachCurrentThread(&jni, 0);
#else
    vm->AttachCurrentThread(reinterpret_cast<void**>(&jni), 0);
#endif
    //jniEnv->CallVoidMethod(jObj, jmdOnPrepared);
    //javaVM->DetachCurrentThread();
}
void profile(int id){
    wait_until(UNTIL_TEXT);
    if(id==0){
        StopAlloc();
        map<string,long> results = PrintAlloc(SAMPLE_ALLOC_N);
        //print_vector(results);
        //tune_all_fields(TUNE_RULES, results);
    }else{
      for (int i=0;i<TUNING_N;i++){
        StartBPF(id);
        StopBPF();
        map<string,long> results = PrintBPF(id);
        //print_vector(results);
        tune_all_fields(TUNE_RULES, results);
      }
    }
}
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    cout << "|***************************************|"<< endl;
    PID = getpid();
    InitFile();
    gen_bpf_map();
    vm->GetEnv((void**) &jvmti, JVMTI_VERSION_1_0);
    getJNI(vm);
    jvmti->CreateRawMonitor("tree_lock", &tree_lock);
    int id = -1;
    if (options != NULL) {
        vector<string> vkv = parse_options(string(options),';');
        for (string kv : vkv){
            string k = get_key(kv, "=");
            string v = get_value(kv, "=");
            cout << k << "=" << v << endl;
            int i = do_single_options(k,v);
            if (i>-1) id=i;
        }
    }
    cout << "|************* sleep "<<WAIT<<"s **************|"<< endl;
    sleep(WAIT);
    if(ATTACH){
        cout<<"Attach Mode: "<<endl;
        profile(id);
    }else{
        cout<<"Agent Mode: "<<endl;
        //profile(id);
        thread p = thread(profile,id);
        p.detach();
    }
    cout << "Main JVMTI Done."<< endl;
    return 0;
}
void closeAllFiles() {
    fclose(out);
    fclose(out_perf);
}
JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm){
    cout<<"Agent Unload."<<endl;
    disableAllEvents();
    closeAllFiles();
}
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (jvmti != NULL) return 0;
    ATTACH = true;
    return Agent_OnLoad(vm, options, reserved);
}

static int check_socket(int pid) {
    string path = "/tmp/.java_pid"+to_string(pid);
    //cout<<"check_socket: "<<path<<endl;
    const char* cpath = path.c_str();
    struct stat stats;
    return stat(cpath, &stats) == 0 && S_ISSOCK(stats.st_mode);
}
static int connect_socket(int pid) {
    int fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    int bytes = snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/.java_pid%d", pid);
    if (bytes >= sizeof(addr.sun_path)) {
        addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }
    //cout<<"connected socket: "<<fd<<" "<<addr.sun_path<<endl;
    return fd;
}
static int create_attach_socket(int pid) {
    string path = "/proc/"+to_string(pid)+"/cwd/.attach_pid"+to_string(pid);
    const char* cpath = path.c_str();
    int fd = creat(cpath, 0660);
    //cout<<"create ("<<fd<<") '"<<cpath<<"'"<<endl;
    if (fd == -1 || close(fd) == 0 ) {
        path = ("/tmp/.attach_pid"+to_string(pid)).c_str();
        fd = creat(cpath, 0660);
        //cout<<"create ("<<fd<<") "<<cpath<<endl;
        if (fd == -1) {
            return 0;
        }
        close(fd);
    }
    kill(pid, SIGQUIT);
    //cout<<"kill "<<pid<<endl;
    
    int result;
    struct timespec ts = {0, 300*1000*1000};
    int retry = 0;
    do {
        nanosleep(&ts, NULL);
        result = check_socket(pid);
    } while (!result && ++retry < 10);
    //cout<<"check_socket: result="<<result<<" retry="<<retry<<endl;
    unlink(cpath);
    return result;
}
static void CMD(int s, string m){
    int i = write(s, m.c_str(), m.size()+1);
}
static int write_command(int s, string opts) {
    //<ver>0<cmd>0<arg>0<arg>0<arg>0
    //msleep(10);
    CMD(s, "1");
    vector<string> vo = str_2_vec(opts,' ');
    for (string o : vo){
	cout<<"("<<o.size()<<")\t"<<o<<endl;
        CMD(s, o);
    }
    CMD(s, "");
    return 1;
}

static int read_response(int fd) {
    char buf[8192];
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    if (bytes <= 0) {
        perror("Error reading response");
        return 1;
    }

    buf[bytes] = 0;
    int result = atoi(buf);

    do {
        fwrite(buf, 1, bytes, stdout);
        bytes = read(fd, buf, sizeof(buf));
    } while (bytes > 0);

    return result;
}

int main(int argc, char* argv[]) {
    // VirtualMachineImpl.java
    if (argc < 4) {
        printf("jBProF v" __DATE__ "Copyright 2020 Weixing.Sun@Gmail.Com\nUsage: ./jbprof <pid> <agent> <opt> \n");
        return 1;
    }
    int pid = atoi(argv[1]);
    string opts = "load "+string(argv[2])+" true "+string(argv[3]);
    //cout<<"write_cmd: "<<opts<<endl;
    signal(SIGPIPE, SIG_IGN);
    if (!check_socket(pid) && !create_attach_socket(pid)) {
        perror("Cannot create socket");
        return 1;
    }

    int fd = connect_socket(pid);
    if (fd == -1) {
        perror("Could not connect to socket");
        return 1;
    }
    
    printf("Connected to remote JVM: %d\n",pid);
    if (!write_command(fd, opts)) {
        perror("Error writing to socket");
        close(fd);
        return 1;
    }

    fflush(stdout);
    int result = read_response(fd);
    //cout<<"response: "<<result<<endl;
    close(fd);

    return result;
}

