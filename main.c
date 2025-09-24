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

bool queue_push(Queue *q, void *data) {
	pthread_mutex_lock(&q->lock);

	int64_t cur_size = (int64_t)q->tail - (int64_t)q->head;
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

void *queue_pop(Queue *q) {
	pthread_mutex_lock(&q->lock);

	int64_t cur_size = (int64_t)q->tail - (int64_t)q->head;
	if (cur_size <= 0) {
		pthread_mutex_unlock(&q->lock);
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];
	q->head += 1;

	pthread_mutex_unlock(&q->lock);
	return data;
}

void *queue_peek(Queue *q) {
	pthread_mutex_lock(&q->lock);

	int64_t cur_size = (int64_t)q->tail - (int64_t)q->head;
	if (cur_size <= 0) {
		pthread_mutex_unlock(&q->lock);
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	void *data = q->list[wrapped_head];

	pthread_mutex_unlock(&q->lock);
	return data;
}

typedef struct {
	uint8_t *data;
	uint64_t size;
	int64_t pts_us;
} Sample;

typedef struct {
	uint8_t *pixels;
	uint8_t *buffers[4];
	int      strides[4];
	int64_t  pts_us;
} Frame;

typedef struct {
	char *filename;
	int64_t audio_idx;

	int width;
	int height;

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

void audio_callback(void *userdata, uint8_t *stream, int len) {
	PlaybackState *pb = (PlaybackState *)userdata;

	Sample *sample = (Sample *)queue_pop(&pb->samples);
	if (sample != NULL) {
		uint64_t rem_len = MIN(len, sample->size);
		if (len != rem_len) {
			printf("flushing audio buffer tail\n");
			memset(stream + rem_len, 0, len - rem_len);
		}
		memcpy(stream, sample->data, rem_len);

		pb->audio_idx += rem_len;

		free(sample->data);
		free(sample);
	} else {
		printf("nothing to grab\n");
		memset(stream, 0, len);
	}

	return;
}

void *decode_video(void *userdata) {
	PlaybackState *state = (PlaybackState *)userdata;

	AVFormatContext *fmt_ctx = NULL;
	if (avformat_open_input(&fmt_ctx, state->filename, NULL, NULL) < 0) {
		printf("unable to open clip %s\n", state->filename);
		return NULL;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		printf("unable to find stream info?\n");
		return NULL;
	}

	int video_stream_idx = -1;
	int audio_stream_idx = -1;
	for (int i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
			video_stream_idx = i;
		}
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0) {
			audio_stream_idx = i;
		}
	}
	if (video_stream_idx == -1) {
		printf("unable to find video stream!\n");
		return NULL;
	}
	if (audio_stream_idx == -1) {
		printf("unable to find audio stream!\n");
		return NULL;
	}

	AVStream *video_stream = fmt_ctx->streams[video_stream_idx];
	AVStream *audio_stream = fmt_ctx->streams[audio_stream_idx];

	const AVCodec *audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
	if (!audio_codec) {
		printf("audio codec not found?\n");
		return NULL;
	}
	AVCodecContext *audio_ctx = avcodec_alloc_context3(audio_codec);
	avcodec_parameters_to_context(audio_ctx, audio_stream->codecpar);

	if (avcodec_open2(audio_ctx, audio_codec, NULL) < 0) {
		printf("could not open audio codec\n");
		return NULL;
	}

	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, state->channels);

	SwrContext *swr = NULL;
	if (swr_alloc_set_opts2(
		&swr,
		&out_layout, state->sample_fmt, state->sample_rate,
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

	const AVCodec *video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
	if (!video_codec) {
		printf("video codec not found?\n");
		return NULL;
	}
	AVCodecContext *video_ctx = avcodec_alloc_context3(video_codec);
	avcodec_parameters_to_context(video_ctx, video_stream->codecpar);

	if (avcodec_open2(video_ctx, video_codec, NULL) < 0) {
		printf("could not open video codec\n");
		return NULL;
	}

	struct SwsContext *sws_ctx = sws_getContext(video_ctx->width, video_ctx->height, video_ctx->pix_fmt, state->width, state->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
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

				int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, state->width, state->height, 32);
				Frame *f = (Frame *)calloc(1, sizeof(Frame));
				f->pts_us = pts;
				f->pixels = malloc(num_bytes);

				av_image_fill_arrays(f->buffers, f->strides, f->pixels, AV_PIX_FMT_YUV420P, state->width, state->height, 32);
				sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, video_ctx->height, f->buffers, f->strides);

				while (!queue_push(&state->frames, (void *)f)) {
					sleep_ns(10000);
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
				if (state->sample_rate != audio_ctx->sample_rate) {
					printf("TODO: pts calc needs to get fixed for new sample rate\n");
					exit(1);
				}

				int dst_samples = av_rescale_rnd(
					swr_get_delay(swr, audio_ctx->sample_rate) + frame->nb_samples,
					state->sample_rate,
					audio_ctx->sample_rate,
					AV_ROUND_UP
				);

				int audio_buf_size = av_samples_get_buffer_size(NULL, state->channels, dst_samples, state->sample_fmt, 1);
				uint8_t *audio_buf = calloc(1, audio_buf_size);

				int dst_chan_sample_count = swr_convert(swr, &audio_buf, dst_samples * state->channels, (const uint8_t **)frame->data, frame->nb_samples);
				if (dst_chan_sample_count < 0) {
					printf("Failed to convert samples\n");
					return NULL;
				}
				int sample_bytes = av_get_bytes_per_sample(state->sample_fmt);
				int total_size_bytes = dst_chan_sample_count * state->channels * sample_bytes;

				Sample *s = (Sample *)malloc(sizeof(Sample));
				*s = (Sample){
					.data = audio_buf,
					.size = total_size_bytes,
					.pts_us = pts
				};

				while (!queue_push(&state->samples, (void *)s)) {
					sleep_ns(10000);
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
	int64_t start_time = av_gettime();

	for (;;) {
		int64_t cur_time_us = av_gettime();
		int64_t rescaled_time_us = cur_time_us - start_time;

		SDL_Event event;
		SDL_PollEvent(&event);
		switch (event.type) {
			case SDL_QUIT: {
				SDL_Quit();
				return 0;
			} break;
		}


		Frame *frame = (Frame *)queue_peek(&pb.frames);
		if (frame != NULL) {
			if (rescaled_time_us >= frame->pts_us) {
				SDL_Rect rect = (SDL_Rect){.x = 0, .y = 0, .w = pb.width, .h = pb.height};
				SDL_UpdateYUVTexture(texture, &rect,
					frame->buffers[0], frame->strides[0],
					frame->buffers[1], frame->strides[1],
					frame->buffers[2], frame->strides[2]
				);

				free(frame->pixels);
				Frame *used_frame = queue_pop(&pb.frames);
				free(used_frame);
			}
		}

		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);
	}

	return 0;
}
