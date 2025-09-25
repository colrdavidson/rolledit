#pragma once

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ELEM_COUNT(x) (sizeof(x) / (sizeof((x)[0])))

void sleep_ns(uint64_t ns) {
	struct timespec requested_time = (struct timespec){.tv_nsec = ns};
	struct timespec remaining_time = {};
	nanosleep(&requested_time, &remaining_time);
}

double lerp(double start, double end, double perc) {
	return start + perc * (end - start);
}
