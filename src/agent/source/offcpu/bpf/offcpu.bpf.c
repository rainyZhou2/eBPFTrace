// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include <stdbool.h>
#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define TASK_COMM_LEN 16
#define MAX_STACK_DEPTH 64
#define MAX_HOPS 4
#define TASK_RUNNING 0
#define TLS_MAGIC 0x45545243u
#define TLS_VERSION 1

#define REASON_BLOCKED 0
#define REASON_PREEMPTED 1
#define REASON_GPU 2

/* Indexes of the integer argument registers in pt_regs, and the offset of
 * the first stack argument from the stack pointer at function entry. */
#if defined(__TARGET_ARCH_x86)
#define UPROBE_ARG_REG(n) ((n) == 0 ? 14 : (n) == 1 ? 13 : (n) == 2 ? 12 : (n) == 3 ? 11 : (n) == 4 ? 9 : 8)
#define UPROBE_REG_ARGS 6
#define UPROBE_STACK_ARGS 8 /* above the return address */
#define UPROBE_SP 19
#elif defined(__TARGET_ARCH_arm64)
#define UPROBE_ARG_REG(n) (n)
#define UPROBE_REG_ARGS 8
#define UPROBE_STACK_ARGS 0
#define UPROBE_SP 31
#else
#error "unsupported target architecture"
#endif

#define NMI_MASK 0x00f00000
#define HARDIRQ_MASK 0x000f0000
#define SOFTIRQ_OFFSET 0x00000100

struct thread_info {
	__u64 preempt_count;
} __attribute__((preserve_access_index));

struct task_struct {
	struct thread_info thread_info;
	int pid;
	int tgid;
	char comm[TASK_COMM_LEN];
	unsigned int __state;
	void *mm;
} __attribute__((preserve_access_index));

struct task_struct___pre_5_14 {
	long state;
} __attribute__((preserve_access_index));

struct trace_ctx {
	__u8 trace_id[16];
	__u8 span_id[8];
};

struct user_tls {
	__u32 magic;
	__u32 version;
	__u64 seq;
	struct trace_ctx ctx;
};

struct tls_ref {
	__u64 addr;
	__u32 tgid;
	__u32 _pad;
};

struct hop {
	__u32 pid;
	__u32 tgid;
	char comm[TASK_COMM_LEN];
	__u64 wake_kstack;
	__u64 wake_ustack;
	__u64 wait_kstack;
	__u64 wait_ustack;
	__u64 wait_ns;
	struct trace_ctx trace;
	__u8 has_trace;
	__u8 has_wait;
	__u8 _pad[6];
};

struct chain {
	__u32 n;
	__u32 _pad;
	struct hop hops[MAX_HOPS];
};

struct stack {
	__u32 len;
	__u32 _pad;
	__u64 ips[MAX_STACK_DEPTH];
};

struct start_rec {
	__u64 ts;
	__u32 tgid;
	__u32 _pad0;
	__u64 ustack;
	__u64 kstack;
	struct trace_ctx trace;
	__u8 reason;
	__u8 has_trace;
	__u8 in_gpu;
	__u8 _pad[5];
};

/* A thread inside a host-blocking CUDA call. Off-CPU intervals that start
 * while it is set are folded into the GPU wait instead of being counted on
 * their own; the chain of the last one becomes the GPU wait's chain. obj is
 * the stream or event the call waits on, per argkind. */
struct gpu_wait {
	__u64 ts;
	__u64 sp;
	__u64 ustack;
	__u64 obj;
	struct trace_ctx trace;
	__u32 tgid;
	__u8 api;
	__u8 depth;
	__u8 has_trace;
	__u8 has_chain;
	__u8 argkind;
	__u8 _pad[7];
	struct chain chain;
};

/* Sync attach cookies carry the API index in bits 0-7 and what argument 0
 * is in bits 8-15. Calls without a stream or event wait for the whole
 * process's work, like the legacy default stream. */
#define GPU_ARG_NONE 0
#define GPU_ARG_STREAM 1
#define GPU_ARG_EVENT 2

/* Launch attach cookies select the argument layout of the launch call. */
#define LAUNCH_CU 1	  /* cuLaunchKernel(f, grid x3, block x3, shmem, stream, ...) */
#define LAUNCH_CU_EX 2	  /* cuLaunchKernelEx(config, f, ...) */
#define LAUNCH_CUDA 3	  /* cudaLaunchKernel(stub, dim3 grid, dim3 block, args, shmem, stream) */
#define LAUNCH_CUDA_EX 4  /* cudaLaunchKernelExC(config, stub, args) */
#define LAUNCH_CU_GRAPH 5  /* cuGraphLaunch(exec, stream) */
#define LAUNCH_CUDA_GRAPH 6  /* cudaGraphLaunch(exec, stream) */
#define LAUNCH_CONFIG_STREAM 32 /* offset of the stream in both launch configs */

/* What a recorded kernel identifier is. A runtime stub is the host address
 * of the __global__ function and symbolizes to the kernel name; a driver
 * function handle is named through fn_names. */
#define KERNEL_STUB 1
#define KERNEL_FUNC 2
#define KERNEL_GRAPH 3

/* A runtime launch calls the driver itself. The launch entry probe opens a
 * scope in the calling thread and the matching uretprobe closes it; a driver
 * launch inside that scope is the runtime's own call and is not counted
 * again. The timeout only guards a probe that never returns (a thread that
 * dies inside the call, a tail call that skips the return probe). */
#define LAUNCH_SCOPE_TIMEOUT_NS 1000000000ULL

#define KERNEL_NAME_LEN 128

/* The last kernel queued on a stream or by a process, and how many were
 * queued since the last wait on it. */
struct launch {
	__u64 kernel;
	__u64 ts;
	__u32 tid;
	__u32 count;
	__u8 kind;
	__u8 _pad[7];
};

/* Per-thread nesting of runtime launches, see LAUNCH_SCOPE_TIMEOUT_NS. */
struct launch_scope {
	__u32 depth;
	__u32 _pad;
	__u64 ts;
};

struct handle_key {
	__u32 tgid;
	__u32 _pad;
	__u64 handle;
};

struct kernel_name {
	char name[KERNEL_NAME_LEN];
};

struct fn_lookup {
	__u64 out;
	__u64 name;
};

