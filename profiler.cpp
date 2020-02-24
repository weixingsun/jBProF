#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <stack>
#include <iostream>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <bcc/BPF.h>
#include <jvmti.h>

using namespace std;

#define MAX_STACK_DEPTH 128
static bool writing_perf = false;
static FILE* out_cpu;
static FILE* out_mem;
static FILE* out_perf;
static jlong start_time;
static jrawMonitorID vmtrace_lock;
static jvmtiEnv* jvmti = NULL;
static jrawMonitorID tree_lock;
static int duration = 10;
static ebpf::BPF bpf;
static int PERF_TYPE_SOFTWARE = 1;
static int PERF_COUNT_HW_CPU_CYCLES = 0;

struct Frame {
    jlong samples;
    jlong bytes;
    map<jmethodID, Frame> children;
};
static map<string, Frame> root;

// Define the same struct to use in user space.
struct stack_key_t {
  int pid;
  uint64_t kernel_ip;
  uint64_t kernel_ret_ip;
  int user_stack_id;
  int kernel_stack_id;
  char name[16];
};

string BPF_TXT = R"(
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

int do_perf_event(struct bpf_perf_event_data *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = id >> 32;
    u32 pid = id;
    if (pid == 0) return 0;
    if (!PID) return 0;
    struct stack_key_t key = {.pid = tgid};
    bpf_get_current_comm(&key.name, sizeof(key.name));
    // get stacks
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
/*
static void trace(jvmtiEnv* jvmti, const char* fmt, ...) {
    jlong current_time;
    jvmti->GetTime(&current_time);

    char buf[MAX_STACK_DEPTH];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    jvmti->RawMonitorEnter(vmtrace_lock);

    fprintf(out_mem, "[%.5f] %s\n", (current_time - start_time) / 1000000000.0, buf);
    
    jvmti->RawMonitorExit(vmtrace_lock);
}
*/

// Converts JVM internal class signature to human readable name
static string decode_class_signature(char* class_sig) {
    switch (class_sig[0]) {
        case 'B': return "byte";
        case 'C': return "char";
        case 'D': return "double";
        case 'F': return "float";
        case 'I': return "int";
        case 'J': return "long";
        case 'S': return "short";
        case 'Z': return "boolean";
        case '[': return decode_class_signature(class_sig + 1) + "[]";
    }
    // rm first 'L' and last ';'
    class_sig++;
    class_sig[strlen(class_sig) - 1] = 0;

    // Replace '/' with '.'
    for (char* c = class_sig; *c; c++) {
        if (*c == '/') *c = '.';
    }

    return class_sig;
}

static string get_method_name(jmethodID method) {
    jclass method_class;
    char* class_sig = NULL;
    char* method_name = NULL;
    string result;

    if (jvmti->GetMethodDeclaringClass(method, &method_class) == 0 &&
        jvmti->GetClassSignature(method_class, &class_sig, NULL) == 0 &&
        jvmti->GetMethodName(method, &method_name, NULL, NULL) == 0) {
        result.assign(decode_class_signature(class_sig) + "." + method_name);
    } else {
        result.assign("(NONAME)");
    }

    jvmti->Deallocate((unsigned char*) method_name);
    jvmti->Deallocate((unsigned char*) class_sig);
    return result;
}

static void dump_tree(const string stack_line, const string& class_name, const Frame* f) {
    if (f->samples > 0) {
        //cout << stack_line << class_name << "_[i] " << f->samples << endl;
        fprintf(out_mem, "%s %s_[%ld] \n", stack_line.c_str(), class_name.c_str(), f->samples);
    }
    for (auto it = f->children.begin(); it != f->children.end(); ++it) {
        dump_tree(stack_line + get_method_name(it->first) + ";", class_name, &it->second);
    }
}

static void dump_profile() {
    for (auto it = root.begin(); it != root.end(); ++it) {
        dump_tree("", it->first, &it->second);
    }
}

static void record_stack_trace(char* class_sig, jvmtiFrameInfo* frames, jint count, jlong size) {
    Frame* f = &root[decode_class_signature(class_sig)];
    while (--count >= 0) {
        f = &f->children[frames[count].method];
    }
    f->samples++;
    f->bytes += size;
}

void JNICALL SampledObjectAlloc(jvmtiEnv* jvmti, JNIEnv* env, jthread thread,
                                jobject object, jclass object_klass, jlong size) {

    jvmtiFrameInfo frames[MAX_STACK_DEPTH];
    jint count;
    if (jvmti->GetStackTrace(thread, 0, MAX_STACK_DEPTH, frames, &count) != 0) {
        return;
    }

    char* class_sig;
    if (jvmti->GetClassSignature(object_klass, &class_sig, NULL) != 0) {
        return;
    }

    jvmti->RawMonitorEnter(tree_lock);
    record_stack_trace(class_sig, frames, count, size);
    jvmti->RawMonitorExit(tree_lock);

    jvmti->Deallocate((unsigned char*) class_sig);
}

void JNICALL DataDumpRequest(jvmtiEnv* jvmti) {
    jvmti->RawMonitorEnter(tree_lock);
    dump_profile();
    jvmti->RawMonitorExit(tree_lock);
}

void JNICALL VMDeath(jvmtiEnv* jvmti, JNIEnv* env) {
    DataDumpRequest(jvmti);
}

void JNICALL GarbageCollectionStart(jvmtiEnv *jvmti) {
    DataDumpRequest(jvmti);
}

void JNICALL GarbageCollectionFinish(jvmtiEnv *jvmti_env) {
    DataDumpRequest(jvmti);
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
/////////////////////////////////////////////////////////////////////////
bool str_replace(string& str, const string& from, const string& to) {
    size_t start_pos = str.find(from);
    if(start_pos == std::string::npos)
        return false;
    str.replace(start_pos, from.length(), to);
    return true;
}

void StartBPF() {
    //ebpf::BPF bpf;
    int pid = getpid();
    const string PID = "(tgid=="+to_string(pid)+")";
    str_replace(BPF_TXT, "PID", PID);
    //cout << "BPF:" << endl << BPF_TXT << endl;

    auto init_r = bpf.init(BPF_TXT);
    if (init_r.code() != 0) {
        cerr << init_r.msg() << endl;
    }
    int pid2=-1;
    auto att_r = bpf.attach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES, "do_perf_event", 99, 0, pid2);
    if (att_r.code() != 0) {
        cerr << att_r.msg() << endl;
    }else{
        cout << "attached to pid:" << pid << " perf event "<< endl;
    }
}
void StopBPF(){
    cout << "BPF sampling " << duration << " seconds" << endl;
    sleep(duration);
    if (out_perf!=NULL){
        writing_perf=true;
        fclose(out_perf);
        writing_perf=false;
        out_perf=NULL;
    }
    bpf.detach_perf_event(PERF_TYPE_SOFTWARE, PERF_COUNT_HW_CPU_CYCLES);
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
        //fprintf(out_cpu, "%s;", string(it.first.name).c_str());
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
void registerMemoryCapa(jvmtiEnv* jvmti){
    jvmtiCapabilities capa = {0};
    capa.can_tag_objects = 1;
    capa.can_generate_sampled_object_alloc_events = 1;
    capa.can_generate_all_class_hook_events = 1;
    capa.can_generate_compiled_method_load_events = 1;
    capa.can_generate_garbage_collection_events = 1;
    capa.can_generate_object_free_events = 1;
    capa.can_generate_vm_object_alloc_events = 1;
    jvmti->AddCapabilities(&capa);
}
void registerMemoryCall(jvmtiEnv* jvmti){
    jvmtiEventCallbacks call = {0};
    call.SampledObjectAlloc = SampledObjectAlloc;
    call.DataDumpRequest = DataDumpRequest;
    call.VMDeath = VMDeath;
    call.GarbageCollectionStart = GarbageCollectionStart;
    call.GarbageCollectionFinish = GarbageCollectionFinish;
    call.CompiledMethodLoad = CompiledMethodLoad;
    call.CompiledMethodUnload = CompiledMethodUnload;
    call.DynamicCodeGenerated = DynamicCodeGenerated;
    jvmti->SetEventCallbacks(&call, sizeof(call));
}
void enableMemoryEvent(jvmtiEnv* jvmti){
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_SAMPLED_OBJECT_ALLOC, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DATA_DUMP_REQUEST, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_START, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, NULL);

    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);

    jvmti->GenerateEvents(JVMTI_EVENT_DYNAMIC_CODE_GENERATED);
    jvmti->GenerateEvents(JVMTI_EVENT_COMPILED_METHOD_LOAD);
}
void do_options(string k, string v, jvmtiEnv* jvmti){
    if (k.compare("duration") == 0){
        duration=stoi(v);
    }else if(k.compare("sample_cpu")==0){
        StartBPF();
        out_cpu = fopen(v.c_str(), "w");
    }else if(k.compare("sample_mem")==0){
        registerMemoryCapa(jvmti);
        jvmti->SetHeapSamplingInterval(1024*1024); //1m
        registerMemoryCall(jvmti);
	enableMemoryEvent(jvmti);
        out_mem = fopen(v.c_str(), "w");
    }
}
void InitFile() {
    out_mem = stderr;
    out_cpu = stdout;
    string pid = to_string(getpid());
    string path = ("/tmp/perf-"+pid+".map"); //.c_str();
    cout << "perf map: "<< path<< endl;
    out_perf = fopen(path.c_str(),"w");
}
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    InitFile();
    vm->GetEnv((void**) &jvmti, JVMTI_VERSION_1_0);
    jvmti->CreateRawMonitor("tree_lock", &tree_lock);
    if (options != NULL) {
        vector<string> vkv = parse_options(string(options),';');
        for (string kv : vkv){
	    string k = get_key(kv, "=");
	    string v = get_value(kv, "=");
            cout << "k=" << k << " v=" << v << endl;
            do_options(k,v,jvmti);
	}
    }
    StopBPF();
    fclose(out_mem);
    return 0;
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (jvmti != NULL) {
        return 0;
    }
    return Agent_OnLoad(vm, options, reserved);
}
