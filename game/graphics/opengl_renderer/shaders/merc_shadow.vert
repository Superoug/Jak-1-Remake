#version 410 core

// depth-only pass that puts CHARACTERS AND PROPS into the sun shadow map
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;

uniform mat4 sun_light_from_cam;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

void main() {
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  // the game may send garbage bones when the weight is zero
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
  }
  // vtx_pos has w = -1
  vec4 sp = sun_light_from_cam * vtx_pos;
  gl_Position = vec4(sp.xyz, 1.0);
}