struct nccl_mark {
	__u64 ts;
	__u8 op;
	__u8 _pad[7];
};

struct last_wait {
	__u64 start;
	__u64 end;
	__u64 kstack;
	__u64 ustack;
	struct chain chain;
};

struct offcpu_key {
	__u32 pid;
	__u32 tgid;
	__u64 ustack;
	__u64 kstack;
	__u64 chain_id;
	char comm[TASK_COMM_LEN];
	__u8 reason;
	__u8 gpu_api;
	__u8 gpu_op;
	__u8 gpu_kernel_kind;
	__u8 _pad[4];
	__u64 gpu_kernel;
};

struct offcpu_val {
	__u64 total_ns;
	__u64 count;
	__u64 hop_wait_ns[MAX_HOPS];
};

struct event {
	__u64 start_ns;
	__u64 end_ns;
	__u32 pid;
	__u32 tgid;
	char comm[TASK_COMM_LEN];
	__u64 kstack;
	__u64 ustack;
	struct trace_ctx trace;
	__u8 reason;
	__u8 gpu_api;
	__u8 gpu_op;
	__u8 gpu_kernel_kind;
	__u8 _pad[4];
	__u64 gpu_kernel;
	__u64 gpu_stream;
	__u32 gpu_launches;
	__u32 _pad2;
	struct chain chain;
};

const volatile __u32 target_tgid = 0;
const volatile __u64 min_block_ns = 1000000;
const volatile __u64 max_block_ns = 0xffffffffffffffffULL;
const volatile __u8 include_preempted = 0;
const volatile __u8 capture_user_stacks = 1;
const volatile __u8 emit_traced_events = 1;
const volatile __u64 slow_event_ns = 0;
const volatile __u32 chain_depth = MAX_HOPS;
const volatile __u64 gpu_min_wait_ns = 1000000;

#define GPU_WAIT_TIMEOUT_NS 60000000000ULL

#define DEFINE_MAP(_name, _type, _key, _value, _max)	\
	struct {					\
		__uint(type, _type);			\
		__uint(max_entries, _max);		\
		__type(key, _key);			\
		__type(value, _value);			\
	} _name SEC(".maps")

DEFINE_MAP(starts, BPF_MAP_TYPE_LRU_HASH, __u32, struct start_rec, 65536);
DEFINE_MAP(wakes, BPF_MAP_TYPE_LRU_HASH, __u32, struct chain, 16384);
DEFINE_MAP(last_waits, BPF_MAP_TYPE_LRU_HASH, __u32, struct last_wait, 16384);
DEFINE_MAP(tls_reg, BPF_MAP_TYPE_LRU_HASH, __u32, struct tls_ref, 65536);
DEFINE_MAP(stacks, BPF_MAP_TYPE_LRU_HASH, __u64, struct stack, 32768);
DEFINE_MAP(chains, BPF_MAP_TYPE_LRU_HASH, __u64, struct chain, 16384);
DEFINE_MAP(counts_0, BPF_MAP_TYPE_HASH, struct offcpu_key, struct offcpu_val, 65536);
DEFINE_MAP(counts_1, BPF_MAP_TYPE_HASH, struct offcpu_key, struct offcpu_val, 65536);
DEFINE_MAP(scratch_stack, BPF_MAP_TYPE_PERCPU_ARRAY, __u32, struct stack, 1);
DEFINE_MAP(scratch_chain, BPF_MAP_TYPE_PERCPU_ARRAY, __u32, struct chain, 2);
DEFINE_MAP(scratch_wait, BPF_MAP_TYPE_PERCPU_ARRAY, __u32, struct last_wait, 1);
DEFINE_MAP(scratch_gpu, BPF_MAP_TYPE_PERCPU_ARRAY, __u32, struct gpu_wait, 1);
DEFINE_MAP(gpu_waits, BPF_MAP_TYPE_LRU_HASH, __u32, struct gpu_wait, 4096);
DEFINE_MAP(nccl_marks, BPF_MAP_TYPE_LRU_HASH, __u32, struct nccl_mark, 4096);
DEFINE_MAP(gpu_sync_ends, BPF_MAP_TYPE_LRU_HASH, __u32, __u64, 4096);
DEFINE_MAP(stream_launches, BPF_MAP_TYPE_LRU_HASH, struct handle_key, struct launch, 16384);
DEFINE_MAP(proc_launches, BPF_MAP_TYPE_LRU_HASH, __u32, struct launch, 4096);
DEFINE_MAP(event_streams, BPF_MAP_TYPE_LRU_HASH, struct handle_key, __u64, 16384);
DEFINE_MAP(fn_lookups, BPF_MAP_TYPE_LRU_HASH, __u32, struct fn_lookup, 4096);
DEFINE_MAP(kernel_names, BPF_MAP_TYPE_LRU_HASH, struct handle_key, struct kernel_name, 16384);
DEFINE_MAP(launch_hits, BPF_MAP_TYPE_PERCPU_ARRAY, __u32, __u64, 1);
DEFINE_MAP(launch_scope, BPF_MAP_TYPE_LRU_HASH, __u32, struct launch_scope, 65536);
DEFINE_MAP(drops, BPF_MAP_TYPE_ARRAY, __u32, __u64, 8);
DEFINE_MAP(generation, BPF_MAP_TYPE_ARRAY, __u32, __u32, 1);

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 8 << 20);
} events SEC(".maps");

enum drop_reason {
	DROP_STACK = 0,
	DROP_COUNTS = 1,
	DROP_START = 2,
	DROP_WAKE = 3,
	DROP_EVENT = 4,
	DROP_CHAIN = 5,
	DROP_GPU = 6,
};

static __always_inline void record_drop(__u32 reason)
{
	__u64 *v = bpf_map_lookup_elem(&drops, &reason);
	if (v)
		__sync_fetch_and_add(v, 1);
}

#if defined(__TARGET_ARCH_x86)
struct pcpu_hot {
	int preempt_count;
} __attribute__((preserve_access_index));

extern void __per_cpu_offset __ksym __weak;
extern void __preempt_count __ksym __weak;
extern void pcpu_hot __ksym __weak;

