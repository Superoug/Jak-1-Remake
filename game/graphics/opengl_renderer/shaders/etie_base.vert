#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;  // (index A, index B) : milieux de subdivision = moyenne
// PC-REMAKE 21/09 : normale (monde, deja tournee par l'instance a l'unpack)
layout (location = 3) in vec4 normal_in;
layout (location = 6) in uint color3_in;  // PC-REMAKE : 3e couleur + poids (prototypes remodeles)
out vec3 w_nor;
// PC-REMAKE 13/10 : ombre du soleil (projection de lumiere sur les objets
// precurseurs) — ils etaient les seuls du decor a ne pas la recevoir
uniform mat4 sun_light_vp;
out vec4 sun_light_pos;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;

// etie stuff
uniform vec4 persp0;
uniform vec4 persp1;
uniform mat4 cam_no_persp;

void main() {
  float fog1 = camera[3].w + camera[0].w * position_in.x + camera[1].w * position_in.y + camera[2].w * position_in.z;
  fogginess = 255 - clamp(fog1 + hvdf_offset.w, fog_min, fog_max);
  sun_light_pos = sun_light_vp * vec4(position_in, 1.0);
  vec4 vf17 = cam_no_persp[3];
  vf17 += cam_no_persp[0] * position_in.x;
  vf17 += cam_no_persp[1] * position_in.y;
  vf17 += cam_no_persp[2] * position_in.z;
  vec4 p_proj = vec4(persp1.x * vf17.x, persp1.y * vf17.y, persp1.z, persp1.w);
  p_proj += persp0 * vf17.z;

  float pQ = 1.f / p_proj.w;
  vec4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  w_nor = normal_in.xyz;



  if (decal == 1) {
    fragment_color = vec4(1.0, 1.0, 1.0, 1.0);
  } else {
    // time of day lookup
    // PC-REMAKE : prototype remodele : melange des couleurs des trois coins du triangle d'origine
    // (original : 0.5 * (A + B) seulement)
    uint cw0 = (color3_in >> 16) & 0xffu, cw1 = (color3_in >> 24) & 0xffu;
    if (cw0 + cw1 > 0u) {
      float a = float(cw0) / 255.0, b = float(cw1) / 255.0;
      fragment_color = a * texelFetch(tex_T10, time_of_day_index.x, 0) +
                       b * texelFetch(tex_T10, time_of_day_index.y, 0) +
                       max(1.0 - a - b, 0.0) * texelFetch(tex_T10, int(color3_in & 0xffffu), 0);
    } else {
      fragment_color = (0.5 * (texelFetch(tex_T10, time_of_day_index.x, 0) + texelFetch(tex_T10, time_of_day_index.y, 0)));
    }
    // color adjustment
    fragment_color *= 2;
    fragment_color.a *= 2;
  }

  tex_coord = tex_coord_in;
}
