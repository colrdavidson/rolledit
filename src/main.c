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
#include "dynarr.h"
#include "queue.h"
#include "types.h"
#include "ui.h"
#include "video.h"

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

void audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total) {
	PlaybackState *pb = (PlaybackState *)userdata;

	if (pb->pause) {
		return;
	}

	int64_t cur_time_us = pb->cur_time_us;

	// skip audio frames until we're at the closest one to presentation time
	int64_t skip_count = 0;
	Sample *cur_sample = NULL;

	pthread_mutex_lock(&pb->samples.lock);
	while (queue_size_unsafe(&pb->samples) > 0) {
		Sample *s = queue_peek_unsafe(&pb->samples);
		if (s->pts_us <= cur_time_us) {
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

		free_sample(cur_sample);
	}

	return;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
	if (argc < 2) {
		printf("expected file to open\n");
		return 1;
	}
	char *file_path = argv[1];

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
	TTF_SetFontHinting(state->sans_font, TTF_HINTING_NORMAL);

	state->mono_font = TTF_OpenFontIO(SDL_IOFromConstMem(mono_ttf, sizeof(mono_ttf)), true, em);
	if (!state->mono_font) {
		printf("Failed to open mono font\n");
		return SDL_APP_FAILURE;
	}
	TTF_SetFontHinting(state->sans_font, TTF_HINTING_MONO);

	state->icon_font = TTF_OpenFontIO(SDL_IOFromConstMem(icon_ttf, sizeof(icon_ttf)), true, em);
	if (!state->icon_font) {
		printf("Failed to open icon font\n");
		return SDL_APP_FAILURE;
	}
	TTF_SetFontHinting(state->sans_font, TTF_HINTING_NORMAL);

	dyn_init(&state->clips, 8);
	dyn_append(&state->clips, ((ClipState){
		.file_path = file_path,
	}));
	dyn_append(&state->clips, ((ClipState){
		.file_path = file_path,
	}));
	dyn_append(&state->clips, ((ClipState){
		.file_path = file_path,
	}));

	state->pb = (PlaybackState){
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

	char *out_file_path = realpath("test.mp4", NULL);
	state->tr = (TranscodeState){
		.out_file_path = out_file_path,

		.width = width,
		.height = height,
		.pix_fmt = AV_PIX_FMT_YUV420P,

		.sample_rate = 48000,
		.channels = 2,
		.sample_fmt = AV_SAMPLE_FMT_FLTP,
	};
	av_channel_layout_default(&state->tr.ch_layout, state->tr.channels);
	pthread_create(&state->tr.transcode_thread, NULL, transcode_video_thread, (void *)state);

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

	ClipState *cur_clip = NULL;
	int64_t total_duration_us = 0;
	int64_t cur_clip_offset_us = 0;
	if (pb->clips_loaded) {
		for (int i = 0; i < state->clips.size; i++) {
			total_duration_us += dyn_get(&state->clips, i).duration_us;
		}

		if (!pb->pause) {
			pb->cur_time_us = CLAMP(pb->cur_time_us + dt_us, 0, total_duration_us);
		}

		cur_clip = get_clip(state, pb->cur_time_us, &cur_clip_offset_us);
	}
	int64_t total_duration_s = (double)total_duration_us / 1000000.0;

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
		if (f->pts_us <= pb->cur_time_us) {
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

	float preview_h = (pb->height / 4) * 3;
	FRect preview_rect = {.x = pb->width / 4, .y = 0, .w = (pb->width / 4) * 3, .h = preview_h};
	{
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
				sws_scale(cur_clip->ps.sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, frame->height, buffers, strides);

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
	}

	// Draw timeline backdrop
	FRect timeline_rect = {.x = 0, .y = preview_h, .w = pb->width, .h = pb->height - preview_h};
	draw_rect(state,
		timeline_rect,
		(BVec4){.r = 30, .g = 30, .b = 30, .a = 255}
	);

	float edge_pad = em / 2;
	float toolbar_h = em * 3;
	{

		// Draw timeline toolbar
		draw_rect(state,
			(FRect){.x = timeline_rect.x, .y = timeline_rect.y, .w = timeline_rect.w, .h = toolbar_h},
			(BVec4){.r = 50, .g = 50, .b = 50, .a = 255}
		);

		float button_w = 2 * em;
		BVec4 button_color = (BVec4){.r = 80, .g = 80, .b = 80, .a = 255};

		// play button
		{
			char *play_str = "\uf04b";
			char *pause_str = "\uf04c";

			char *start_stop_str = pause_str;
			if (pb->pause) {
				start_stop_str = play_str;
			}

			int64_t start_stop_str_w = measure_text(state->icon_font, start_stop_str);

			FRect play_rect = (FRect){
				.x = preview_rect.x + (preview_rect.w / 2) - button_w - (edge_pad / 2),
				.y = timeline_rect.y + (toolbar_h / 2) - (button_w / 2),
				.w = button_w,
				.h = button_w
			};

			draw_rect(state, play_rect, button_color);
			draw_text(state, state->icon_font, start_stop_str,
				(FVec2){.x = play_rect.x + (button_w / 2) - (start_stop_str_w / 2), .y = play_rect.y + (button_w / 2) - (em / 2)},
				color_white
			);

			if (state->cur.clicked && pt_in_rect(state->cur.clicked_pos, play_rect)) {
				pb->was_paused = !pb->pause;
				pb->pause = !pb->pause;
			}
		}

		// transcode button
		FRect transcode_button_rect = (FRect){
			.x = r_end_x(timeline_rect) - button_w - edge_pad,
			.y = timeline_rect.y + (toolbar_h / 2) - (button_w / 2),
			.w = button_w,
			.h = button_w
		};
		draw_rect(state, transcode_button_rect, button_color);

		char *transcode_str = "\uf019";
		int64_t transcode_str_w = measure_text(state->icon_font, transcode_str);
		draw_text(state, state->icon_font, transcode_str,
			(FVec2){.x = transcode_button_rect.x + (button_w / 2) - (transcode_str_w / 2), .y = transcode_button_rect.y + (button_w / 2) - (em / 2)},
			color_white
		);

		int64_t time_s = pb->cur_time_us / 1000000;

		char *cur_time_str = secs_to_timestr(time_s);
		char *total_time_str = secs_to_timestr(total_duration_s);
		char *time_str = NULL;
		asprintf(&time_str, "%s / %s", cur_time_str, total_time_str);
		free(cur_time_str);
		free(total_time_str);

		int64_t time_str_w = measure_text(state->mono_font, time_str);

		draw_text(state, state->mono_font, time_str,
			(FVec2){
				.x = transcode_button_rect.x - time_str_w - edge_pad,
				.y = timeline_rect.y + (toolbar_h / 2) - (em / 2)
			},
			color_white
		);
		free(time_str);

		if (state->cur.clicked && pt_in_rect(state->cur.clicked_pos, transcode_button_rect) && !state->transcoding) {
			state->start_transcode = true;
		}
	}

	// video scrubber
	double scrub_pos = 0.0;
	float scrub_bar_h = 2 * em;
	{
		float cursor_w = (float)(int)((em / 3) * 2);
		float scrub_start_x = timeline_rect.x + edge_pad;
		float scrub_end_x   = r_end_x(timeline_rect) - edge_pad;
		float scrub_width = scrub_end_x - scrub_start_x;

		float bar_y = timeline_rect.y + toolbar_h;
		float scrub_height = em + (em / 2);

		if (total_duration_us != 0) {
			double watch_perc = 0.0;
			if (state->transcoding) {
				watch_perc = (double)state->tr.cur_time_us / total_duration_us;
			} else {
				watch_perc = (double)pb->cur_time_us / total_duration_us;
			}
			scrub_pos = lerp(0.0, scrub_width, watch_perc);
		}

		// bar background
		draw_rect(state,
			(FRect){.x = timeline_rect.x, .y = bar_y, .w = timeline_rect.w, .h = scrub_bar_h},
			(BVec4){.r = 20, .g = 20, .b = 20, .a = 255}
		);

		float overlay_y = bar_y + edge_pad;
		float overlay_h = scrub_bar_h / 2;
		// scrub overlay
		draw_rect(state,
			(FRect){.x = scrub_start_x, .y = overlay_y, .w = scrub_width, .h = overlay_h},
			(BVec4){.r = 60, .g = 60, .b = 60, .a = 255}
		);

		// scrub cursor
		FRect cursor_rect = (FRect){
			.x = scrub_start_x + scrub_pos - (cursor_w / 2),
			.y = overlay_y + (overlay_h / 2) - (scrub_height / 2),
			.w = cursor_w,
			.h = scrub_height
		};

		if (!state->transcoding) {
			if (state->cur.clicked && pt_in_rect(state->cur.clicked_pos, cursor_rect) && !state->seeking) {
				state->seeking = true;
				pb->pause = true;
				pb->seek_time_us = pb->cur_time_us;
			}
			if (state->cur.is_down && state->seeking) {
				float seek_perc = pan_delta.x / scrub_width;
				int64_t watch_off_us = lerp(0.0, (double)total_duration_us, seek_perc);

				int64_t new_time = CLAMP(pb->cur_time_us + watch_off_us, 0, total_duration_us);
				pb->seek_time_us = new_time;
				pb->cur_time_us  = new_time;
			}
			if (state->cur.up_now && state->seeking) {
				state->seeking = false;
				pb->should_seek = true;
			}
		}

		draw_rect(state, cursor_rect, (BVec4){.r = 200, .g = 200, .b = 200, .a = 255});
	}

	float timeline_vids_y = timeline_rect.y + toolbar_h + scrub_bar_h;
	float timeline_vids_h = pb->height - timeline_vids_y;

	// Draw videos on timeline
	float video_h = 3 * em;
	BVec4 colors[4] = {
		color_green,
		color_blue,
		color_purple,
		color_orange
	};

	for (int i = 0; i < state->clips.size; i++) {
		ClipState *clip = &dyn_get(&state->clips, i);

		char *filename = shortname(clip->file_path);
		int64_t name_w = measure_text(state->sans_font, filename);

		float min_video_w = name_w + edge_pad;
		
		float vid_duration_s = (double)clip->duration_us / 1000000.0;
		float dur_perc = vid_duration_s / total_duration_s;
		float video_w = lerp(timeline_rect.x, timeline_rect.w - (edge_pad * 2), dur_perc);

		float video_rect_w = MAX(video_w, min_video_w);

		FRect video_rect = (FRect){
			.x = timeline_rect.x + edge_pad + (video_rect_w * i),
			.y = timeline_vids_y + (timeline_vids_h / 2) - (video_h / 2),
			.w = video_rect_w,
			.h = video_h
		};

		BVec4 color = colors[i];
		draw_rect(state, video_rect, color);
		draw_text(state, state->sans_font, filename,
			(FVec2){.x = video_rect.x + (edge_pad / 2), .y = video_rect.y + (video_rect.h / 2) - (em / 2)},
			color_white
		);
	}

	// Draw scrub stalk
	draw_rect(state,
	(FRect){
		.x = timeline_rect.x + edge_pad + scrub_pos,
		.y = timeline_rect.y + toolbar_h + (scrub_bar_h / 2),
		.w = 1,
		.h = timeline_vids_h - (2 * edge_pad)
	}, (BVec4){.r = 200, .g = 200, .b = 200, .a = 255});

	float progress_bar_h = em;
	float progress_bar_start_x = timeline_rect.x + edge_pad;
	float progress_bar_end_x = timeline_rect.x + timeline_rect.w - edge_pad;
	float progress_bar_w = progress_bar_end_x - progress_bar_start_x;
	float progress_bar_y = pb->height - progress_bar_h - edge_pad;
	if (state->transcoding) {
		// Draw progress bar background
		draw_rect(state,
		(FRect){
			.x = progress_bar_start_x,
			.y = progress_bar_y,
			.w = progress_bar_w,
			.h = progress_bar_h
		}, (BVec4){.r = 10, .g = 10, .b = 10, .a = 255});

		float progress_perc = (double)state->tr.cur_time_us / (double)total_duration_us;
		float progress_w = lerp(progress_bar_start_x, progress_bar_end_x, progress_perc);

		// Draw progress bar
		draw_rect(state,
		(FRect){
			.x = progress_bar_start_x,
			.y = progress_bar_y,
			.w = progress_w,
			.h = progress_bar_h
		}, (BVec4){.r = 0, .g = 150, .b = 0, .a = 255});
	}
	if (state->done_transcoding) {
		char *done_str = NULL;
		asprintf(&done_str, "Finished encoding to %s", shortname(state->tr.out_file_path));

		int64_t done_str_w = measure_text(state->sans_font, done_str);
		draw_text(state, state->sans_font, done_str,
			(FVec2){.x = progress_bar_start_x + progress_bar_w - done_str_w, .y = progress_bar_y},
			color_white
		);
		free(done_str);
	}

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
