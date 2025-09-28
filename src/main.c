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
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
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
	pthread_t transcode_thread;

	char *in_filename;
	char *out_filename;
} TranscodeState;

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

typedef struct {
	AVCodecContext *in_ctx;
	int             in_idx;
	AVStream       *in_stream;
	AVFrame        *in_frame;

	AVCodecContext *out_ctx;
	AVStream       *out_stream;
	int             out_idx;
	AVPacket       *out_pkt;

	AVFilterContext *buffer_sink_ctx;
	AVFilterContext *buffer_src_ctx;
	AVFilterGraph   *filter_graph;
	AVFrame         *filter_frame;
} StreamContext;

bool setup_video_filter(StreamContext *s) {
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterGraph *filter_graph = avfilter_graph_alloc();

	const AVFilter *buffer_src  = avfilter_get_by_name("buffer");
	const AVFilter *buffer_sink = avfilter_get_by_name("buffersink");

	char *args = NULL;
	asprintf(&args, "width=%d:height=%d:pix_fmt=%s:time_base=%d/%d:frame_rate=%d/%d:pixel_aspect=%d/%d:colorspace=%s:range=%s",
		s->in_ctx->width, s->in_ctx->height, av_get_pix_fmt_name(s->in_ctx->pix_fmt),
		s->in_ctx->pkt_timebase.num, s->in_ctx->pkt_timebase.den,
		s->in_ctx->framerate.num, s->in_ctx->framerate.den,
		s->in_ctx->sample_aspect_ratio.num, s->in_ctx->sample_aspect_ratio.den,
		av_color_space_name(s->in_ctx->colorspace),
		av_color_range_name(s->in_ctx->color_range));
	//printf("video filter: %s\n", args);

	AVFilterContext *buffer_src_ctx = NULL;
	if (avfilter_graph_create_filter(&buffer_src_ctx, buffer_src, "in", args, NULL, filter_graph) < 0) {
		printf("Failed to create buffer source\n");
		return false;
	}
	AVFilterContext *buffer_sink_ctx = avfilter_graph_alloc_filter(filter_graph, buffer_sink, "out");
	if (!buffer_sink_ctx) {
		printf("Failed to create buffer sink\n");
		return false;
	}

	if (avfilter_init_dict(buffer_sink_ctx, NULL) < 0) {
		printf("Failed to init buffer sink\n");
		return false;
	}

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = buffer_src_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name        = av_strdup("out");
	inputs->filter_ctx  = buffer_sink_ctx;
	inputs->pad_idx     = 0;
	inputs->next        = NULL;

	if (avfilter_graph_parse_ptr(filter_graph, "null", &inputs, &outputs, NULL) < 0) {
		printf("Failed to parse filtergraph\n");
		return false;
	}

	if (avfilter_graph_config(filter_graph, NULL) < 0) {
		printf("Failed to set up filtergraph\n");
		return false;
	}

	s->buffer_src_ctx  = buffer_src_ctx;
	s->buffer_sink_ctx = buffer_sink_ctx;
	s->filter_graph    = filter_graph;

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return true;
}

bool setup_audio_filter(StreamContext *s) {
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterGraph *filter_graph = avfilter_graph_alloc();

	const AVFilter *buffer_src  = avfilter_get_by_name("abuffer");
	const AVFilter *buffer_sink = avfilter_get_by_name("abuffersink");

	char buf[64];
	av_channel_layout_describe(&s->in_ctx->ch_layout, buf, sizeof(buf));

	char *args = NULL;
	asprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
		s->in_ctx->pkt_timebase.num, s->in_ctx->pkt_timebase.den, s->in_ctx->sample_rate,
		av_get_sample_fmt_name(s->in_ctx->sample_fmt), buf);

	AVFilterContext *buffer_src_ctx = NULL;
	if (avfilter_graph_create_filter(&buffer_src_ctx, buffer_src, "in", args, NULL, filter_graph) < 0) {
		printf("Failed to create buffer source\n");
		return false;
	}

	AVFilterContext *buffer_sink_ctx = avfilter_graph_alloc_filter(filter_graph, buffer_sink, "out");
	if (!buffer_sink_ctx) {
		printf("Failed to create buffer sink\n");
		return false;
	}

	if (s->out_ctx->frame_size > 0) {
		av_buffersink_set_frame_size(buffer_sink_ctx, s->out_ctx->frame_size);
	}

	if (avfilter_init_dict(buffer_sink_ctx, NULL) < 0) {
		printf("Failed to init buffer sink\n");
		return false;
	}

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = buffer_src_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name        = av_strdup("out");
	inputs->filter_ctx  = buffer_sink_ctx;
	inputs->pad_idx     = 0;
	inputs->next        = NULL;

	if (avfilter_graph_parse_ptr(filter_graph, "anull", &inputs, &outputs, NULL) < 0) {
		printf("Failed to parse filtergraph\n");
		return false;
	}

	if (avfilter_graph_config(filter_graph, NULL) < 0) {
		printf("Failed to set up filtergraph\n");
		return false;
	}

	s->buffer_src_ctx  = buffer_src_ctx;
	s->buffer_sink_ctx = buffer_sink_ctx;
	s->filter_graph    = filter_graph;

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return true;
}

