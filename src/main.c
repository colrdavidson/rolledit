#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>


#include "utils.h"
#include "queue.h"

#include <CoreVideo/CoreVideo.h>

#define SDL_AUDIO_BUFFER_SIZE 1024

uint8_t sans_ttf[] = {
	#embed "../fonts/Montserrat-Regular.ttf"
};

uint8_t mono_ttf[] = {
	#embed "../fonts/FiraMono-Regular.ttf"
};

uint8_t icon_ttf[] = {
	#embed "../fonts/fontawesome-webfont.ttf"
};

bool quit = false;
double p_height = 16;
double em = 0;

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
	SDL_Renderer *renderer;
	uint64_t frame_count;
	uint64_t last_frame_count;

	PlaybackState pb;

	uint64_t now;
	double dpr;

	Cursor cur;

	bool seeking;

	TTF_Font *sans_font;
	TTF_Font *mono_font;
	TTF_Font *icon_font;

	SDL_Texture *sw_tex;
} AppState;

void free_frame(Frame *frame) {
	av_frame_free(&frame->f);
	free(frame);
}

void free_sample(Sample *sample) {
	free(sample->data);
	free(sample);
}

void flush_frames(Queue *frames) {
	pthread_mutex_lock(&frames->lock);
	while (queue_size_unsafe(frames) > 0) {
		Frame *tmp = (Frame *)queue_pop_unsafe(frames);
		free_frame(tmp);
	}
	pthread_mutex_unlock(&frames->lock);
}

void flush_samples(Queue *samples) {
	pthread_mutex_lock(&samples->lock);
	while (queue_size_unsafe(samples) > 0) {
		Sample *tmp = (Sample *)queue_pop_unsafe(samples);
		free_sample(tmp);
	}
	pthread_mutex_unlock(&samples->lock);
}

void audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total) {
	PlaybackState *pb = (PlaybackState *)userdata;

	if (pb->pause) {
		return;
	}

	int64_t cur_time = pb->cur_time;

	// skip audio frames until we're at the closest one to presentation time
	int64_t skip_count = 0;
	Sample *cur_sample = NULL;

	pthread_mutex_lock(&pb->samples.lock);
	while (queue_size_unsafe(&pb->samples) > 0) {
		Sample *s = queue_peek_unsafe(&pb->samples);
		if (s->pts_us <= cur_time) {
			if (cur_sample) {
				skip_count += 1;
				free_sample(cur_sample);
			}
			cur_sample = queue_pop_unsafe(&pb->samples);
		} else {
			if (!cur_sample) {
				cur_sample = queue_pop_unsafe(&pb->samples);
			}
			break;
		}
	}
	pthread_mutex_unlock(&pb->samples.lock);

	if (skip_count > 0) {
		//printf("skipping %lld samples\n", skip_count);
	}

	if (cur_sample) {
		uint64_t rem_len = MIN(additional, cur_sample->size);
		SDL_PutAudioStreamData(stream, cur_sample->data, rem_len);
		pb->audio_idx += rem_len;

		free_sample(cur_sample);
	}

	return;
}

enum AVPixelFormat get_hw_fmt(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
	VideoState *vs = (VideoState *)ctx->opaque;
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++) { 
		if (*p == vs->pix_fmt) {
			return *p;
		}
	}

	return AV_PIX_FMT_NONE;
}

bool find_hw_accel(const AVCodec *codec, enum AVHWDeviceType *hw_type, enum AVPixelFormat *hw_pix_fmt) {
	enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

	while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
		for (int i = 0;; i++) {

			const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
			if (!config) {
				printf("HW decode accel for %s not found!\n", codec->name);
				return false;
			}

			if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX & config->device_type == type) {
				*hw_type = config->device_type;
				*hw_pix_fmt = config->pix_fmt;
				return true;
			}
		}
	}
	return false;
}

