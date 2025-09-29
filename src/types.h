#pragma once

double p_height = 16;
double em = 0;

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
} HWState;

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
	AVFormatContext *fmt_ctx;
	SwsContext *sws_ctx;
	SwrContext *swr;

	AVCodecContext *video_ctx;
	int video_stream_idx;
	AVStream *video_stream;

	AVCodecContext *audio_ctx;
	int audio_stream_idx;
	AVStream *audio_stream;
} PlaybackStreamState;

typedef struct {
	AVCodecContext *in_ctx;
	AVStream       *in_stream;
	AVFrame        *in_frame;

	AVCodecContext *out_ctx;
	AVStream       *out_stream;
	int             out_idx;
	AVPacket       *out_pkt;

	AVFilterContext *buffer_src_ctx;
	AVFilterContext *buffer_sink_ctx;
	AVFilterGraph   *filter_graph;
	AVFrame         *filter_frame;
} StreamContext;

typedef struct {
	AVFormatContext *in_fmt_ctx;
	int in_video_stream_idx;
	int in_audio_stream_idx;
	StreamContext streams[2];
} TranscodeStreamState;

typedef struct {
	char *file_path;
	int64_t duration_us;

	PlaybackStreamState ps;
	TranscodeStreamState ts;
} ClipState;

typedef struct {
	pthread_t decode_thread;

	bool clips_loaded;
	int64_t cur_time_us;
	int64_t seek_time_us;

	bool pause;
	bool was_paused;
	bool should_seek;
	bool done_seeking;

	Frame *cur_frame;

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

	char *out_file_path;
	int64_t cur_time_us;

	int width;
	int height;
	enum AVPixelFormat pix_fmt;

	uint32_t sample_rate;
	uint32_t channels;
	enum AVSampleFormat sample_fmt;
	AVChannelLayout ch_layout;

	int64_t last_audio_pts;
	int64_t last_video_pts;
} TranscodeState;

typedef struct {
	void *data;
	int size;
	int cap;
} DynArr;

typedef struct {
	bool quit;

	SDL_Renderer *renderer;
	uint64_t frame_count;
	uint64_t last_frame_count;

	DynArray(ClipState) clips;
	PlaybackState pb;
	TranscodeState tr;

	uint64_t now;
	double dpr;

	Cursor cur;

	bool start_transcode;
	bool transcoding;
	bool done_transcoding;

	bool seeking;

	TTF_Font *sans_font;
	TTF_Font *mono_font;
	TTF_Font *icon_font;

	SDL_Texture *sw_tex;
} AppState;
