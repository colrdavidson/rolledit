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
#include "types.h"
#include "ui.h"
#include "video.h"

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

double p_height = 16;
double em = 0;

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
	if (!SDL_CreateWindowAndRenderer("Viewer", width, height, SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &state->renderer)) {
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
	state->sw_tex = SDL_CreateTexture(state->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_TARGET, state->pb.width, state->pb.height);
	SDL_SetTextureScaleMode(state->sw_tex, SDL_SCALEMODE_NEAREST);
	SDL_SetRenderTarget(state->renderer, state->sw_tex);
	SDL_SetRenderDrawColor(state->renderer, 0, 0, 0, 255);
	SDL_RenderClear(state->renderer);
	SDL_SetRenderTarget(state->renderer, NULL);

	*appstate = state;

	state->tr = (TranscodeState){
		.in_filename = filename,
		.out_filename = "test.mp4",
	};
	//pthread_create(&state->tr.transcode_thread, NULL, transcode_video, (void *)state);

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
	AppState *state = (AppState *)appstate;
	PlaybackState *pb = &state->pb;

	if (state->quit) {
		return SDL_APP_SUCCESS;
	}

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

	SDL_SetRenderDrawColor(state->renderer, 15, 15, 15, 255);
	SDL_RenderClear(state->renderer);

	FRect preview_rect = {.x = pb->width / 4, .y = 0, .w = (pb->width / 4) * 3, .h = (pb->height / 4) * 3};
	SDL_FRect vid_tex_rect = (SDL_FRect){.x = preview_rect.x, .y = preview_rect.y, .w = preview_rect.w, .h = preview_rect.h};

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
	} else {
		// splat *something* to the screen
		SDL_RenderTexture(state->renderer, state->sw_tex, NULL, &vid_tex_rect);
	}

	float scrub_start_x = preview_rect.x + (preview_rect.w / 4);
	float scrub_end_x   = (preview_rect.x + preview_rect.w) - (preview_rect.w / 4);
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
	float bar_y = (preview_rect.y + preview_rect.h) - bar_h;
	float scrub_height = em + (em / 2);
	// bar background
	draw_rect(state,
		(FRect){.x = preview_rect.x, .y = bar_y, .w = preview_rect.w, .h = bar_h},
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
		.x = (preview_rect.x + (preview_rect.w / 2)) - (play_w / 2) + play_pad,
		.y = bar_y + play_pad,
		.w = play_w - (2 * play_pad),
		.h = play_w - (2 * play_pad)
	};

	draw_text(state, state->icon_font, start_stop_str,
		(FVec2){.x = (preview_rect.x + (preview_rect.w / 2)) - (start_stop_str_w / 2), .y = bar_y + (em / 2)},
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

	// Draw preview outline
	SDL_SetRenderDrawColor(state->renderer, 10, 10, 10, 255);
	SDL_RenderRect(state->renderer, &vid_tex_rect);

	SDL_RenderPresent(state->renderer);

	reset_cursor(&state->cur);
	state->frame_count += 1;
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	AppState *state = (AppState *)appstate;

	state->quit = true;
	pthread_join(state->pb.decode_thread, NULL);
}
