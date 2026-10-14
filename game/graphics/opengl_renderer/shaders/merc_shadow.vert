#version 410 core

// PC-REMAKE: depth-only pass that puts CHARACTERS AND PROPS into the sun shadow
// map. Skinning is identical to merc2.vert — the same bone buffer, the same
// weights — so the shadow matches the animated pose exactly. The only
// difference is the destination: sun_light_from_cam takes the view-space vertex
// straight to shadow clip space (derivation in background_common.cpp).
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
  // vtx_pos has w = -1 (the game negates its bone matrices; the receive path
  // consumes .xyz without dividing, so it never notices). Rasterization DOES
  // divide by w, and dividing by -1 mirrored every vertex and splatted the
  // whole shadow map at depth ~0 — which erased every cast shadow in the game.
  // The xyz of this product is already correct; re-home it onto w = +1.
  vec4 sp = sun_light_from_cam * vtx_pos;
  gl_Position = vec4(sp.xyz, 1.0);
}
