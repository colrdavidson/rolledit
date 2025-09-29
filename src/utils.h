#pragma once

#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#define ELEM_COUNT(x) (sizeof(x) / (sizeof((x)[0])))

#define RAT_TO_STRS(x) (x).num, (x).den

typedef struct {
	float x;
	float y;
	float w;
	float h;
} FRect;

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

void sleep_ns(uint64_t ns) {
	struct timespec requested_time = (struct timespec){.tv_nsec = ns};
	struct timespec remaining_time = {};
	nanosleep(&requested_time, &remaining_time);
}

double lerp(double start, double end, double perc) {
	return start + perc * (end - start);
}

bool pt_in_rect(FVec2 p, FRect r) {
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

static SDL_PixelFormat GetTextureFormat(enum AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_RGB8: return SDL_PIXELFORMAT_RGB332;
    case AV_PIX_FMT_RGB444: return SDL_PIXELFORMAT_XRGB4444;
    case AV_PIX_FMT_RGB555: return SDL_PIXELFORMAT_XRGB1555;
    case AV_PIX_FMT_BGR555: return SDL_PIXELFORMAT_XBGR1555;
    case AV_PIX_FMT_RGB565: return SDL_PIXELFORMAT_RGB565;
    case AV_PIX_FMT_BGR565: return SDL_PIXELFORMAT_BGR565;
    case AV_PIX_FMT_RGB24: return SDL_PIXELFORMAT_RGB24;
    case AV_PIX_FMT_BGR24: return SDL_PIXELFORMAT_BGR24;
    case AV_PIX_FMT_0RGB32: return SDL_PIXELFORMAT_XRGB8888;
    case AV_PIX_FMT_0BGR32: return SDL_PIXELFORMAT_XBGR8888;
    case AV_PIX_FMT_NE(RGB0, 0BGR): return SDL_PIXELFORMAT_RGBX8888;
    case AV_PIX_FMT_NE(BGR0, 0RGB): return SDL_PIXELFORMAT_BGRX8888;
    case AV_PIX_FMT_RGB32: return SDL_PIXELFORMAT_ARGB8888;
    case AV_PIX_FMT_RGB32_1: return SDL_PIXELFORMAT_RGBA8888;
    case AV_PIX_FMT_BGR32: return SDL_PIXELFORMAT_ABGR8888;
    case AV_PIX_FMT_BGR32_1: return SDL_PIXELFORMAT_BGRA8888;
    case AV_PIX_FMT_YUV420P: return SDL_PIXELFORMAT_IYUV;
    case AV_PIX_FMT_YUYV422: return SDL_PIXELFORMAT_YUY2;
    case AV_PIX_FMT_UYVY422: return SDL_PIXELFORMAT_UYVY;
    case AV_PIX_FMT_NV12: return SDL_PIXELFORMAT_NV12;
    case AV_PIX_FMT_NV21: return SDL_PIXELFORMAT_NV21;
    case AV_PIX_FMT_P010: return SDL_PIXELFORMAT_P010;
    default: return SDL_PIXELFORMAT_UNKNOWN;
    }
}

char *shortname(char *full_path) {
	char *name = strrchr(full_path, '/');
	return name + 1;
}

char *secs_to_timestr(int64_t secs) {
	int64_t disp_mins = secs / 60;
	int64_t disp_secs = secs % 60;
	char *time_str = NULL;
	asprintf(&time_str, "%02lld:%02lld", disp_mins, disp_secs);
	return time_str;
}