bool encode_and_write_frame(AVFormatContext *out_fmt_ctx, StreamContext *s, bool flush) {
	AVFrame *in_frame = s->in_frame;
	AVFrame *filt_frame = s->filter_frame;
	if (flush) {
		in_frame = NULL;
		filt_frame = NULL;
	}

	if (av_buffersrc_add_frame_flags(s->buffer_src_ctx, in_frame, 0) < 0) {
		printf("Failed to feed filtergraph\n");
		return false;
	}

	for (;;) {
		int ret = av_buffersink_get_frame(s->buffer_sink_ctx, filt_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		} else if (ret < 0) {
			printf("Failed to get filtered frame!\n");
			return false;
		}

		s->filter_frame->time_base = av_buffersink_get_time_base(s->buffer_sink_ctx);
		s->filter_frame->pict_type = AV_PICTURE_TYPE_NONE;

		av_packet_unref(s->out_pkt);

		s->filter_frame->pts = av_rescale_q(s->filter_frame->pts, s->filter_frame->time_base, s->out_ctx->time_base);

		ret = avcodec_send_frame(s->out_ctx, filt_frame);
		if (ret < 0) {
			printf("Failed to send filtered frame?\n");
			return false;
		}

		do {
			ret = avcodec_receive_packet(s->out_ctx, s->out_pkt);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			} else if (ret < 0) {
				printf("Failed to get out packet!\n");
				return false;
			}

			s->out_pkt->stream_index = s->out_idx;
			av_packet_rescale_ts(s->out_pkt, s->out_ctx->time_base, out_fmt_ctx->streams[s->out_idx]->time_base);
			if (av_interleaved_write_frame(out_fmt_ctx, s->out_pkt) < 0) {
				printf("Failed to write interleaved frame!\n");
				return false;
			}
		} while (ret >= 0);

		av_frame_unref(s->filter_frame);
	}

	return true;
}

void *transcode_video(void *userdata) {
	AppState *state = (AppState *)userdata;
	TranscodeState *tr = &state->tr;

	AVFormatContext *in_fmt_ctx = NULL;
	if (avformat_open_input(&in_fmt_ctx, tr->in_filename, NULL, NULL) < 0) {
		printf("unable to open clip %s\n", tr->in_filename);
		return NULL;
	}

	if (avformat_find_stream_info(in_fmt_ctx, NULL) < 0) {
		printf("unable to find stream info?\n");
		return NULL;
	}

	AVFormatContext *out_fmt_ctx = NULL;
	avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, tr->out_filename);
	if (!out_fmt_ctx) {
		printf("Unable to make new output context for %s\n", tr->out_filename);
		return NULL;
	}

	StreamContext streams[2];

	// Video Stack
	const AVCodec *in_video_codec = NULL;
	int in_video_stream_idx = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &in_video_codec, 0);
	if (in_video_stream_idx < 0) {
		printf("Cannot find an video stream\n");
		return NULL;
	}
	AVStream *in_video_stream = in_fmt_ctx->streams[in_video_stream_idx];

	AVCodecContext *in_video_ctx = avcodec_alloc_context3(in_video_codec);
	avcodec_parameters_to_context(in_video_ctx, in_video_stream->codecpar);

	in_video_ctx->pkt_timebase = in_video_stream->time_base;
	in_video_ctx->framerate = av_guess_frame_rate(in_fmt_ctx, in_video_stream, NULL);

	if (avcodec_open2(in_video_ctx, in_video_codec, NULL) < 0) {
		printf("could not open video codec\n");
		return NULL;
	}

	int out_video_stream_idx = 0;
	const AVCodec *out_video_codec = avcodec_find_encoder(in_video_ctx->codec_id);
	if (!out_video_codec) {
		printf("Failed to get video encoder!\n");
		return NULL;
	}
	AVCodecContext *out_video_ctx = avcodec_alloc_context3(out_video_codec);

	AVStream *out_video_stream = avformat_new_stream(out_fmt_ctx, NULL);
	if (!out_video_stream) {
		printf("Failed to create out video stream!\n");
		return NULL;
	}

	out_video_ctx->height = in_video_ctx->height;
	out_video_ctx->width  = in_video_ctx->width;
	out_video_ctx->sample_aspect_ratio = in_video_ctx->sample_aspect_ratio;
	out_video_ctx->time_base = av_inv_q(in_video_ctx->framerate);
