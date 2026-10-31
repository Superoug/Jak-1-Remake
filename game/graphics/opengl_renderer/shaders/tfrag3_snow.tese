#version 410 core
// SOL DE NEIGE DEFORMABLE (etape deformation)
layout (triangles, equal_spacing, ccw) in;

in vec3 tc_pos[];
in vec3 tc_tc[];
in vec4 tc_color[];
in vec3 tc_nor[];

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform float fog_min;
uniform float fog_max;
uniform mat4 sun_light_vp;
uniform sampler2D snow_trail_tex;  // unite 7 : R relief (m), G pieds (m), B instant (s)
uniform vec4 snow_trail;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec4 sun_light_pos;
out vec3 w_nor;
flat out float v_foliage;
out float v_dist;
out vec3 v_pos_w;
out vec3 v_vue;  // camera -> sommet (parallaxe de la sculpture du decor)

// --- TRACE DANS LA NEIGE
const float SNOW_N = 1024.0;  // texels de la carte (fenetre de 102,4 m)
const float SNOW_TW = 0.10 * SNOW_N;
float snow_hash(ivec2 i) {
  uint h = uint(i.x) * 374761393u + uint(i.y) * 668265263u;
  h = (h ^ (h >> 13u)) * 1274126177u;
  return float((h ^ (h >> 16u)) & 0xffffffu) / 16777216.0;
}
float snow_vnoise(vec2 p) {
  vec2 fl = floor(p);
  ivec2 i = ivec2(fl);
  vec2 f = p - fl;
  f = f * f * (3.0 - 2.0 * f);
  float a = snow_hash(i), b = snow_hash(i + ivec2(1, 0));
  float c = snow_hash(i + ivec2(0, 1)), d = snow_hash(i + ivec2(1, 1));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// epaisseur : la neige epaisse (surface au-dessus des pieds de Jak) se tasse davantage
float snow_thick(float dyg) {
  return clamp(dyg / 0.35, 1.0, 3.5);
}
// relief (m) au point pm (m) de la surface d'origine
float snow_relief(vec3 pm, float now) {
  vec2 px = pm.xz / SNOW_TW * SNOW_N - 0.5;
  vec2 fl = floor(px);
  vec2 f = px - fl;
  ivec2 i0 = ivec2(fl);
  float dist = 0.0, depth = 0.0, fill = 0.0, feet = 0.0, wv = 0.0;
  for (int k = 0; k < 4; k++) {
    vec4 t = texelFetch(snow_trail_tex, (i0 + ivec2(k & 1, k >> 1)) & 1023, 0);
    float w = ((k & 1) == 1 ? f.x : 1.0 - f.x) * ((k >> 1) == 1 ? f.y : 1.0 - f.y);
    dist += w * t.r;
    depth += w * t.a;
    if (t.b > -1e5) {
      float age = now - t.b;
      if (age < 0.0) age += 3600.0;
      fill += w * (1.0 - smoothstep(120.0, 240.0, age));  // la neige recomble en 2 a 4 min
      feet += w * t.g;
      wv += w;
    }
  }
  // hors de portee du sillon le plus large
  if (wv < 1e-3 || fill < 1e-3 || dist > 1.7) return 0.0;
  float dyg = pm.y - feet / wv;
  if (dyg < -1.5 || dyg > 2.6) return 0.0;
  float thick = snow_thick(dyg);
  // largeur : x1 en neige fine, jusqu'a x1,75 en neige epaisse
  float r = 0.40 * (1.0 + 0.30 * (thick - 1.0));
  float jag = (snow_vnoise(pm.xz * 1.3) - 0.5) * 0.22 + (snow_vnoise(pm.xz * 3.5) - 0.5) * 0.10;
  float d = dist * (1.0 + jag);
  if (d > 1.95 * r) return 0.0;
  // profondeur
  float dp = min(max(depth * (1.0 + 0.6 * (thick - 1.0)), dyg * smoothstep(0.0, 0.08, depth)),
                 max(dyg, 0.0) + 0.2);
  // fond plat, paroi en S, puis bourrelet de neige repoussee
  float h = -dp * (1.0 - smoothstep(0.35 * r, 1.05 * r, d));
  if (d > 0.8 * r) {
    float t = clamp((d - 0.8 * r) / (1.1 * r), 0.0, 1.0);
    h += 0.28 * depth * sqrt(thick) * sin(3.14159 * t) * smoothstep(0.8 * r, 1.1 * r, d);
  }
  return h * fill;
}

void main() {
  vec3 bc = gl_TessCoord;
  vec3 pos_w = bc.x * tc_pos[0] + bc.y * tc_pos[1] + bc.z * tc_pos[2];
  vec3 nor = normalize(bc.x * tc_nor[0] + bc.y * tc_nor[1] + bc.z * tc_nor[2] + vec3(1e-5));
  const float M = 4096.0;

  // une partie du sol de neige a des normales INVERSEES (vers le bas) dans les donnees
  vec3 pos_ombre = pos_w;
  float nsign = nor.y < 0.0 ? -1.0 : 1.0;
  vec3 nup = nor * nsign;
  if (snow_trail.x > 0.5 && nup.y > 0.45) {
    vec3 pm = pos_w / M;
    if (length(pm.xz - snow_trail.yz / M) < 50.0) {
      float now = snow_trail.w;
      float h = snow_relief(pm, now);
      // normale du creux (differences finies a 6 cm)
      const float e = 0.06;
      float hx = snow_relief(pm + vec3(e, 0.0, 0.0), now) - snow_relief(pm - vec3(e, 0.0, 0.0), now);
      float hz = snow_relief(pm + vec3(0.0, 0.0, e), now) - snow_relief(pm - vec3(0.0, 0.0, e), now);
      pos_w.y += h * M;
      nor = nsign * normalize(nup + vec3(-hx, 0.0, -hz) / (2.0 * e));
    }
  }

  vec3 vert = pos_w - cam_trans.xyz;
  v_dist = length(vert);
  v_pos_w = pos_w;
  v_vue = vert;
  vec4 transformed = -pc_camera[3];
  transformed.w = 0;
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  sun_light_pos = sun_light_vp * vec4(pos_ombre, 1.0);
  w_nor = nor;
  v_foliage = 0.0;
  fragment_color = bc.x * tc_color[0] + bc.y * tc_color[1] + bc.z * tc_color[2];
  tex_coord = bc.x * tc_tc[0] + bc.y * tc_tc[1] + bc.z * tc_tc[2];
}
