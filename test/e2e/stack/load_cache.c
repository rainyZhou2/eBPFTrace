// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { sec_ns = 1000000000, ms_ns = 1000000, wait_timeout_ms = 5000, signal_delay_ms = 60, independent_release_ms = 250 };

struct load_cache_case {
	pthread_mutex_t lock;
	sem_t locked;
	atomic_bool entering_lock;
	long waiter_tid;
	int gate[2];
	bool overlap;
	int64_t gate_to_unlock_ns;
};

static void require(bool ok, const char *message)
{
	if (!ok) {
		fprintf(stderr, "load_cache: %s\n", message);
		abort();
	}
}

static int64_t monotonic_ns(void)
{
	struct timespec ts;
	require(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "read monotonic clock");
	return (int64_t)ts.tv_sec * sec_ns + ts.tv_nsec;
}

static void *hold_cache(void *argument)
{
	struct load_cache_case *c = argument;
	require(pthread_setname_np(pthread_self(), "lc_holder") == 0, "name holder");
	require(pthread_mutex_lock(&c->lock) == 0, "acquire cache lock in holder");
	int64_t deadline = monotonic_ns() + (c->overlap ? independent_release_ms : wait_timeout_ms) * (int64_t)ms_ns;
	require(sem_post(&c->locked) == 0, "publish that cache lock is held");
	struct pollfd gate = {.fd = c->gate[0], .events = POLLIN};
	int64_t remaining = deadline - monotonic_ns();
	if (remaining < 0)
		remaining = 0;
	struct timespec timeout = {.tv_sec = remaining / sec_ns, .tv_nsec = remaining % sec_ns};
	int ready = ppoll(&gate, 1, &timeout, NULL);
	int64_t gate_return = monotonic_ns();
	if (c->overlap) {
		// The deadline controls unlock even if the poll gets no notification.
		// Stay runnable after an early signal so no other wait replaces this one.
		while (monotonic_ns() < deadline) {}
	}
	require(pthread_mutex_unlock(&c->lock) == 0, "release cache lock in holder");
	c->gate_to_unlock_ns = monotonic_ns() - gate_return;
	require(ready == 1 && (gate.revents & POLLIN), "holder did not see the readiness notification");
	return NULL;
}

static void *signal_holder(void *argument)
{
	struct load_cache_case *c = argument;
	require(pthread_setname_np(pthread_self(), c->overlap ? "lc_decoy" : "lc_release") == 0, "name signaler");
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/task/%ld/wchan", c->waiter_tid);
	int64_t deadline = monotonic_ns() + wait_timeout_ms * (int64_t)ms_ns;
	while (monotonic_ns() < deadline) {
		if (atomic_load(&c->entering_lock)) {
			char channel[128];
			FILE *file = fopen(path, "r");
			require(file != NULL, "open waiter wchan");
			bool blocked = fgets(channel, sizeof(channel), file) && strstr(channel, "futex");
			fclose(file);
			if (blocked) {
				usleep(signal_delay_ms * 1000);
				require(write(c->gate[1], "x", 1) == 1, "notify holder");
				return NULL;
			}
		}
		usleep(1000);
	}
	require(false, "cache waiter did not block in futex");
	return NULL;
}

int64_t etrace_load_cache(int overlap)
{
	struct load_cache_case c = {.lock = PTHREAD_MUTEX_INITIALIZER, .overlap = overlap != 0};
	atomic_init(&c.entering_lock, false);
	c.waiter_tid = syscall(SYS_gettid);
	require(pipe(c.gate) == 0, "open readiness pipe");
	require(sem_init(&c.locked, 0, 0) == 0, "initialize holder handshake");
	pthread_t holder, signaler;
	require(pthread_create(&holder, NULL, hold_cache, &c) == 0, "launch holder");
	require(sem_wait(&c.locked) == 0, "wait for holder to acquire cache lock");
	require(pthread_create(&signaler, NULL, signal_holder, &c) == 0, "launch signaler");
	char name[16];
	require(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0, "read original waiter name");
	require(pthread_setname_np(pthread_self(), c.overlap ? "lc_overlap" : "lc_handoff") == 0, "name waiter");
	atomic_store(&c.entering_lock, true);
	require(pthread_mutex_lock(&c.lock) == 0, "acquire cache lock in waiter");
	require(pthread_mutex_unlock(&c.lock) == 0, "release cache lock in waiter");
	require(pthread_setname_np(pthread_self(), name) == 0, "restore waiter name");
	require(pthread_join(holder, NULL) == 0 && pthread_join(signaler, NULL) == 0, "join worker threads");
	sem_destroy(&c.locked);
	pthread_mutex_destroy(&c.lock);
	close(c.gate[0]);
	close(c.gate[1]);
	return c.gate_to_unlock_ns;
}
