// SPDX-License-Identifier: Apache-2.0
// Waits on "the GPU" the way a training step does. Prints its tid and the
// host stub address of matmul_kernel, waits for a byte on stdin, then:
//  1. launches flash_attn_fwd through the driver on stream 0x5000 and syncs
//     that stream (sleeps 40ms inside the call);
//  2. launches matmul_kernel three times through the runtime on stream
//     0x6000, records event 0x77 on it and syncs the event (sleeps 40ms);
//  3. issues an NCCL collective and syncs the default stream through the
//     runtime (sleeps 40ms inside the nested driver call);
//  4. syncs the context, spinning 40ms on the CPU.
// Then waits for another byte so the caller can symbolize, and exits.
//
// With the argument "hang" it instead launches stuck_kernel on stream
// 0x5000, syncs the context (spinning) and then the stream (sleeping), each
// as long as FAKE_CUDA_MS says, printing "spin" and "sleep" as it enters
// them.
#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct {
	unsigned x, y, z;
} dim3;

int ncclAllReduce(const void *send, void *recv, unsigned long count, int type, int op, void *comm, void *s);
int cuModuleGetFunction(void **out, void *m, const char *name);
int cuLaunchKernel(void *f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz,
		   unsigned shmem, void *s, void **params, void **extra);
int cuStreamSynchronize(void *s);
int cudaLaunchKernel(const void *func, dim3 grid, dim3 block, void **args, size_t shmem, void *s);
int cudaEventRecord(void *e, void *s);
int cudaEventSynchronize(void *e);
int cudaStreamSynchronize(void *s);
int cuCtxSynchronize(void);
int cudaGraphLaunch(void *e, void *s);
int cuGraphLaunch(void *e, void *s);

/* Stands for the host stub nvcc generates for a __global__ function. */
__attribute__((noinline)) void matmul_kernel(void)
{
	__asm__ volatile("");
}

int main(int argc, char **argv)
{
	void *f = NULL;
	dim3 one = { 1, 1, 1 };
	char c;

	printf("%ld %p\n", (long)syscall(SYS_gettid), (void *)matmul_kernel);
	fflush(stdout);
	if (read(0, &c, 1) != 1)
		return 1;

	if (argc > 1 && strcmp(argv[1], "graph") == 0) {
		/* Runtime and driver graph launches, each followed by its own
		 * stream sync. The agent must count one queued graph per stream,
		 * not the runtime's nested driver call as well. */
		cudaGraphLaunch((void *)0x1234, (void *)0x7000);
		cudaStreamSynchronize((void *)0x7000);
		cuGraphLaunch((void *)0x5678, (void *)0x7100);
		cuStreamSynchronize((void *)0x7100);
		printf("done\n");
		fflush(stdout);
		if (read(0, &c, 1) != 1)
			return 1;
		return 0;
	}

	if (argc > 1 && strcmp(argv[1], "hang") == 0) {
		cuModuleGetFunction(&f, (void *)0x1000, "stuck_kernel");
		cuLaunchKernel(f, 1, 1, 1, 1, 1, 1, 0, (void *)0x5000, NULL, NULL);
		printf("spin\n");
		fflush(stdout);
		cuCtxSynchronize();
		printf("sleep\n");
		fflush(stdout);
		cuStreamSynchronize((void *)0x5000);
		return 0;
	}

	cuModuleGetFunction(&f, (void *)0x1000, "flash_attn_fwd");
	cuLaunchKernel(f, 1, 1, 1, 1, 1, 1, 0, (void *)0x5000, NULL, NULL);
	cuStreamSynchronize((void *)0x5000);

	for (int i = 0; i < 3; i++)
		cudaLaunchKernel((const void *)matmul_kernel, one, one, NULL, 0, (void *)0x6000);
	cudaEventRecord((void *)0x77, (void *)0x6000);
	cudaEventSynchronize((void *)0x77);

	ncclAllReduce(NULL, NULL, 0, 0, 0, NULL, NULL);
	cudaStreamSynchronize(NULL);
	cuCtxSynchronize();

	printf("done\n");
	fflush(stdout);
	if (read(0, &c, 1) != 1)
		return 1;
	return 0;
}
