#pragma once

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
		state->quit = true;
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
