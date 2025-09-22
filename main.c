#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
	AVPacket **list;
	int64_t size;

	uint32_t head;
	uint32_t tail;
} PacketQueue;

PacketQueue packet_queue_init(void) {
	int64_t initial_size = 64;
	return (PacketQueue){
		.list = calloc(sizeof(AVPacket *), initial_size),
		.size = initial_size,
		.head = 0,
		.tail = 0
	};
}

void packet_queue_push(PacketQueue *q, AVPacket *pkt) {
	int64_t cur_size = (int64_t)q->tail - (int64_t)q->head;
	if (cur_size >= q->size) {
		printf("growing queue! cur size: %lld\n", cur_size);
		int64_t new_size = q->size * 2;
		AVPacket **new_list = calloc(sizeof(AVPacket *), new_size);

		for (uint32_t i = q->head; i < q->tail; i++) {
			new_list[i % new_size] = q->list[i % q->size];
		}
		free(q->list);
		q->list = new_list;
		q->size = new_size;
	}

	uint32_t wrapped_tail = q->tail % q->size; 
	q->list[wrapped_tail] = pkt;
	q->tail += 1;
}

AVPacket *packet_queue_pop(PacketQueue *q) {
	int64_t cur_size = (int64_t)q->tail - (int64_t)q->head;
	if (cur_size <= 0) {
		return NULL;
	}

	uint32_t wrapped_head = q->head % q->size; 
	AVPacket *pkt = q->list[wrapped_head];
	q->head += 1;

	return pkt;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}
	char *filename = argv[1];

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

	AVFormatContext *fmt_ctx = NULL;
	if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0) {
		printf("unable to open clip %s\n", filename);
		return 1;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		printf("unable to find stream info?\n");
		return 1;
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
		return 1;
	}
	if (audio_stream_idx == -1) {
		printf("unable to find audio stream!\n");
		return 1;
	}

	AVCodecParameters *video_codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
	AVCodecParameters *audio_codec_params = fmt_ctx->streams[audio_stream_idx]->codecpar;

	const AVCodec *audio_codec = avcodec_find_decoder(audio_codec_params->codec_id);
	if (!audio_codec) {
		printf("audio codec not found?\n");
		return 1;
	}
	AVCodecContext *audio_ctx = avcodec_alloc_context3(audio_codec);
	avcodec_parameters_to_context(audio_ctx, audio_codec_params);

	printf("Requesting %d channels @ %d freq\n", audio_ctx->ch_layout.nb_channels, audio_ctx->sample_rate);

	SDL_AudioSpec wanted_specs = (SDL_AudioSpec){
		.freq = audio_ctx->sample_rate,
		.format = AUDIO_S16SYS,
		.channels = audio_ctx->ch_layout.nb_channels,
	};
	SDL_AudioSpec specs;
	SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_specs, &specs, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if (audio_dev == 0) {
		printf("Failed to open audio device: %s\n", SDL_GetError());
		return 1;
	}
	SDL_PauseAudioDevice(audio_dev, 0);

	if (avcodec_open2(audio_ctx, audio_codec, NULL) < 0) {
		printf("could not open audio codec\n");
		return 1;
	}

	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, specs.channels);

	SwrContext *swr = NULL;
	if (swr_alloc_set_opts2(
		&swr,
		&out_layout, AV_SAMPLE_FMT_S16, audio_ctx->sample_rate,
		&audio_ctx->ch_layout, audio_ctx->sample_fmt, audio_ctx->sample_rate,
		0, NULL
	) < 0) {
		printf("failed to configure audio resampling\n");
		return 1;
	}
	if (swr_init(swr) < 0) {
		printf("failed to init audio resampling\n");
		return 1;
	}

	const AVCodec *video_codec = avcodec_find_decoder(video_codec_params->codec_id);
	if (!video_codec) {
		printf("video codec not found?\n");
		return 1;
	}
	AVCodecContext *video_ctx = avcodec_alloc_context3(video_codec);
	avcodec_parameters_to_context(video_ctx, video_codec_params);

	if (avcodec_open2(video_ctx, video_codec, NULL) < 0) {
		printf("could not open video codec\n");
		return 1;
	}

	SDL_Window *window = SDL_CreateWindow(
		"Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		video_ctx->width / 2, video_ctx->height / 2, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
	);
	SDL_GL_SetSwapInterval(1);

	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, video_ctx->width, video_ctx->height);

	struct SwsContext *sws_ctx = sws_getContext(video_ctx->width, video_ctx->height, video_ctx->pix_fmt, video_ctx->width, video_ctx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

	int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, video_ctx->width, video_ctx->height, 32);
	uint8_t *rgb_buffer = (uint8_t *)av_malloc(num_bytes);

	AVFrame *rgb_frame = av_frame_alloc();
	av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer, AV_PIX_FMT_YUV420P, video_ctx->width, video_ctx->height, 32);

	double fps = av_q2d(fmt_ctx->streams[video_stream_idx]->r_frame_rate);
	double sleep_time = 1.0 / fps;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->stream_index == video_stream_idx) {
			if (avcodec_send_packet(video_ctx, pkt) < 0) {
				printf("Error sending packet for decoding\n");
				return 1;
			}

			int ret = 0;
			do {
				ret = avcodec_receive_frame(video_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("video decode error!\n");
					return 1;
				}

				sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, video_ctx->height, rgb_frame->data, rgb_frame->linesize);

				SDL_Delay((1000 * sleep_time) - 10);

				SDL_Rect rect = (SDL_Rect){.x = 0, .y = 0, .w = video_ctx->width, .h = video_ctx->height};
				SDL_UpdateYUVTexture(texture, &rect, rgb_frame->data[0], rgb_frame->linesize[0], rgb_frame->data[1], rgb_frame->linesize[1], rgb_frame->data[2], rgb_frame->linesize[2]);
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, texture, NULL, NULL);
				SDL_RenderPresent(renderer);
			} while (ret >= 0);
		} else if (pkt->stream_index == audio_stream_idx) {
			if (avcodec_send_packet(audio_ctx, pkt) < 0) {
				printf("Error sending audio packet for decoding\n");
				return 1;
			}

			int ret = 0;
			do {
				ret = avcodec_receive_frame(audio_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("audio decode error!\n");
					return 1;
				}

				int dst_samples = av_rescale_rnd(
					swr_get_delay(swr, audio_ctx->sample_rate) + frame->nb_samples,
					audio_ctx->sample_rate,
					audio_ctx->sample_rate,
					AV_ROUND_UP
				);

				uint8_t *audio_buf = NULL;
				if (av_samples_alloc(&audio_buf, NULL, frame->ch_layout.nb_channels, dst_samples, AV_SAMPLE_FMT_S16, 1) < 0) {
					printf("failed to alloc destination samples\n");
					return 1;
				}

				dst_samples = swr_convert(swr, &audio_buf, dst_samples * frame->ch_layout.nb_channels, (const uint8_t **)frame->data, frame->nb_samples);
				if (av_samples_fill_arrays(frame->data, frame->linesize, audio_buf, frame->ch_layout.nb_channels, dst_samples, AV_SAMPLE_FMT_S16, 1) < 0) {
					printf("failed to fill arrays\n");
					return 1;
				}

				SDL_QueueAudio(audio_dev, frame->data[0], frame->linesize[0]);
			} while (ret >= 0);
		} else {
			av_packet_unref(pkt);
		}


		SDL_Event event;
		SDL_PollEvent(&event);
		switch (event.type) {
			case SDL_QUIT: {
				SDL_Quit();
				return 0;
			} break;
		}
	}

	return 0;
}
