// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "etrace_ctx.h"

static int pipefd[2];

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static void *writer(void *arg)
{
	sleep_ms(40);
	if (write(pipefd[1], "x", 1) != 1)
		abort();
	return arg;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

static void block_once(void)
{
	pthread_t t;
	char c;

	if (pipe(pipefd))
		abort();
	pthread_create(&t, NULL, writer, NULL);
	if (read(pipefd[0], &c, 1) != 1)
		abort();
	pthread_join(t, NULL);
	close(pipefd[0]);
	close(pipefd[1]);
}

int main(int argc, char **argv)
{
	uint8_t trace_id[16], span_id[8];
	char go;

	if (argc != 3 || strlen(argv[1]) != 32 || strlen(argv[2]) != 16)
		return 2;
	hex_to_bytes(argv[1], trace_id, sizeof(trace_id));
	hex_to_bytes(argv[2], span_id, sizeof(span_id));

	if (read(STDIN_FILENO, &go, 1) != 1)
		return 1;
	etrace_set_context(trace_id, span_id);
	block_once();
	etrace_clear_context();
	block_once();
	printf("%ld\n", (long)syscall(SYS_gettid));
	return 0;
}