static __always_inline __u32 preempt_count(void)
{
	__u64 base = (__u64)&__per_cpu_offset;
	__u64 addr = (__u64)&__preempt_count;
	__u64 off;
	int pc;

	if (!base)
		return 0;
	if (!addr && (__u64)&pcpu_hot && bpf_core_type_exists(struct pcpu_hot))
		addr = (__u64)&pcpu_hot + bpf_core_field_offset(struct pcpu_hot, preempt_count);
	if (!addr)
		return 0;
	if (bpf_probe_read_kernel(&off, sizeof(off), (void *)(base + (__u64)bpf_get_smp_processor_id() * 8)))
		return 0;
	if (bpf_probe_read_kernel(&pc, sizeof(pc), (void *)(addr + off)))
		return 0;
	return pc;
}
#else
static __always_inline __u32 preempt_count(void)
{
	struct task_struct *t = (void *)bpf_get_current_task_btf();
	return (__u32)BPF_CORE_READ(t, thread_info.preempt_count);
}
#endif

static __always_inline bool wanted(struct task_struct *t)
{
	if (t->pid == 0)
		return false;
	return target_tgid == 0 || (__u32)t->tgid == target_tgid;
}

static __always_inline long task_state(struct task_struct *t)
{
	struct task_struct___pre_5_14 *old = (void *)t;

	if (bpf_core_field_exists(t->__state))
		return BPF_CORE_READ(t, __state);
	return BPF_CORE_READ(old, state);
}

static __always_inline __u64 mix(__u64 h, __u64 v)
{
	h ^= v;
	h *= 0x100000001b3ULL;
	return h;
}

/* Stores the stack held in the per-CPU scratch slot under its content hash. */
static __always_inline __u64 intern_stack(struct stack *s, __u64 flags)
{
	__u64 h = 0xcbf29ce484222325ULL;
	for (int i = 0; i < MAX_STACK_DEPTH; i++) {
		if (i >= s->len)
			break;
		h = mix(h, s->ips[i]);
	}
	h = mix(h, flags);
	if (h == 0)
		h = 1;

	if (bpf_map_update_elem(&stacks, &h, s, BPF_NOEXIST) && !bpf_map_lookup_elem(&stacks, &h)) {
		record_drop(DROP_STACK);
		return 0;
	}
	return h;
}

static __always_inline __u64 capture_stack(void *ctx, __u64 flags)
{
	__u32 zero = 0;
	struct stack *s = bpf_map_lookup_elem(&scratch_stack, &zero);
	if (!s)
		return 0;

	long n = bpf_get_stack(ctx, s->ips, sizeof(s->ips), flags);
	if (n <= 0)
		return 0;
	s->len = n / sizeof(__u64);
	return intern_stack(s, flags);
}

#ifdef ETRACE_DWARF_UNWIND
/* User stack walker driven by .eh_frame tables that user space loads into
 * unwind_rows. Frames without table coverage fall back to frame pointers, so
 * the result is never worse than bpf_get_stack. Needs bpf_loop (5.17) and
 * bpf_task_pt_regs (5.15); the agent loads this object only where available. */

#define MAX_UNWIND_MAPPINGS 64
#define MAPPING_SEARCH_STEPS 7
#define ROW_SEARCH_STEPS 24

#define CFA_NONE 0
#define CFA_SP 1
#define CFA_FP 2

#define RULE_SAME 0
#define RULE_OFFSET 1
#define RULE_UNDEFINED 2

#if defined(__TARGET_ARCH_arm64)
/* Clears pointer authentication bits; user addresses fit in 48 bits. */
#define STRIP_RA(ra) ((ra) & 0x0000ffffffffffffULL)
#else
#define STRIP_RA(ra) (ra)
#endif

struct unwind_row {
	__u64 pc;
	__s16 cfa_offset;
	__s16 fp_offset;
	__s16 ra_offset;
	__u8 cfa;
	__u8 rules; /* fp rule in bits 0-3, return address rule in bits 4-7 */
};

/* One executable mapping. ELF virtual address = pc - bias. A mapping with
 * row_cnt 0 is known but has no table and is walked with frame pointers. */
struct unwind_mapping {
	__u64 start;
	__u64 end;
	__u64 bias;
	__u32 row_off;
	__u32 row_cnt;
};

struct proc_unwind {
	__u32 n;
	__u32 _pad;
	struct unwind_mapping maps[MAX_UNWIND_MAPPINGS];
};

struct unwind_state {
	__u64 pc;
	__u64 sp;
	__u64 fp;
	__u64 lr;
	__u32 tgid;
	__u32 len;
	__u8 missed;
	__u8 _pad[7];
};

#if defined(__TARGET_ARCH_x86)
struct pt_regs___user {
	unsigned long bp;
	unsigned long sp;
	unsigned long ip;
} __attribute__((preserve_access_index));
#else
struct pt_regs___user {
	__u64 regs[31];
	__u64 sp;
	__u64 pc;
} __attribute__((preserve_access_index));
#endif

/* Max entries are sized by the agent before load. */
DEFINE_MAP(unwind_rows, BPF_MAP_TYPE_ARRAY, __u32, struct unwind_row, 1);
DEFINE_MAP(unwind_miss, BPF_MAP_TYPE_LRU_HASH, __u32, __u64, 4096);

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, __u32);
	__type(value, struct proc_unwind);
} unwind_procs SEC(".maps");

/* Asks user space to (re)load the tables of a process. */
static __always_inline void note_miss(__u32 tgid)
{
	if (bpf_map_lookup_elem(&unwind_miss, &tgid))
		return;
	__u64 now = bpf_ktime_get_ns();
	bpf_map_update_elem(&unwind_miss, &tgid, &now, BPF_NOEXIST);
}

/* All-ones when a > b, zero otherwise. Valid because user addresses stay
 * below 2^63. The searches below select with this mask instead of branching
 * so the verifier walks one path per iteration rather than 2^steps. */
static __always_inline __u64 gt_mask(__u64 a, __u64 b)
{
	__u64 m = (__u64)((__s64)(b - a) >> 63);
	barrier_var(m);
	return m;
}

static __always_inline struct unwind_mapping *find_mapping(struct proc_unwind *p, __u64 pc)
{
	__u32 n = p->n;
	if (n == 0 || n > MAX_UNWIND_MAPPINGS)
		return NULL;
	__u32 base = 0, len = n;
	for (int i = 0; i < MAPPING_SEARCH_STEPS; i++) {
		__u32 half = len >> 1;
		__u64 start = p->maps[(base + half) & (MAX_UNWIND_MAPPINGS - 1)].start;
		base += half & ~(__u32)gt_mask(start, pc);
		len -= half;
	}
	struct unwind_mapping *m = &p->maps[base & (MAX_UNWIND_MAPPINGS - 1)];
	if (pc < m->start || pc >= m->end)
		return NULL;
	return m;
}

