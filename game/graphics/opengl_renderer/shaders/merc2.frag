#version 410 core

out vec4 color;
// PC-REMAKE 21/09 : G-buffer de normales (attachement 1) — rgb = normale monde
// *0.5+0.5, a = 1 quand le fragment en porte une (0 = le post reconstruit)
layout (location = 1) out vec4 out_normal;
in vec3 w_nor;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;
in vec3 v_nrm;
in vec4 v_rgba;

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;
uniform vec3 light_dir2;
uniform vec4 light_col0;
uniform vec4 light_col1;
uniform vec4 light_col2;
uniform vec4 light_ambient;
// PC-REMAKE 22/09 : 1 = eclairage par pixel (Phong), 0 = Gouraud d'origine (A/B)
uniform float per_pixel_light;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

// PC-REMAKE: characters receive sun shadows
in vec4 sun_light_pos;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz;
  if (abs(sun_light_pos.w) > 1e-5) {
    p /= sun_light_pos.w;
  }
  p = p * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float bias = 0.0075;  // characters are small: a touch more slack
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      // (original : px * 2.0) — 22/09 : penombre plus large, ombres moins dures
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px * 3.0, p.z - bias));
    }
  }
  // fondu de bordure : cohérent avec le décor (tfrag3.frag)
  vec2 edge = min(p.xy, 1.0 - p.xy);
  float rim = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

void main() {
  // PC-REMAKE 22/09 : le calcul de lumiere de la VU1, refait PAR PIXEL sur la
  // normale interpolee (meme formule que merc2.vert, meme resultat aux sommets)
  vec4 lit_color = vtx_color;
  if (per_pixel_light > 0.5) {
    vec3 n = normalize(v_nrm);
    vec3 li = light_dir0_fade.xyz * n.x + light_dir1_fade_en.xyz * n.y + light_dir2 * n.z;
    li = max(li, vec3(0.0));
    vec4 lc = light_ambient + li.x * light_col0 + li.y * light_col1 + li.z * light_col2;
    lit_color = v_rgba * lc;
  }
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = lit_color * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = lit_color.rgb;

    if (decal_enable == 0) {
      color.a = lit_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  // PC-REMAKE: darken the character when it stands in the sun's shadow
  color.xyz *= 1.0 - sun_shadow_factor();

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  {
    float nl = length(w_nor);
    out_normal = (nl > 0.05) ? vec4(w_nor / nl * 0.5 + 0.5, 1.0) : vec4(0.0);
  }
}