void *decode_video(void *userdata) {
	AppState *state = (AppState *)userdata;
	PlaybackState *pb = &state->pb;

	av_log_set_level(AV_LOG_QUIET);

	AVFormatContext *fmt_ctx = NULL;
	if (avformat_open_input(&fmt_ctx, pb->filename, NULL, NULL) < 0) {
		printf("unable to open clip %s\n", pb->filename);
		return NULL;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		printf("unable to find stream info?\n");
		return NULL;
	}

	const AVCodec *audio_codec = NULL;
	int audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
	if (audio_stream_idx < 0) {
		printf("Cannot find an audio stream\n");
		return NULL;
	}
	AVStream *audio_stream = fmt_ctx->streams[audio_stream_idx];

	AVCodecContext *audio_ctx = avcodec_alloc_context3(audio_codec);
	avcodec_parameters_to_context(audio_ctx, audio_stream->codecpar);

	if (avcodec_open2(audio_ctx, audio_codec, NULL) < 0) {
		printf("could not open audio codec\n");
		return NULL;
	}

	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, pb->channels);

	SwrContext *swr = NULL;
	if (swr_alloc_set_opts2(
		&swr,
		&out_layout, pb->sample_fmt, pb->sample_rate,
		&audio_ctx->ch_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate,
		0, NULL
	) < 0) {
		printf("failed to configure audio resampling\n");
		return NULL;
	}
	if (swr_init(swr) < 0) {
		printf("failed to init audio resampling\n");
		return NULL;
	}

	const AVCodec *video_codec = NULL;
	int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
	if (video_stream_idx < 0) {
		printf("Cannot find an video stream\n");
		return NULL;
	}

	AVStream *video_stream = fmt_ctx->streams[video_stream_idx];
	AVCodecContext *video_ctx = avcodec_alloc_context3(video_codec);
	avcodec_parameters_to_context(video_ctx, video_stream->codecpar);

	enum AVPixelFormat hw_pix_fmt;
	enum AVHWDeviceType hw_type;
	VideoState vs = {};
	video_ctx->opaque = (void *)&vs;

	AVBufferRef *hw_device_ctx = NULL;
	if (find_hw_accel(video_codec, &hw_type, &hw_pix_fmt)) {
		vs.pix_fmt = hw_pix_fmt;
		video_ctx->get_format = get_hw_fmt;

		if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
			printf("Failed to init hw decoder\n");
			return NULL;
		}
		video_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
	}

	if (avcodec_open2(video_ctx, video_codec, NULL) < 0) {
		printf("could not open video codec\n");
		return NULL;
	}
	pb->frame_rate = av_q2d(video_stream->r_frame_rate);
	pb->duration_us = ((double)fmt_ctx->duration / AV_TIME_BASE) * 1000000;

	pb->sws_ctx = sws_getContext(video_ctx->width, video_ctx->height, video_ctx->pix_fmt, pb->width, pb->height, pb->pix_fmt, SWS_LANCZOS, NULL, NULL, NULL);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	while (!quit) {
		if (pb->should_seek) {
			if (avformat_seek_file(fmt_ctx, -1, INT64_MIN, pb->seek_time, INT64_MAX, AVSEEK_FLAG_ANY) < 0) {
				printf("failed to seek in file?\n");
				return NULL;
			}
			avcodec_flush_buffers(video_ctx);
			avcodec_flush_buffers(audio_ctx);

			flush_frames(&pb->frames);
			flush_samples(&pb->samples);

			pb->cur_time = pb->seek_time;
			if (!pb->was_paused) {
				pb->pause = false;
			}
			pb->should_seek = false;
		}

		int read_ret = av_read_frame(fmt_ctx, pkt);
		if (read_ret < 0) {
			sleep_ns(1000);
			continue;
		}
		if (pkt->stream_index == video_stream_idx) {
			if (avcodec_send_packet(video_ctx, pkt) < 0) {
				continue;
			}

			int ret = 0;
			do {
				AVFrame *video_frame = av_frame_alloc();
				ret = avcodec_receive_frame(video_ctx, video_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("video decode error!\n");
					return NULL;
				}

				int64_t pts = 0;
				if (video_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
					pts = av_rescale_q(video_frame->best_effort_timestamp, video_stream->time_base, AV_TIME_BASE_Q);
				}

				Frame *f = (Frame *)calloc(1, sizeof(Frame));
				f->pts_us = pts;
				f->f = video_frame;

				while (!queue_push(&pb->frames, (void *)f) && !quit && !pb->should_seek) {
					sleep_ns(10000);
				}

				if (quit) {
					return NULL;
				}
				if (pb->should_seek) {
					break;
				}
			} while (ret >= 0);
		} else if (pkt->stream_index == audio_stream_idx) {
			if (avcodec_send_packet(audio_ctx, pkt) < 0) {
				printf("Error sending audio packet for decoding\n");
				return NULL;
			}

			int ret = 0;
			do {
				ret = avcodec_receive_frame(audio_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("audio decode error!\n");
					return NULL;
				}

				int64_t pts = 0;
				if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
					pts = av_rescale_q(frame->best_effort_timestamp, audio_stream->time_base, AV_TIME_BASE_Q);
				}
				if (pb->sample_rate != audio_ctx->sample_rate) {
					printf("TODO: pts calc needs to get fixed for new sample rate\n");
					printf("pb rate: %d != ctx rate: %d\n", pb->sample_rate, audio_ctx->sample_rate);
					exit(1);
				}

				int dst_samples = av_rescale_rnd(
					swr_get_delay(swr, audio_ctx->sample_rate) + frame->nb_samples,
					pb->sample_rate,
					audio_ctx->sample_rate,
					AV_ROUND_UP
				);

				int audio_buf_size = av_samples_get_buffer_size(NULL, pb->channels, dst_samples, pb->sample_fmt, 1);
				uint8_t *audio_buf = calloc(1, audio_buf_size);

				int dst_chan_sample_count = swr_convert(swr, &audio_buf, dst_samples * pb->channels, (const uint8_t **)frame->data, frame->nb_samples);
				if (dst_chan_sample_count < 0) {
					printf("Failed to convert samples\n");
					return NULL;
				}
				int sample_bytes = av_get_bytes_per_sample(pb->sample_fmt);
				int total_size_bytes = dst_chan_sample_count * pb->channels * sample_bytes;

				Sample *s = (Sample *)malloc(sizeof(Sample));
				*s = (Sample){
					.data = audio_buf,
					.size = total_size_bytes,
					.pts_us = pts
				};

				while (!queue_push(&pb->samples, (void *)s) && !quit && !pb->should_seek) {
					sleep_ns(10000);
				}
				if (quit) {
					return NULL;
				}
				if (pb->should_seek) {
					break;
				}
			} while (ret >= 0);
		} else {
			av_packet_unref(pkt);
		}
	}
	return NULL;
}

