#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "utils.h"
#include "queue.h"

#define SDL_AUDIO_BUFFER_SIZE 1024

bool quit = false;
double p_height = 16;
double em = 0;

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
	int64_t pts_us;

	uint8_t *data;
	uint64_t size;
} Sample;

typedef struct {
	int64_t  pts_us;

	uint8_t *pixels;
	uint8_t *buffers[4];
	int      strides[4];
} Frame;

typedef struct {
	char *filename;

	int64_t cur_time;

	bool pause;
	int64_t start_time;
	double frame_rate;
	double duration_us;
	int64_t audio_idx;

	SDL_Texture *vid_tex;

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
	SDL_Renderer *renderer;

	PlaybackState pb;
	pthread_t decode_thread;

	uint64_t now;

	double dpr;
} AppState;

void free_frame(Frame *frame) {
	free(frame->pixels);
	free(frame);
}

void free_sample(Sample *sample) {
	free(sample->data);
	free(sample);
}

void audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total) {
	PlaybackState *pb = (PlaybackState *)userdata;

	if (pb->pause) {
		return;
	}

	int64_t cur_time_us = av_gettime();
	int64_t rescaled_time_us = cur_time_us - pb->start_time;

	// skip audio frames until we're at the closest one to presentation time
	int64_t skip_count = 0;
	Sample *cur_sample = NULL;
	pthread_mutex_lock(&pb->samples.lock);
	while (queue_size_unsafe(&pb->samples) > 0) {
		Sample *next_sample = (Sample *)queue_peek_unsafe(&pb->samples);
		if (next_sample->pts_us > rescaled_time_us) {
			break;
		} else {
			if (cur_sample) {
				skip_count += 1;
				free_sample(cur_sample);
			}
			cur_sample = (Sample *)queue_pop_unsafe(&pb->samples);
		}
	}
	pthread_mutex_unlock(&pb->samples.lock);
	
	if (skip_count > 0) {
		printf("skipping %lld samples\n", skip_count);
	}

	if (cur_sample) {
		uint64_t rem_len = MIN(additional, cur_sample->size);
		SDL_PutAudioStreamData(stream, cur_sample->data, rem_len);
		pb->audio_idx += rem_len;

		free_sample(cur_sample);
	} else {
		printf("nothing to grab\n");
	}

	return;
}

void *decode_video(void *userdata) {
	PlaybackState *pb = (PlaybackState *)userdata;

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

	if (avcodec_open2(video_ctx, video_codec, NULL) < 0) {
		printf("could not open video codec\n");
		return NULL;
	}
	pb->frame_rate = av_q2d(video_stream->r_frame_rate);
	pb->duration_us = ((double)fmt_ctx->duration / AV_TIME_BASE) * 1000000;

	struct SwsContext *sws_ctx = sws_getContext(video_ctx->width, video_ctx->height, video_ctx->pix_fmt, pb->width, pb->height, pb->pix_fmt, SWS_LANCZOS, NULL, NULL, NULL);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	while (av_read_frame(fmt_ctx, pkt) >= 0 && !quit) {
		if (pkt->stream_index == video_stream_idx) {
			if (avcodec_send_packet(video_ctx, pkt) < 0) {
				printf("Error sending video packet for decoding\n");
				return NULL;
			}

			int ret = 0;
			do {
				ret = avcodec_receive_frame(video_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("video decode error!\n");
					return NULL;
				}

				int64_t pts = 0;
				if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
					pts = av_rescale_q(frame->best_effort_timestamp, video_stream->time_base, AV_TIME_BASE_Q);
				}

				int num_bytes = av_image_get_buffer_size(pb->pix_fmt, pb->width, pb->height, 1);
				Frame *f = (Frame *)calloc(1, sizeof(Frame));
				f->pts_us = pts;
				f->pixels = malloc(num_bytes);

				av_image_fill_arrays(f->buffers, f->strides, f->pixels, pb->pix_fmt, pb->width, pb->height, 1);
				sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, video_ctx->height, f->buffers, f->strides);

				while (!queue_push(&pb->frames, (void *)f) && !quit) {
					sleep_ns(10000);
				}
				if (quit) {
					return NULL;
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

				while (!queue_push(&pb->samples, (void *)s) && !quit) {
					sleep_ns(10000);
				}
				if (quit) {
					return NULL;
				}
			} while (ret >= 0);
		} else {
			av_packet_unref(pkt);
		}
	}
	return NULL;
}

