#version 410 core

out vec4 color;
// G-buffer de normales (attachement 1)
layout (location = 1) out vec4 out_normal;
in vec3 w_nor;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;
// metal et verre (pc_reflect_texture_kind)
uniform float reflet_kind;

// ombre du soleil (memes reglages que le decor, tfrag3.frag)
in vec4 sun_light_pos;
uniform vec3 sun_dir_w;
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

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float nl = length(w_nor);
  float slope = nl > 0.05 ? 1.0 - clamp(abs(dot(w_nor / nl, normalize(sun_dir_w))), 0.0, 1.0) : 0.5;
  float bias = 0.0016 + 0.0060 * slope * slope;
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  // penombre continue (pc_ombre_douce)
  acc = pc_ombre_douce(p.xy, p.z - bias) * 9.0;
  // fondu en CERCLE sur le dernier quart du rayon
  float rim = 1.0 - smoothstep(0.72, 0.97, length(p.xy - 0.5) * 2.0);
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}


void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // QA draws a decoupe peints en couleur pleine (tfrag=rouge, tie_wind=bleu, etie=jaune)

  // ALPHA AFFUTE pour l'alpha-to-coverage (MSAA)
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  color.rgb *= 1.0 - sun_shadow_factor();
  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));

  {
    float nl = length(w_nor);
    // drapeau des reflets du post
    float tag = reflet_kind > 1.5 ? 254.0 / 255.0 : reflet_kind > 0.75 ? 250.0 / 255.0 :
                reflet_kind > 0.25 ? 252.0 / 255.0 : 1.0;
    out_normal = (nl > 0.05) ? vec4(w_nor / nl * 0.5 + 0.5, tag) : vec4(0.0);
  }
}