static __always_inline struct unwind_row *find_row(struct unwind_mapping *m, __u64 vaddr)
{
	__u32 base = m->row_off, len = m->row_cnt;
	struct unwind_row *row;

	for (int i = 0; i < ROW_SEARCH_STEPS; i++) {
		__u32 half = len >> 1;
		__u32 idx = base + half;
		row = bpf_map_lookup_elem(&unwind_rows, &idx);
		if (!row)
			return NULL;
		base += half & ~(__u32)gt_mask(row->pc, vaddr);
		len -= half;
	}
	row = bpf_map_lookup_elem(&unwind_rows, &base);
	if (!row || row->pc > vaddr)
		return NULL;
	return row;
}

/* Frame record layout is [fp] = caller fp, [fp+8] = return address on both
 * x86-64 and arm64. Caller frames live at higher addresses, which also
 * stops loops through corrupted records. */
static __always_inline long fp_step(struct unwind_state *u)
{
	__u64 rec[2];

	if (u->fp == 0 || bpf_probe_read_user(rec, sizeof(rec), (void *)u->fp))
		return 1;
	if (rec[0] != 0 && rec[0] <= u->fp)
		return 1;
	u->sp = u->fp + 16;
	u->fp = rec[0];
	u->pc = STRIP_RA(rec[1]);
	u->lr = 0;
	return u->pc == 0;
}

static __always_inline long dwarf_step(struct unwind_state *u, struct unwind_row *row)
{
	__u8 fp_rule = row->rules & 0xf, ra_rule = row->rules >> 4;
	__u64 base, cfa, ra, fp = u->fp;

	if (row->cfa == CFA_SP) {
		base = u->sp;
	} else {
		if (u->fp == 0)
			return 1;
		base = u->fp;
	}
	cfa = base + (__s64)row->cfa_offset;
	if (cfa < u->sp)
		return 1;

	if (ra_rule == RULE_OFFSET) {
		if (bpf_probe_read_user(&ra, sizeof(ra), (void *)(cfa + (__s64)row->ra_offset)))
			return 1;
	} else if (ra_rule == RULE_SAME && u->lr) {
		ra = u->lr;
	} else {
		return 1;
	}
	if (fp_rule == RULE_OFFSET) {
		if (bpf_probe_read_user(&fp, sizeof(fp), (void *)(cfa + (__s64)row->fp_offset)))
			return 1;
	} else if (fp_rule == RULE_UNDEFINED) {
		fp = 0;
	}
	u->sp = cfa;
	u->fp = fp;
	u->pc = STRIP_RA(ra);
	u->lr = 0;
	return u->pc == 0;
}

static long unwind_step(__u32 i, void *arg)
{
	struct unwind_state *u = arg;
	__u32 zero = 0;
	struct stack *s = bpf_map_lookup_elem(&scratch_stack, &zero);
	if (!s || u->len >= MAX_STACK_DEPTH)
		return 1;
	s->ips[u->len & (MAX_STACK_DEPTH - 1)] = u->pc;
	u->len++;

	struct proc_unwind *p = bpf_map_lookup_elem(&unwind_procs, &u->tgid);
	if (!p)
		return 1;
	/* A return address may point one past the end of a noreturn call's
	 * function, so callers are looked up at the call instruction. */
	__u64 pc = u->len == 1 ? u->pc : u->pc - 1;
	struct unwind_mapping *m = find_mapping(p, pc);
	if (!m) {
		u->missed = 1;
		return fp_step(u);
	}
	if (m->row_cnt == 0)
		return fp_step(u);
	struct unwind_row *row = find_row(m, pc - m->bias);
	if (!row || (row->cfa != CFA_SP && row->cfa != CFA_FP))
		return fp_step(u);
	return dwarf_step(u, row);
}

static __always_inline __u64 capture_user_stack(void *ctx)
{
	struct task_struct *task = (void *)bpf_get_current_task_btf();
	__u32 tgid = task->tgid;
	__u32 zero = 0;

	if (!task->mm)
		return 0;
	if (!bpf_map_lookup_elem(&unwind_procs, &tgid)) {
		note_miss(tgid);
		return capture_stack(ctx, BPF_F_USER_STACK);
	}
	struct pt_regs___user *regs = (void *)bpf_task_pt_regs((void *)task);
	if (!regs)
		return capture_stack(ctx, BPF_F_USER_STACK);

	struct unwind_state u = { .tgid = tgid };
#if defined(__TARGET_ARCH_x86)
	u.pc = BPF_CORE_READ(regs, ip);
	u.sp = BPF_CORE_READ(regs, sp);
	u.fp = BPF_CORE_READ(regs, bp);
#else
	u.pc = BPF_CORE_READ(regs, pc);
	u.sp = BPF_CORE_READ(regs, sp);
	u.fp = BPF_CORE_READ(regs, regs[29]);
	u.lr = STRIP_RA(BPF_CORE_READ(regs, regs[30]));
#endif
	if (u.pc == 0)
		return 0;
	bpf_loop(MAX_STACK_DEPTH, unwind_step, &u, 0);
	if (u.missed)
		note_miss(tgid);

	struct stack *s = bpf_map_lookup_elem(&scratch_stack, &zero);
	if (!s || u.len == 0)
		return 0;
	s->len = u.len;
	return intern_stack(s, BPF_F_USER_STACK);
}

/* The new image invalidates every table of the process. */
SEC("tp_btf/sched_process_exec")
int BPF_PROG(on_exec, struct task_struct *p)
{
	__u32 tgid = p->tgid;

	bpf_map_delete_elem(&unwind_procs, &tgid);
	return 0;
}
#else
static __always_inline __u64 capture_user_stack(void *ctx)
{
	return capture_stack(ctx, BPF_F_USER_STACK);
}
#endif

/* Returns integer argument n of a uprobed function at entry; n must be a
 * constant. Arguments past the registers are read from the user stack. */
