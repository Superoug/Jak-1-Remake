#version 410 core

// PC-REMAKE sun shadow pass: depth-only render from the sun's point of view.
// `light_vp` is world->light clip for static geometry, or light*instance for
// wind-animated instances (palm crowns), so one shader serves both.

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;

uniform mat4 light_vp;

out vec2 uv;

void main() {
  gl_Position = light_vp * vec4(position_in, 1.0);
  uv = tex_coord_in.xy;
}
