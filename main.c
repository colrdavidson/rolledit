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
#include <libswscale/swscale.h>

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}
	char *filename = argv[1];

	SDL_Init(SDL_INIT_VIDEO);

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
	for (int i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_idx = i;
			break;
		}
	}
	if (video_stream_idx == -1) {
		printf("unable to find video stream!\n");
		return 1;
	}

	AVCodecParameters *codec_params = fmt_ctx->streams[video_stream_idx]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
	if (!codec) {
		printf("codec not found?\n");
		return 1;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, codec_params);

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		printf("could not open codec\n");
		return 1;
	}

	SDL_Window *window = SDL_CreateWindow(
		"Viewer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		ctx->width / 2, ctx->height / 2, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
	);
	SDL_GL_SetSwapInterval(1);

	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, ctx->width, ctx->height);

	struct SwsContext *sws_ctx = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt, ctx->width, ctx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

	int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, ctx->width, ctx->height, 32);
	uint8_t *rgb_buffer = (uint8_t *)av_malloc(num_bytes);

	AVFrame *rgb_frame = av_frame_alloc();
	av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer, AV_PIX_FMT_YUV420P, ctx->width, ctx->height, 32);

	double fps = av_q2d(fmt_ctx->streams[video_stream_idx]->r_frame_rate);
	double sleep_time = 1.0 / fps;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->stream_index != video_stream_idx) {
			goto end;
		}

		if (avcodec_send_packet(ctx, pkt) < 0) {
			printf("Error sending packet for decoding\n");
			return 1;
		}

		int ret = 0;
		do {
			ret = avcodec_receive_frame(ctx, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			} else if (ret < 0) {
				printf("decode error!\n");
				return 1;
			}

			sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, ctx->height, rgb_frame->data, rgb_frame->linesize);

			SDL_Delay((1000 * sleep_time) - 10);

			SDL_Rect rect = (SDL_Rect){.x = 0, .y = 0, .w = ctx->width, .h = ctx->height};
			SDL_UpdateYUVTexture(texture, &rect, rgb_frame->data[0], rgb_frame->linesize[0], rgb_frame->data[1], rgb_frame->linesize[1], rgb_frame->data[2], rgb_frame->linesize[2]);
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);
		} while (ret >= 0);
end:
		av_packet_unref(pkt);

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
