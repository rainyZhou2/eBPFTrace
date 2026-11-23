// SPDX-License-Identifier: Apache-2.0
// Blocks on a pipe three calls below main in code built without frame
// pointers, so only .eh_frame can recover the callers. Prints the blocked
// thread id once the wait ends and exits after a second byte on stdin, which
// keeps the process alive for symbolization.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int pipefd[2];

static void *writer(void *arg)
{
	struct timespec ts = { .tv_nsec = 40 * 1000000L };
	nanosleep(&ts, NULL);
	if (write(pipefd[1], "x", 1) != 1)
		abort();
	return arg;
}

/* The empty asm after each call prevents tail calls, which would remove the
 * caller's frame. */
__attribute__((noinline)) static void level3(void)
{
	char c;
	if (read(pipefd[0], &c, 1) != 1)
		abort();
	__asm__ volatile("");
}

__attribute__((noinline)) static void level2(void)
{
	level3();
	__asm__ volatile("");
}

__attribute__((noinline)) static void level1(void)
{
	level2();
	__asm__ volatile("");
}

int main(void)
{
	pthread_t t;
	char c;

	if (pipe(pipefd) || read(STDIN_FILENO, &c, 1) != 1)
		return 1;
	pthread_create(&t, NULL, writer, NULL);
	level1();
	pthread_join(t, NULL);
	printf("%ld\n", (long)syscall(SYS_gettid));
	fflush(stdout);
	return read(STDIN_FILENO, &c, 1) == 1 ? 0 : 1;
}