static __always_inline __u64 uprobe_arg(void *ctx, int n)
{
	__u64 *regs = ctx;
	__u64 v = 0;

	if (n < UPROBE_REG_ARGS)
		return regs[UPROBE_ARG_REG(n)];
	bpf_probe_read_user(&v, sizeof(v), (void *)(regs[UPROBE_SP] + UPROBE_STACK_ARGS + 8 * (n - UPROBE_REG_ARGS)));
	return v;
}

/* Reads the per-thread trace context the instrumented application published.
 * The writer makes seq odd while updating; a torn or stale record is ignored. */
static __always_inline bool read_trace(__u32 tid, __u32 tgid, struct trace_ctx *out)
{
	struct tls_ref *ref = bpf_map_lookup_elem(&tls_reg, &tid);
	if (!ref || ref->tgid != tgid)
		return false;

	struct user_tls tls;
	if (bpf_probe_read_user(&tls, sizeof(tls), (void *)ref->addr))
		return false;
	if (tls.magic != TLS_MAGIC || tls.version != TLS_VERSION || (tls.seq & 1))
		return false;

	__u64 seq2;
	if (bpf_probe_read_user(&seq2, sizeof(seq2), (void *)(ref->addr + __builtin_offsetof(struct user_tls, seq))))
		return false;
	if (seq2 != tls.seq)
		return false;

	__u64 *w = (__u64 *)tls.ctx.trace_id;
	if (w[0] == 0 && w[1] == 0)
		return false;
	*out = tls.ctx;
	return true;
}

SEC("uprobe")
int on_tls_register(void *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	__u32 tid = (__u32)id;
	struct tls_ref ref = {
		.addr = uprobe_arg(ctx, 0),
		.tgid = id >> 32,
	};

	if (ref.addr == 0) {
		bpf_map_delete_elem(&tls_reg, &tid);
		return 0;
	}
	bpf_map_update_elem(&tls_reg, &tid, &ref, BPF_ANY);
	return 0;
}

static __always_inline __u64 chain_hash(struct chain *c)
{
	__u64 h = 0xcbf29ce484222325ULL;

	for (int i = 0; i < MAX_HOPS; i++) {
		if (i >= c->n)
			break;
		struct hop *hp = &c->hops[i];
		__u64 *comm = (__u64 *)hp->comm;
		h = mix(h, comm[0]);
		h = mix(h, comm[1]);
		h = mix(h, hp->wake_kstack);
		h = mix(h, hp->wake_ustack);
		h = mix(h, hp->wait_kstack);
		h = mix(h, hp->wait_ustack);
		h = mix(h, hp->has_wait);
	}
	return h ? h : 1;
}

/* Runs in the waker's context. Hop 0 is the waker; if the waker had itself
 * just finished a wait overlapping the wakee's wait, the chain that ended
 * that wait is appended so blocking propagates transitively. */
SEC("tp_btf/sched_waking")
int BPF_PROG(on_waking, struct task_struct *p)
{
	if (chain_depth == 0 || !wanted(p))
		return 0;

	__u32 wakee = p->pid;
	struct start_rec *start = bpf_map_lookup_elem(&starts, &wakee);
	if (!start || start->reason != REASON_BLOCKED || start->tgid != p->tgid)
		return 0;
	__u64 wakee_start = start->ts;

	__u32 zero = 0;
	struct chain *c = bpf_map_lookup_elem(&scratch_chain, &zero);
	if (!c)
		return 0;
	__builtin_memset(c, 0, sizeof(*c));

	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	struct hop *h0 = &c->hops[0];
	__u32 pc = preempt_count();

	__builtin_memset(h0, 0, sizeof(*h0));
	c->n = 1;
	h0->wake_kstack = capture_stack(ctx, 0);

	/* In interrupt context the current task is merely interrupted, so the
	 * waker is the interrupt itself and the chain stops here. */
	if (pc & (NMI_MASK | HARDIRQ_MASK)) {
		__builtin_memcpy(h0->comm, "[hardirq]", 10);
		goto store;
	}
	if (pc & SOFTIRQ_OFFSET) {
		__builtin_memcpy(h0->comm, "[softirq]", 10);
		goto store;
	}

	__u32 waker = cur->pid;
	h0->pid = waker;
	h0->tgid = cur->tgid;
	if (waker == 0) {
		__builtin_memcpy(h0->comm, "[idle]", 7);
		goto store;
	}
	bpf_get_current_comm(h0->comm, sizeof(h0->comm));
	if (capture_user_stacks)
		h0->wake_ustack = capture_user_stack(ctx);
	h0->has_trace = read_trace(waker, cur->tgid, &h0->trace);

	struct last_wait *lw = bpf_map_lookup_elem(&last_waits, &waker);
	if (lw && lw->end > wakee_start) {
		__u64 from = lw->start > wakee_start ? lw->start : wakee_start;
		h0->has_wait = 1;
		h0->wait_kstack = lw->kstack;
		h0->wait_ustack = lw->ustack;
		h0->wait_ns = lw->end - from;

		for (int i = 0; i < MAX_HOPS - 1; i++) {
			if (i >= lw->chain.n || i + 1 >= chain_depth)
				break;
			c->hops[i + 1] = lw->chain.hops[i];
			c->n = i + 2;
		}
	}

store:
	if (bpf_map_update_elem(&wakes, &wakee, c, BPF_ANY))
		record_drop(DROP_WAKE);
	return 0;
}

static __always_inline void accumulate_into(void *map, struct offcpu_key *key, __u64 delta, struct chain *c)
{
	struct offcpu_val *v = bpf_map_lookup_elem(map, key);
	if (!v) {
		struct offcpu_val init = {};
		bpf_map_update_elem(map, key, &init, BPF_NOEXIST);
		v = bpf_map_lookup_elem(map, key);
		if (!v) {
			record_drop(DROP_COUNTS);
			return;
		}
	}
	__sync_fetch_and_add(&v->total_ns, delta);
	__sync_fetch_and_add(&v->count, 1);
	if (!c)
		return;
	for (int i = 0; i < MAX_HOPS; i++) {
		if (i >= c->n)
			break;
		__sync_fetch_and_add(&v->hop_wait_ns[i], c->hops[i].wait_ns);
	}
}

static __always_inline void accumulate(struct offcpu_key *key, __u64 delta, struct chain *c)
{
	__u32 zero = 0;
	__u32 *gen = bpf_map_lookup_elem(&generation, &zero);
	if (gen && (*gen & 1))
		accumulate_into(&counts_1, key, delta, c);
	else
		accumulate_into(&counts_0, key, delta, c);
}

