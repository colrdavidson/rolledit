#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

void save_img(unsigned char *buf, int wrap, int xsize, int ysize, char *filename) {
	FILE *f = fopen(filename, "wb");
	fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
	for (int i = 0; i < ysize; i++) {
		fwrite(buf + i * wrap, 1, xsize, f);
	}
	fclose(f);
}

void flush_frame(AVCodecContext *ctx, AVFrame *frame, char *filename) {
	char buf[1024] = {};
	snprintf(buf, sizeof(buf), filename, ctx->frame_num);
	save_img(frame->data[0], frame->linesize[0], frame->width, frame->height, buf);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}

	char *filename = argv[1];

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
	printf("got %s\n", codec->name);

	char *out_filename = "/tmp/test%d.ppm";

	uint64_t inbuf_size = 4096;
	uint8_t *in_buffer = calloc(1, inbuf_size + AV_INPUT_BUFFER_PADDING_SIZE);
	uint8_t *buffer = calloc(1, 1024);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();

	 while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(ctx, pkt) < 0) {
				printf("failed to send packet?\n");
				return 1;
            }

            while (avcodec_receive_frame(ctx, frame) >= 0) {
				flush_frame(ctx, frame, out_filename);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
		flush_frame(ctx, frame, out_filename);
        av_frame_unref(frame);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt_ctx);

	return 0;
}