/*
	out_video_ctx->framerate = in_video_ctx->framerate;
	out_video_ctx->gop_size = in_video_ctx->framerate.den / 2;
	out_video_ctx->max_b_frames = 2;
*/

	if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		out_video_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	const enum AVPixelFormat *pix_fmts = NULL;
	if (avcodec_get_supported_config(in_video_ctx, NULL, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&pix_fmts, NULL) < 0) {
		printf("Failed to get supported video configs\n");
		return NULL;
	}
	out_video_ctx->pix_fmt = in_video_ctx->pix_fmt; //pix_fmts[0];
	out_video_stream->time_base = out_video_ctx->time_base;

	if (avcodec_open2(out_video_ctx, out_video_codec, NULL) < 0) {
		printf("Cannot open video encoder!\n");
		return NULL;
	}
	if (avcodec_parameters_from_context(out_video_stream->codecpar, out_video_ctx) < 0) {
		printf("failed to copy encoder parameters to output video stream\n");
		return NULL;
	}

	streams[0] = (StreamContext){
		.in_ctx    = in_video_ctx,
		.in_idx    = in_video_stream_idx,
		.in_stream = in_video_stream,

		.out_ctx    = out_video_ctx,
		.out_idx    = out_video_stream_idx,
		.out_stream = out_video_stream,

		.in_frame     = av_frame_alloc(),
		.filter_frame = av_frame_alloc(),
		.out_pkt      = av_packet_alloc(),
	};

	if (!setup_video_filter(&streams[0])) {
		return NULL;
	}

	// Audio Stack
	const AVCodec *in_audio_codec = NULL;
	int in_audio_stream_idx = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &in_audio_codec, 0);
	if (in_audio_stream_idx < 0) {
		printf("Cannot find an audio stream\n");
		return NULL;
	}
	AVStream *in_audio_stream = in_fmt_ctx->streams[in_audio_stream_idx];

	AVCodecContext *in_audio_ctx = avcodec_alloc_context3(in_audio_codec);
	avcodec_parameters_to_context(in_audio_ctx, in_audio_stream->codecpar);

	in_audio_ctx->pkt_timebase = in_audio_stream->time_base;

	if (avcodec_open2(in_audio_ctx, in_audio_codec, NULL) < 0) {
		printf("could not open input audio codec\n");
		return NULL;
	}

	int out_audio_stream_idx = 1;
	AVStream *out_audio_stream = avformat_new_stream(out_fmt_ctx, NULL);
	if (!out_audio_stream) {
		printf("Failed to create out audio stream!\n");
		return NULL;
	}
	const AVCodec *out_audio_codec = avcodec_find_encoder(in_audio_ctx->codec_id);
	if (!out_audio_codec) {
		printf("Failed to get audio encoder!\n");
		return NULL;
	}
	AVCodecContext *out_audio_ctx = avcodec_alloc_context3(out_audio_codec);
	out_audio_ctx->sample_rate = in_audio_ctx->sample_rate;
	if (av_channel_layout_copy(&out_audio_ctx->ch_layout, &in_audio_ctx->ch_layout) < 0) {
		printf("Failed to copy audio channel layout\n");
		return NULL;
	}

	const enum AVSampleFormat *sample_fmts = NULL;
	if (avcodec_get_supported_config(in_audio_ctx, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&sample_fmts, NULL) < 0) {
		printf("Failed to get supported audio configs\n");
		return NULL;
	}
	out_audio_ctx->sample_fmt = sample_fmts[0];
	out_audio_ctx->time_base = (AVRational){1, out_audio_ctx->sample_rate};

	if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		out_audio_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if (avcodec_open2(out_audio_ctx, out_audio_codec, NULL) < 0) {
		printf("Cannot open audio encoder!\n");
		return NULL;
	}
	if (avcodec_parameters_from_context(out_audio_stream->codecpar, out_audio_ctx) < 0) {
		printf("failed to copy encoder parameters to output audio stream\n");
		return NULL;
	}
	out_audio_stream->time_base = out_audio_ctx->time_base;

	streams[1] = (StreamContext){
		.in_ctx    = in_audio_ctx,
		.in_idx    = in_audio_stream_idx,
		.in_stream = in_audio_stream,

		.out_ctx    = out_audio_ctx,
		.out_idx    = out_audio_stream_idx,
		.out_stream = out_audio_stream,

		.in_frame        = av_frame_alloc(),
		.filter_frame = av_frame_alloc(),
		.out_pkt      = av_packet_alloc(),
	};

	if (!setup_audio_filter(&streams[1])) {
		return NULL;
	}

	if (avio_open(&out_fmt_ctx->pb, tr->out_filename, AVIO_FLAG_WRITE) < 0) {
		printf("Error opening output file\n");
		return NULL;
	}

	if (avformat_write_header(out_fmt_ctx, NULL) < 0) {
		printf("Failed to write header to output file!\n");
		return NULL;
	}

	AVPacket *pkt = av_packet_alloc();

	// Transcode video and audio
	while (!quit) {
		if (av_read_frame(in_fmt_ctx, pkt) < 0) {
			break;
		}

		StreamContext *s = NULL;
		if (pkt->stream_index == in_video_stream_idx) {
			s = &streams[out_video_stream_idx];
		} else if (pkt->stream_index == in_audio_stream_idx) {
			s = &streams[out_audio_stream_idx];
		}
		if (!s) {
			av_packet_unref(pkt);
			continue;
		}

		if (avcodec_send_packet(s->in_ctx, pkt) < 0) {
			printf("Failed to decode video packet!\n");
			return NULL;
		}

		int ret = 0;
		do {
			ret = avcodec_receive_frame(s->in_ctx, s->in_frame);
			if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
				break;
			} else if (ret < 0) {
				printf("Failed to receive frame!\n");
				return NULL;
			}

			s->in_frame->pts = s->in_frame->best_effort_timestamp;
			if (!encode_and_write_frame(out_fmt_ctx, s, false)) {
				return NULL;
			}
		} while (ret >= 0);
		av_packet_unref(pkt);
	}

	// Flush video and audio streams
	for (int i = 0; i < 2; i++) {
		StreamContext *s = NULL;
		if (i == out_video_stream_idx) {
			s = &streams[out_video_stream_idx];
		} else if (i == out_audio_stream_idx) {
			s = &streams[out_audio_stream_idx];
		}
		if (!s) {
			printf("unhandled stream flush?\n");
		}

		if (avcodec_send_packet(s->in_ctx, NULL) < 0) {
			printf("Failed to flush decoder\n");
			return NULL;
		}

		int ret = 0;
		do {
			ret = avcodec_receive_frame(s->in_ctx, s->in_frame);
			if (ret == AVERROR_EOF) {
				break;
			} else if (ret < 0) {
				printf("Failed to flush frames! %d\n", ret);
				return NULL;
			}

			s->in_frame->pts = s->in_frame->best_effort_timestamp;
			if (!encode_and_write_frame(out_fmt_ctx, s, true)) {
				return NULL;
			}
		} while (ret >= 0);

		if (!encode_and_write_frame(out_fmt_ctx, s, true)) {
			printf("flush failed\n");
			return NULL;
		}
	}

	av_write_trailer(out_fmt_ctx);
	printf("Finished transcode!\n");
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

	av_log_set_level(AV_LOG_QUIET);
	//av_log_set_level(AV_LOG_VERBOSE);

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

	//pthread_create(&state->pb.decode_thread, NULL, decode_video, (void *)state);

	state->now = SDL_GetPerformanceCounter();
	state->sw_tex = SDL_CreateTexture(state->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, state->pb.width, state->pb.height);
	SDL_SetTextureScaleMode(state->sw_tex, SDL_SCALEMODE_NEAREST);
	*appstate = state;

	state->tr = (TranscodeState){
		.in_filename = filename,
		.out_filename = "test.mp4",
	};
	pthread_create(&state->tr.transcode_thread, NULL, transcode_video, (void *)state);

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