/* stream and launches describe a GPU wait and are 0 for off-CPU waits. */
static __always_inline void emit_event(__u64 start_ns, struct trace_ctx *trace, struct offcpu_key *key, __u64 now,
				       struct chain *c, __u64 stream, __u32 launches)
{
	struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e) {
		record_drop(DROP_EVENT);
		return;
	}
	__builtin_memset(e, 0, sizeof(*e));
	e->start_ns = start_ns;
	e->end_ns = now;
	e->pid = key->pid;
	e->tgid = key->tgid;
	__builtin_memcpy(e->comm, key->comm, sizeof(e->comm));
	e->kstack = key->kstack;
	e->ustack = key->ustack;
	e->reason = key->reason;
	e->gpu_api = key->gpu_api;
	e->gpu_op = key->gpu_op;
	e->gpu_kernel_kind = key->gpu_kernel_kind;
	e->gpu_kernel = key->gpu_kernel;
	e->gpu_stream = stream;
	e->gpu_launches = launches;
	if (trace)
		e->trace = *trace;
	else
		__builtin_memset(&e->trace, 0, sizeof(e->trace));
	if (c)
		bpf_probe_read_kernel(&e->chain, sizeof(e->chain), c);
	else
		e->chain.n = 0;
	bpf_ringbuf_submit(e, 0);
}

/* Sets key->chain_id to the interned chain, or leaves it 0 when the chains
 * map is full. */
static __always_inline void intern_chain(struct offcpu_key *key, struct chain *c)
{
	if (!c || c->n == 0)
		return;
	key->chain_id = chain_hash(c);
	if (bpf_map_update_elem(&chains, &key->chain_id, c, BPF_NOEXIST) && !bpf_map_lookup_elem(&chains, &key->chain_id)) {
		record_drop(DROP_CHAIN);
		key->chain_id = 0;
	}
}

static __always_inline void on_block(void *ctx, struct task_struct *prev, __u64 now)
{
	__u8 reason = task_state(prev) == TASK_RUNNING ? REASON_PREEMPTED : REASON_BLOCKED;
	if (reason == REASON_PREEMPTED && !include_preempted)
		return;

	__u32 pid = prev->pid;
	struct start_rec rec = {
		.ts = now,
		.tgid = prev->tgid,
		.reason = reason,
	};
	struct gpu_wait *w = bpf_map_lookup_elem(&gpu_waits, &pid);
	rec.in_gpu = w && w->tgid == rec.tgid && now >= w->ts && now - w->ts < GPU_WAIT_TIMEOUT_NS;
	rec.kstack = capture_stack(ctx, 0);
	if (capture_user_stacks)
		rec.ustack = capture_user_stack(ctx);
	rec.has_trace = read_trace(pid, rec.tgid, &rec.trace);
	if (bpf_map_update_elem(&starts, &pid, &rec, BPF_ANY))
		record_drop(DROP_START);
}

static __always_inline void on_resume(struct task_struct *next, __u64 now)
{
	__u32 pid = next->pid;
	struct start_rec *start = bpf_map_lookup_elem(&starts, &pid);
	if (!start)
		return;
	if (start->tgid != next->tgid) {
		bpf_map_delete_elem(&starts, &pid);
		return;
	}

	__u64 delta = now - start->ts;
	struct chain *c = NULL;
	if (start->reason == REASON_BLOCKED)
		c = bpf_map_lookup_elem(&wakes, &pid);

	if (start->reason == REASON_BLOCKED) {
		__u32 zero = 0;
		struct last_wait *lw = bpf_map_lookup_elem(&scratch_wait, &zero);
		if (lw) {
			__builtin_memset(lw, 0, sizeof(*lw));
			lw->start = start->ts;
			lw->end = now;
			lw->kstack = start->kstack;
			lw->ustack = start->ustack;
			if (c)
				bpf_probe_read_kernel(&lw->chain, sizeof(lw->chain), c);
			else
				lw->chain.n = 0;
			bpf_map_update_elem(&last_waits, &pid, lw, BPF_ANY);
		}
	}

	if (start->in_gpu) {
		struct gpu_wait *w = bpf_map_lookup_elem(&gpu_waits, &pid);
		if (w && c) {
			bpf_probe_read_kernel(&w->chain, sizeof(w->chain), c);
			w->has_chain = 1;
		}
		goto out;
	}

	if (delta < min_block_ns || delta > max_block_ns)
		goto out;

	struct offcpu_key key;
	__builtin_memset(&key, 0, sizeof(key));
	key.pid = pid;
	key.tgid = start->tgid;
	key.ustack = start->ustack;
	key.kstack = start->kstack;
	key.reason = start->reason;
	bpf_probe_read_kernel_str(key.comm, sizeof(key.comm), next->comm);

	intern_chain(&key, c);
	accumulate(&key, delta, key.chain_id ? c : NULL);

	if ((emit_traced_events && start->has_trace) || (slow_event_ns && delta >= slow_event_ns))
		emit_event(start->ts, start->has_trace ? &start->trace : NULL, &key, now, c, 0, 0);

out:
	if (c)
		bpf_map_delete_elem(&wakes, &pid);
	bpf_map_delete_elem(&starts, &pid);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	__u64 now = bpf_ktime_get_ns();

	if (wanted(prev))
		on_block(ctx, prev, now);
	if (wanted(next))
		on_resume(next, now);
	return 0;
}

/* GPU waits. The agent attaches on_gpu_enter and on_gpu_exit as a uprobe
 * and uretprobe pair to every host-blocking CUDA call, with the API index as
 * the attach cookie (Linux 5.15). The wait is timed from call to return, so
 * it is seen whether the driver spins on the CPU or sleeps. cudart calls
 * libcuda, so both layers may fire; a call made deeper on the stack of an
 * open wait only bumps the depth and the outermost call is recorded. */
