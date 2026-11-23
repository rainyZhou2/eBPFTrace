// SPDX-License-Identifier: Apache-2.0
// Stand-ins for the CUDA driver, CUDA runtime and NCCL libraries, exporting
// the real symbol names and signatures so the GPU probes attach to them.
// Built three times by `make scenarios` with FAKE_CUDA, FAKE_CUDART or
// FAKE_NCCL.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default"), noinline))

typedef int CUresult;
typedef void *CUstream;
typedef void *CUevent;
typedef void *CUfunction;
typedef void *CUgraphExec;
typedef void *CUmodule;
typedef struct {
	unsigned x, y, z;
} dim3;

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Keeps calls and their arguments from being optimized away. */
static volatile uintptr_t sink;

#if defined(FAKE_CUDA)
/* How long "the GPU" takes, 40ms unless FAKE_CUDA_MS says otherwise. */
static long gpu_ms(void)
{
	const char *v = getenv("FAKE_CUDA_MS");
	return v ? atol(v) : 40;
}

/* Blocking-sync waits: the thread sleeps until "the GPU" is done. */
EXPORT CUresult cuStreamSynchronize(CUstream s)
{
	sink = (uintptr_t)s;
	sleep_ms(gpu_ms());
	return 0;
}

EXPORT CUresult cuStreamSynchronize_ptsz(CUstream s)
{
	return cuStreamSynchronize(s);
}

EXPORT CUresult cuEventSynchronize(CUevent e)
{
	sink = (uintptr_t)e;
	sleep_ms(gpu_ms());
	return 0;
}

/* A spinning wait, the default when contexts do not outnumber CPUs. */
EXPORT CUresult cuCtxSynchronize(void)
{
	uint64_t end = now_ns() + (uint64_t)gpu_ms() * 1000000ULL;
	while (now_ns() < end)
		;
	return 0;
}

EXPORT CUresult cuModuleGetFunction(CUfunction *out, CUmodule m, const char *name)
{
	sink = (uintptr_t)m + (uintptr_t)name;
	*out = (CUfunction)0xf00d0000;
	return 0;
}

EXPORT CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by,
			       unsigned bz, unsigned shmem, CUstream s, void **params, void **extra)
{
	sink = (uintptr_t)f + gx + gy + gz + bx + by + bz + shmem + (uintptr_t)s + (uintptr_t)params + (uintptr_t)extra;
	return 0;
}

EXPORT CUresult cuEventRecord(CUevent e, CUstream s)
{
	sink = (uintptr_t)e + (uintptr_t)s;
	return 0;
}

/* Queues a graph; the following stream sync is what waits for it. */
EXPORT CUresult cuGraphLaunch(CUgraphExec e, CUstream s)
{
	sink = (uintptr_t)e + (uintptr_t)s;
	return 0;
}
#elif defined(FAKE_CUDART)
CUresult cuStreamSynchronize(CUstream s);
CUresult cuEventSynchronize(CUevent e);
CUresult cuEventRecord(CUevent e, CUstream s);
CUresult cuGraphLaunch(CUgraphExec e, CUstream s);
CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz,
			unsigned shmem, CUstream s, void **params, void **extra);

static volatile int last_error;

/* Like the real runtime, records the driver result after each call, so the
 * driver call is nested rather than a tail call. */
EXPORT int cudaStreamSynchronize(CUstream s)
{
	last_error = cuStreamSynchronize(s);
	return last_error;
}

EXPORT int cudaEventSynchronize(CUevent e)
{
	last_error = cuEventSynchronize(e);
	return last_error;
}

EXPORT int cudaEventRecord(CUevent e, CUstream s)
{
	last_error = cuEventRecord(e, s);
	return last_error;
}

/* The runtime launches through the driver with a handle of its own, which
 * the agent must not mistake for the kernel. */
EXPORT int cudaLaunchKernel(const void *func, dim3 grid, dim3 block, void **args, size_t shmem, CUstream s)
{
	sink = (uintptr_t)func;
	last_error = cuLaunchKernel((CUfunction)0xdead0000, grid.x, grid.y, grid.z, block.x, block.y, block.z,
				    (unsigned)shmem, s, args, NULL);
	return last_error;
}

/* Like the real runtime, launches the graph through the driver. */
EXPORT int cudaGraphLaunch(CUgraphExec e, CUstream s)
{
	last_error = cuGraphLaunch(e, s);
	return last_error;
}
#elif defined(FAKE_NCCL)
EXPORT int ncclAllReduce(const void *send, void *recv, unsigned long count, int type, int op, void *comm,
			 CUstream s)
{
	sink = (uintptr_t)send + (uintptr_t)recv + count + type + op + (uintptr_t)comm + (uintptr_t)s;
	return 0;
}
#endif
