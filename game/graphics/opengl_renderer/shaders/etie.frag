#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
in vec3 v_env_nrm;
in vec3 v_env_pos;

// le reflet (passe ADDITIVE) ne recevait pas l'ombre du soleil
in vec4 sun_light_pos;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;
// PENOMBRE CONTINUE
float pc_ombre_douce(vec2 uv, float zref) {
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  const float R = 2.6;
  float e = texture(sun_shadow_tex, vec3(uv, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(-R, -R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(R, -R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(-R, R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(R, R) * px, zref));
  if (e < 0.001 || e > 4.999) {
    return e / 5.0;
  }
  float acc = 0.0;
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      acc += texture(sun_shadow_tex, vec3(uv + vec2(dx, dy) * (1.3 * px), zref));
    }
  }
  return acc / 25.0;
}

float env_sun_shadow() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  // penombre continue (pc_ombre_douce)
  acc = pc_ombre_douce(p.xy, p.z - 0.004) * 9.0;
  // fondu en CERCLE sur le dernier quart du rayon
  float rim = 1.0 - smoothstep(0.72, 0.97, length(p.xy - 0.5) * 2.0);
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

// (reflets corrects mais POINTS parasites sur les objets precurseurs)
vec2 envmap_uv(vec3 nrm, vec3 pt) {
  vec3 n = nrm;
  n.z -= 1.0;
  float nrm_dot = dot(pt, n);
  vec3 rfl = pt * n.z;
  float q = -0.5 / max(length(rfl), 1e-6);
  n.xy *= nrm_dot;
  n.xy += rfl.xy;
  return n.xy * q + 0.5;
}
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    // coordonnee PAR SOMMET (comme le jeu)
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
    // reflets precurseur renforces (murs du temple de la jungle, objets)
    color.rgb *= 2.2 / (1.0 + 1.1 * dot(color.rgb, vec3(0.299, 0.587, 0.114)));
    // a l'ombre il reste le reflet du ciel (30 %)
    color.rgb *= 1.0 - 0.7 * env_sun_shadow();
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // ALPHA AFFUTE pour l'alpha-to-coverage (MSAA)
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