SEC("uprobe")
int on_gpu_enter(void *ctx)
{
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	__u32 tid = cur->pid;
	__u64 now = bpf_ktime_get_ns();
	__u64 sp = ((__u64 *)ctx)[UPROBE_SP];
	struct gpu_wait *open = bpf_map_lookup_elem(&gpu_waits, &tid);
	if (open && open->tgid == (__u32)cur->tgid && open->depth > 0 && now >= open->ts &&
	    now - open->ts < GPU_WAIT_TIMEOUT_NS && sp < open->sp) {
		if (open->depth < 255)
			open->depth++;
		return 0;
	}

	__u32 zero = 0;
	struct gpu_wait *w = bpf_map_lookup_elem(&scratch_gpu, &zero);
	if (!w)
		return 0;
	__u64 cookie = bpf_get_attach_cookie(ctx);
	__builtin_memset(w, 0, sizeof(*w));
	w->ts = now;
	w->sp = sp;
	w->tgid = cur->tgid;
	w->api = (__u8)cookie;
	w->argkind = (__u8)(cookie >> 8);
	w->obj = w->argkind != GPU_ARG_NONE ? uprobe_arg(ctx, 0) : 0;
	w->depth = 1;
	w->ustack = capture_user_stacks ? capture_user_stack(ctx) : 0;
	w->has_trace = read_trace(tid, w->tgid, &w->trace);
	if (bpf_map_update_elem(&gpu_waits, &tid, w, BPF_ANY))
		record_drop(DROP_GPU);
	return 0;
}

/* Returns the NCCL collective the process issued after its previous GPU
 * wait ended and before this one started, or 0. A wait behind an NCCL
 * kernel is waiting for the network and the other ranks. */
static __always_inline __u8 pending_nccl(__u32 tgid, __u64 start, __u64 now)
{
	__u8 op = 0;
	struct nccl_mark *m = bpf_map_lookup_elem(&nccl_marks, &tgid);
	__u64 *last = bpf_map_lookup_elem(&gpu_sync_ends, &tgid);
	if (m && m->ts <= start && (!last || m->ts > *last))
		op = m->op;
	bpf_map_update_elem(&gpu_sync_ends, &tgid, &now, BPF_ANY);
	return op;
}

/* Default stream handles: 0, CU_STREAM_LEGACY and CU_STREAM_PER_THREAD. The
 * legacy default stream waits for every blocking stream, so a wait on one
 * is attributed like a context-wide wait. */
static __always_inline bool default_stream(__u64 s)
{
	return s <= 2;
}

/* Takes the kernel queued last, before start, on the stream the wait is for
 * (or by the process for context-wide waits), and the number queued since
 * the previous wait on it, which is reset. */
static __always_inline __u64 queued_kernel(struct gpu_wait *w, __u32 tgid, __u64 *stream, struct offcpu_key *key)
{
	struct handle_key hk = { .tgid = tgid };
	struct launch *l;

	*stream = 0;
	if (w->argkind == GPU_ARG_STREAM) {
		*stream = w->obj;
	} else if (w->argkind == GPU_ARG_EVENT) {
		hk.handle = w->obj;
		__u64 *s = bpf_map_lookup_elem(&event_streams, &hk);
		if (s)
			*stream = *s;
	}
	if (w->argkind != GPU_ARG_NONE && !default_stream(*stream)) {
		hk.handle = *stream;
		l = bpf_map_lookup_elem(&stream_launches, &hk);
	} else {
		l = bpf_map_lookup_elem(&proc_launches, &tgid);
	}
	if (!l || l->ts > w->ts)
		return 0;
	key->gpu_kernel = l->kernel;
	key->gpu_kernel_kind = l->kind;
	__u32 n = l->count;
	l->count = 0;
	return n;
}

SEC("uretprobe")
int on_gpu_exit(void *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	__u32 tid = (__u32)id, tgid = id >> 32;
	struct gpu_wait *w = bpf_map_lookup_elem(&gpu_waits, &tid);
	if (!w || w->tgid != tgid || w->depth == 0)
		return 0;
	if (w->depth > 1) {
		w->depth--;
		return 0;
	}
	w->depth = 0;

	__u64 now = bpf_ktime_get_ns();
	__u64 delta = now - w->ts;
	if (delta < gpu_min_wait_ns || delta > max_block_ns)
		goto out;

	__u8 op = pending_nccl(tgid, w->ts, now);
	struct offcpu_key key;
	__builtin_memset(&key, 0, sizeof(key));
	__u64 stream;
	__u32 launches = queued_kernel(w, tgid, &stream, &key);

	key.pid = tid;
	key.tgid = tgid;
	key.ustack = w->ustack;
	key.reason = REASON_GPU;
	key.gpu_api = w->api;
	key.gpu_op = op;
	bpf_get_current_comm(key.comm, sizeof(key.comm));

	struct chain *c = w->has_chain ? &w->chain : NULL;
	intern_chain(&key, c);
	accumulate(&key, delta, key.chain_id ? c : NULL);
	if ((emit_traced_events && w->has_trace) || (slow_event_ns && delta >= slow_event_ns))
		emit_event(w->ts, w->has_trace ? &w->trace : NULL, &key, now, c, stream, launches);
out:
	bpf_map_delete_elem(&gpu_waits, &tid);
	return 0;
}

SEC("uprobe")
int on_nccl_enter(void *ctx)
{
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	__u32 tgid = cur->tgid;
	struct nccl_mark m = {
		.ts = bpf_ktime_get_ns(),
		.op = (__u8)bpf_get_attach_cookie(ctx),
	};
	bpf_map_update_elem(&nccl_marks, &tgid, &m, BPF_ANY);
	return 0;
}

/* Launch tracking. These probes fire for every kernel launch, so the agent
 * counts hits in launch_hits and detaches them when the rate gets too high
 * for the uprobe trap cost. */
static __always_inline void note_launch(void *map, void *key, __u64 kernel, __u8 kind, __u32 tid, __u64 now)
{
	struct launch *l = bpf_map_lookup_elem(map, key);
	if (!l) {
		struct launch init = { .kernel = kernel, .ts = now, .tid = tid, .count = 1, .kind = kind };
		bpf_map_update_elem(map, key, &init, BPF_NOEXIST);
		return;
	}
	l->kernel = kernel;
	l->kind = kind;
	l->tid = tid;
	l->ts = now;
	l->count++;
}