void draw_rect(AppState *state, Rect r, BVec4 color) {
	SDL_SetRenderDrawColor(state->renderer, color.r, color.g, color.b, color.a);

	SDL_FRect rect = (SDL_FRect){
		.x = r.x,
		.y = r.y,
		.w = r.w,
		.h = r.h
	};
	SDL_RenderFillRect(state->renderer, &rect);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	if (event->type == SDL_EVENT_QUIT) {
		return SDL_APP_SUCCESS;
	}
	return SDL_APP_CONTINUE;
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

	SDL_Texture *vid_tex = SDL_CreateTexture(state->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, width, height);
	SDL_SetTextureScaleMode(vid_tex, SDL_SCALEMODE_NEAREST);

	state->pb = (PlaybackState){
		.filename = filename,

		.width = width,
		.height = height,
		.pix_fmt = AV_PIX_FMT_YUV420P,

		.vid_tex = vid_tex,

		.frames = queue_init(),
		.samples = queue_init(),
	};

	SDL_AudioSpec specs = (SDL_AudioSpec){
		.freq = 48000,
		.format = SDL_AUDIO_S16,
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

	pthread_create(&state->decode_thread, NULL, decode_video, (void *)&state->pb);
	state->pb.start_time = av_gettime();
	*appstate = state;

	state->pb.cur_time = av_gettime();
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
	AppState *state = appstate;
	PlaybackState *pb = &state->pb;

	uint64_t last = SDL_GetPerformanceCounter();
	state->now = SDL_GetPerformanceCounter();
	double dt = (double)((state->now - last) * 1000.0) / (double)SDL_GetPerformanceFrequency();

	if (!pb->pause) {
		pb->cur_time = av_gettime();
	}
	int64_t rescaled_time_us = pb->cur_time - pb->start_time;

	// skip video frames until we're at the closest one to presentation time
	int64_t skip_count = 0;
	Frame *cur_frame = NULL;

	pthread_mutex_lock(&pb->frames.lock);
	while (queue_size_unsafe(&pb->frames) > 0) {
		Frame *next_frame = (Frame *)queue_peek_unsafe(&pb->frames);
		if (next_frame->pts_us > rescaled_time_us) {
			break;
		} else {
			if (cur_frame) {
				skip_count += 1;
				free_frame(cur_frame);
			}
			cur_frame = (Frame *)queue_pop_unsafe(&pb->frames);
		}
	}
	pthread_mutex_unlock(&pb->frames.lock);

	if (skip_count > 0) {
		printf("skipping %lld frames\n", skip_count);
	}

	if (cur_frame && !pb->pause) {
		SDL_UpdateYUVTexture(pb->vid_tex, NULL,
			cur_frame->buffers[0], cur_frame->strides[0],
			cur_frame->buffers[1], cur_frame->strides[1],
			cur_frame->buffers[2], cur_frame->strides[2]
		);
		free_frame(cur_frame);
	}

	double scrub_start_x = pb->width / 4;
	double scrub_end_x   = pb->width - (scrub_start_x);
	double scrub_width = scrub_end_x - scrub_start_x;

	int64_t frame = 0;
	double scrub_pos = 0.0;
	if (pb->frame_rate != 0 && pb->duration_us != 0) {
		double us_per_frame = 1000000.0 / pb->frame_rate;
		frame = (int64_t)((double)rescaled_time_us / us_per_frame);

		double watch_perc = (double)rescaled_time_us / pb->duration_us;
		scrub_pos = lerp(0.0, scrub_width, watch_perc);
	}

	int sample_bytes = av_get_bytes_per_sample(pb->sample_fmt);
	int64_t bytes_per_s = pb->sample_rate * sample_bytes * pb->channels;
	double audio_clock_s = (double)pb->audio_idx / (double)bytes_per_s;

	SDL_RenderClear(state->renderer);

	SDL_FRect out_rect = (SDL_FRect){.x = 0, .y = 0, .w = pb->width, .h = pb->height};
	SDL_RenderTexture(state->renderer, pb->vid_tex, NULL, &out_rect);

	double bar_h = 2 * em;
	double bar_y = pb->height - bar_h;
	double scrub_height = em + (em / 2);
	// bar background
	draw_rect(state,
		(Rect){.x = 0, .y = bar_y, .w = pb->width, .h = bar_h},
		(BVec4){.r = 20, .g = 20, .b = 20, .a = 150}
	);
	// scrub overlay
	draw_rect(state,
		(Rect){.x = scrub_start_x, .y = bar_y, .w = scrub_width, .h = bar_h},
		(BVec4){.r = 40, .g = 40, .b = 40, .a = 150}
	);
	// scrub cursor
	draw_rect(state,
		(Rect){.x = scrub_start_x + scrub_pos + (em / 4), .y = bar_y + (bar_h / 2) - (scrub_height / 2), .w = em, .h = scrub_height},
		(BVec4){.r = 200, .g = 200, .b = 200, .a = 150}
	);
	SDL_RenderPresent(state->renderer);

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	AppState *state = (AppState *)appstate;

	quit = true;
	pthread_join(state->decode_thread, NULL);
}
