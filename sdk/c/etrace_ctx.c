/* SPDX-License-Identifier: Apache-2.0 */
#include "etrace_ctx.h"

#include <string.h>
#include <time.h>

#define ETRACE_TLS_MAGIC 0x45545243u
#define ETRACE_TLS_VERSION 1
#define REREGISTER_NS 1000000000ull

struct etrace_tls {
	uint32_t magic;
	uint32_t version;
	uint64_t seq;
	uint8_t trace_id[16];
	uint8_t span_id[8];
};

_Static_assert(sizeof(struct etrace_tls) == 40, "layout shared with the BPF program");

static __thread struct etrace_tls tls;
static __thread uint64_t last_register_ns;

__attribute__((noinline, used))
void etrace_register_tls(void *p)
{
	__asm__ __volatile__("" : : "r"(p) : "memory");
}

static uint64_t now_ns(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The agent may start after this thread registered, so registration is
 * repeated periodically; the call is a no-op unless a probe is attached. */
static void ensure_registered(void)
{
	uint64_t now = now_ns();

	if (tls.magic == 0) {
		tls.magic = ETRACE_TLS_MAGIC;
		tls.version = ETRACE_TLS_VERSION;
	}
	if (last_register_ns == 0 || now - last_register_ns >= REREGISTER_NS) {
		last_register_ns = now;
		etrace_register_tls(&tls);
	}
}

static void publish(const uint8_t *trace_id, const uint8_t *span_id)
{
	__atomic_store_n(&tls.seq, tls.seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	if (trace_id)
		memcpy(tls.trace_id, trace_id, sizeof(tls.trace_id));
	else
		memset(tls.trace_id, 0, sizeof(tls.trace_id));
	if (span_id)
		memcpy(tls.span_id, span_id, sizeof(tls.span_id));
	else
		memset(tls.span_id, 0, sizeof(tls.span_id));
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&tls.seq, tls.seq + 1, __ATOMIC_RELAXED);
}

void etrace_set_context(const uint8_t trace_id[16], const uint8_t span_id[8])
{
	ensure_registered();
	publish(trace_id, span_id);
}

void etrace_clear_context(void)
{
	if (tls.magic == 0)
		return;
	publish(NULL, NULL);
}
