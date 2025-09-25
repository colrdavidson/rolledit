#pragma once

#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ELEM_COUNT(x) (sizeof(x) / (sizeof((x)[0])))

typedef struct {
	float x;
	float y;
	float w;
	float h;
} Rect;

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} BVec4;

typedef struct {
	float x;
	float y;
} FVec2;

BVec4 color_white = {.r = 255, .g = 255, .b = 255, .a = 255};

void sleep_ns(uint64_t ns) {
	struct timespec requested_time = (struct timespec){.tv_nsec = ns};
	struct timespec remaining_time = {};
	nanosleep(&requested_time, &remaining_time);
}

double lerp(double start, double end, double perc) {
	return start + perc * (end - start);
}

bool pt_in_rect(FVec2 p, Rect r) {
	float x1 = r.x;
	float y1 = r.y;
	float x2 = r.x + r.w;
	float y2 = r.y + r.h;

	return x1 <= p.x && p.x <= x2 && y1 <= p.y && p.y <= y2;
}

double distance(FVec2 a, FVec2 b) {
	double dx = b.x - a.x;
	double dy = b.y - a.y;

	return sqrt((dx * dx) + (dy * dy));
}

FVec2 vec2_sub(FVec2 a, FVec2 b) {
	return (FVec2){.x = a.x - b.x, .y = a.y - b.y};
}
