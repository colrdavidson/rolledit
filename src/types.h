#pragma once

typedef struct {
	bool clicked;
	bool is_down;
	bool was_down;
	bool up_now;

	FVec2 pos;
	FVec2 last_pos;
	FVec2 clicked_pos;

	int64_t clicked_ms;
} Cursor;

typedef struct {
	enum AVPixelFormat pix_fmt;
} VideoState;

typedef struct {
	int64_t pts_us;

	uint8_t *data;
	uint64_t size;
} Sample;

typedef struct {
	int64_t  pts_us;
	AVFrame *f;
} Frame;

typedef struct {
	pthread_t decode_thread;

	char *filename;

	int64_t cur_time;

	bool pause;
	bool was_paused;
	bool should_seek;
	int64_t seek_time;

	double frame_rate;
	double duration_us;
	int64_t audio_idx;

	Frame *cur_frame;
	SwsContext *sws_ctx;

	int width;
	int height;
	enum AVPixelFormat pix_fmt;

	uint32_t sample_rate;
	uint32_t channels;
	enum AVSampleFormat sample_fmt;

	Queue frames;
	Queue samples;
} PlaybackState;

typedef struct {
	pthread_t transcode_thread;

	char *in_filename;
	char *out_filename;
} TranscodeState;

typedef struct {
	bool quit;

	SDL_Renderer *renderer;
	uint64_t frame_count;
	uint64_t last_frame_count;

	PlaybackState pb;
	TranscodeState tr;

	uint64_t now;
	double dpr;

	Cursor cur;

	bool seeking;

	TTF_Font *sans_font;
	TTF_Font *mono_font;
	TTF_Font *icon_font;

	SDL_Texture *sw_tex;
} AppState;
