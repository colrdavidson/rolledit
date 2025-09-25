#pragma once

typedef struct {
	float x;
	float y;
} FVec2;

typedef struct {
	float x;
	float y;
	float z;
	float w;
} FVec4;

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} BVec4;

typedef struct __attribute__((packed)) {
	FVec4 pos;
	BVec4 color;
	FVec2 uv;
} DrawRect;

typedef enum {
	V_idx_pos  = 0,
	V_rect_pos = 1,
	V_color    = 2,
	V_uv       = 3,
} VertAttrs;

FVec2 idx_pos[] = {
	{0, 0},
	{1, 0},
	{0, 1},
	{1, 1}
};

char *get_shader_type_str(GLenum type) {
	switch (type) {
	case GL_VERTEX_SHADER: return "vertex";
	case GL_FRAGMENT_SHADER: return "fragment";
	default: return "unknown";
	}
}

GLint build_shader(const char *str, GLenum type) {
	GLuint shdr = glCreateShader(type);
	glShaderSource(shdr, 1, &str, NULL);
	glCompileShader(shdr);

	GLint success = GL_FALSE;
	glGetShaderiv(shdr, GL_COMPILE_STATUS, &success);

	if (!success) {
		printf("%s shader %d compile error!\n", get_shader_type_str(type), shdr);
		GLint err_log_max_length = 0;
		glGetShaderiv(shdr, GL_INFO_LOG_LENGTH, &err_log_max_length);
		char *err_log = (char *)malloc(err_log_max_length);

		GLsizei err_log_length = 0;
		glGetShaderInfoLog(shdr, err_log_max_length, &err_log_length, err_log);
		printf("%s\n", err_log);
		return 0;
	}

	return shdr;
}
