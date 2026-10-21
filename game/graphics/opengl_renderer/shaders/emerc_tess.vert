#version 410 core
// PC-REMAKE 21/10 — PERSONNAGES DECOUPES, passe des reflets (etape 1/3 : os) : copie conforme de
// merc2_tess.vert (meme surface que la passe de base). Meme calcul que merc2.vert pour la
// position et la normale (pose animee, espace camera) ; la projection, le brouillard et la lumiere
// se font apres le decoupage (merc2_tess.tese). La normale soudee (pad0 du sommet, voir
// Tfrag3Data pc_prepare_characters) suit les memes os.

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;
layout (location = 6) in uint weld_in;

out vec4 vs_pos;
out vec3 vs_nrm;
out vec3 vs_weld;
out float vs_vif;
out vec2 vs_st;
out vec4 vs_rgba;
out vec3 vs_ref;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};

vec3 normale_soudee(uint w, vec3 repli) {
  if ((w & 1u) == 0u) {
    return repli;
  }
  vec2 f = vec2(float((w >> 2) & 0x7fffu), float((w >> 17) & 0x7fffu)) / 32767.0 * 2.0 - 1.0;
  vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
  if (n.z < 0.0) {
    n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  }
  return normalize(n);
}

void main() {
  vec4 p = vec4(position_in, 1);
  vec3 wn = normale_soudee(weld_in, normal_in);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  vec3 rotated_nrm = bones[mats[0]].R * normal_in * weights_in[0];
  vec3 rotated_weld = bones[mats[0]].R * wn * weights_in[0];

  // game may send garbage bones if the weight is 0, don't let NaNs sneak in.
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
    rotated_nrm += bones[mats[1]].R * normal_in * weights_in[1];
    rotated_weld += bones[mats[1]].R * wn * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
    rotated_nrm += bones[mats[2]].R * normal_in * weights_in[2];
    rotated_weld += bones[mats[2]].R * wn * weights_in[2];
  }

  vs_pos = vtx_pos;
  vs_nrm = normalize(rotated_nrm);
  vs_weld = normalize(rotated_weld);
  vs_vif = ((weld_in & 2u) != 0u || (weld_in & 1u) == 0u) ? 1.0 : 0.0;
  vs_st = st_in;
  vs_rgba = rgba;
  vs_ref = position_in;
}
