// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t ready;
static int pipefd[2];
static pid_t tid_holder, tid_waiter, tid_sleeper;

static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static void *holder(void *arg)
{
	char c;
	tid_holder = gettid_();
	pthread_mutex_lock(&mu);
	pthread_barrier_wait(&ready);
	if (read(pipefd[0], &c, 1) != 1)
		abort();
	pthread_mutex_unlock(&mu);
	return arg;
}

static void *waiter(void *arg)
{
	tid_waiter = gettid_();
	pthread_barrier_wait(&ready);
	sleep_ms(5);
	pthread_mutex_lock(&mu);
	pthread_mutex_unlock(&mu);
	return arg;
}

static void *sleeper(void *arg)
{
	tid_sleeper = gettid_();
	pthread_barrier_wait(&ready);
	sleep_ms(40);
	if (write(pipefd[1], "x", 1) != 1)
		abort();
	return arg;
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 1;
	char go;

	if (read(STDIN_FILENO, &go, 1) != 1)
		return 1;
	for (int i = 0; i < rounds; i++) {
		pthread_t t[3];
		if (pipe(pipefd))
			return 1;
		pthread_barrier_init(&ready, NULL, 3);
		pthread_create(&t[0], NULL, holder, NULL);
		pthread_create(&t[1], NULL, waiter, NULL);
		pthread_create(&t[2], NULL, sleeper, NULL);
		for (int j = 0; j < 3; j++)
			pthread_join(t[j], NULL);
		pthread_barrier_destroy(&ready);
		close(pipefd[0]);
		close(pipefd[1]);
		printf("%d %d %d\n", tid_waiter, tid_holder, tid_sleeper);
		fflush(stdout);
	}
	return 0;
}