void draw_rect(AppState *state, FRect r, BVec4 color) {
	SDL_SetRenderDrawColor(state->renderer, color.r, color.g, color.b, color.a);

	SDL_FRect rect = (SDL_FRect){
		.x = r.x,
		.y = r.y,
		.w = r.w,
		.h = r.h
	};
	SDL_RenderFillRect(state->renderer, &rect);
}

int64_t measure_text(TTF_Font *font, char *str) {
	int width = 0;
	TTF_MeasureString(font, str, 0, 0, &width, NULL);
	return width;
}

void draw_text(AppState *state, TTF_Font *font, char *str, FVec2 pos, BVec4 color) {
	SDL_Surface *tex_surf = TTF_RenderText_Blended(font, str, 0, (SDL_Color){.r = color.r, .g = color.g, .b = color.b, .a = color.a});
	if (!tex_surf) {
		printf("Failed to render text? %s\n", SDL_GetError());
		quit = true;
		return;
	}
	SDL_Texture *text_tex = SDL_CreateTextureFromSurface(state->renderer, tex_surf);
	SDL_DestroySurface(tex_surf);

	float w = 0;
	float h = 0;
	SDL_GetTextureSize(text_tex, &w, &h);
	SDL_FRect rect = (SDL_FRect){
		.x = pos.x,
		.y = pos.y,
		.w = w,
		.h = h
	};
	SDL_RenderTexture(state->renderer, text_tex, NULL, &rect);
	SDL_DestroyTexture(text_tex);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}
	char *filename = argv[1];

	int width = 1920;
	int height = 1080;
	AppState *state = (AppState *)malloc(sizeof(AppState));

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
		printf("failed to init SDL\n");
		return SDL_APP_FAILURE;
	}

	if (!TTF_Init()) {
		printf("failed to init SDL_TTF\n");
		return SDL_APP_FAILURE;
	}

	SDL_Window *window;
	if (!SDL_CreateWindowAndRenderer("Viewer", width / 2, height / 2, SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &state->renderer)) {
		printf("failed to create window/renderer\n");
		return SDL_APP_FAILURE;
	}
	SDL_SetRenderVSync(state->renderer, 1);
	SDL_SetRenderDrawBlendMode(state->renderer, SDL_BLENDMODE_BLEND);

	SDL_GetWindowSizeInPixels(window, &width, &height);
	state->dpr = SDL_GetWindowDisplayScale(window);
	em = p_height * state->dpr;


	state->sans_font = TTF_OpenFontIO(SDL_IOFromConstMem(sans_ttf, sizeof(sans_ttf)), true, em);
	if (!state->sans_font) {
		printf("Failed to open sans font\n");
		return SDL_APP_FAILURE;
	}

	state->mono_font = TTF_OpenFontIO(SDL_IOFromConstMem(mono_ttf, sizeof(mono_ttf)), true, em);
	if (!state->mono_font) {
		printf("Failed to open mono font\n");
		return SDL_APP_FAILURE;
	}

	state->icon_font = TTF_OpenFontIO(SDL_IOFromConstMem(icon_ttf, sizeof(icon_ttf)), true, em);
	if (!state->icon_font) {
		printf("Failed to open icon font\n");
		return SDL_APP_FAILURE;
	}

	state->pb = (PlaybackState){
		.filename = filename,

		.width = width,
		.height = height,
		.pix_fmt = AV_PIX_FMT_YUV420P,

		.frames = queue_init(32),
		.samples = queue_init(64),

		.pause = true,
		.was_paused = true,
	};

	SDL_AudioSpec specs = (SDL_AudioSpec){
		.freq = 48000,
		.format = SDL_AUDIO_S16LE,
		.channels = 2,
	};
	SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &specs, audio_callback, &state->pb);
	if (!stream) {
		printf("failed to open audio device!\n");
		return SDL_APP_FAILURE;
	}
	
	state->pb.sample_rate = specs.freq;
	state->pb.channels = specs.channels;
	state->pb.sample_fmt = AV_SAMPLE_FMT_S16;

	SDL_ResumeAudioStreamDevice(stream);

	pthread_create(&state->pb.decode_thread, NULL, decode_video, (void *)state);
	state->now = SDL_GetPerformanceCounter();
	state->sw_tex = SDL_CreateTexture(state->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, state->pb.width, state->pb.height);
	SDL_SetTextureScaleMode(state->sw_tex, SDL_SCALEMODE_NEAREST);
	*appstate = state;

	return SDL_APP_CONTINUE;
}

