#pragma once
#include <stdint.h>
#include <time.h>

inline uint64_t get_clock_count() {
#ifdef __i386__
	uint64_t ret;
	__asm__ volatile ("rdtsc" : "=A" (ret));
	return ret;
#elif __x86_64__
	uint32_t low, high;
	__asm__ volatile("rdtsc" : "=a" (low), "=d" (high));
	return (uint64_t)low | (((uint64_t)high) << 32);
#else
	// avoid compiler complaints
	return 0;
#endif
}

inline double gettime_d() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

inline void wait_cycles(uint64_t cycles) {
	uint64_t start = get_clock_count();
	uint64_t end;

	while(true) {
		end = get_clock_count();

		if(end - start > cycles) {
			break;
		}
	}
}