SEC("uprobe")
int on_gpu_launch(void *ctx)
{
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	__u32 zero = 0;
	__u64 *hits = bpf_map_lookup_elem(&launch_hits, &zero);
	if (hits)
		(*hits)++;

	__u64 kernel = 0, stream = 0, cfg;
	__u8 kind = 0, open_scope = 0, consume_scope = 0;
	switch (bpf_get_attach_cookie(ctx)) {
	case LAUNCH_CU:
		kernel = uprobe_arg(ctx, 0);
		stream = uprobe_arg(ctx, 8);
		kind = KERNEL_FUNC;
		consume_scope = 1;
		break;
	case LAUNCH_CU_EX:
		cfg = uprobe_arg(ctx, 0);
		bpf_probe_read_user(&stream, sizeof(stream), (void *)(cfg + LAUNCH_CONFIG_STREAM));
		kernel = uprobe_arg(ctx, 1);
		kind = KERNEL_FUNC;
		consume_scope = 1;
		break;
	case LAUNCH_CUDA:
		kernel = uprobe_arg(ctx, 0);
		stream = uprobe_arg(ctx, 7);
		kind = KERNEL_STUB;
		open_scope = 1;
		break;
	case LAUNCH_CUDA_EX:
		cfg = uprobe_arg(ctx, 0);
		bpf_probe_read_user(&stream, sizeof(stream), (void *)(cfg + LAUNCH_CONFIG_STREAM));
		kernel = uprobe_arg(ctx, 1);
		kind = KERNEL_STUB;
		open_scope = 1;
		break;
	case LAUNCH_CU_GRAPH:
		kernel = uprobe_arg(ctx, 0);
		stream = uprobe_arg(ctx, 1);
		kind = KERNEL_GRAPH;
		consume_scope = 1;
		break;
	case LAUNCH_CUDA_GRAPH:
		kernel = uprobe_arg(ctx, 0);
		stream = uprobe_arg(ctx, 1);
		kind = KERNEL_GRAPH;
		open_scope = 1;
		break;
	default:
		return 0;
	}

	__u32 tid = cur->pid, tgid = cur->tgid;
	__u64 now = bpf_ktime_get_ns();
	if (open_scope) {
		struct launch_scope *scope = bpf_map_lookup_elem(&launch_scope, &tid);
		if (scope) {
			struct launch_scope open = *scope;
			if (now - open.ts > LAUNCH_SCOPE_TIMEOUT_NS)
				open.depth = 0;
			open.depth++;
			open.ts = now;
			bpf_map_update_elem(&launch_scope, &tid, &open, BPF_ANY);
		} else {
			struct launch_scope init = { .depth = 1, .ts = now };
			bpf_map_update_elem(&launch_scope, &tid, &init, BPF_NOEXIST);
		}
	} else if (consume_scope) {
		struct launch_scope *scope = bpf_map_lookup_elem(&launch_scope, &tid);
		if (scope) {
			if (now - scope->ts <= LAUNCH_SCOPE_TIMEOUT_NS && scope->depth > 0)
				return 0;
			bpf_map_delete_elem(&launch_scope, &tid);
		}
	}
	if (!default_stream(stream)) {
		struct handle_key hk = { .tgid = tgid, .handle = stream };
		note_launch(&stream_launches, &hk, kernel, kind, tid, now);
	}
	note_launch(&proc_launches, &tgid, kernel, kind, tid, now);
	return 0;
}

/* Closes the runtime launch scope opened on entry. */
SEC("uretprobe")
int on_gpu_launch_ret(void *ctx)
{
	(void)ctx;
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	__u32 tid = cur->pid;
	struct launch_scope *scope = bpf_map_lookup_elem(&launch_scope, &tid);
	if (!scope)
		return 0;
	if (bpf_ktime_get_ns() - scope->ts > LAUNCH_SCOPE_TIMEOUT_NS) {
		bpf_map_delete_elem(&launch_scope, &tid);
		return 0;
	}
	struct launch_scope open = *scope;
	if (open.depth > 0)
		open.depth--;
	if (open.depth == 0)
		bpf_map_delete_elem(&launch_scope, &tid);
	else
		bpf_map_update_elem(&launch_scope, &tid, &open, BPF_ANY);
	return 0;
}

/* cuEventRecord(event, stream) and the runtime's cudaEventRecord: a later
 * event sync waits for the work queued on that stream. */
SEC("uprobe")
int on_gpu_event_record(void *ctx)
{
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	struct handle_key hk = { .tgid = cur->tgid, .handle = uprobe_arg(ctx, 0) };
	__u64 stream = uprobe_arg(ctx, 1);
	bpf_map_update_elem(&event_streams, &hk, &stream, BPF_ANY);
	return 0;
}

/* cuModuleGetFunction(CUfunction *out, CUmodule, const char *name) and
 * cuLibraryGetKernel(CUkernel *out, CUlibrary, const char *name): the
 * handle written to *out is named on return. */
SEC("uprobe")
int on_gpu_fn_enter(void *ctx)
{
	struct task_struct *cur = (void *)bpf_get_current_task_btf();
	if (!wanted(cur))
		return 0;

	__u32 tid = cur->pid;
	struct fn_lookup fl = { .out = uprobe_arg(ctx, 0), .name = uprobe_arg(ctx, 2) };
	bpf_map_update_elem(&fn_lookups, &tid, &fl, BPF_ANY);
	return 0;
}

SEC("uretprobe")
int on_gpu_fn_exit(void *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	__u32 tid = (__u32)id;
	struct fn_lookup *fl = bpf_map_lookup_elem(&fn_lookups, &tid);
	if (!fl)
		return 0;

	struct handle_key hk = { .tgid = id >> 32 };
	struct kernel_name n = {};
	if (!bpf_probe_read_user(&hk.handle, sizeof(hk.handle), (void *)fl->out) && hk.handle &&
	    bpf_probe_read_user_str(n.name, sizeof(n.name), (void *)fl->name) > 0)
		bpf_map_update_elem(&kernel_names, &hk, &n, BPF_ANY);
	bpf_map_delete_elem(&fn_lookups, &tid);
	return 0;
}

SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_exit, struct task_struct *p)
{
	__u32 tid = p->pid;
	bpf_map_delete_elem(&starts, &tid);
	bpf_map_delete_elem(&wakes, &tid);
	bpf_map_delete_elem(&last_waits, &tid);
	bpf_map_delete_elem(&tls_reg, &tid);
	bpf_map_delete_elem(&gpu_waits, &tid);
	bpf_map_delete_elem(&launch_scope, &tid);
	bpf_map_delete_elem(&fn_lookups, &tid);
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
