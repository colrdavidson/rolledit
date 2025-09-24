#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

bool quit = false;

typedef struct {
	void **list;
	int64_t size;

	uint32_t head;
	uint32_t tail;

	pthread_mutex_t lock;
} Queue;

Queue queue_init(void) {
	int64_t initial_size = 64;
	Queue q = (Queue){
		.list = calloc(sizeof(void *), initial_size),
		.size = initial_size,
		.head = 0,
		.tail = 0
	};

	pthread_mutex_init(&q.lock, NULL);
	return q;
}

int64_t queue_size_unsafe(Queue *q) {
	return (int64_t)q->tail - (int64_t)q->head;
}

bool queue_push(Queue *q, void *data) {
	pthread_mutex_lock(&q->lock);

	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size >= q->size) {
		pthread_mutex_unlock(&q->lock);
		return false;
	}

	uint32_t wrapped_tail = q->tail % q->size; 
	q->list[wrapped_tail] = data;
	q->tail += 1;

	pthread_mutex_unlock(&q->lock);
	return true;
}

void *queue_pop_unsafe(Queue *q) {
	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size <= 0) {
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];
	q->head += 1;
	return data;
}

void *queue_peek_unsafe(Queue *q) {
	int64_t cur_size = queue_size_unsafe(q);
	if (cur_size <= 0) {
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];
	return data;
}

void *queue_pop(Queue *q) {
	pthread_mutex_lock(&q->lock);
	void *data = queue_pop_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return data;
}

void *queue_peek(Queue *q) {
	pthread_mutex_lock(&q->lock);
	void *data = queue_peek_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return data;
}

int64_t queue_size(Queue *q) {
	pthread_mutex_lock(&q->lock);
	int64_t cur_size = queue_size_unsafe(q);
	pthread_mutex_unlock(&q->lock);
	return cur_size;
}

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

	bool pause;
	int64_t start_time;
	double frame_rate;
	int64_t audio_idx;

	int width;
	int height;
	enum AVPixelFormat pix_fmt;

	uint32_t sample_rate;
	uint32_t channels;
	enum AVSampleFormat sample_fmt;

	Queue frames;
	Queue samples;
} PlaybackState;

void sleep_ns(uint64_t ns) {
	struct timespec requested_time = (struct timespec){.tv_nsec = ns};
	struct timespec remaining_time = {};
	nanosleep(&requested_time, &remaining_time);
}

void free_frame(Frame *frame) {
	free(frame->pixels);
	free(frame);
}

void free_sample(Sample *sample) {
	free(sample->data);
	free(sample);
}

void audio_callback(void *userdata, uint8_t *stream, int len) {
	PlaybackState *pb = (PlaybackState *)userdata;
	memset(stream, 0, len);

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
		uint64_t rem_len = MIN(len, cur_sample->size);
		memcpy(stream, cur_sample->data, rem_len);

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

				int num_bytes = av_image_get_buffer_size(pb->pix_fmt, pb->width, pb->height, 32);
				Frame *f = (Frame *)calloc(1, sizeof(Frame));
				f->pts_us = pts;
				f->pixels = malloc(num_bytes);

				av_image_fill_arrays(f->buffers, f->strides, f->pixels, pb->pix_fmt, pb->width, pb->height, 32);
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

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}
	char *filename = argv[1];

	int width = 1920;
	int height = 1080;
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
	SDL_Window *window = SDL_CreateWindow(
		"Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width / 2, height / 2, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
	);
	SDL_GL_SetSwapInterval(1);

	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, width, height);

	PlaybackState pb = {
		.filename = filename,

		.width = width,
		.height = height,
		.pix_fmt = AV_PIX_FMT_YUV420P,

		.frames = queue_init(),
		.samples = queue_init(),
	};
	SDL_AudioSpec wanted_specs = (SDL_AudioSpec){
		.freq = 48000,
		.format = AUDIO_S16SYS,
		.channels = 2,
		.samples = 1024,
		.callback = audio_callback,
		.userdata = &pb,
	};
	SDL_AudioSpec specs;
	SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_specs, &specs, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if (audio_dev == 0) {
		printf("Failed to open audio device: %s\n", SDL_GetError());
		return 1;
	}
	
	pb.sample_rate = specs.freq;
	pb.channels = specs.channels;
	pb.sample_fmt = AV_SAMPLE_FMT_S16;

	SDL_PauseAudioDevice(audio_dev, 0);

	pthread_t decode_thread;
	pthread_create(&decode_thread, NULL, decode_video, (void *)&pb);
	pb.start_time = av_gettime();

	int64_t cur_time_us = av_gettime();
	for (;;) {
		if (!pb.pause) {
			cur_time_us = av_gettime();
		}
		int64_t rescaled_time_us = cur_time_us - pb.start_time;

		SDL_Event event;
		SDL_PollEvent(&event);
		switch (event.type) {
			case SDL_QUIT: {
				quit = true;
				goto end;
			} break;
		}

		// skip video frames until we're at the closest one to presentation time
		int64_t skip_count = 0;
		Frame *cur_frame = NULL;

		pthread_mutex_lock(&pb.frames.lock);
		while (queue_size_unsafe(&pb.frames) > 0) {
			Frame *next_frame = (Frame *)queue_peek_unsafe(&pb.frames);
			if (next_frame->pts_us > rescaled_time_us) {
				break;
			} else {
				if (cur_frame) {
					skip_count += 1;
					free_frame(cur_frame);
				}
				cur_frame = (Frame *)queue_pop_unsafe(&pb.frames);
			}
		}
		pthread_mutex_unlock(&pb.frames.lock);
		
		if (skip_count > 0) {
			printf("skipping %lld frames\n", skip_count);
		}

		if (cur_frame && !pb.pause) {
			SDL_Rect rect = (SDL_Rect){.x = 0, .y = 0, .w = pb.width, .h = pb.height};
			SDL_UpdateYUVTexture(texture, &rect,
				cur_frame->buffers[0], cur_frame->strides[0],
				cur_frame->buffers[1], cur_frame->strides[1],
				cur_frame->buffers[2], cur_frame->strides[2]
			);

			free_frame(cur_frame);
		}

		int64_t frame = 0;
		if (pb.frame_rate != 0) {
			double us_per_frame = 1000000.0 / pb.frame_rate;
			frame = (int64_t)((double)rescaled_time_us / us_per_frame);
		}

		int sample_bytes = av_get_bytes_per_sample(pb.sample_fmt);
		int64_t bytes_per_s = pb.sample_rate * sample_bytes * pb.channels;
		double audio_clock_s = (double)pb.audio_idx / (double)bytes_per_s;

		if (pb.pause) {
			continue;
		}

		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);
	}

end:
	pthread_join(decode_thread, NULL);
	SDL_Quit();
	return 0;
}
