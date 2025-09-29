#pragma once

#include <CoreVideo/CoreVideo.h>

BVec4 color_white = {.r = 255, .g = 255, .b = 255, .a = 255};
BVec4 color_black = {.r = 0, .g = 0, .b = 0, .a = 255};

BVec4 color_green  = {.r = 0,   .g = 155, .b = 53, .a = 255};
BVec4 color_blue   = {.r = 0,   .g = 121, .b = 155, .a = 255};
BVec4 color_purple = {.r = 85,  .g =   0, .b = 200, .a = 255};
BVec4 color_orange = {.r = 170, .g =  77, .b =  54, .a = 255};

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

float r_end_x(FRect r) {
	return r.x + r.w;
}
float r_end_y(FRect r) {
	return r.y + r.h;
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
		state->quit = true;
		return;
	}
	SDL_Texture *text_tex = SDL_CreateTextureFromSurface(state->renderer, tex_surf);
	SDL_DestroySurface(tex_surf);

	float w = 0;
	float h = 0;
	SDL_GetTextureSize(text_tex, &w, &h);
	SDL_FRect rect = (SDL_FRect){
		.x = (float)(int)pos.x,
		.y = (float)(int)pos.y,
		.w = w,
		.h = h
	};
	SDL_RenderTexture(state->renderer, text_tex, NULL, &rect);
	SDL_DestroyTexture(text_tex);
}