void reset_cursor(Cursor *c) {
	c->clicked = false;
	c->was_down = false;
	c->up_now = false;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	AppState *state = (AppState *)appstate;
	switch (event->type) {
		case SDL_EVENT_QUIT: {
			return SDL_APP_SUCCESS;
		} break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN: {
			float x = event->button.x;
			float y = event->button.y;
			SDL_RenderCoordinatesFromWindow(state->renderer, x, y, &x, &y);

			if (state->frame_count != state->last_frame_count) {
				state->cur.last_pos = state->cur.pos;
				state->last_frame_count = state->frame_count;
			}

			state->cur.is_down = true;
			state->cur.pos = (FVec2){.x = x, .y = y};
			state->cur.clicked = true;
			state->cur.clicked_pos = state->cur.pos;
		} break;
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			float x = event->button.x;
			float y = event->button.y;
			SDL_RenderCoordinatesFromWindow(state->renderer, x, y, &x, &y);

			if (state->frame_count != state->last_frame_count) {
				state->cur.last_pos = state->cur.pos;
				state->last_frame_count = state->frame_count;
			}

			state->cur.is_down = false;
			state->cur.pos = (FVec2){.x = x, .y = y};
			state->cur.was_down = true;
			state->cur.up_now = true;
		} break;
		case SDL_EVENT_MOUSE_MOTION: {
			float x = event->motion.x;
			float y = event->motion.y;
			SDL_RenderCoordinatesFromWindow(state->renderer, x, y, &x, &y);

			if (state->frame_count != state->last_frame_count) {
				state->cur.last_pos = state->cur.pos;
				state->last_frame_count = state->frame_count;

			}
			state->cur.pos = (FVec2){.x = x, .y = y};
		} break;
		case SDL_EVENT_KEY_DOWN: {
			if (event->key.scancode == SDL_SCANCODE_SPACE) {
				state->pb.pause = !state->pb.pause;
			}
		} break;
	}

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
	if (quit) {
		return SDL_APP_SUCCESS;
	}

	AppState *state = (AppState *)appstate;
	PlaybackState *pb = &state->pb;

	uint64_t last = state->now;
	state->now = SDL_GetPerformanceCounter();
	int64_t dt_us = (int64_t)((double)((state->now - last) * 1000000.0) / (double)SDL_GetPerformanceFrequency());
	if (!pb->pause) {
		pb->cur_time = CLAMP(pb->cur_time + dt_us, 0, pb->duration_us);
	}

	bool panned = false;
	if (state->cur.is_down || state->cur.up_now) {
		double pan_dist = distance(state->cur.pos, state->cur.clicked_pos);
		if (pan_dist > 5.0) {
			panned = true;
		}
	}

	FVec2 pan_delta = {};
	if (panned) {
		pan_delta = vec2_sub(state->cur.pos, state->cur.last_pos);
	}

	// skip video frames until we're at the closest one to presentation time
	int64_t skip_count = 0;
	Frame *cur_frame = NULL;

	pthread_mutex_lock(&pb->frames.lock);
	while (queue_size_unsafe(&pb->frames) > 0) {
		Frame *f = (Frame *)queue_peek_unsafe(&pb->frames);
		if (f->pts_us <= pb->cur_time) {
			if (cur_frame) {
				skip_count += 1;
				free_frame(cur_frame);
			}
			cur_frame = (Frame *)queue_pop_unsafe(&pb->frames);
		} else {
			break;
		}
	}
	pthread_mutex_unlock(&pb->frames.lock);

	if (skip_count > 0) {
		//printf("skipping %lld frames\n", skip_count);
	}

	if (cur_frame && !pb->pause) {
		if (pb->cur_frame != NULL) {
			free_frame(pb->cur_frame);
		}
		pb->cur_frame = cur_frame;
	}

	SDL_RenderClear(state->renderer);
	SDL_FRect vid_tex_rect = (SDL_FRect){.x = 0, .y = 0, .w = pb->width, .h = pb->height};

	if (pb->cur_frame) {
		AVFrame *frame = pb->cur_frame->f;
		if (frame->hw_frames_ctx) {
			CVPixelBufferRef pix_buffer = (CVPixelBufferRef)frame->data[3];

			AVHWFramesContext *frames = (AVHWFramesContext *)(frame->hw_frames_ctx->data);
			int width = frames->width;
			int height = frames->height;
			SDL_PixelFormat format = GetTextureFormat(frames->sw_format);

			SDL_PropertiesID props = SDL_CreateProperties();
			SDL_Colorspace colorspace = SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR,
															  frame->color_range,
															  frame->color_primaries,
															  frame->color_trc,
															  frame->colorspace,
															  frame->chroma_location);

			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
			SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER, pix_buffer);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);

			SDL_Texture *tex = SDL_CreateTextureWithProperties(state->renderer, props);
			SDL_DestroyProperties(props);
			SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

			SDL_RenderTexture(state->renderer, tex, NULL, &vid_tex_rect);
			SDL_DestroyTexture(tex);
		} else {
			int num_bytes = av_image_get_buffer_size(pb->pix_fmt, pb->width, pb->height, 1);
			uint8_t *pixels = malloc(num_bytes);
			uint8_t *buffers[4] = {};
			int strides[4] = {};

			av_image_fill_arrays(buffers, strides, pixels, pb->pix_fmt, pb->width, pb->height, 1);
			sws_scale(pb->sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, frame->height, buffers, strides);

			SDL_UpdateYUVTexture(state->sw_tex, NULL,
				buffers[0], strides[0],
				buffers[1], strides[1],
				buffers[2], strides[2]
			);
			free(pixels);

			SDL_RenderTexture(state->renderer, state->sw_tex, NULL, &vid_tex_rect);
		}
	}

	float scrub_start_x = pb->width / 4;
	float scrub_end_x   = pb->width - (scrub_start_x);
	float scrub_width = scrub_end_x - scrub_start_x;

	int64_t frame = 0;
	double scrub_pos = 0.0;
	if (pb->frame_rate != 0 && pb->duration_us != 0) {
		double us_per_frame = 1000000.0 / pb->frame_rate;
		frame = (int64_t)((double)pb->cur_time / us_per_frame);

		double watch_perc = (double)pb->cur_time / pb->duration_us;
		scrub_pos = lerp(0.0, scrub_width, watch_perc);
	}

	int sample_bytes = av_get_bytes_per_sample(pb->sample_fmt);
	int64_t bytes_per_s = pb->sample_rate * sample_bytes * pb->channels;
	double audio_clock_s = (double)pb->audio_idx / (double)bytes_per_s;

	float bar_h = 4 * em;
	float bar_y = pb->height - bar_h;
	float scrub_height = em + (em / 2);
	// bar background
	draw_rect(state,
		(FRect){.x = 0, .y = bar_y, .w = pb->width, .h = bar_h},
		(BVec4){.r = 20, .g = 20, .b = 20, .a = 150}
	);

	float overlay_y = bar_y + bar_h / 2;
	float overlay_h = bar_h / 2;
	// scrub overlay
	draw_rect(state,
		(FRect){.x = scrub_start_x, .y = bar_y + overlay_h, .w = scrub_width, .h = overlay_h},
		(BVec4){.r = 40, .g = 40, .b = 40, .a = 150}
	);

	// scrub cursor
	FRect cursor_rect = (FRect){
		.x = scrub_start_x + scrub_pos + (em / 4),
		.y = overlay_y + (overlay_h / 2) - (scrub_height / 2),
		.w = em,
		.h = scrub_height
	};
	draw_rect(state, cursor_rect, (BVec4){.r = 200, .g = 200, .b = 200, .a = 150});

	// play button
	char *play_str = "\uf04b";
	char *pause_str = "\uf04c";

	char *start_stop_str = pause_str;
	if (pb->pause) {
		start_stop_str = play_str;
	}

	int64_t start_stop_str_w = measure_text(state->icon_font, start_stop_str);

	float play_w = 2 * em;
	float play_pad = em / 4;
	FRect play_rect = (FRect){
		.x = (pb->width / 2) - (play_w / 2) + play_pad,
		.y = bar_y + play_pad,
		.w = play_w - (2 * play_pad),
		.h = play_w - (2 * play_pad)
	};

	draw_text(state, state->icon_font, start_stop_str,
		(FVec2){.x = (pb->width / 2) - (start_stop_str_w / 2), .y = bar_y + (em / 2)},
		color_white
	);

	if (state->cur.clicked && pt_in_rect(state->cur.clicked_pos, play_rect)) {
		pb->was_paused = !pb->pause;
		pb->pause = !pb->pause;
	}

	int64_t time_s = pb->cur_time / 1000000;
	int64_t disp_mins = time_s / 60;
	int64_t disp_secs = time_s % 60;
	char *time_str = NULL;
	asprintf(&time_str, "%02lld:%02lld", disp_mins, disp_secs);

	int64_t time_str_w = measure_text(state->mono_font, time_str);
	draw_text(state, state->mono_font, time_str,
		(FVec2){.x = scrub_start_x + scrub_width + (em / 2), .y = overlay_y + (overlay_h / 2) - (em / 2)},
		color_white
	);

	if (state->cur.clicked && pt_in_rect(state->cur.clicked_pos, cursor_rect) && !state->seeking) {
		state->seeking = true;
		pb->pause = true;
		pb->seek_time = pb->cur_time;
	}
	if (state->cur.is_down && state->seeking) {
		float seek_perc = pan_delta.x / scrub_width;
		int64_t watch_off_us = lerp(0.0, (double)pb->duration_us, seek_perc);

		int64_t new_time = CLAMP(pb->cur_time + watch_off_us, 0, pb->duration_us);
		pb->seek_time = new_time;
		pb->cur_time  = new_time;
	}
	if (state->cur.up_now && state->seeking) {
		state->seeking = false;
		pb->should_seek = true;
	}

	SDL_RenderPresent(state->renderer);

	reset_cursor(&state->cur);
	state->frame_count += 1;
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	AppState *state = (AppState *)appstate;

	quit = true;
	pthread_join(state->pb.decode_thread, NULL);
}
