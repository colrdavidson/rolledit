#version 330 core

// idx_pos  {x, y}
layout(location=0) in vec2 idx_pos;

// rect_pos {x, y, width, height}
layout(location=1) in vec4 in_rect_pos;
layout(location=2) in vec4 color;
layout(location=3) in vec2 uv;

uniform float u_dpr;
uniform vec2  u_resolution;

out vec4 v_color;
out vec2 v_uv;
out vec4 v_rect_pos;
out vec2 v_idx_pos;

void main() {
	vec4 rect_pos = in_rect_pos * u_dpr;

	// if line
	if (uv.y < 0) {
		float width = (uv.x / 2) * u_dpr;
		vec2 a = rect_pos.xy;
		vec2 b = rect_pos.zw;
		vec2 center = mix(a, b, 0.5);
		vec2 fwd = normalize(b - a);
		vec2 norm = vec2(-fwd.y, fwd.x) * width;

		vec2 p0 = a - fwd + norm;
		vec2 p1 = b + fwd - norm;
		vec2 s = -2.0 * norm;
		vec2 t = p1 - p0 - s;

		vec2 xy = p0 + (idx_pos.x * s) + (idx_pos.y * t);
		gl_Position = vec4((xy / u_resolution) * 2.0 - 1.0, 0.0, 1.0);
		gl_Position.y = -gl_Position.y;

	// if rect
	} else {
		vec2 xy = vec2(rect_pos.x, rect_pos.y) + (idx_pos * vec2(rect_pos.z, rect_pos.w));

		gl_Position = vec4((xy / u_resolution) * 2.0 - 1.0, 0.0, 1.0);
		gl_Position.y = -gl_Position.y;
	}

	v_rect_pos = rect_pos;
	v_color = color;
	v_uv = uv;
	v_idx_pos = idx_pos;
}
