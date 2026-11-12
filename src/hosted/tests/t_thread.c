// t_thread: raw threads (clone) and futexes, the parts the hosted nanokernel's thread model rests on:
// one Linux thread per nanothread, parked on a futex unless the scheduler picked it (the Win32
// emulator's model with futexes in place of Win32 events). Checks a baton pass between N threads
// where exactly one runs at a time, and the wake-up latency.
#include "hrt.h"

enum { NT = 4, ROUNDS = 500 };
static volatile int baton;               // index of the thread allowed to run
static volatile int running;             // must never exceed 1
static volatile int overlap, steps;
static uint8_t stacks[NT][16384] __attribute__((aligned(16)));

static int worker(void *arg) {
	int me = (int)(uint32_t)arg;
	for (int r = 0; r < ROUNDS; r++) {
		int b;
		while ((b = baton) != me) hfutex_wait(&baton, b);
		if (__atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST) != 1) overlap++;
		steps++;
		__atomic_sub_fetch(&running, 1, __ATOMIC_SEQ_CST);
		baton = (me + 1) % NT;
		hfutex_wake(&baton, NT);
	}
	return 0;
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	volatile int ctid[NT];
	baton = -1;
	for (int i = 0; i < NT; i++) {
		ctid[i] = 0;
		HCHECK(hthread(worker, (void *)(uint32_t)i, stacks[i], sizeof stacks[i], &ctid[i]) > 0, "thread %d", i);
	}
	uint64_t t0 = hnow_ns();
	baton = 0;
	hfutex_wake(&baton, NT);
	for (int i = 0; i < NT; i++) hthread_join(&ctid[i]);
	uint64_t t1 = hnow_ns();
	HCHECK(steps == NT * ROUNDS, "baton passed %d times", steps);
	HCHECK(overlap == 0, "never two threads at once (%d overlaps)", overlap);
	htest_note("baton hand-over: %u ns each (futex wake + switch, TCG time)", (uint32_t)((t1 - t0) / (NT * ROUNDS)));
	return htest_done();
}
