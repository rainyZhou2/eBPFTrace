#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint_fast64_t operations;
static atomic_bool stop_workers;

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *worker(void *arg) {
	(void)arg;
	while (!atomic_load_explicit(&stop_workers, memory_order_relaxed)) {
		pthread_mutex_lock(&lock);
		atomic_fetch_add_explicit(&operations, 1, memory_order_relaxed);
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

int main(int argc, char **argv) {
	int threads = argc > 1 ? atoi(argv[1]) : 8;
	int seconds = argc > 2 ? atoi(argv[2]) : 10;
	pthread_t *ids = calloc((size_t)threads, sizeof(*ids));
	for (int i = 0; i < threads; i++) {
		if (pthread_create(&ids[i], NULL, worker, NULL) != 0) {
			perror("pthread_create");
			return 1;
		}
	}
	uint64_t start = now_ns();
	printf("start %llu\n", (unsigned long long)start);
	fflush(stdout);
	usleep((useconds_t)seconds * 1000000);
	atomic_store(&stop_workers, 1);
	for (int i = 0; i < threads; i++) {
		pthread_join(ids[i], NULL);
	}
	uint64_t elapsed = now_ns() - start;
	printf("ops %llu elapsed_ns %llu\n", (unsigned long long)atomic_load(&operations),
	       (unsigned long long)elapsed);
	free(ids);
	return 0;
}
