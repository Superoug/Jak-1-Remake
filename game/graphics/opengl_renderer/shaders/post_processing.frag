#version 410 core

uniform sampler2D tex_T0;
out vec4 color;
in vec2 tex_coord;

uniform vec4 color_mult;
uniform vec4 color_add;

// post effects, all neutral at zero (uniform default): x = bloom intensity
uniform vec4 post_fx;

// toon / cel-shading, neutral at zero: x = posterize strength (0..1), y = edge-line strength (0..1)
uniform vec4 toon_fx;

// depth-aware post, neutral at zero: x = SSAO strength, y = aerial fog density, z = far DOF strength
uniform vec4 depth_fx;
uniform sampler2D depth_tex;
// G-buffer de normales (attachement 1 resolu)
uniform sampler2D normal_tex;
uniform float has_normal_tex;
// [QA] 1 = afficher les normales (G-buffer en couleur, repli en gris)
uniform float qa_show_normals;

// rebuilt lighting rig light_fx
uniform vec4 light_fx;
// RENDU REMAKE
uniform vec4 grade_fx;
// x = exposition, y = contraste, z = saturation, w = libre
uniform vec4 grade_amt2;
// la scene AVANT sprites/HUD/textes
uniform sampler2D scene_tex;
uniform float has_scene_tex;
// BLOOM LARGE (chaine de mips -> C++ pc_bloom_run), unite 11
uniform sampler2D bloom_tex;
uniform float has_bloom_tex;
// la meme scene SANS HUD (prise avant les sprites d'interface)
uniform sampler2D hud_ref_tex;
// PERF POST EN DEUX PASSES
#ifdef POST_PASS2
uniform sampler2D lit_tex;
#endif
uniform float has_hud_ref;
// x = reflets (0-2), y = halos, z = contact, w = gain des LENS FLARES
uniform vec4 fx_quality;
uniform vec4 sun_screen;
uniform vec4 sky_color;
uniform vec4 ground_color;
uniform vec4 night_fx;
// scintillement de l'eau
uniform vec4 water_glitter;
// 1 = la surface de l'eau a pose la brume (ocean_common.frag)
uniform float water_murk_ocean;
// xyz = normalized direction TOWARDS the sun (world), w = 1 when it is up
uniform vec4 sun_dir;
uniform float foliage_transl;  // translucidite du feuillage (0 = coupee)
// reflets du metal et du verre
uniform vec4 reflet_fx;
// panorama de l'entourage vu par la camera (reflet_env.frag)
uniform sampler2D reflet_env_tex;
uniform float has_reflet_env;
// clarte moyenne de l'image actuelle (1 x 1, reflet_moy.frag)
uniform sampler2D reflet_moy_tex;
// lumiere rebondie (gi_rebond.frag, quart de resolution adouci)
uniform sampler2D gi_tex;
uniform float has_gi_tex;
uniform vec4 gi_fx;
// y = caustiques, z = temps (s)
uniform vec4 modern_fx;

// World position is rebuilt from the depth buffer with the inverse of the game's own camera matrix
uniform mat4 inv_camera;
uniform mat4 clip_from_world;  // the forward transform, for screen-space ray marching
uniform vec4 cam_trans;

#define MAX_LAMPS 48
// faisceaux d'eco (2 extremites monde par faisceau)
uniform vec4 beam_seg[24];
uniform float beam_n;
// rayons de soleil volumetriques (jungle)
uniform sampler2DShadow sun_shadow_tex;
uniform mat4 sun_light_vp;
uniform float sun_map_ok;  // 1 = carte d'ombre du soleil liee (unite 14) et sun_light_vp valide
uniform float shaft_amt;
// traces dans la neige (unite 7)
uniform sampler2D snow_trail_tex;
uniform vec4 snow_trail;
// empreintes dans le sable (background_common.cpp, SandPrints)
uniform sampler2D sable_tex;
uniform vec4 sable;
uniform float snow_geom;  // 1 = le sol est vraiment creuse (geometrie decoupee)
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
// rayons au quart de resolution, adoucis (unite 8)
uniform sampler2D shaft_tex;
uniform float has_shaft_tex;
// --- FAISCEAUX DE CANOPEE (jungle)
const float CB_CELL = 7.0;
vec3 cb_axis(vec3 to_sun) {
  return normalize(vec3(to_sun.x * 0.36, 1.0, to_sun.z * 0.36));
}
// projection le long de L sur le plan y = 0 (metres)
vec2 cb_proj(vec3 p_m, vec3 L) {
  return p_m.xz - L.xz / L.y * p_m.y;
}
float cb_hash(vec2 c, float k) {
  return fract(sin(dot(c, vec2(127.1, 311.7)) + k * 74.7) * 43758.5453);
}
// colonne de la cellule
vec4 cb_beam(vec2 cell, float t) {
  if (cb_hash(cell, 0.0) > 0.24) {
    return vec4(0.0);
  }
  float h = cb_hash(cell, 1.0);
  float r = mix(0.45, 1.3, h);
  vec2 off = vec2(cb_hash(cell, 2.0), cb_hash(cell, 3.0));
  vec2 ctr = cell * CB_CELL + vec2(r) + off * (CB_CELL - 2.0 * r);
  float w = mix(0.55, 1.0, cb_hash(cell, 4.0));
  // le feuillage bouge
  w *= 0.78 + 0.22 * sin(t * (0.45 + 0.35 * h) + 6.2832 * cb_hash(cell, 5.0));
  return vec4(ctr, r, w);
}
// une colonne n'a de source que s'il y a du ciel au-dessus du feuillage
float cb_ciel(vec3 q, vec3 L) {
  for (int i = 1; i <= 2; i++) {
    vec3 s = (sun_light_vp * vec4(q + L * (12.0 * 4096.0 * float(i)), 1.0)).xyz * 0.5 + 0.5;
    if (s.x < 0.01 || s.x > 0.99 || s.y < 0.01 || s.y > 0.99 || s.z > 1.0) {
      return 1.0;
    }
    if (texture(sun_shadow_tex, vec3(s.xy, s.z - 0.002)) > 0.5) {
      return 1.0;
    }
  }
  return 0.0;
}
// liseré d'ecume et ressac le long du trait de cote (cote mer)
const bool NAPPE_SABLE_MER = false;
// --- VAGUES DE RIVAGE
float sh_hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float sh_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(sh_hash(i), sh_hash(i + vec2(1.0, 0.0)), f.x),
             mix(sh_hash(i + vec2(0.0, 1.0)), sh_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float sh_phase(vec2 xz_m, float t) {
  float n = sh_noise(xz_m * 0.045) * 1.7 + sh_noise(xz_m * 0.13 + 3.7) * 0.35;
  return fract(t / 7.0 + n);
}
// hauteur (m au-dessus de la mer) atteinte par la nappe a la phase ph
float sh_runup(float ph, float hmax) {
  if (ph < 0.55) {
    return 0.0;
  }
  float s = (ph - 0.55) / 0.45;
  return s < 0.3 ? hmax * sin(s / 0.3 * 1.5708) : hmax * pow(1.0 - (s - 0.3) / 0.7, 1.4);
}
// dentelle d'ecume (motif ancre au monde, qui derive doucement)
float sh_lace(vec2 xz_m, float t) {
  // ecume plus fine, en filaments (trois echelles, la plus fine etiree)
  float a = sh_noise(xz_m * 2.6 + vec2(t * 0.11, -t * 0.07));
  float b = sh_noise(xz_m * vec2(7.0, 3.2) - vec2(t * 0.05, t * 0.09));
  float f = sh_noise(xz_m * 13.0 + vec2(t * 0.2, 0.0));
  float v = 0.5 * a + 0.32 * b + 0.18 * f;
  return smoothstep(0.38, 0.66, v) * (0.75 + 0.25 * f);
}
// --- PC-REMAKE
vec2 houle_pente(vec2 xz, float t, float pix_m) {
  // directions etalees autour du vent (pas de rangees paralleles), longueurs non harmoniques
  const vec2 D[6] = vec2[6](vec2(0.80, 0.60), vec2(0.93, -0.37), vec2(0.38, 0.93),
                            vec2(0.99, 0.10), vec2(0.55, -0.83), vec2(0.07, 1.00));
  const float L[6] = float[6](12.3, 7.4, 4.6, 2.9, 1.75, 1.05);
  const float K[6] = float[6](0.050, 0.052, 0.050, 0.044, 0.036, 0.028);
  // champ deforme a deux echelles et en groupes de vagues
  vec2 n2 = vec2(sh_noise(xz * 0.045 + 1.3), sh_noise(xz * 0.045 + 7.9));
  vec2 n3 = vec2(sh_noise(xz * 0.17 + 4.1), sh_noise(xz * 0.17 + 9.6));
  vec2 w0 = xz + n2 * 9.0 + n3 * 1.6;
  vec2 g = vec2(0.0);
  for (int i = 0; i < 6; i++) {
    float k = 6.2832 / L[i];
    float om = sqrt(9.81 * k);
    float f = 1.0 - smoothstep(L[i] * 0.10, L[i] * 0.30, pix_m);
    if (f <= 0.0) {
      continue;
    }
    // chaque train a ses propres groupes (zones calmes, zones plus agitees) qui derivent
    float grp = sh_noise(xz * (0.9 / L[i]) + vec2(float(i) * 3.1 + t * 0.04, float(i) * 1.7));
    grp = 0.2 + 1.6 * grp * grp;
    g += D[i] * (K[i] * f * grp * cos(k * dot(D[i], w0) - om * t + float(i) * 1.7));
  }
  return g;
}
// EAU VIVANTE
uniform vec4 eau_houle_p;
vec2 houle_vivante_pente(vec2 p) {
  const float L[6] = float[6](8.5, 13.0, 19.0, 28.0, 41.0, 63.0);
  const float DIR[6] = float[6](0.95, 0.35, 0.70, 0.15, 0.55, 0.42);
  const float AMP[6] = float[6](0.035, 0.05, 0.07, 0.09, 0.11, 0.13);
  const float PHI[6] = float[6](0.0, 1.7, 4.1, 2.6, 5.3, 3.4);
  vec2 g = vec2(0.0);
  for (int i = 0; i < 6; i++) {
    float k = 6.2831853 / L[i];
    float w = sqrt(9.81 * k);
    vec2 D = vec2(cos(DIR[i]), sin(DIR[i]));
    g += D * (AMP[i] * k * cos(k * dot(D, p) - w * eau_houle_p.x + PHI[i]));
  }
  return g;
}
// Eclairage de la surface par la houle (meme mer turquoise, rien n'est blanchi)
uniform vec4 lune;  // direction de la lune, w = presence (ciel de nuit)
vec3 houle_surface(vec3 c, vec3 N, vec3 I, float jour, float fondu) {
  float cv = clamp(dot(-I, N), 0.0, 1.0);
  float cf = clamp(-I.y, 0.0, 1.0);
  float F = 0.02 + 0.98 * pow(1.0 - cv, 5.0);
  float F0 = 0.02 + 0.98 * pow(1.0 - cf, 5.0);
  // contraste doux
  float d = clamp((F - F0) * 3.0, -0.18, 0.24) * fondu;
  vec3 clair = c * 1.20 + vec3(0.03, 0.06, 0.06) * max(jour, 0.2);
  vec3 sombre = c * 0.86;
  c = d > 0.0 ? mix(c, clair, d / 0.24 * 0.5) : mix(c, sombre, -d / 0.18 * 0.5);
  if (sun_dir.w > 0.5) {
    vec3 R = reflect(I, N);
    float rs = max(dot(R, sun_dir.xyz), 0.0);
    float glint = pow(rs, 900.0) * 3.0 + pow(rs, 120.0) * 0.22;
    c += vec3(1.0, 0.95, 0.82) * glint * jour * fondu;
  }
  // CHEMIN DE LUNE sur la mer la nuit
  if (lune.w > 0.001) {
    vec3 R = reflect(I, N);
    float rm = max(dot(R, lune.xyz), 0.0);
    float gl = pow(rm, 700.0) * 2.2 + pow(rm, 60.0) * 0.10;
    c += vec3(0.78, 0.84, 1.0) * gl * lune.w * fondu;
  }
  return c;
}
float cb_hash3(vec3 c) {
  return fract(sin(dot(c, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
// bruit doux (taches de feuilles dans la lumiere au sol)
float cb_vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(cb_hash(i, 7.0), cb_hash(i + vec2(1.0, 0.0), 7.0), f.x),
             mix(cb_hash(i + vec2(0.0, 1.0), 7.0), cb_hash(i + vec2(1.0, 1.0), 7.0), f.x), f.y);
}
uniform sampler2D caustique_tex;  // motif des caustiques (pc_caustique_tex)
// CAUSTIQUES
float caustique(vec2 p, float t, float px) {
  p += 0.32 * vec2(sin(p.y * 0.83 + t * 0.35) + sin(p.y * 0.37 - t * 0.21),
                   sin(p.x * 0.71 - t * 0.29) + sin(p.x * 0.41 + t * 0.17));
  const float K = 1.35;  // la texture est normalisee par 1,35
  float l1 = texture(caustique_tex, p / 8.0 + vec2(t * 0.013, t * 0.008)).r * K;
  vec2 p2 = mat2(0.81, -0.59, 0.59, 0.81) * p * 1.53 + vec2(3.1, 7.7);
  float l2 = texture(caustique_tex, p2 / 8.0 - vec2(t * 0.010, t * 0.015)).r * K;
  float l = l1 * 0.45 + l2 * 0.35 + l1 * l2 * 1.4;
  // l'intensite varie lentement d'un endroit a l'autre
  l *= 0.62 + 0.38 * sin(p.x * 0.13 + t * 0.05) * sin(p.y * 0.11 - t * 0.04);
  return l * (1.0 - smoothstep(0.12, 0.35, px));
}
uniform vec4 lamp_pos[MAX_LAMPS];  // xyz = world position, w = radius
uniform vec4 lamp_color[MAX_LAMPS];  // rgb = colour, w = intensity
// PERF position ecran des lampes, calculee une fois par image par le C++
uniform vec4 lamp_scr[MAX_LAMPS];
// rang d'origine de chaque lampe (la liste est triee par le C++)
uniform float lamp_orig[MAX_LAMPS];
// PERF tuiles de lampes (16 x 9)
uniform uvec2 lamp_tiles[144];
uvec2 pc_tile_mask() {
  int tx = clamp(int(tex_coord.x * 16.0), 0, 15);
  int ty = clamp(int(tex_coord.y * 9.0), 0, 8);
  return lamp_tiles[ty * 16 + tx];
}
// prochaine lampe du masque (ordre croissant, comme l'ancienne boucle), -1 = fini
int pc_next_lamp(inout uvec2 m) {
  if (m.x != 0u) {
    int i = findLSB(m.x);
    m.x &= m.x - 1u;
    return i;
  }
  if (m.y != 0u) {
    int i = 32 + findLSB(m.y);
    m.y &= m.y - 1u;
    return i;
  }
  return -1;
}

// PERF directions des echantillons PRECALCULEES
const vec2 kRing6[6] = vec2[6](vec2(1.00000000, 0.00000000), vec2(0.50000000, 0.86602540), vec2(-0.50000000, 0.86602540), vec2(-1.00000000, 0.00000000), vec2(-0.50000000, -0.86602540), vec2(0.50000000, -0.86602540));
const vec2 kRing10[10] = vec2[10](vec2(0.95105652, 0.30901699), vec2(0.58778525, 0.80901699), vec2(0.00000000, 1.00000000), vec2(-0.58778525, 0.80901699), vec2(-0.95105652, 0.30901699), vec2(-0.95105652, -0.30901699), vec2(-0.58778525, -0.80901699), vec2(-0.00000000, -1.00000000), vec2(0.58778525, -0.80901699), vec2(0.95105652, -0.30901699));
const vec2 kOct8[8] = vec2[8](vec2(1.00000000, 0.00000000), vec2(0.70710690, 0.70710667), vec2(0.00000033, 1.00000000), vec2(-0.70710643, 0.70710713), vec2(-1.00000000, 0.00000065), vec2(-0.70710736, -0.70710620), vec2(-0.00000098, -1.00000000), vec2(0.70710597, -0.70710759));
const vec2 kSsao8[8] = vec2[8](vec2(0.32631457, 0.12656540), vec2(0.30336061, 0.68779238), vec2(-0.18205318, 0.46937466), vec2(-0.82818694, 0.36528355), vec2(-0.61243474, -0.23754098), vec2(-0.16489631, -0.37386024), vec2(0.29302880, -0.75549481), vec2(0.51425480, -0.22681926));

// couleur de l'eau vue a travers une colonne de col_m metres
vec3 sea_murk(float col_m) {
  float day_w = clamp(1.0 - night_fx.x, 0.0, 1.0);
  vec3 murk_shallow = mix(vec3(0.03, 0.12, 0.22), vec3(0.16, 0.52, 0.54), day_w);
  vec3 murk_deep = mix(vec3(0.015, 0.075, 0.16), vec3(0.11, 0.42, 0.47), day_w);
  return mix(murk_shallow, murk_deep, clamp(col_m / 9.0, 0.0, 1.0));
}

vec3 world_from_depth(vec2 uv, float d) {
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 p = inv_camera * ndc;
  if (abs(p.w) < 1e-8) {
    return cam_trans.xyz;
  }
  return p.xyz / p.w + cam_trans.xyz;
}

// part d'interieur de la cabane de Samos AU POINT P (unites du jeu), 0..1
float pc_hut_inside(vec3 P) {
  vec3 q = P * (1.0 / 4096.0);
  vec2 dq = q.xz - vec2(-130.5, 208.4);
  float in_r = clamp((16.4 - length(dq)) / 0.8, 0.0, 1.0);
  float in_y = clamp((q.y - 31.5) * 0.5, 0.0, 1.0) * clamp((80.0 - q.y) * 0.5, 0.0, 1.0);
  return in_r * in_y;
}

// Surface orientation is not available anywhere in the original pipeline
vec3 raw_normal(vec2 uv, float d, vec3 P, float radius) {
  vec2 px = radius / vec2(textureSize(depth_tex, 0));

  float dxp = texture(depth_tex, uv + vec2(px.x, 0.0)).r;
  float dxm = texture(depth_tex, uv - vec2(px.x, 0.0)).r;
  // au bord de l'ecran, le voisin exterieur n'existe pas
  bool px_ok = uv.x + px.x <= 1.0, mx_ok = uv.x - px.x >= 0.0;
  bool use_xp = (px_ok && mx_ok) ? abs(dxp - d) < abs(dxm - d) : px_ok;
  vec3 tx = use_xp ? world_from_depth(uv + vec2(px.x, 0.0), dxp) - P
                   : P - world_from_depth(uv - vec2(px.x, 0.0), dxm);

  float dyp = texture(depth_tex, uv + vec2(0.0, px.y)).r;
  float dym = texture(depth_tex, uv - vec2(0.0, px.y)).r;
  bool py_ok = uv.y + px.y <= 1.0, my_ok = uv.y - px.y >= 0.0;
  bool use_yp = (py_ok && my_ok) ? abs(dyp - d) < abs(dym - d) : py_ok;
  vec3 ty = use_yp ? world_from_depth(uv + vec2(0.0, px.y), dyp) - P
                   : P - world_from_depth(uv - vec2(0.0, px.y), dym);

  vec3 n = cross(tx, ty);
  float l = length(n);
  if (l < 1e-6) {
    return vec3(0.0, 1.0, 0.0);
  }
  n /= l;
  return dot(n, cam_trans.xyz - P) < 0.0 ? -n : n;
}

vec3 world_normal(vec2 uv, float d, vec3 P) {
  vec3 near = raw_normal(uv, d, P, 1.0);
  vec3 wide = raw_normal(uv, d, P, 4.0);
  // the wide estimate carries the smoothing, the near one keeps real detail
  vec3 n = normalize(near * 0.30 + wide * 0.70);
  return dot(n, cam_trans.xyz - P) < 0.0 ? -n : n;
}

float scene_depth(vec2 uv) {
  return texture(depth_tex, uv).r;
}

// PRESENCE du luminaire d'une lampe a l'ecran (0..1), pour son halo et ses fantomes d'objectif
float lamp_presence(vec2 luv, float ldist, float rad) {
  float pres = 0.0;
  for (int t = 0; t < 5; t++) {
    vec2 o = t == 0 ? vec2(0.0)
                    : vec2(cos(1.5707963 * float(t)), sin(1.5707963 * float(t))) * rad * 0.15;
    vec2 q = clamp(luv + o, 0.001, 0.999);
    float od = scene_depth(q);
    if (od < 1e-5) {
      continue;  // ciel : rien n'est dessine la
    }
    float d_occ = length(world_from_depth(q, od) - cam_trans.xyz);
    pres += smoothstep(-4096.0 * 2.0, -4096.0 * 1.0, d_occ - ldist);
  }
  return smoothstep(0.0, 0.6, pres / 5.0);
}
// vrai si le point monde q, vu a l'ecran, montre la mer
bool sh_mer(vec3 q) {
  vec4 qc = clip_from_world * vec4(q - cam_trans.xyz, 1.0);
  if (qc.w < 1e-4) {
    return false;
  }
  vec2 quv = (qc.xy / qc.w) * 0.5 + 0.5;
  if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) {
    return false;
  }
  float qd = scene_depth(quv);
  if (qd < 1e-5) {
    return false;
  }
  vec3 w = world_from_depth(quv, qd);
  if (w.y > water_glitter.x - 0.15 * 4096.0) {
    return false;
  }
  // la surface traversee par ce regard doit etre LA (a moins de 1,5 m de q)
  vec3 cr = cam_trans.xyz + (w - cam_trans.xyz) * ((water_glitter.x - cam_trans.y) / (w.y - cam_trans.y));
  return length(cr.xz - q.xz) < 1.5 * 4096.0;
}
// vrai si le point monde q (au niveau de la mer), vu a l'ecran, montre la terre ferme
bool sh_terre(vec3 q) {
  vec4 qc = clip_from_world * vec4(q - cam_trans.xyz, 1.0);
  if (qc.w < 1e-4) {
    return false;
  }
  vec2 quv = (qc.xy / qc.w) * 0.5 + 0.5;
  if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) {
    return false;
  }
  float qd = scene_depth(quv);
  return qd > 1e-5 && world_from_depth(quv, qd).y > water_glitter.x - 0.05 * 4096.0;
}
// distance horizontale (m) du point P a la mer (vers_mer) ou a la terre ferme, au plus 4
float sh_distance(vec3 P0, bool vers_mer) {
  float best = 99.0;
  for (int k = 0; k < 8; k++) {
    vec3 d = vec3(kOct8[k].x, 0.0, kOct8[k].y) * 4096.0;
    float lo = 0.0, hi = 0.0;
    for (int ri = 0; ri < 3; ri++) {
      float r = ri == 0 ? 1.2 : (ri == 1 ? 2.6 : 4.5);
      if (r > best) {
        break;
      }
      vec3 q = P0 + d * r;
      if (vers_mer ? sh_mer(q) : sh_terre(q)) {
        hi = r;
        break;
      }
      lo = r;
    }
    if (hi > 0.0) {
      for (int it = 0; it < 4; it++) {
        float rm = 0.5 * (lo + hi);
        vec3 q = P0 + d * rm;
        if (vers_mer ? sh_mer(q) : sh_terre(q)) {
          hi = rm;
        } else {
          lo = rm;
        }
      }
      best = min(best, 0.5 * (lo + hi));
    }
  }
  return best;
}

// CARTES DU RIVAGE (une par niveau cotier, outils/rivages.py)
uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
// ondes autour de Jak dans l'eau (OpenGLRenderer.cpp)
uniform vec4 ondes[12];
uniform vec4 riv_box[2];  // x0, z0 (m), 1/largeur, 1/profondeur
uniform int riv_n;
// EAU INTERIEURE
bool riv_interieur(sampler2D t, vec2 uv) {
  ivec2 sz = textureSize(t, 0);
  ivec2 i0 = clamp(ivec2(floor(uv * vec2(sz) - 0.5)), ivec2(0), sz - 2);
  float m = max(max(texelFetch(t, i0, 0).r, texelFetch(t, i0 + ivec2(1, 0), 0).r),
                max(texelFetch(t, i0 + ivec2(0, 1), 0).r, texelFetch(t, i0 + ivec2(1, 1), 0).r));
  return m > 36.0;
}
float riv_dist(vec2 xz_m) {
  float d = 99.0;
  if (riv_n > 0) {
    vec2 uv = (xz_m - riv_box[0].xy) * riv_box[0].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = riv_interieur(riv_tex0, uv) ? -95.0 : texture(riv_tex0, uv).r;
    }
  }
  if (riv_n > 1) {
    vec2 uv = (xz_m - riv_box[1].xy) * riv_box[1].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = min(d, riv_interieur(riv_tex1, uv) ? -95.0 : texture(riv_tex1, uv).r);
    }
  }
  return d;
}

// crude screen-space AO from depth alone: neighbors closer than us occlude Ambient occlusion done
float contact_shadow(vec3 P, vec3 L, float pix_dist) {
  // 9 pas au lieu de 16
  const int STEPS = 7;
  float step_len = 4096.0 * (0.36 + 0.50 * clamp(pix_dist / (4096.0 * 60.0), 0.0, 1.0));
  // Ordered dither instead of a hash: a hash gives every pixel an independent offset
  ivec2 ip = ivec2(gl_FragCoord.xy) & 3;
  const float kDither[16] =
      float[16](0.0625, 0.5625, 0.1875, 0.6875, 0.8125, 0.3125, 0.9375, 0.4375, 0.25, 0.75, 0.125,
                0.625, 1.0, 0.5, 0.875, 0.375);
  float jitter = kDither[ip.y * 4 + ip.x];

  // Accumulate partial occlusion rather than stopping at the first hit: a hard stop turns a smooth
  float occ = 0.0;
  float weight = 0.0;
  for (int i = 1; i <= STEPS; i++) {
    vec3 sp = P + L * (step_len * (float(i) + jitter));
    vec4 clip = clip_from_world * vec4(sp - cam_trans.xyz, 1.0);
    if (abs(clip.w) < 1e-6) {
      break;
    }
    vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      break;
    }
    float sd = scene_depth(uv);
    float w = 1.0 - float(i - 1) / float(STEPS);  // near occluders matter most
    weight += w;
    if (sd < 1e-5) {
      continue;  // sky: nothing in the way
    }
    // un echantillon est dans l'ombre s'il est CACHE a la camera
    float behind = length(sp - cam_trans.xyz) - length(world_from_depth(uv, sd) - cam_trans.xyz);
    // smooth entry and exit of the thickness window, so the shadow fades in PC-REMAKE
    float hit = smoothstep(4096.0 * 0.03, 4096.0 * 0.15, behind) *
                (1.0 - smoothstep(4096.0 * 0.6, 4096.0 * 1.2, behind));
    occ += hit * w;
  }
  return weight > 0.0 ? clamp(occ / weight * 2.4, 0.0, 1.0) : 0.0;
}

float compute_ssao(vec2 uv, float d, vec3 P, vec3 N) {
  vec2 px = 1.0 / vec2(textureSize(depth_tex, 0));
  const float R = 4096.0 * 2.2;  // 2.2 m hemisphere
  float occ = 0.0;
  // les tirages hors de l'ecran sont ignores
  float valides = 0.0;
  // 8 taps au lieu de 12 (angle d'or, couverture equivalente)
  for (int i = 0; i < 8; i++) {
    // suv = uv + vec2(cos(a), sin(a)) * 11.0 * px * jitter)
    vec2 suv = uv + kSsao8[i] * 11.0 * px;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) {
      continue;
    }
    valides += 1.0;
    float sd = scene_depth(suv);
    if (sd < 1e-5) {
      continue;  // sky occludes nothing
    }
    vec3 dv = world_from_depth(suv, sd) - P;
    float len = length(dv);
    if (len < 24.0 || len > R) {
      continue;
    }
    occ += max(dot(N, dv / len), 0.0) * (1.0 - len / R);
  }
  return clamp(occ * (8.0 / max(valides, 1.0)) / 3.4, 0.0, 1.0);
}

vec3 filmic_aces(vec3 x) {
  // Narkowicz ACES approximation
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

// contribution d'un echantillon au petit bloom
vec3 bloom_tap(vec3 s, float thresh) {
  float l = luma(s);
  float mx = max(s.r, max(s.g, s.b));
  float mn = min(s.r, min(s.g, s.b));
  float sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
  float exces = max(l - thresh, 0.0) * mix(1.0, 0.25, smoothstep(0.35, 0.75, sat));
  return l > 1e-4 ? s * (exces / l) : vec3(0.0);
}

void main() {
  bool pc_feuillage = false;  // pixel de feuillage (translucidite, bloc soleil)
#ifdef POST_PASS2
  // reprise de la passe 1
  vec4 lit = texelFetch(lit_tex, ivec2(tex_coord * vec2(textureSize(lit_tex, 0))), 0);
  vec3 c = lit.rgb;
  bool sky_pixel = lit.a < -0.5;
  // feuillage code par +2 dans l'alpha de la passe 1 (voir plus bas)
  pc_feuillage = lit.a > 1.5;
  float edge_amt = max(lit.a - (pc_feuillage ? 2.0 : 0.0), 0.0);
  // part d'interieur de cabane du pixel (voir pc_hut_inside)
  float hut_px = 0.0;
  if (grade_amt2.w > 0.001 && !sky_pixel) {
    hut_px = clamp(grade_amt2.w, 0.0, 1.0) *
             pc_hut_inside(world_from_depth(tex_coord, scene_depth(tex_coord)));
  }
#else
  vec3 base = texture(tex_T0, tex_coord).rgb;

  // Rebuild the geometry of this pixel once: depth -> world position -> normal
  float center_d = scene_depth(tex_coord);
  bool sky_pixel = center_d < 1e-5;
  vec3 P = cam_trans.xyz;
  vec3 N = vec3(0.0, 1.0, 0.0);
  float view_dist = 1e9;
  // SILHOUETTES (contours persistants)
  float edge_amt = 0.0;
  bool has_true_normal = false;
  // 1.0 = pixel d'une surface EMISSIVE (alpha 0.75 du G-buffer, tfrag3.frag) -> source de lumiere
  float emissive_px = 0.0;
  // metal et verre (drapeau 250 a 254/255 du G-buffer, tfrag3.frag)
  float metal_px = 0.0;
  float metal_poli = 0.0;
  float verre_px = 0.0;
  // part d'interieur de cabane du pixel (voir pc_hut_inside)
  float hut_px = 0.0;
  if (!sky_pixel) {
    P = world_from_depth(tex_coord, center_d);
    view_dist = length(P - cam_trans.xyz);
    if (grade_amt2.w > 0.001) {
      hut_px = clamp(grade_amt2.w, 0.0, 1.0) * pc_hut_inside(P);
    }
    {
      vec2 epx = 2.0 / vec2(textureSize(depth_tex, 0));
      for (int ei = 0; ei < 4; ei++) {
        vec2 eo = (ei == 0) ? vec2(epx.x, 0.0)
                 : (ei == 1) ? vec2(-epx.x, 0.0)
                 : (ei == 2) ? vec2(0.0, epx.y)
                             : vec2(0.0, -epx.y);
        float eds = scene_depth(tex_coord + eo);
        if (eds < 1e-5) {
          edge_amt += 0.5;
          continue;
        }
        float dn = length(world_from_depth(tex_coord + eo, eds) - cam_trans.xyz);
        edge_amt += clamp(abs(dn - view_dist) / (4096.0 * 1.5) - 0.3, 0.0, 1.0);
      }
      edge_amt = clamp(edge_amt * 0.6, 0.0, 1.0);
    }
    // VRAIE normale (lissee, par sommet) quand le G-buffer en a une
    bool got_gbuffer_n = false;
    bool foliage_px = false;
    if (has_normal_tex > 0.5) {
      vec4 gn = texture(normal_tex, tex_coord);
      if (gn.a > 0.6 && gn.a < 0.9) {
        emissive_px = 1.0;
      }
      // alpha 0.95 = feuillage (tie_wind, shrub, feuilles tfrag)
      foliage_px = gn.a > 0.92 && gn.a < 0.98;
      {
        float a8 = gn.a * 255.0;
        if (a8 > 249.5 && a8 < 253.5) {
          // 250 poli : 1
          metal_px = a8 < 252.5 ? mix(1.0, 0.8, clamp((a8 - 250.0) * 0.5, 0.0, 1.0)) : 0.4;
          metal_poli = clamp((252.0 - a8) * 0.5, 0.0, 1.0);
        } else if (a8 >= 253.5 && a8 < 254.5) {
          verre_px = 1.0;
        }
      }
      pc_feuillage = foliage_px;
      if (gn.a > 0.5) {
        vec3 tn = gn.rgb * 2.0 - 1.0;
        float tl = length(tn);
        if (tl > 0.3) {
          N = tn / tl;
          got_gbuffer_n = true;
        }
      }
    }
    if (got_gbuffer_n) {
      has_true_normal = true;
      // une surface visible fait face a la camera
      if (!foliage_px && N.y < 0.995 && dot(N, cam_trans.xyz - P) < 0.0) {
        N = -N;
      }
    } else {
      // au bord : la normale LARGE (4 px) contamine
      vec3 near_n = raw_normal(tex_coord, center_d, P, 1.0);
      vec3 wide_n = raw_normal(tex_coord, center_d, P, 4.0);
      N = normalize(mix(near_n * 0.30 + wide_n * 0.70, near_n, edge_amt));
      if (dot(N, cam_trans.xyz - P) < 0.0) {
        N = -N;
      }
    }
  }

  if (qa_show_normals > 0.5) {
    // couleur = normale monde (x rouge, y vert, z bleu)
    vec3 nc = N * 0.5 + 0.5;
    color = vec4(has_true_normal ? nc : nc * 0.5, 1.0);
    // les surfaces emissives (tag alpha 0.75) en magenta
    if (emissive_px > 0.5) {
      color = vec4(1.0, 0.0, 1.0, 1.0);
    }
    return;
  }

  // --- far DOF (soft blur of distant scenery)
  if (depth_fx.z > 0.001) {
    float farness = clamp((view_dist - 4096.0 * 60.0) / (4096.0 * 140.0), 0.0, 1.0);
    if (farness > 0.01) {
      vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
      vec3 blur = base;
      for (int i = 0; i < 8; i++) {
        blur += texture(tex_T0, tex_coord + kOct8[i] * 2.5 * px).rgb;
      }
      blur /= 9.0;
      base = mix(base, blur, farness * depth_fx.z);
    }
  }

  // --- bloom: two spiral rings of taps, soft threshold
  vec3 bloom = vec3(0.0);
  if (post_fx.x > 0.001) {
    float thresh = 0.60 + post_fx.y;
    vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
    float r1 = 6.0;  // inner ring radius in pixels
    float r2 = 14.0;  // outer ring radius in pixels
    vec3 acc = vec3(0.0);
    // PERF 24 -> 16 taps a ENERGIE CONSTANTE
    for (int i = 0; i < 6; i++) {
      vec3 s = texture(tex_T0, tex_coord + kRing6[i] * r1 * px).rgb;
      acc += bloom_tap(s, thresh) * 1.3333;
    }
    // outer ring: 10 taps (8.= 0.88 chacun)
    for (int i = 0; i < 10; i++) {
      vec3 s = texture(tex_T0, tex_coord + kRing10[i] * r2 * px).rgb;
      acc += bloom_tap(s, thresh) * 0.88;
    }
    // center contribution so bright pixels glow themselves
    acc += bloom_tap(base, thresh) * 1.5;
    bloom = acc * (post_fx.x / 12.0);
    // warm-tinted bloom reads as sunlight
    bloom *= vec3(1.0 + 0.25 * post_fx.w, 1.0 + 0.08 * post_fx.w, 1.0);
  }

  vec3 c = base + bloom;

  // --- grade: saturation + warmth, both offsets from neutral
  float sat = 1.0 + post_fx.z;
  c = mix(vec3(luma(c)), c, sat);
  c.r *= 1.0 + 0.055 * post_fx.w;
  c.g *= 1.0 + 0.012 * post_fx.w;
  c.b *= 1.0 - 0.045 * post_fx.w;

  // gentle highlight rolloff to avoid clipping when bloom is on
  if (post_fx.x > 0.001) {
    c = c / (1.0 + 0.12 * max(vec3(0.0), c - vec3(1.0)));
  }

  // FONDU CHAMP PROCHE (polygones trop visibles en cinematique)
  float near_flat = 0.0;
  if (!sky_pixel) {
    near_flat = 1.0 - smoothstep(4096.0 * 3.0, 4096.0 * 8.0, view_dist);
    float upn = clamp(N.y, 0.0, 1.0);
    near_flat *= 1.0 - upn * upn;
    // CINEMATIQUE (toon_fx.w = movie?)
    near_flat = max(near_flat, clamp(toon_fx.w, 0.0, 1.0));
    // avec une VRAIE normale lissee, les facettes que near_flat cachait n'existent plus
    if (has_true_normal) {
      near_flat *= 0.4;
    }
  }
  // PARTICULES (texture absente derriere l'eau qui tombe, cascade de la plage)
  const float PART_FLAT = 1.0;
  if (!sky_pixel && PART_FLAT > 0.5 && has_scene_tex > 0.5 && has_hud_ref > 0.5) {
    ivec2 ip = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
    vec3 pre_sprites = texelFetch(scene_tex, ip, 0).rgb;
    vec3 with_sprites = texelFetch(hud_ref_tex, ip, 0).rgb;
    vec3 dd = abs(with_sprites - pre_sprites);
    near_flat = max(near_flat, smoothstep(0.04, 0.30, max(dd.r, max(dd.g, dd.b))));
  }
  // FONDU CHAMP LOINTAIN
  float far_flat = sky_pixel ? 0.0 : smoothstep(4096.0 * 180.0, 4096.0 * 320.0, view_dist);
  float flat_amt = max(near_flat, far_flat);
  // SOUS L'EAU, ECLAIRAGE RECONSTRUIT EN DEGRADE
  float neige_creux = 0.0;
  if (!sky_pixel && snow_trail.x > 0.5 && snow_geom > 0.5) {
    vec3 pm0 = P / 4096.0;
    if (length(pm0.xz - snow_trail.yz / 4096.0) < 49.0) {
      float h0 = snow_relief(pm0, snow_trail.w);
      float h1 = snow_relief(vec3(pm0.x, pm0.y - h0, pm0.z), snow_trail.w);
      neige_creux = smoothstep(0.005, 0.06, -h1);
    }
  }
  float sous_eau = 0.0;
  float haut_fond = 1.0;  // part de haut-fond autour du point (1 = hors de l'eau)
  // distance de la camera a la SURFACE de la mer devant le point
  float dist_surf = view_dist;
  if (!sky_pixel && (water_glitter.y > 0.001 || water_glitter.z > 0.001) &&
      cam_trans.y > water_glitter.x && P.y < water_glitter.x) {
    float ts = (water_glitter.x - cam_trans.y) / (P.y - cam_trans.y);
    float col = view_dist * (1.0 - clamp(ts, 0.0, 1.0)) / 4096.0;
    // des 25 cm d'eau
    sous_eau = smoothstep(0.02, 0.25, col);
    dist_surf = view_dist * clamp(ts, 0.0, 1.0);
    if (sous_eau > 0.001) {
      vec3 ps = cam_trans.xyz + (P - cam_trans.xyz) * clamp(ts, 0.0, 1.0);
      float hf = 2.0 * (P.y > water_glitter.x - 4.5 * 4096.0 ? 1.0 : 0.0);
      float hn = 2.0;
      for (int k = 0; k < 12; k++) {
        float r = (k < 8 ? 3.0 : 1.5) * 4096.0;
        vec2 o = k < 8 ? kOct8[k] : kOct8[(k - 8) * 2 + 1];
        vec4 qc = clip_from_world * vec4(ps + vec3(o.x, 0.0, o.y) * r - cam_trans.xyz, 1.0);
        if (qc.w < 1e-4) {
          continue;
        }
        vec2 quv = (qc.xy / qc.w) * 0.5 + 0.5;
        if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) {
          continue;
        }
        float qd = scene_depth(quv);
        float qy = qd > 1e-5 ? world_from_depth(quv, qd).y : -1e9;
        hf += qy > water_glitter.x - 4.5 * 4096.0 ? 1.0 : 0.0;
        hn += 1.0;
      }
      haut_fond = hf / hn;
    }
  }

  // SOUS L'EAU, PAS DE FACETTES
  flat_amt = max(flat_amt, sous_eau);
  // --- depth-aware: SSAO (contact shading in crevices)
  float ao_px = 0.0;  // occlusion du pixel, gardee pour les reflets
                      // au fond d'un recoin)
  if (depth_fx.x > 0.001 && !sky_pixel) {
    float ao = compute_ssao(tex_coord, center_d, P, N);
    // tint the occlusion slightly cool: shaded crevices lose the warm sun first
    ao *= 1.0 - edge_amt;  // pas d'anneau d'occlusion derrière les objets fins
    ao *= 1.0 - near_flat * 0.9;  // champ proche/cinematique : ne pas cerner les facettes
    ao *= (1.0 - emissive_px);  // pas d'occlusion sur une source
    ao *= 1.0 - sous_eau;  // pas d'occlusion du fond vu a travers l'eau
    c *= mix(vec3(1.0), vec3(0.88, 0.92, 1.02), ao * depth_fx.x) * (1.0 - ao * depth_fx.x * 0.85);
    ao_px = clamp(ao * depth_fx.x, 0.0, 1.0);
  }

  // --- perspective atmosphérique du LOINTAIN (v2 )
  if (depth_fx.y > 0.001) {
    // partout) et trop faible pour cacher les MAILLAGES-DECOR de 2001 (montagnes vista quasi sans texture
    float far_m = view_dist / 4096.0;
    float haze = smoothstep(400.0, 900.0, far_m) * clamp(depth_fx.y * 4.0, 0.0, 0.92);
    float nt = clamp(night_fx.x, 0.0, 1.0);
    vec3 haze_color = mix(vec3(0.70, 0.80, 0.96), vec3(0.06, 0.09, 0.22), nt);
    c = mix(c, haze_color, haze);
  }

  // ================= rebuilt lighting ================= Everything below runs as a deferred pass: the
  if (!sky_pixel && water_glitter.y > 0.001 && view_dist < 4096.0 * 90.0) {
    float hy = (P.y - water_glitter.x) / 4096.0;
    // avec une carte du rivage, la nappe monte aussi sur le haut de plage
    float drv = riv_dist(P.xz / 4096.0);
    float hmax = drv < 90.0 ? 1.5 : 0.9;
    // pentes douces seulement (plage)
    float ny_plage = N.y;
    if (hy > -0.25 && hy < hmax && N.y > 0.35) {
      ny_plage = max(N.y, raw_normal(tex_coord, center_d, P, 4.0).y);
    }
    if (hy > -0.25 && hy < hmax && ny_plage > 0.6) {
      // distance a la mer lue dans la carte du rivage
      float dmer = drv < 90.0 ? max(-drv, 0.0) : sh_distance(P, true);
      if (dmer < 90.0) {
        vec2 xz = P.xz / 4096.0;
        float t = water_glitter.w;
        float ph = sh_phase(xz, t);
        float fmax = 1.6 + 1.6 * sh_noise(xz * 0.08 + 1.3);
        float front = sh_runup(ph, fmax);
        float jour = clamp(1.0 - night_fx.x, 0.25, 1.0);
        float loin = (1.0 - smoothstep(50.0, 90.0, view_dist / 4096.0)) *
                     (1.0 - smoothstep(hmax * 0.6, hmax, hy)) * smoothstep(0.6, 0.78, ny_plage);
        // sable mouille
        float zone = 1.0 - smoothstep(fmax * 0.75, fmax * 1.05, dmer);
        float seche = ph < 0.55 ? ph / 0.55 : 0.0;
        float mouille = max(zone * (1.0 - 0.45 * seche), 1.0 - smoothstep(0.0, 0.35, dmer)) * loin;
        c = mix(c, c * vec3(0.72, 0.74, 0.78), mouille * 0.75);
        // nappe : fine pellicule d'eau claire, plus epaisse loin du front coupee
        const bool NAPPE_SABLE = false;
        if (NAPPE_SABLE && front > 0.02) {
          float nappe = (1.0 - smoothstep(front - 0.10, front + 0.02, dmer)) * loin;
          float epais = smoothstep(0.0, 0.9, front - dmer);
          // sous la nappe
          vec3 Vn = normalize(cam_trans.xyz - P);
          float fres = 0.04 + 0.96 * pow(1.0 - clamp(Vn.y, 0.0, 1.0), 5.0);
          // nappe plus lisible
          vec3 sous = c * vec3(0.66, 0.80, 0.84);
          vec3 ciel = sky_color.rgb * (0.55 + 0.45 * jour);
          vec3 film = mix(sous, ciel, clamp(0.12 + fres * (0.40 + 0.45 * epais), 0.0, 0.65));
          c = mix(c, film, nappe * (0.70 + 0.25 * epais));
          // front d'ecume (dentelle) et bulles qui trainent derriere lui
          float lace = sh_lace(xz, t);
          float bq = (dmer - front) / 0.2;
          float bord = exp(-bq * bq);
          float traine = (1.0 - smoothstep(0.0, 0.6, front - dmer)) * nappe;
          float ecume = clamp(bord * (0.6 + 0.4 * lace) + traine * lace * 0.55, 0.0, 1.0) * loin;
          vec3 blanc = c + (vec3(0.97, 0.98, 1.0) - c) * 0.9 * jour;
          c = mix(c, blanc, ecume * 0.95);
        }
      }
    } else if (hy < -0.15 && cam_trans.y > water_glitter.x + 0.3 * 4096.0) {
      // COTE MER : le pixel montre l'eau (fond sous la surface)
      vec3 ps = cam_trans.xyz + (P - cam_trans.xyz) * ((water_glitter.x - cam_trans.y) / (P.y - cam_trans.y));
      float dps = length(ps - cam_trans.xyz) / 4096.0;
      // avec une carte du rivage, les vagues se voient de plus loin
      float drs = riv_dist(ps.xz / 4096.0);
      // pas de vagues la ou la carte dit terre ou eau interieure
      const bool VAGUES_BORD = false;
      if (VAGUES_BORD && dps < (drs < 90.0 ? 90.0 : 45.0) && drs > -0.5) {
        float dter = drs < 90.0 ? max(drs, 0.0) : sh_distance(ps, false);
        if (dter < 90.0) {
          vec2 xz = ps.xz / 4096.0;
          float t = water_glitter.w;
          float ph = sh_phase(xz, t);
          float jour = clamp(1.0 - night_fx.x, 0.25, 1.0);
          float loin = 1.0 - smoothstep(drs < 90.0 ? 60.0 : 30.0, drs < 90.0 ? 90.0 : 45.0, dps);
          float lace = sh_lace(xz, t);
          float ecume = 0.0;
          float rouleau = 0.0;
          // l'ecume et les vagues qui deferlent ne vont que sur un haut-fond
          float hautfond_v = 1.0 - smoothstep(1.5, 4.0, (water_glitter.x - P.y) / 4096.0);
          if (drs < 90.0) {
            // VAGUE QUI ARRIVE DU LARGE
            if (ph < 0.55) {
              // chaque vague a sa force, qui varie aussi le long de la cote
              float force = 0.25 + 0.75 * sh_noise(xz * 0.16 + vec2(t * 0.021, t * 0.013));
              float sp = ph / 0.55;
              float xc = 18.0 * pow(1.0 - sp, 1.25);
              float dx = dter - xc;
              float bosse = dx > 0.0 ? exp(-dx * dx / 4.8) : exp(-dx * dx / 0.5);
              float casse = smoothstep(5.5, 3.2, xc) * force;
              rouleau = bosse * smoothstep(18.0, 8.0, xc) * (1.0 - 0.7 * casse) * (0.5 + 0.5 * force);
              float cq = dx / ((0.28 + 0.05 * xc) * (0.7 + 0.6 * sh_noise(xz * 0.5 + 2.3)));
              float crete = exp(-cq * cq) * casse;
              float tapis = (dx > 0.0 ? exp(-dx / 2.6) : exp(dx / 0.3)) * casse;
              // la crete n'est jamais un trait continu
              ecume = crete * smoothstep(0.05, 0.55, lace) + tapis * smoothstep(0.25, 0.85, lace) * 0.6;
            }
            // ressac : la nappe redescend du sable, ecume dechiree pres du bord
            float retrait = ph > 0.55 ? (1.0 - smoothstep(0.0, 2.2, dter)) * (1.0 - (ph - 0.55) / 0.45) : 0.0;
            float lisere = 1.0 - smoothstep(0.0, 0.35 + 0.2 * sin(ph * 6.2832), dter);
            // liseré et ressac colles au trait de cote
            if (NAPPE_SABLE_MER) {
              ecume = max(ecume, (lisere * 0.7 + retrait * 0.5) * (0.25 + 0.75 * lace));
            }
            ecume = clamp(ecume, 0.0, 1.0) * loin;
            rouleau *= loin;
          } else {
            // sans carte
            float crete = 0.0;
            if (ph < 0.55) {
              float xc = 6.0 * (1.0 - ph / 0.55);
              float w = 0.22 + 0.07 * xc;
              float cq = (dter - xc) / w;
              crete = exp(-cq * cq) * smoothstep(6.0, 1.5, xc);
            }
            float retrait = ph > 0.55 ? (1.0 - smoothstep(0.0, 1.6, dter)) * (1.0 - (ph - 0.55) / 0.45) * 0.6 : 0.0;
            float lisere = 1.0 - smoothstep(0.0, 0.3 + 0.2 * sin(ph * 6.2832), dter);
            ecume = clamp(lisere * 0.7 * (0.5 + 0.5 * lace) + (crete * 0.85 + retrait) * lace, 0.0, 1.0) * loin;
          }
          ecume *= hautfond_v;
          rouleau *= hautfond_v;
          // levre de la vague
          c = mix(c, c * 1.16 + vec3(0.02, 0.07, 0.06) * jour, rouleau * 0.5);
          vec3 blanc = c + (vec3(0.95, 0.98, 1.0) - c) * 0.85 * jour;
          c = mix(c, blanc, ecume * 0.9);
        }
      }
    }
  }

  // neige creusee eclairee comme la surface intacte qui l'entoure
  N = normalize(mix(N, vec3(0.0, 1.0, 0.0), neige_creux));
  // EMPREINTES DANS LE SABLE
  if (!sky_pixel && sable.x > 0.5) {
    vec2 pm = P.xz * (1.0 / 4096.0);
    vec2 dj = pm - sable.yz * (1.0 / 4096.0);
    if (max(abs(dj.x), abs(dj.y)) < 12.0) {
      vec2 tc = pm / 0.025 - 0.5;
      ivec2 i0 = ivec2(floor(tc));
      vec2 fr = tc - vec2(i0);
      vec4 sa = texelFetch(sable_tex, i0 & 1023, 0);
      vec4 sb = texelFetch(sable_tex, (i0 + ivec2(1, 0)) & 1023, 0);
      vec4 sc = texelFetch(sable_tex, (i0 + ivec2(0, 1)) & 1023, 0);
      vec4 sd = texelFetch(sable_tex, (i0 + ivec2(1, 1)) & 1023, 0);
      if (max(max(sa.w, sb.w), max(sc.w, sd.w)) > 0.5) {
        float gy = sa.w > 0.5 ? sa.z : (sb.w > 0.5 ? sb.z : (sc.w > 0.5 ? sc.z : sd.z));
        if (abs(P.y * (1.0 / 4096.0) - gy) < 0.6) {
          float age = sable.w - max(max(sa.y, sb.y), max(sc.y, sd.y));
          if (age < 0.0) {
            age += 3600.0;
          }
          float vie = (1.0 - smoothstep(120.0, 240.0, age)) *
                      (1.0 - smoothstep(30.0, 60.0, view_dist * (1.0 / 4096.0)));
          float h = mix(mix(sa.x, sb.x, fr.x), mix(sc.x, sd.x, fr.x), fr.y) * vie;
          float gx = (mix(sb.x, sd.x, fr.y) - mix(sa.x, sc.x, fr.y)) * (vie / 0.025);
          float gz = (mix(sc.x, sd.x, fr.x) - mix(sa.x, sb.x, fr.x)) * (vie / 0.025);
          N = normalize(N + vec3(-gx, 0.0, -gz) * 1.4);
          c *= 1.0 - 0.14 * clamp(-h / 0.024, 0.0, 1.0);
        }
      }
    }
  }
  if ((light_fx.y > 0.001 || light_fx.z > 0.001 || night_fx.x > 0.001) && !sky_pixel) {
    vec3 V = normalize(cam_trans.xyz - P);
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    // champ proche / cinematique / LOINTAIN
    up = mix(up, 0.5, flat_amt);
    float day = clamp(1.0 - night_fx.x, 0.0, 1.0);

    // --- sun: real directional light
    if (light_fx.y > 0.001 && sun_dir.w > 0.5) {
      // PERF sun_dir arrive NORMALISE du C++ (OpenGLRenderer ~1805)
      vec3 L = sun_dir.xyz;
      // (ombres parasites omnipresentes sur le sable de la plage, soleil bas)
      float ground_w = smoothstep(0.55, 0.9, N.y);
      vec3 Nr = normalize(mix(N, vec3(0.0, 1.0, 0.0), ground_w * 0.7));
      float wrap = mix(0.35, 1.0, ground_w);
      float ndl = max(dot(Nr, L), 0.0);
      // wrapped diffuse: keeps the shadow side readable instead of pure black
      float lit = clamp((dot(Nr, L) + wrap) / (1.0 + wrap), 0.0, 1.0);
      // warm direct sun vs cool sky-lit shade
      vec3 sun_col = vec3(1.32, 1.16, 0.86);
      vec3 shade_col = vec3(0.74, 0.82, 1.04);
      // small-scale occlusion of the sun, marched through the depth buffer PERF au-dela de 90 m le fondu
      if (depth_fx.z >= 0.0 && light_fx.w >= 0.0 && ndl > 0.001 && view_dist < 4096.0 * 90.0) {
        float cs = contact_shadow(P, L, view_dist) * clamp(1.0 - view_dist / (4096.0 * 90.0), 0.0, 1.0) *
                   (1.0 - sous_eau);
        lit *= 1.0 - cs * 0.85 * (1.0 - near_flat);
      }
      // champ proche
      lit = mix(lit, 0.62, flat_amt);
      vec3 lighting = mix(shade_col * 0.78, sun_col, lit);
      // TRANSLUCIDITE DU FEUILLAGE (rendu recent)
      if (pc_feuillage && foliage_transl > 0.001) {
        float contre = pow(clamp(dot(-V, L), 0.0, 1.0), 3.0);
        // de dos (on voit la face non eclairee)
        float dos = clamp(-dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
        lighting += vec3(1.10, 1.12, 0.62) * contre * (0.35 + 0.65 * dos) * 0.75 * foliage_transl *
                    (1.0 - flat_amt * 0.6);
      }
      // specular sheen so surfaces catch the sun instead of staying matte
      if (light_fx.w > 0.001) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(Nr, H), 0.0), 42.0) * ndl;
        lighting += vec3(1.0, 0.95, 0.85) * spec * light_fx.w * 1.6 * (1.0 - flat_amt);
      }
      // sous l'eau, dose par la part de haut-fond (voir sous_eau)
      c *= mix(vec3(1.0), lighting, clamp(light_fx.y, 0.0, 1.0) * day * (1.0 - emissive_px) * mix(1.0, haut_fond, sous_eau));
    }

    // --- sky ambient: cool light from the sky above, warm bounce from below
    if (light_fx.z > 0.001) {
      vec3 amb = mix(ground_color.rgb, sky_color.rgb, up);
      // AMBIANCE DU LIEU (lumiere rebondie)
      if (has_reflet_env > 0.5 && gi_fx.y > 0.001) {
        vec2 euv = vec2(atan(N.x, N.z) * (0.5 / 3.14159265) + 0.5,
                        asin(clamp(N.y, -1.0, 1.0)) / 3.14159265 + 0.5);
        vec4 e = textureLod(reflet_env_tex, euv, 6.0);
        if (e.a > 0.05) {
          float moy_e = max(luma(texelFetch(reflet_moy_tex, ivec2(0), 0).rgb), 0.02);
          vec3 amb_env = (e.rgb / e.a) / moy_e * luma(mix(ground_color.rgb, sky_color.rgb, 0.5));
          amb_env = clamp(amb_env, vec3(0.0), vec3(1.6));
          // a moitie desaturee
          amb_env = mix(vec3(luma(amb_env)), amb_env, 0.5);
          amb = mix(amb, amb_env, 0.6 * gi_fx.y * smoothstep(0.1, 0.5, e.a) * (1.0 - flat_amt * 0.5));
        }
      }
      // dans une HUTTE (grade_amt2.w, volumes de time-of-day.gc), pas de ciel bleu sur les murs
      amb = mix(amb, vec3(0.74, 0.66, 0.56), hut_px);
      // sous l'eau, dose par la part de haut-fond (voir sous_eau)
      c *= mix(vec3(1.0), amb * 1.55, clamp(light_fx.z, 0.0, 1.0) * day * (1.0 - emissive_px) * mix(1.0, haut_fond, sous_eau));
    }

    // ECLAIRAGE UNIFIE (reecriture )
    float night = clamp(night_fx.x, 0.0, 1.0);
    // NIVEAU INTERIEUR
    float interior = clamp(ground_color.w - grade_amt2.w, 0.0, 1.0) + hut_px;
    float lamp_hour = max(night, interior);
    {
      // --- 1. ambiant selon l'heure
      vec3 ambient = vec3(1.0);
      if (night > 0.001) {
        // PERF normalize(vec3(0.35,0.86,-0.36)) plie a la main
        vec3 moon_dir = vec3(0.3515, 0.8636, -0.3615);
        float mndl = clamp(dot(N, moon_dir) * 0.5 + 0.5, 0.0, 1.0);
        float moon = pow(mndl, 1.7);  // face eclairee par la lune / face sombre
        moon = mix(moon, 0.55, near_flat);  // champ proche : pas de facettes lunaires
        vec3 moon_col = vec3(0.36, 0.52, 1.00);  // la lune est BLEUE
        vec3 amb_col = vec3(0.16, 0.22, 0.42);  // ambiante profonde
        vec3 night_light = moon_col * moon * 0.36 + amb_col * (0.30 + 0.16 * up);
        ambient = mix(vec3(1.0), night_light, night);
        ambient = mix(ambient, vec3(1.0), smoothstep(4096.0 * 250.0, 4096.0 * 600.0,
                                                     mix(view_dist, dist_surf, sous_eau)));
      }
      // HUTTE — l'interieur est plus sombre que le dehors
      ambient *= mix(1.0, 0.80, hut_px);

      // --- 2. somme LINEAIRE des lampes
      int n = int(night_fx.w);
      vec3 L_sum = vec3(0.0);
      // depot ADDITIF des lumieres d'eco
      vec3 L_eco_add = vec3(0.0);
      if (n > 0) {
        // PERF seules les lampes de la tuile
        uvec2 tmask = pc_tile_mask();
        for (int it = 0; it < MAX_LAMPS; it++) {
          int i = pc_next_lamp(tmask);
          if (i < 0 || i >= n) {
            break;
          }
          // PERF test de portee AU CARRE d'abord
          vec3 dv = lamp_pos[i].xyz - P;
          float d2 = dot(dv, dv);
          float reach_mult = (lamp_color[i].w < 0.0) ? 1.0 : max(night_fx.y, 0.05);
          // dans une HUTTE, un lampion eclaire SON coin
          if (lamp_color[i].w >= 0.0) {
            // par pixel
            reach_mult = mix(reach_mult, 1.0, hut_px);
          }
          float r = max(lamp_pos[i].w * reach_mult, 1.0);
          float r2 = r * r;
          if (d2 >= r2) {
            continue;
          }
          float inten = lamp_color[i].w;
          bool world_emissive = inten < -8.0;
          bool em_fixture = inten < -40.0;
          bool is_eco = false;
          // presence selon l'heure, par famille de source
          float presence;
          if (em_fixture) {
            // luminaires du monde
            inten = -inten - 64.0;
            presence = mix(0.30, 1.0, lamp_hour);
          } else if (world_emissive) {
            // VRAI FEU (lave, braises)
            inten = (-inten - 16.0) * 1.35;
            // CREPITEMENT (torches qui crépitent)
            {
              float fw = water_glitter.w;
              float ph = lamp_orig[i] * 7.0;
              float fl = 0.84 + 0.09 * sin(fw * 2.3 + ph) + 0.06 * sin(fw * 7.1 + ph * 1.7) +
                         0.04 * sin(fw * 13.7 + ph * 2.3);
              fl += 0.12 * max(0.0, sin(fw * 5.3 + ph) * sin(fw * 8.9 + ph * 0.6) - 0.55);
              inten *= fl;
            }
            presence = 1.0;
          } else if (inten < 0.0) {
            // ecos ramassables
            is_eco = true;
            inten = -inten;
            // A/B decisif (aucun effet visible)
            inten *= max(night_fx.z, 1.2) * 0.45;
            presence = mix(0.85, 1.0, lamp_hour);
          } else {
            // lampadaires classiques (GOAL)
            presence = lamp_hour;
            inten *= max(night_fx.z, 0.0);
          }
          if (presence * inten < 0.001) {
            continue;
          }
          float dist = sqrt(d2);
          vec3 L = dv / max(dist, 1e-4);
          // fenetre douce sur l'inverse-carre
          float wnd = clamp(1.0 - (d2 * d2) / (r2 * r2), 0.0, 1.0);
          // lampion de hutte = flaque plus serree (12 -> 20)
          float fall = (wnd * wnd) / (1.0 + (world_emissive ? 6.0 : (is_eco ? 8.0 :
                       mix(12.0, 20.0, clamp(grade_amt2.w, 0.0, 1.0)))) * d2 / r2);
          // terminateur doux (demi-Lambert au carre) + enveloppement par famille
          float ndl = dot(N, L) * 0.5 + 0.5;
          ndl = ndl * ndl;
          // l'eco est une source VOLUMIQUE (une sphere d'energie)
          float wrap = is_eco ? 0.55 : (em_fixture ? 0.14 : (world_emissive ? 0.45 : 0.18));
          float diff = mix(ndl, 1.0, wrap);
          diff = mix(diff, 1.0, near_flat);  // champ proche : la lampe eclaire sans facetter
          // silhouettes
          diff = mix(diff, 0.55, edge_amt * 0.6);
          float lamp_contrib = inten * presence * fall * diff;
          L_sum += lamp_color[i].rgb * lamp_contrib;
          if (is_eco) {
            L_eco_add += lamp_color[i].rgb * lamp_contrib;
          }
          // reflet speculaire discret
          if (!world_emissive) {
            vec3 H = normalize(L + V);
            float miroir_l = max(metal_px, verre_px) * reflet_fx.x;
            float sp_pw = mix(24.0, mix(90.0, 260.0, max(metal_poli, verre_px)), min(miroir_l, 1.0));
            float sp_k = 0.35 * mix(1.0, 2.0 + sp_pw / 70.0, min(miroir_l, 1.0));
            // le metal poli (bronze precurseur, or) renvoie les lampes plus fort
            sp_k *= 1.0 + 0.8 * metal_poli * min(miroir_l, 1.0);
            L_sum += lamp_color[i].rgb * pow(max(dot(N, H), 0.0), sp_pw) * fall * sp_k *
                     presence * (1.0 - near_flat);
          }
        }
      }

      // --- 2b. les FAISCEAUX D'ECO eclairent (la nuit) ---- Lumiere en tube
      if (beam_n > 0.5 && night > 0.02) {
        int nbm = int(beam_n);
        for (int bi = 0; bi < 12; bi++) {
          if (bi >= nbm) {
            break;
          }
          vec3 ba = beam_seg[2 * bi].xyz;
          vec3 bb = beam_seg[2 * bi + 1].xyz;
          vec3 ab = bb - ba;
          float bt = clamp(dot(P - ba, ab) / max(dot(ab, ab), 1.0), 0.0, 1.0);
          vec3 bl = ba + ab * bt - P;
          float bd = length(bl);
          float br = 4096.0 * 32.0;
          if (bd < br) {
            float bf = pow(1.0 - bd / br, 1.5);
            float bdiff = mix(0.35, 1.0, clamp(dot(N, bl / max(bd, 1.0)), 0.0, 1.0));
            bdiff = mix(bdiff, 0.6, near_flat);
            vec3 bc = vec3(0.18, 0.45, 1.0) * (bf * bdiff * 2.4 * night);
            L_sum += bc;
            L_eco_add += bc * 0.4;  // depot colore modere : les murs restent bleus, pas blancs
          }
        }
      }

      // --- 3. application unique + genou doux anti-clip
      vec3 lighting = ambient + L_sum;
      // une surface emissive garde sa propre clarte (ni nuit, ni lampes)
      lighting = mix(lighting, vec3(1.0), emissive_px);
      L_eco_add *= (1.0 - emissive_px);
      float ll = luma(lighting);
      if (ll > 1.0) {
        // seule l'EXCEDENT au-dela du plein jour est compresse
        lighting *= (1.0 + (ll - 1.0) / (1.0 + 0.85 * (ll - 1.0))) / ll;
      }
      c *= lighting;
      // depot colore des ecos, avec epaule anti-cramage naturel
      vec3 eco_dep = mix(L_eco_add, vec3(luma(L_eco_add)) * 1.4, 0.12);
      c += (eco_dep / (1.0 + 0.6 * luma(eco_dep))) * 0.30;
    }

    // --- eau : reflets du soleil (jour) / de la lune (nuit) + étincelles
    if (water_glitter.y > 0.001 || water_glitter.z > 0.001) {
      float cy = cam_trans.y;
      float dy_ray = P.y - cy;
      float t_surf = (abs(dy_ray) > 1.0) ? (water_glitter.x - cy) / dy_ray : -1.0;
      // la ou la carte du rivage dit TERRE ou EAU INTERIEURE, le plan de la mer n'est pas la mer
      bool mer_ici = true;
      if (t_surf > 0.0 && t_surf < 1.04) {
        // + fond a plus d'1,5 m sous la mer (camera au-dessus) hors eau interieure
        float drm = riv_dist(mix(cam_trans.xyz, P, min(t_surf, 1.0)).xz / 4096.0);
        mer_ici = drm > -0.5 ||
                  (drm > -90.0 && !sky_pixel && P.y < water_glitter.x - 4096.0 * 1.5);
      } else if (cy < water_glitter.x - 4096.0 * 3.5) {
        mer_ici = riv_dist(cam_trans.xz / 4096.0) > -0.5;
      }
      // --- BRUME SOUS-MARINE v3
      float pix_dist = view_dist;
      float col_m = 0.0;
      // (eau qui clignote / disparait)
      if (cy < water_glitter.x - 4096.0 * 3.5 && mer_ici) {
        // camera immergee
        float t_exit = (t_surf > 0.0 && t_surf < 1.0) ? t_surf : 1.0;
        col_m = pix_dist * t_exit / 4096.0;
      } else if (t_surf > 0.0 && t_surf < 1.04 && water_murk_ocean < 0.5 && mer_ici) {
        // camera en l'air
        col_m = pix_dist * max(0.0, 1.0 - min(t_surf, 1.0)) / 4096.0;
      }
      // (v0.0.3) : couleurs passees du bleu roi au TURQUOISE des artistes
      if (modern_fx.y > 0.001 && mer_ici && !sky_pixel && P.y < water_glitter.x && sun_dir.w > 0.5) {
        float prof = (water_glitter.x - P.y) / 4096.0;
        float jour_c = clamp(1.0 - night_fx.x, 0.0, 1.0);
        float col_vue = (cy > water_glitter.x && t_surf > 0.0 && t_surf < 1.0)
                            ? pix_dist * (1.0 - t_surf) / 4096.0
                            : 0.0;
        float kc = modern_fx.y * jour_c * smoothstep(0.03, 0.35, prof) * exp(-prof * 0.28) *
                   exp(-col_vue * 0.22) * smoothstep(0.1, 0.4, sun_dir.y) *
                   (1.0 - smoothstep(35.0 * 4096.0, 70.0 * 4096.0, view_dist));
        if (kc > 0.001) {
          vec3 L = sun_dir.xyz;
          // projetees le long du soleil
          vec2 q = (P.xz + L.xz / max(L.y, 0.2) * (water_glitter.x - P.y) * 0.75) / 4096.0;
          const float CELL = 0.85;  // m
          float nv = abs(dot(N, normalize(cam_trans.xyz - P)));
          float px = (view_dist / 4096.0) * 0.0011 / max(nv, 0.15) / CELL;
          float l = caustique(q / CELL, modern_fx.z, px);
          if (l > 0.001) {
            float vis = 1.0;
            vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
            if (sun_map_ok > 0.5 && sp.x > 0.01 && sp.x < 0.99 && sp.y > 0.01 && sp.y < 0.99 &&
                sp.z < 1.0) {
              vis = texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
            }
            float a = l * kc * vis;
            c += c * vec3(0.95, 1.10, 1.10) * (a * 1.05) + vec3(0.55, 0.85, 0.90) * (a * 0.05);
          }
        }
      }
      if (col_m > 0.02) {
        c = mix(c, sea_murk(col_m), clamp(1.0 - exp(-col_m * 0.50), 0.0, 0.94));
      }
      // le rayon traverse la surface entre la caméra et la géométrie touchée
      if (t_surf > 0.0 && t_surf < 1.04 && mer_ici) {
        vec3 Psurf = mix(cam_trans.xyz, P, min(t_surf, 1.0));
        float t = water_glitter.w;
        vec2 wuv = Psurf.xz * (0.55 / 4096.0);
        float n1 = sin(dot(wuv, vec2(3.1, 1.7)) + t * 1.6);
        float n2 = sin(dot(wuv, vec2(-1.3, 3.7)) + t * 2.2 + 1.3);
        float n3 = sin(dot(wuv, vec2(5.3, -2.9)) - t * 1.1);
        vec3 Nw = normalize(vec3(n1 + 0.6 * n3, 16.0, n2 - 0.6 * n3));
        float day = clamp(1.0 - night_fx.x, 0.0, 1.0);
        vec3 sky_day = vec3(0.55, 0.72, 1.00);
        vec3 sky_night = vec3(0.10, 0.16, 0.34);
        vec3 sky_ref = mix(sky_night, sky_day, day);
        vec3 I = normalize(Psurf - cam_trans.xyz);
        // miroir presque plan (0.14)
        float surf_m = length(Psurf - cam_trans.xyz) / 4096.0;
        vec2 hp = houle_pente(Psurf.xz / 4096.0, t, surf_m * 0.0011 / max(abs(I.y), 0.04));
        float crete_bord = 0.0;  // crete des ondes de bord (voir plus bas)
        // + la grande houle en relief (eau vivante), meme enveloppe que la grille
        if (eau_houle_p.w > 0.5) {
          vec2 pm = Psurf.xz / 4096.0;
          float d_riv = riv_dist(pm);
          float a_riv = (d_riv < -90.0 || (d_riv > 35.0 && d_riv < 45.0)) ? 0.0 : smoothstep(0.5, 6.0, d_riv);
          float a_bord = 1.0 - smoothstep(0.55, 0.9, surf_m / eau_houle_p.z);
          hp += houle_vivante_pente(pm) * (eau_houle_p.y * a_riv * a_bord);
          // ondes de bord (eau_vivante.vert, ondes_bord_h)
          if (d_riv > -0.3 && d_riv < 22.0) {
            const float K = 6.2831853 / 7.0;
            const float W = 6.2831853 / 5.5;
            vec2 gd = vec2(riv_dist(pm + vec2(0.5, 0.0)), riv_dist(pm + vec2(0.0, 0.5))) - d_riv;
            gd *= 2.0;  // par metre
            if (max(abs(gd.x), abs(gd.y)) < 2.0) {
              float env = smoothstep(-0.3, 1.0, d_riv) * (1.0 - smoothstep(8.0, 20.0, d_riv));
              float ph = sh_noise(pm * 0.025) * 4.0;
              float arg = K * d_riv + W * eau_houle_p.x + ph;
              hp += gd * (0.16 * env * K * cos(arg)) * a_bord;
              // crete eclairee
              crete_bord = pow(max(sin(arg), 0.0), 2.0) * env * a_bord;
            }
          }
        }
        // ONDES AUTOUR DE JAK
        float ondes_ec = 0.0;
        // avec l'eau vivante, la simulation remplace ces anneaux
        if (surf_m < 40.0 && eau_houle_p.w < 0.5) {
          vec2 ondes_g = vec2(0.0);
          for (int oi = 0; oi < 12; oi++) {
            vec4 o = ondes[oi];
            float age = t - o.z;
            if (o.w <= 0.0 || age < 0.0 || age > 3.2) {
              continue;
            }
            vec2 dv = (Psurf.xz - o.xy) / 4096.0;
            float r = length(dv);
            float wr = 0.09 + 0.06 * age;
            float x = (r - (0.25 + 0.9 * age)) / wr;
            if (abs(x) > 3.0) {
              continue;
            }
            float amp = o.w * exp(-age * 1.1) / (1.0 + 0.5 * r);
            float g = exp(-x * x);
            // pente d'un petit bourrelet d'eau (derivee de son profil en cloche)
            ondes_g += (dv / max(r, 1e-3)) * (-2.0 * x * g / wr) * amp * 0.05;
            ondes_ec += g * amp;
          }
          hp += ondes_g;
        }
        vec3 Nh = normalize(vec3(-hp.x, 1.0, -hp.y));
        vec3 Nr = normalize(vec3(Nw.x * 0.14 - hp.x * 0.25, 1.0, Nw.z * 0.14 - hp.y * 0.25));
        vec3 Rdir = reflect(I, Nr);
        vec3 refl_col = sky_ref;
        float hit_w = 0.0;
        // perf (saccades)
        float surf_dist = length(Psurf - cam_trans.xyz);
        if (Rdir.y > 0.02 && surf_dist < 4096.0 * 70.0) {
          // pas proportionnels a la distance (fins pres de la camera)
          float rstep = clamp(surf_dist * 0.03, 4096.0 * 0.3, 4096.0 * 1.2);
          // decalage d'un pixel a l'autre seulement de pres (pas fins)
          float decal_w = mix(fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))),
                              0.5, smoothstep(30.0 * 4096.0, 80.0 * 4096.0, surf_dist));
          float rt_prev = 0.0;
          // pas qui grandissent moins vite (1 + 0,22 i -> 1 + 0,1 i) et 26 pas
          for (int i = 1; i <= 26; i++) {
            float rt = rstep * (float(i) - decal_w) * (1.0 + 0.1 * float(i));
            vec3 Pw = Psurf + Rdir * rt;
            vec4 rclip = clip_from_world * vec4(Pw - cam_trans.xyz, 1.0);
            if (rclip.w < 1e-4) {
              break;
            }
            vec2 ruv = (rclip.xy / rclip.w) * 0.5 + 0.5;
            if (ruv.x < 0.0 || ruv.x > 1.0 || ruv.y < 0.0 || ruv.y > 1.0) {
              break;
            }
            float sd = scene_depth(ruv);
            if (sd < 1e-5) {
              rt_prev = rt;
              continue;  // ciel : rien à toucher
            }
            // un rayon reflechi monte
            vec3 Pscene = world_from_depth(ruv, sd);
            if (Pscene.y < Psurf.y - 4096.0 * 0.05) {
              rt_prev = rt;
              continue;
            }
            float scene_dist = length(Pscene - cam_trans.xyz);
            float ray_dist = length(Pw - cam_trans.xyz);
            float behind = ray_dist - scene_dist;
            // EPAISSEUR. Un rayon qui passe DERRIERE un objet fin (Jak, un poteau) n'est pas arrete par lui
            float epais = max(4096.0 * 0.6, (rt - rt_prev) * 1.5);
            if (behind > 4096.0 * 0.10 && behind < epais) {
              // dichotomie entre le pas precedent et celui-ci
              float ta = rt_prev, tb = rt;
              for (int j = 0; j < 5; j++) {
                float tm = 0.5 * (ta + tb);
                vec4 mc = clip_from_world * vec4(Psurf + Rdir * tm - cam_trans.xyz, 1.0);
                vec2 muv = (mc.xy / max(mc.w, 1e-4)) * 0.5 + 0.5;
                float md = scene_depth(muv);
                float bh = md > 1e-5 ? length(Psurf + Rdir * tm - cam_trans.xyz) -
                                           length(world_from_depth(muv, md) - cam_trans.xyz)
                                     : -1.0;
                if (bh > 0.0) {
                  tb = tm;
                } else {
                  ta = tm;
                }
              }
              vec4 hc = clip_from_world * vec4(Psurf + Rdir * tb - cam_trans.xyz, 1.0);
              ruv = clamp((hc.xy / max(hc.w, 1e-4)) * 0.5 + 0.5, vec2(0.0), vec2(1.0));
              // la copie sans HUD
              refl_col = (has_scene_tex > 0.5) ? texture(scene_tex, ruv).rgb : texture(tex_T0, ruv).rgb;
              // REFLET ADOUCI
              {
                vec2 rpx = 2.5 / vec2(textureSize(tex_T0, 0));
                vec3 acc_r = vec3(0.0);
                for (int rk = 0; rk < 5; rk++) {
                  vec2 ro2 = rk == 0 ? vec2(0.0) : kOct8[rk * 2 - 1] * rpx;
                  vec2 ru = clamp(ruv + ro2, vec2(0.0), vec2(1.0));
                  acc_r += ((has_scene_tex > 0.5) ? texture(scene_tex, ru).rgb : texture(tex_T0, ru).rgb) *
                           (rk == 0 ? 2.0 : 1.0);
                }
                refl_col = acc_r / 6.0;
              }
              // le reflet s'efface en approchant du bord de l'ecran et en fin de course du rayon
              vec2 bord = min(ruv, 1.0 - ruv);
              hit_w = smoothstep(0.0, 0.10, bord.x) * smoothstep(0.0, 0.10, bord.y) *
                      (1.0 - smoothstep(21.0, 26.5, float(i)));
              break;
            }
            rt_prev = rt;
          }
        }
        // PHILOSOPHIE FINALE
        float fresR = 0.35 + 0.65 * pow(1.0 - clamp(dot(-I, Nr), 0.0, 1.0), 2.5);
        float strength = mix(water_glitter.z * 0.7, water_glitter.y, day);
        float shore = smoothstep(0.0, 0.06, t_surf);
        // composition VALIDEE restauree a l'identique
        if (hit_w > 0.001) {
          c = mix(c, refl_col, clamp(fresR * 0.55 * strength * shore, 0.0, 0.65) * hit_w);
        }
        // HOULE (eclairage de la surface et reflets du soleil)
        float prof_m = max(water_glitter.x - P.y, 0.0) / 4096.0;
        c = houle_surface(c, Nh, I, day, (1.0 - smoothstep(120.0, 260.0, surf_m)) *
                                             mix(0.35, 1.0, smoothstep(0.5, 2.5, prof_m)) *
                                             smoothstep(0.0, 0.15, prof_m));
        // ONDES DE BORD
        c = mix(c, c * 1.28 + vec3(0.02, 0.07, 0.07) * mix(0.3, 1.0, day), clamp(crete_bord * 0.75, 0.0, 0.75));
        // ecume fine sur la crete des ondes autour de Jak
        c = mix(c, vec3(0.86, 0.93, 0.95) * mix(0.35, 1.0, day), clamp(ondes_ec * 0.32, 0.0, 0.40));
      }
    }
  } else if (night_fx.x > 0.001) {
    // sky at night: deepen it and pull it blue, but keep the stars
    float night = clamp(night_fx.x, 0.0, 1.0);
    float lum = luma(c);
    vec3 sky_night = max(mix(vec3(lum), c, 1.15), vec3(0.0)) * vec3(0.34, 0.46, 0.92) * 0.86;
    // stars and the moon are the bright pixels: let them survive
    sky_night += vec3(0.9, 0.95, 1.0) * pow(clamp(lum - 0.55, 0.0, 1.0), 1.5) * 1.8;
    c = mix(c, sky_night, night);
  }

  // LUMIERE REBONDIE (illumination globale a l'ecran, gi_rebond.frag)
  if (!sky_pixel && has_gi_tex > 0.5 && gi_fx.x > 0.001) {
    // (effet fluo sur l'herbe)
    vec3 gi = texture(gi_tex, tex_coord).rgb;
    float lb = luma(base);
    vec3 teinte_b = base / max(lb, 0.03);
    vec3 alb = mix(vec3(1.0), teinte_b, 0.5) * clamp(0.20 + lb, 0.20, 0.60);
    float ombre = 1.0 - 0.6 * smoothstep(0.15, 0.55, lb);
    float kgi = 1.6 * gi_fx.x * ombre * (1.0 - emissive_px) * mix(1.0, 0.6, sous_eau) *
                (1.0 - smoothstep(80.0 * 4096.0, 120.0 * 4096.0, view_dist)) *
                (pc_feuillage ? 0.3 : 1.0);
    // (encore trop vert)
    {
      vec3 ch_b = base - vec3(lb);
      vec3 ch_g = gi - vec3(luma(gi));
      float meme = (length(ch_b) > 0.02 && length(ch_g) > 0.02)
                       ? max(dot(normalize(ch_b), normalize(ch_g)), 0.0)
                       : 0.0;
      kgi *= 1.0 - 0.75 * smoothstep(0.6, 0.95, meme);
    }
    float sat_avant = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    vec3 c_gi = c + alb * gi * kgi;
    float l_gi = luma(c_gi);
    float sat_gi = max(c_gi.r, max(c_gi.g, c_gi.b)) - min(c_gi.r, min(c_gi.g, c_gi.b));
    if (sat_gi > sat_avant * 1.05 && sat_gi > 1e-4) {
      c_gi = mix(vec3(l_gi), c_gi, (sat_avant * 1.05) / sat_gi);
    }
    c = c_gi;
  }

  // REFLETS DU METAL ET DU VERRE
  if (!sky_pixel && (metal_px > 0.001 || verre_px > 0.001) && reflet_fx.x > 0.001 &&
      view_dist < 4096.0 * 150.0) {
    const float M = 4096.0;
    if (reflet_fx.y > 0.5) {
      vec3 qa = verre_px > 0.5 ? vec3(0.1, 0.9, 1.0)
                               : mix(vec3(1.0, 0.45, 0.0), vec3(1.0, 1.0, 0.1), metal_poli);
      c = mix(c, qa * max(metal_px, verre_px), 0.75);
    } else {
      vec3 I = normalize(P - cam_trans.xyz);
      // normale du reflet LISSEE sur les pixels voisins de la meme surface
      vec3 Nm = N;
      if (has_normal_tex > 0.5) {
        vec2 npx = 1.0 / vec2(textureSize(normal_tex, 0));
        vec3 acc = N;
        for (int k = 0; k < 16; k++) {
          vec2 quv = tex_coord + kOct8[k & 7] * npx * (k < 8 ? 10.0 : 26.0);
          vec4 g = texture(normal_tex, quv);
          if (g.a < 0.5) {
            continue;
          }
          vec3 nk = normalize(g.rgb * 2.0 - 1.0);
          if (dot(nk, N) < 0.0) {
            nk = -nk;
          }
          // meme surface
          float qd = scene_depth(quv);
          float dk = abs(qd - center_d) / max(center_d, 1e-6);
          acc += nk * smoothstep(0.72, 0.94, dot(nk, N)) * (1.0 - smoothstep(0.02, 0.06, dk)) *
                 (k < 8 ? 1.0 : 0.7);
        }
        Nm = normalize(acc);
      }
      // part de sol (surface tournee vers le haut)
      float sol_w = smoothstep(0.70, 0.90, N.y);
      float nv = clamp(dot(-I, Nm), 0.0, 1.0);
      vec3 R = reflect(I, Nm);
      float jour = clamp(1.0 - night_fx.x, 0.0, 1.0);
      // clarte moyenne de ce qu'on voit maintenant
      float moy = 0.45;
      if (has_reflet_env > 0.5) {
        moy = max(luma(texelFetch(reflet_moy_tex, ivec2(0), 0).rgb), 0.01);
      }
      float interieur = clamp(clamp(ground_color.w - grade_amt2.w, 0.0, 1.0) + hut_px, 0.0, 1.0);
      float loin = 1.0 - smoothstep(90.0 * M, 150.0 * M, view_dist);

      // 1. ENVIRONNEMENT (repli)
      float h = R.y;
      // le sol reflete est nettement plus sombre que le ciel
      vec3 haut = mix(vec3(1.10, 1.06, 0.98), sky_color.rgb * 1.20, smoothstep(0.0, 0.6, h));
      vec3 bas = mix(ground_color.rgb * 0.34, ground_color.rgb * 0.12, smoothstep(0.0, 0.45, -h));
      vec3 env = mix(bas, haut, smoothstep(-0.02, 0.02, h));
      {
        vec4 dc = clip_from_world * vec4(R * (2000.0 * M), 1.0);
        if (dc.w > 1e-4) {
          vec2 duv = (dc.xy / dc.w) * 0.5 + 0.5;
          if (duv.x > 0.0 && duv.x < 1.0 && duv.y > 0.0 && duv.y < 1.0 && scene_depth(duv) < 1e-5) {
            vec3 ciel_ecran = (has_scene_tex > 0.5) ? texture(scene_tex, duv).rgb : texture(tex_T0, duv).rgb;
            vec2 db = min(duv, 1.0 - duv);
            env = mix(env, ciel_ecran, smoothstep(0.0, 0.08, min(db.x, db.y)));
          }
        }
      }
      env *= mix(0.22, 1.0, jour);
      // a l'interieur, pas de ciel
      env = mix(env, mix(ground_color.rgb * 0.16, sky_color.rgb * 0.34, smoothstep(-0.6, 0.6, h)),
                interieur);
      // le repli prend la clarte de ce qu'on voit
      if (has_reflet_env > 0.5) {
        env *= clamp(moy / 0.45, 0.2, 1.6);
      }
      // l'entourage reellement vu par la camera dans la direction R (panorama rempli image par image)
      if (has_reflet_env > 0.5) {
        vec2 euv = vec2(atan(R.x, R.z) * (0.5 / 3.14159265) + 0.5,
                        asin(clamp(R.y, -1.0, 1.0)) / 3.14159265 + 0.5);
        // panorama deux fois plus fin
        float lod = verre_px > 0.5 ? 1.6 : mix(3.7, 2.4, metal_poli);
        // alpha = confiance
        vec4 e = textureLod(reflet_env_tex, euv, lod);
        if (e.a > 0.02) {
          env = mix(env, e.rgb / e.a, smoothstep(0.05, 0.45, e.a));
        }
      }
      // recoin : le ciel n'arrive pas jusque-la
      env *= 1.0 - 0.75 * ao_px;

      // 2. VRAI REFLET
      vec3 refl = env;
      // un rayon qui revient vers la camera part vers ce que l'ecran ne montre pas
      float vers_scene = smoothstep(-0.10, 0.25, dot(R, I));
      if (view_dist < 90.0 * M && vers_scene > 0.001) {
        vec3 P0 = P + N * (0.03 * M + view_dist * 0.003);
        float pas = max(0.12 * M, view_dist * 0.03);
        // depart decale d'une fraction de pas, differente pour chaque pixel
        float decal = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float t_prev = 0.0;
        for (int i = 1; i <= 16; i++) {
          float rt = pas * (float(i) - decal) * (1.0 + 0.2 * float(i));
          vec3 Pw = P0 + R * rt;
          vec4 rc = clip_from_world * vec4(Pw - cam_trans.xyz, 1.0);
          if (rc.w < 1e-4) {
            break;
          }
          vec2 ruv = (rc.xy / rc.w) * 0.5 + 0.5;
          if (ruv.x < 0.0 || ruv.x > 1.0 || ruv.y < 0.0 || ruv.y > 1.0) {
            break;
          }
          float sd = scene_depth(ruv);
          if (sd < 1e-5) {
            t_prev = rt;
            continue;  // ciel : rien a toucher
          }
          float behind = length(Pw - cam_trans.xyz) - length(world_from_depth(ruv, sd) - cam_trans.xyz);
          if (behind > 0.0 && behind < max(0.3 * M, rt * 0.25)) {
            float ta = t_prev, tb = rt;
            for (int j = 0; j < 6; j++) {
              float tm = 0.5 * (ta + tb);
              vec3 Pm = P0 + R * tm;
              vec4 mc = clip_from_world * vec4(Pm - cam_trans.xyz, 1.0);
              vec2 muv = (mc.xy / max(mc.w, 1e-4)) * 0.5 + 0.5;
              float md = scene_depth(muv);
              float bh = md > 1e-5 ? length(Pm - cam_trans.xyz) -
                                         length(world_from_depth(muv, md) - cam_trans.xyz)
                                   : -1.0;
              if (bh > 0.0) {
                tb = tm;
              } else {
                ta = tm;
              }
            }
            vec4 hc = clip_from_world * vec4(P0 + R * tb - cam_trans.xyz, 1.0);
            vec2 huv = clamp((hc.xy / max(hc.w, 1e-4)) * 0.5 + 0.5, vec2(0.0), vec2(1.0));
            vec3 hit;
            // REFLET FLOU SELON LA DISTANCE (comme un vrai metal)
            float rugo = mix(0.22, 0.65, 1.0 - metal_poli) * (1.0 - verre_px * 0.7) + sol_w * 0.18;
            float fl = 0.0012 + rugo * 0.035 * clamp(tb / (tb + view_dist), 0.0, 1.0);
            if (has_scene_tex > 0.5) {
              hit = texture(scene_tex, huv).rgb;
              for (int k = 0; k < 8; k++) {
                vec2 ok = kOct8[k] * fl * ((k & 1) == 0 ? 1.0 : 0.55);
                hit += texture(scene_tex, clamp(huv + ok * vec2(1.0, 1.6), 0.0, 1.0)).rgb;
              }
              hit /= 9.0;
            } else {
              hit = texture(tex_T0, huv).rgb;
            }
            vec2 bord = min(huv, 1.0 - huv);
            // sols : reflet de l'ecran un peu moins present
            float hw = smoothstep(0.0, 0.08, bord.x) * smoothstep(0.0, 0.08, bord.y) *
                       (1.0 - smoothstep(12.0, 16.5, float(i))) * vers_scene * (1.0 - 0.2 * sol_w);
            // surface touchee par l'envers (le rayon passe derriere un objet)
            if (has_normal_tex > 0.5) {
              vec4 hn = texture(normal_tex, huv);
              if (hn.a > 0.5) {
                vec3 nh = normalize(hn.rgb * 2.0 - 1.0);
                vec3 Ph = world_from_depth(huv, scene_depth(huv));
                if (dot(nh, cam_trans.xyz - Ph) < 0.0) {
                  nh = -nh;  // comme plus haut : la normale visible fait face a la camera
                }
                hw *= 1.0 - smoothstep(0.05, 0.35, dot(nh, R));
              }
            }
            refl = mix(env, hit, hw);
            break;
          }
          t_prev = rt;
        }
      }

      float fres = pow(1.0 - nv, 5.0);
      // 3. METAL : le reflet prend la couleur du metal (bronze, or, acier)
      if (metal_px > 0.001) {
        float cm = max(c.r, max(c.g, c.b));
        vec3 teinte = cm > 1e-3 ? c / cm : vec3(1.0);
        teinte = mix(vec3(1.0), teinte, 0.85);
        float dose = metal_px * reflet_fx.x * loin;
        float F0 = mix(0.55, 0.75, metal_poli);
        float k = (F0 + (1.0 - F0) * fres) * dose;
        // le metal MULTIPLIE ce qu'il reflete par sa propre couleur
        float lr_ = luma(refl);
        float rapport = lr_ / max(moy, 0.03);
        float rs = 2.0 * rapport / (1.0 + rapport);
        // metal POLI
        rs = mix(rs, clamp(rapport, 0.5, 2.8), metal_poli);
        vec3 refl_n = refl / max(lr_, 1e-3);
        // amplitude 0,75
        c *= mix(vec3(1.0), mix(vec3(1.0), refl_n, mix(0.25, 0.12, metal_poli)) * rs,
                 k * mix(0.75, 1.0, metal_poli));
        c += refl * teinte * (k * mix(0.06, 0.22, metal_poli));
        // LUSTRE DU CIEL sur le metal poli
        {
          float vers_ciel = pow(max(R.y, 0.0), 1.4);
          c += teinte * mix(vec3(1.0), sky_color.rgb, 0.35) * vers_ciel * 0.32 * metal_poli * dose *
               mix(0.25, 1.0, jour) * (1.0 - interieur) * (1.0 - 0.75 * ao_px);
        }
        // lustre large du soleil sur le metal poli
        if (sun_dir.w > 0.5) {
          float lr = max(dot(R, sun_dir.xyz), 0.0);
          // lustre plus large et plus fort, pow 9 et 0,60
          c += teinte * vec3(1.0, 0.95, 0.85) * (pow(lr, 9.0) * 0.60 * jour * (1.0 - interieur)) *
               metal_poli * dose * (1.0 - 0.75 * ao_px);
        }
      }
      // 4. VERRE : peu de reflet de face, presque un miroir au rasant
      if (verre_px > 0.001) {
        float k = (0.14 + 0.86 * fres) * verre_px * reflet_fx.x * loin;
        c = mix(c, refl, clamp(k, 0.0, 0.85));
      }
      // 5. ECLAT DU SOLEIL
      if (sun_dir.w > 0.5 && jour > 0.01) {
        float rl = max(dot(R, sun_dir.xyz), 0.0);
        if (rl > 0.9) {
          float soleil = 1.0;
          vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
          // sun_map_ok
          if (sun_map_ok > 0.5 && sp.x > 0.01 && sp.x < 0.99 && sp.y > 0.01 && sp.y < 0.99 &&
              sp.z < 1.0) {
            soleil = texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
          }
          float pw = verre_px > 0.5 ? 900.0 : mix(140.0, 650.0, metal_poli);
          // x1,7 sur le metal poli
          float eclat = pow(rl, pw) * (0.4 + pw / 260.0) * (1.0 + 0.7 * metal_poli * (1.0 - verre_px));
          c += vec3(1.0, 0.95, 0.85) * eclat * soleil * jour * (1.0 - interieur) *
               max(metal_px, verre_px) * reflet_fx.x * loin;
        }
      }
    }
  }

  // LA MER JUSQU'A L'HORIZON
  if (sky_pixel && (water_glitter.y > 0.001 || water_glitter.z > 0.001) &&
      cam_trans.y > water_glitter.x) {
    vec3 Ih = normalize(world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99));
    if (Ih.y < -0.002) {
      vec3 Ps = cam_trans.xyz + Ih * ((water_glitter.x - cam_trans.y) / Ih.y);
      float sm = length(Ps - cam_trans.xyz) / 4096.0;
      vec2 hq = houle_pente(Ps.xz / 4096.0, water_glitter.w, sm * 0.0011 / max(abs(Ih.y), 0.04));
      c = houle_surface(c, normalize(vec3(-hq.x, 1.0, -hq.y)), Ih,
                        clamp(1.0 - night_fx.x, 0.0, 1.0), 1.0 - smoothstep(120.0, 260.0, sm));
    }
  }
  if (sky_pixel && (water_glitter.y > 0.001 || water_glitter.z > 0.001) &&
      cam_trans.y > water_glitter.x && water_murk_ocean < 0.5) {
    // direction du regard
    vec3 Dv = world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99);
    if (Dv.y < 0.0) {
      c = mix(c, sea_murk(100.0), 0.94);
    }
  }

  // LA NUIT, LES LUMIERES BRILLENT
  {
    float nb = clamp(night_fx.x, 0.0, 1.0);
    if (nb > 0.001) {
      c *= 1.0 + 0.6 * nb * emissive_px;
      if (has_scene_tex > 0.5 && has_hud_ref > 0.5) {
        ivec2 ipn = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
        vec3 add = max(texelFetch(hud_ref_tex, ipn, 0).rgb - texelFetch(scene_tex, ipn, 0).rgb,
                       vec3(0.0));
        float amax = max(add.r, max(add.g, add.b));
        float asat = amax > 1e-4 ? (amax - min(add.r, min(add.g, add.b))) / amax : 0.0;
        float gp = nb * smoothstep(0.25, 0.60, asat);
        // pas sur les particules d'EAU (chute, gouttes, embruns) ni sur la brume qui les entoure
        if (gp > 0.001) {
          ivec2 tsz = textureSize(hud_ref_tex, 0);
          float eau_m = texelFetch(hud_ref_tex, ipn, 0).a;
          float rpx = float(tsz.y) * 0.03;
          for (int k = 0; k < 16; k++) {
            ivec2 q = clamp(ipn + ivec2(kOct8[k & 7] * (k < 8 ? rpx : rpx * 2.0)), ivec2(0),
                            tsz - 1);
            eau_m = max(eau_m, texelFetch(hud_ref_tex, q, 0).a * (k < 8 ? 0.9 : 0.7));
          }
          gp *= 1.0 - smoothstep(0.1, 0.5, eau_m);
        }
        c += (add * 1.6 + vec3(luma(add)) * 0.9) * gp;
      }
    }
  }

  // TRACES DANS LA NEIGE (neige qui se tasse)
  if (!sky_pixel && snow_trail.x > 0.5 && N.y > (snow_geom > 0.5 ? 0.0 : 0.45)) {
    const float SM = 4096.0;
    vec3 pm = P / SM;
    if (length(pm.xz - snow_trail.yz / SM) < 49.0) {
      float now = snow_trail.w;
      vec3 q = pm;
      float py0 = pm.y;  // hauteur de la surface d'origine (avant tassement)
      float h;
      if (snow_geom > 0.5) {
        // P est deja au fond du creux
        float h0 = snow_relief(pm, now);
        h = snow_relief(vec3(pm.x, pm.y - h0, pm.z), now);
        py0 = pm.y - h;
      } else {
        // parallaxe : le regard (camera -> P) continue sous la surface jusqu'au fond
        vec3 vd = normalize(pm - cam_trans.xyz / SM);
        float hq = snow_relief(pm, now);
        if (hq < -0.005 && vd.y < -0.05) {
          float tmax = min(1.2 / -vd.y, 5.0);
          float tprev = 0.0;
          for (int k = 1; k <= 20; k++) {
            float t = tmax * float(k) / 20.0;
            vec3 qq = pm + vd * t;
            if (qq.y - pm.y <= snow_relief(vec3(qq.x, pm.y, qq.z), now)) {
              float ta = tprev, tb = t;
              for (int r = 0; r < 6; r++) {
                float tm = 0.5 * (ta + tb);
                vec3 qm = pm + vd * tm;
                if (qm.y - pm.y <= snow_relief(vec3(qm.x, pm.y, qm.z), now)) tb = tm; else ta = tm;
              }
              q = pm + vd * tb;
              break;
            }
            tprev = t;
          }
        }
        h = snow_relief(vec3(q.x, pm.y, q.z), now);
      }
      if (h != 0.0) {
        vec3 Ls = sun_dir.w > 0.5 ? sun_dir.xyz : normalize(vec3(0.35, 0.86, -0.36));
        float lit = 1.0;
        if (snow_geom < 0.5) {
          // normale du relief au point vu
          const float e = 0.06;
          vec3 qb = vec3(q.x, pm.y, q.z);
          float hx = snow_relief(qb + vec3(e, 0.0, 0.0), now) - snow_relief(qb - vec3(e, 0.0, 0.0), now);
          float hz = snow_relief(qb + vec3(0.0, 0.0, e), now) - snow_relief(qb - vec3(0.0, 0.0, e), now);
          vec3 tn = normalize(N + vec3(-hx, 0.0, -hz) / (2.0 * e));
          float l0 = max(dot(N, Ls), 0.0) * 0.75 + 0.25;
          float l1 = max(dot(tn, Ls), 0.0) * 0.75 + 0.25;
          lit = clamp(l1 / max(l0, 0.05), 0.45, 1.4);
        }
        // ombre du bord dans le creux
        float shadow = 0.0;
        if (h < -0.01) {
          vec3 sp = vec3(q.x, py0 + h, q.z);
          for (int k = 1; k <= 8; k++) {
            vec3 s2 = sp + Ls * (max(0.08, -h / 5.0) * float(k));  // pas allonge si creux profond
            // ombre douce
            float pen = snow_relief(vec3(s2.x, py0, s2.z), now) - (s2.y - py0);
            shadow = max(shadow, clamp(pen / 0.06, 0.0, 1.0) * (1.0 - float(k - 1) / 8.0));
          }
        }
        // neige creusee BLANCHE
        c *= lit * mix(1.0, 0.78, shadow);
        // neige tassee
        float trough = clamp(-h / 0.35, 0.0, 1.0);
        float bump = clamp(h / 0.08, 0.0, 1.0);
        c *= mix(vec3(1.0), vec3(0.90, 0.94, 0.99), trough);
        c *= 1.0 + 0.10 * bump;
      }
    }
  }

  // RAYONS DE SOLEIL A TRAVERS LE FEUILLAGE (jungle, de jour)
  if (shaft_amt > 0.001 && has_shaft_tex > 0.5) {
    float sadd = texture(shaft_tex, tex_coord).r;
    c += vec3(1.0, 0.90, 0.68) * (sadd / (1.0 + 2.0 * sadd)) * shaft_amt;
  }
  // FAISCEAUX DE CANOPEE
  if (shaft_amt > 0.001 && has_shaft_tex > 0.5 && grade_amt2.w < 0.01) {
    float cba = texture(shaft_tex, tex_coord).g;
    c += vec3(1.0, 0.86, 0.56) * (cba / (1.0 + 1.2 * cba)) * shaft_amt * 0.40;
    // poussieres en suspension dans les colonnes
    if (cba > 0.04) {
      vec3 L = cb_axis(sun_dir.xyz);
      vec3 ro = cam_trans.xyz / 4096.0;
      vec3 rs = normalize(world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99));
      vec2 uvp = tex_coord + vec2(0.0, 1.0 / float(textureSize(tex_T0, 0).y));
      vec3 rs2 = normalize(world_from_depth(uvp, 0.5) - world_from_depth(uvp, 0.99));
      float pxa = length(rs2 - rs);  // angle d'un pixel (rad)
      vec3 rd = rs;
      float tmax = 15.0;
      if (!sky_pixel) {
        vec3 rv = (P - cam_trans.xyz) / 4096.0;
        tmax = min(length(rv), 15.0);
        rd = rv / max(length(rv), 1e-3);
      }
      float tt = water_glitter.w;
      vec2 X0 = cb_proj(ro, L);
      vec2 V = rd.xz - L.xz / L.y * rd.y;
      float v2 = max(dot(V, V), 1e-6);
      float vl = sqrt(v2);
      vec2 cell = floor(X0 / CB_CELL);
      vec2 st = vec2(V.x >= 0.0 ? 1.0 : -1.0, V.y >= 0.0 ? 1.0 : -1.0);
      vec2 av = max(abs(V), vec2(1e-5));
      vec2 tdel = CB_CELL / av;
      vec2 tnext = ((cell + max(st, 0.0)) * CB_CELL - X0) * st / av;
      float grains = 0.0;
      for (int k = 0; k < 5; k++) {
        vec4 b = cb_beam(cell, tt);
        if (b.w > 0.0) {
          float tc = dot(b.xy - X0, V) / v2;
          vec2 dv = X0 + V * tc - b.xy;
          float hw2 = b.z * b.z - dot(dv, dv);
          float hw = sqrt(max(hw2, 0.0)) / vl;
          float t0 = max(tc - hw, 1.0), t1 = min(tc + hw, min(tmax, t0 + 3.0));
          if (hw2 > 0.0 && t1 > t0) {
            vec3 qs = (sun_light_vp * vec4((ro + rd * tc) * 4096.0, 1.0)).xyz * 0.5 + 0.5;
            float ombre = 0.0;
            if (qs.x > 0.01 && qs.x < 0.99 && qs.y > 0.01 && qs.y < 0.99 && qs.z < 1.0) {
              ombre = 1.0 - texture(sun_shadow_tex, vec3(qs.xy, qs.z - 0.002));
            }
            if (ombre > 0.01) {
              const float CS = 0.4;
              vec3 p0 = ro + rd * t0;
              vec3 cc = floor(p0 / CS);
              vec3 s3 = vec3(rd.x >= 0.0 ? 1.0 : -1.0, rd.y >= 0.0 ? 1.0 : -1.0,
                             rd.z >= 0.0 ? 1.0 : -1.0);
              vec3 ad = max(abs(rd), vec3(1e-5));
              vec3 td = CS / ad;
              vec3 tn = t0 + ((cc + max(s3, 0.0)) * CS - p0) * s3 / ad;
              float sg = b.z * 0.62;
              for (int m = 0; m < 10; m++) {
                float hp = cb_hash3(cc);
                if (hp < 0.3) {
                  vec3 j = vec3(cb_hash3(cc + 11.1), cb_hash3(cc + 23.7), cb_hash3(cc + 37.3));
                  vec3 mp = (cc + 0.25 + 0.5 * j) * CS;
                  // derive lente (reste dans sa cellule)
                  mp += 0.07 * vec3(sin(tt * 0.7 + j.x * 6.3), sin(tt * 0.45 + j.y * 6.3),
                                    sin(tt * 0.6 + j.z * 6.3));
                  vec3 w = mp - ro;
                  float tm = dot(w, rd);
                  float dr = length(w - rd * tm);
                  float pxm = max(tm, 0.5) * pxa;  // taille d'un pixel a cette distance
                  float g = 1.0 - smoothstep(0.5 * pxm, 1.9 * pxm, dr);
                  vec2 xm = cb_proj(mp, L) - b.xy;
                  float dans = exp(-dot(xm, xm) / (sg * sg));
                  float scint = 0.35 + 0.65 * max(sin(tt * (1.1 + 6.0 * hp) + j.y * 6.3), 0.0);
                  grains += g * dans * scint * ombre * (1.0 - smoothstep(8.0, 15.0, tm));
                }
                float tout = min(tn.x, min(tn.y, tn.z));
                if (tout > t1) {
                  break;
                }
                if (tn.x <= tn.y && tn.x <= tn.z) {
                  cc.x += s3.x;
                  tn.x += td.x;
                } else if (tn.y <= tn.z) {
                  cc.y += s3.y;
                  tn.y += td.y;
                } else {
                  cc.z += s3.z;
                  tn.z += td.z;
                }
              }
            }
          }
        }
        float tout = min(tnext.x, tnext.y);
        if (tout > tmax) {
          break;
        }
        if (tnext.x < tnext.y) {
          cell.x += st.x;
          tnext.x += tdel.x;
        } else {
          cell.y += st.y;
          tnext.y += tdel.y;
        }
      }
      c += vec3(1.0, 0.93, 0.78) * min(grains, 1.5) * 1.6 * shaft_amt;
    }
    if (!sky_pixel) {
      float dcam = length(P - cam_trans.xyz) / 4096.0;
      if (dcam < 70.0) {
        vec3 L = cb_axis(sun_dir.xyz);
        vec2 X = cb_proj(P / 4096.0, L);
        vec4 b = cb_beam(floor(X / CB_CELL), water_glitter.w);
        // la tache s'etale un peu plus que la colonne (lumiere diffusee), bord doux
        vec2 dx = X - b.xy;
        float tache = b.w > 0.0 ? 1.0 - smoothstep(b.z * 0.75, b.z * 1.35, length(dx)) : 0.0;
        vec2 fv = (X + vec2(0.13, 0.07) * water_glitter.w) * 2.2;
        tache *= smoothstep(0.18, 0.62, 0.65 * cb_vnoise(fv) + 0.35 * cb_vnoise(fv * 2.3 + 5.1));
        if (tache > 0.0) {
          vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
          float ombre = 0.0;
          if (sp.x > 0.01 && sp.x < 0.99 && sp.y > 0.01 && sp.y < 0.99 && sp.z < 1.0) {
            ombre = 1.0 - texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
          }
          if (ombre > 0.01) {
            ombre *= cb_ciel(P, L);
          }
          float face = mix(0.3, 1.0, max(dot(N, L), 0.0));
          float loin = 1.0 - smoothstep(40.0, 70.0, dcam);
          c += c * vec3(1.0, 0.88, 0.58) * (tache * b.w * ombre * face * loin * 3.6 * shaft_amt);
        }
      }
    }
  }
  // TACHES DE SOLEIL dans la cabane de Samos (toit ouvert)
  if (!sky_pixel && shaft_amt > 0.001 && hut_px > 0.01) {
    vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    if (sp.x > 0.0 && sp.x < 1.0 && sp.y > 0.0 && sp.y < 1.0 && sp.z < 1.0) {
      float sndl = max(dot(N, sun_dir.xyz), 0.0);
      float sbias = 0.002 + 0.006 * (1.0 - sndl);
      float slitp = 0.0;
      vec2 spx = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
      for (int sy = -1; sy <= 1; sy++) {
        for (int sx = -1; sx <= 1; sx++) {
          slitp += texture(sun_shadow_tex, vec3(sp.xy + vec2(sx, sy) * spx * 2.0, sp.z - sbias));
        }
      }
      slitp /= 9.0;
      // tache franche
      c += c * vec3(1.0, 0.84, 0.56) * (slitp * sndl * 1.4 * hut_px);
    }
  }

  // FAISCEAU DE LUMIERE MIS EN SCENE dans la cabane de Samos (etage)
  {
    float hb = clamp(grade_amt2.w, 0.0, 1.0) * clamp(1.0 - 1.4 * night_fx.x, 0.0, 1.0);
    if (hb > 0.001) {
      const float M = 4096.0;
      // le faisceau tombe de l'OUVERTURE DU TOIT au-dessus de l'arbre
      vec3 b0 = vec3(-123.5, 61.5, 204.5) * M;  // ouverture du toit
      vec3 b1 = vec3(-129.0, 46.2, 201.5) * M;  // plancher, a gauche de l'arbre
      vec3 bax = b1 - b0;
      float blen = length(bax);
      vec3 bdir = bax / blen;
      float r0 = 0.75 * M, r1 = 1.5 * M;
      // 1. lumiere RECUE par la surface
      if (!sky_pixel) {
        float sp = clamp(dot(P - b0, bdir) / blen, 0.0, 1.06);
        float dp = length(P - (b0 + bdir * (sp * blen)));
        float rp = mix(r0, r1, sp);
        float inside = exp(-(dp * dp) / (rp * rp) * 1.6);
        float facing = mix(0.35, 1.0, max(dot(N, -bdir), 0.0));
        c += c * vec3(1.0, 0.86, 0.60) * (inside * facing * 1.0 * hb);
      }
      // 2. l'air du cone brille (le regard de la camera jusqu'a la surface)
      vec3 ro = cam_trans.xyz;
      vec3 rd;
      float rl;
      if (sky_pixel) {
        rd = normalize(world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99));
        rl = 40.0 * M;
      } else {
        vec3 rv = P - ro;
        rl = length(rv);
        rd = rv / max(rl, 1.0);
      }
      // points les plus proches entre le regard [0, rl] et l'axe [0, blen]
      vec3 w0 = ro - b0;
      float bb = dot(rd, bdir);
      float dd = dot(rd, w0), ee = dot(bdir, w0);
      float den = max(1.0 - bb * bb, 1e-4);
      float tr = clamp((bb * ee - dd) / den, 0.0, rl);
      float ta = clamp(dot(ro + rd * tr - b0, bdir), 0.0, blen);
      tr = clamp(dot(b0 + bdir * ta - ro, rd), 0.0, rl);
      vec3 pa = b0 + bdir * ta;
      float dist = length(ro + rd * tr - pa);
      float sa = ta / blen;
      float ra = mix(r0, r1, sa);
      // bord net mais doux (lumiere solide), coeur un peu plus clair
      float core = 1.0 - smoothstep(0.55, 1.15, dist / ra);
      core *= 0.75 + 0.25 * exp(-(dist * dist) / (ra * ra) * 4.0);
      // longueur traversee (regard de biais = plus de lumiere), fondu aux deux bouts
      float cross_len = 1.0 / max(sqrt(den), 0.35);
      float ends = smoothstep(0.0, 0.05, sa) * (1.0 - smoothstep(0.85, 1.0, sa) * 0.6);
      float t = water_glitter.w;
      // poussiere en suspension
      float shimmer = 0.80 + 0.20 * sin(sa * 23.0 + t * 0.9) * sin(sa * 9.0 - t * 0.5 + dist / M * 2.0);
      float glow = core * ends * shimmer * min(cross_len, 1.7);
      c += vec3(1.0, 0.88, 0.62) * (glow * 0.9 * hb);
    }
  }

  // la VIE DANS L'AIR est dessinee par-dessus cette passe

#ifdef POST_PASS1
  // + 2 sur le feuillage (pour la clarte de l'etalonnage, passe 2)
  color = vec4(c, sky_pixel ? -1.0 : edge_amt + (pc_feuillage ? 2.0 : 0.0));
  return;
#endif
#endif  // POST_PASS2 (fin de la premiere moitie)

  // --- the lamps as visible SOURCES ---- Lighting the surfaces around a lamp is not enough: the lamp
  {
    int n = int(night_fx.w);
    float night_glow = clamp(night_fx.x, 0.0, 1.0);
    // PERF facteur final des halos classiques
    float glow_gate = max(night_glow, clamp(grade_amt2.w, 0.0, 1.0)) * clamp(night_fx.z, 0.0, 2.0);
    if (n > 0) {
      vec2 ts = vec2(textureSize(tex_T0, 0));
      float aspect = ts.x / ts.y;
      vec3 glow = vec3(0.0);
      vec3 glow_em = vec3(0.0);  // auras des émissives (jour et nuit)
      // PERF seules les lampes de la tuile
      uvec2 hmask = pc_tile_mask();
      for (int it = 0; it < MAX_LAMPS; it++) {
        int i = pc_next_lamp(hmask);
        if (i < 0 || i >= n) {
          break;
        }
        vec3 lp = lamp_pos[i].xyz;
        // PERF famille decodee EN TETE
        float iw = lamp_color[i].w;
        bool em_h = iw < -8.0;
        bool fix_h = iw < -40.0;
        // les ECOS ont leur propre halo, jour et nuit (les ecos bleus etaient peu visibles)
        bool eco_h = iw < 0.0 && iw >= -8.0;
        if (!em_h && !eco_h && glow_gate <= 0.0) {
          continue;
        }
        // PERF projection, cadre, rayon et fondu viennent de lamp_scr (C++, une fois par image)
        vec4 ls = lamp_scr[i];
        if (ls.w < 0.0) {
          continue;
        }
        vec2 luv = ls.xy;
        float ldist = length(lp - cam_trans.xyz);
        float rad = ls.z;
        float halo_fade = ls.w;
        /* (original, par pixel)
        vec4 lclip = clip_from_world * vec4(lp - cam_trans.xyz, 1.0);
        // In this projection w is NEGATIVE in front of the camera
        if (lclip.w < 1e-5) {
          continue;
        }
        vec2 luv = (lclip.xy / lclip.w) * 0.5 + 0.5;
        if (luv.x < -0.3 || luv.x > 1.3 || luv.y < -0.3 || luv.y > 1.3) {
          continue;
        }
        float ldist = length(lp - cam_trans.xyz);
        // angular radius, clamped so distant lamps stay a visible point
        float rad = clamp((4096.0 * 3.2) / max(ldist, 1.0), 0.006, 0.075);
        // les halos ecran s'EFFACENT avec la distance
        float halo_fade = 1.0 - smoothstep(4096.0 * 30.0, 4096.0 * 85.0, ldist);
        if (halo_fade < 0.01) {
          continue;
        }
        */
        // décode l'intensité (feu -(i+16), luminaire -(i+64))
        float inten_h = fix_h ? (-iw - 64.0) : (em_h ? (-iw - 16.0) : abs(iw));
        // (flare aussi sur les lampes la nuit)
        if (fx_quality.w > 0.001 && (eco_h || em_h || fix_h || night_glow > 0.2) &&
            luv.x > -0.05 && luv.x < 1.05 && luv.y > -0.05 && luv.y < 1.05) {
          vec2 gaxis = vec2(0.5) - luv;
          vec2 pixc = (tex_coord - vec2(0.5)) * vec2(aspect, 1.0);
          float gsum = 0.0;
          for (int gi = 0; gi < 2; gi++) {
            float k = (gi == 0) ? 0.45 : 1.05;
            float grad = ((gi == 0) ? 0.020 : 0.034) * clamp(rad / 0.03, 0.5, 1.5);
            vec2 gp = (luv + gaxis * k - vec2(0.5)) * vec2(aspect, 1.0);
            float gd = length(pixc - gp) / grad;
            // PERF hors du disque du fantome la contribution vaut exactement 0 (pow(0, 1.5))
            if (gd < 1.0) {
              gsum += pow(1.0 - gd, 1.5) * ((gi == 0) ? 0.6 : 0.4);
            }
          }
          if (gsum > 0.001) {
            // presence douce (lamp_presence) et fondu quand la lampe approche du bord de l'ecran
            float gbord = smoothstep(-0.05, 0.03, min(min(luv.x, 1.0 - luv.x), min(luv.y, 1.0 - luv.y)));
            float seen = gbord > 0.0 ? lamp_presence(luv, ldist, rad) * gbord : 0.0;
            if (seen > 0.0) {
              float ng = fix_h ? mix(0.18, 1.0, night_glow) : 1.0;
              glow_em += lamp_color[i].rgb * gsum * inten_h * ng * halo_fade * fx_quality.w * 2.5 * seen;
            }
          }
        }
        // PERF test du DISQUE avant l'occlusion
        vec2 dd = (tex_coord - luv) * vec2(aspect, 1.0);
        float t = clamp(1.0 - length(dd) / rad, 0.0, 1.0);
        if (t <= 0.0) {
          continue;
        }
        // Soft occlusion: five taps around the lamp instead of one
        /* (original)
        float cd = scene_depth(luv);
        if (cd < 1e-5) {
          continue;
        }
        float d_center = length(world_from_depth(luv, cd) - cam_trans.xyz);
        if (d_center < ldist - 4096.0 * 1.5) {
          continue;  // something clearly stands in front of it
        }
        */
        float lpres = lamp_presence(luv, ldist, rad);
        if (lpres <= 0.0) {
          continue;
        }
        // the neighbours only soften the edge; sky counts as nothing, not as clear line of sight
        float vis = 0.0;
        for (int t = 0; t < 5; t++) {
          vec2 o = t == 0 ? vec2(0.0)
                          : vec2(cos(1.5707963 * float(t)), sin(1.5707963 * float(t))) * rad * 0.6;
          float od = scene_depth(luv + o);
          if (od < 1e-5) {
            continue;
          }
          float d_occ = length(world_from_depth(luv + o, od) - cam_trans.xyz);
          vis += smoothstep(-4096.0 * 2.5, 4096.0 * 0.5, d_occ - ldist);
        }
        vis /= 5.0;
        vis *= lpres;
        if (vis < 0.01) {
          continue;
        }
        float core = pow(t, 3.0);
        float halo = pow(t, 2.0) * 0.12;
        // l'aura d'un luminaire s'efface aussi en plein jour
        if (fix_h) {
          inten_h *= mix(0.18, 1.0, max(clamp(night_fx.x, 0.0, 1.0), clamp(ground_color.w, 0.0, 1.0)));
        }
        if (eco_h) {
          // coeur eclatant sur la boule + aura de sa couleur, a toute heure
          glow_em += lamp_color[i].rgb * (core * 0.55 + halo * 2.6) * inten_h * vis * halo_fade;
        } else if (em_h) {
          // la lave RESPIRE
          float fw = water_glitter.w;
          float ph = lamp_orig[i] * 7.0;
          float fl = 0.84 + 0.09 * sin(fw * 2.3 + ph) + 0.06 * sin(fw * 7.1 + ph * 1.7) +
                     0.04 * sin(fw * 13.7 + ph * 2.3);
          fl += 0.12 * max(0.0, sin(fw * 5.3 + ph) * sin(fw * 8.9 + ph * 0.6) - 0.55);
          // cœur réduit
          glow_em += lamp_color[i].rgb * (core * 0.08 + halo * 2.2) * inten_h * vis * fl * halo_fade;
        } else {
          // cœur écrasé (0.50 → 0.18)
          glow += lamp_color[i].rgb * (core * 0.18 + halo) * inten_h * vis * halo_fade;
        }
      }
      // epaule sur la SOMME
      glow = glow / (1.0 + 0.9 * glow);
      glow_em = glow_em / (1.0 + 0.9 * glow_em);
      c += glow * glow_gate * 0.12;
      // les auras émissives vivent jour ET nuit (un peu plus fortes la nuit)
      c += glow_em * 0.10 * (0.7 + 0.5 * max(night_glow, clamp(ground_color.w, 0.0, 1.0)));
    }
  }

  // --- the sun itself: disc, glare and rays when the camera looks at it
  if (sun_screen.z > 0.5 && sun_screen.w > 0.02 && light_fx.x > 0.001) {
    // aspect-corrected distance so the sun stays round on a 16:9 window
    vec2 asp = vec2(textureSize(tex_T0, 0));
    vec2 dsun = (tex_coord - sun_screen.xy) * vec2(asp.x / asp.y, 1.0);
    float rs = length(dsun);
    float up = clamp(sun_screen.w, 0.0, 1.0);
    // the sun is only drawn where nothing occludes it
    if (sky_pixel) {
      float disc = smoothstep(0.055, 0.026, rs);
      float glare = pow(clamp(1.0 - rs / 0.55, 0.0, 1.0), 3.0);
      vec3 sun_col = mix(vec3(1.35, 0.72, 0.34), vec3(1.30, 1.20, 0.98), up);
      // le disque du post ne se dessinait JAMAIS avant (sun_screen faux)
      c += sun_col * (disc * 0.9 + glare * 0.35) * light_fx.x;
    }
    // warm veiling glare across the whole frame, strongest near the sun
    float veil = pow(clamp(1.0 - rs / 1.15, 0.0, 1.0), 2.5);
    c += vec3(1.0, 0.88, 0.66) * veil * 0.08 * light_fx.x * up;
  }

  // --- god rays: light streaming from the sun's on-screen position
  if (light_fx.x > 0.001 && sun_screen.z > 0.5 && sun_screen.w > 0.02) {
    vec2 delta = (tex_coord - sun_screen.xy) * (1.0 / 24.0) * 0.85;
    vec2 uv = tex_coord;
    float decay = 1.0;
    vec3 shaft = vec3(0.0);
    for (int i = 0; i < 24; i++) {
      uv -= delta;
      vec3 s = (has_scene_tex > 0.5) ? texture(scene_tex, clamp(uv, 0.0, 1.0)).rgb
                                     : texture(tex_T0, clamp(uv, 0.0, 1.0)).rgb;
      // only bright pixels contribute, so shafts come from sky and highlights
      shaft += max(vec3(0.0), s - vec3(0.62)) * decay;
      decay *= 0.955;
    }
    shaft *= (1.0 / 24.0) * light_fx.x * clamp(sun_screen.w, 0.0, 1.0);
    // fade out away from the sun so the effect stays local to it
    float dist = clamp(1.0 - length(tex_coord - sun_screen.xy) * 0.75, 0.0, 1.0);
    c += vec3(1.0, 0.94, 0.80) * shaft * dist * 2.2;
  }

  // --- LENS FLARES (effet léger) ---- Reflets d'objectif
  if (fx_quality.w > 0.001 && sun_screen.z > 0.5 && sun_screen.w > 0.02 && light_fx.x > 0.001) {
    vec2 asp = vec2(textureSize(tex_T0, 0));
    float ar = asp.x / asp.y;
    // 1. visibilité du soleil
    float vis = 0.0;
    for (int i = 0; i < 8; i++) {
      vec2 o = vec2(kOct8[i].x / ar, kOct8[i].y) * 0.018;
      vis += (scene_depth(clamp(sun_screen.xy + o, 0.001, 0.999)) < 1e-5) ? 1.0 : 0.0;
    }
    vis /= 8.0;
    // 2. fondu quand le soleil s'approche du bord / sort du cadre
    vec2 sn = sun_screen.xy * 2.0 - 1.0;
    float in_frame = 1.0 - smoothstep(0.95, 1.12, max(abs(sn.x), abs(sn.y)));
    // la hauteur du soleil ne module pas l'intensite
    float gain = fx_quality.w * light_fx.x * smoothstep(0.02, 0.12, sun_screen.w) * vis * in_frame;
    if (gain > 0.001) {
      vec2 center = vec2(0.5);
      vec2 axis = (center - sun_screen.xy);  // du soleil vers le centre
      vec2 pix = (tex_coord - center) * vec2(ar, 1.0);
      vec3 flare = vec3(0.0);
      // 3. fantômes
      for (int i = 0; i < 5; i++) {
        float k; float rad; vec3 col; float w;
        if (i == 0)      { k = 0.35; rad = 0.045; col = vec3(1.00, 0.72, 0.40); w = 0.55; }
        else if (i == 1) { k = 0.62; rad = 0.075; col = vec3(0.55, 0.80, 1.00); w = 0.45; }
        else if (i == 2) { k = 0.95; rad = 0.028; col = vec3(1.00, 0.55, 0.75); w = 0.55; }
        else if (i == 3) { k = 1.30; rad = 0.110; col = vec3(0.60, 0.95, 0.80); w = 0.30; }
        else             { k = 1.70; rad = 0.060; col = vec3(1.00, 0.90, 0.65); w = 0.40; }
        vec2 gp = (center + axis * k - center) * vec2(ar, 1.0);
        float d = length(pix - gp) / rad;
        // disque doux avec un bord un peu plus dense
        float disc = pow(clamp(1.0 - d, 0.0, 1.0), 1.6) * 0.7 + smoothstep(1.0, 0.85, d) * 0.3;
        flare += col * disc * w;
      }
      // 4. anneau irisé
      {
        vec2 rp = (center + axis * 0.55 - center) * vec2(ar, 1.0);
        float d = length(pix - rp);
        vec3 ring = vec3(smoothstep(0.014, 0.0, abs(d - 0.190)),
                         smoothstep(0.014, 0.0, abs(d - 0.180)),
                         smoothstep(0.014, 0.0, abs(d - 0.170)));
        flare += ring * vec3(1.0, 0.85, 0.9) * 0.22;
      }
      // 5. lueur chromatique douce autour du soleil
      {
        vec2 sp = (sun_screen.xy - center) * vec2(ar, 1.0);
        float d = length(pix - sp);
        float halo = pow(clamp(1.0 - d / 0.42, 0.0, 1.0), 3.0);
        flare += vec3(1.0, 0.82, 0.55) * halo * 0.35;
      }
      // dose legere
      c += flare * gain * 0.50;
    }
  }

  // --- BLOOM LARGE (rendu remake) ---- Le grand halo doux des sources
  if (has_bloom_tex > 0.5 && post_fx.x > 0.001) {
    vec3 wide = texture(bloom_tex, tex_coord).rgb;
    // tout l'ecran
    vec3 wb = wide * post_fx.x;
    c += wb / (1.0 + 1.5 * luma(wb));
  }

  // --- filmic tonemap (gentle, blended)
  if (depth_fx.w > 0.001) {
    vec3 filmic = filmic_aces(c * 1.05);
    // keep blacks from crushing: slight lift
    filmic = filmic * 0.97 + 0.015;
    c = mix(c, filmic, clamp(depth_fx.w, 0.0, 1.0));
  }

  // --- toon / cel-shading pass
  if (toon_fx.x > 0.001 || toon_fx.y > 0.001) {
    vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
    // cross-Sobel on luminance for ink lines
    float l_r = luma(texture(tex_T0, tex_coord + vec2(px.x, 0.0)).rgb);
    float l_l = luma(texture(tex_T0, tex_coord - vec2(px.x, 0.0)).rgb);
    float l_u = luma(texture(tex_T0, tex_coord + vec2(0.0, px.y)).rgb);
    float l_d = luma(texture(tex_T0, tex_coord - vec2(0.0, px.y)).rgb);
    float grad = abs(l_r - l_l) + abs(l_u - l_d);
    float edge = clamp(grad * 7.0 - 0.35, 0.0, 1.0) * toon_fx.y;
    // posterize tones
    float levels = toon_fx.z > 0.5 ? toon_fx.z : 6.0;
    vec3 poster = floor(c * levels + vec3(0.5)) / levels;
    c = mix(c, poster, clamp(toon_fx.x, 0.0, 1.0));
    c *= 1.0 - edge * 0.85;
  }

  // ======================= RENDU REMAKE ======================= Objectif
  if (grade_fx.x > 0.001) {
    float amt = clamp(grade_fx.x, 0.0, 2.0);
    vec3 g = c;
    // NIVEAUX INTERIEURS hors huttes
    float cave = clamp(ground_color.w - grade_amt2.w, 0.0, 1.0);
    // etalonnage de cabane PAR PIXEL
    vec3 g_px = mix(grade_amt2.xyz, vec3(0.95, 1.6, 1.15), hut_px);
    // 1. exposition
    g *= mix(1.0, mix(g_px.x, 1.0, cave), amt);
    // 2. EPAULE DES HAUTES LUMIERES SEULEMENT
    {
      float m = max(g.r, max(g.g, g.b));
      // (p95 0.75 contre 0.88 dans la reference)
      const float knee = 0.80;
      if (m > knee) {
        float ex = m - knee;
        float room = 1.0 - knee;
        float m2 = knee + room * (ex / (ex + room));
        g *= mix(1.0, m2 / m, amt);
      }
    }
    // 3. contraste doux en S (autour de 0.5 affiche)
    {
      // original : mix(1.0, grade_amt2.y, ...)
      float k = mix(1.0, g_px.y, amt * (sky_pixel ? 0.4 : 1.0) * (1.0 - 0.6 * cave));
      vec3 x = clamp(g, 0.0, 1.0);
      vec3 s_curve = x * x * (3.0 - 2.0 * x);  // smoothstep
      g = mix(g, mix(x, s_curve, clamp(k - 1.0, 0.0, 1.0)), step(0.0, k - 1.0));
    }
    // 3b. OMBRES RELEVEES (plage)
    if (!sky_pixel) {
      float lu = luma(g);
      float t = 1.0 - smoothstep(0.0, 0.24, lu);
      // ni dans les grottes ni en interieur
      g += vec3(0.046, 0.050, 0.060) * t * amt * (1.0 - clamp(night_fx.x, 0.0, 1.0)) *
           (1.0 - clamp(cave + hut_px, 0.0, 1.0));
    }
    // 4. saturation
    {
      float lu = luma(g);
      // original : mix(1.0, grade_amt2.z, amt)
      g = mix(vec3(lu), g, mix(1.0, g_px.z, amt));
    }
    // 4b. HERBE MOINS FLUO
    {
      float mx = max(g.r, max(g.g, g.b));
      float mn = min(g.r, min(g.g, g.b));
      float sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
      // pour de l'herbe (il devenait terne et gris)
      float yg = smoothstep(0.02, 0.15, min(g.r, g.g) - g.b) * step(g.b, g.g) *
                 smoothstep(0.92, 0.98, g.g / max(g.r, 1e-4));
      // saturation 0.68 contre 0.58, valeur 0.69 contre 0.57)
      if (sat > 0.50 && yg > 0.0) {
        float k = (0.50 + (sat - 0.50) * 0.35) / sat;
        g = mix(g, mix(vec3(mx), g, k), yg * clamp(amt, 0.0, 1.0));
        g *= mix(1.0, 0.88, yg * smoothstep(0.55, 0.85, mx) * clamp(amt, 0.0, 1.0));
      }
    }
    // 5. etalonnage
    {
      float lu = clamp(luma(g), 0.0, 1.0);
      vec3 shadow_tint = vec3(0.97, 0.99, 1.04);
      vec3 high_tint = vec3(0.99, 1.0, 1.03);
      vec3 tint = mix(shadow_tint, high_tint, smoothstep(0.25, 0.75, lu));
      g *= mix(vec3(1.0), tint, amt * clamp(grade_fx.w, 0.0, 2.0));
    }
    // 6. nettete locale (masque flou leger)
    if (grade_fx.y > 0.001 && !sky_pixel) {
      vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
      vec3 blur = texture(tex_T0, tex_coord + vec2(px.x, 0.0)).rgb +
                  texture(tex_T0, tex_coord - vec2(px.x, 0.0)).rgb +
                  texture(tex_T0, tex_coord + vec2(0.0, px.y)).rgb +
                  texture(tex_T0, tex_coord - vec2(0.0, px.y)).rgb;
      float hp = luma(texture(tex_T0, tex_coord).rgb) - luma(blur * 0.25);
      g += clamp(hp, -0.10, 0.10) * grade_fx.y * amt;
    }
    // 6b. CLARTE (herbe trop transparente)
    if (!sky_pixel) {
      vec2 px2 = 2.0 / vec2(textureSize(tex_T0, 0));
      float l0 = luma(texture(tex_T0, tex_coord).rgb);
      float lb = 0.0;
      for (int i = 0; i < 8; i++) {
        lb += luma(texture(tex_T0, tex_coord + kOct8[i] * px2).rgb);
      }
      lb *= 0.125;
      float ratio = clamp((l0 + 0.03) / (lb + 0.03), 0.75, 1.3);
      // pleine dose sur le feuillage seulement
      float clar = pc_feuillage ? 0.6 : 0.12;
      g *= mix(1.0, ratio, clar * clamp(amt, 0.0, 1.0) * (1.0 - edge_amt));
    }
    // 7. vignette tres douce
    if (grade_fx.z > 0.001) {
      vec2 d = (tex_coord - 0.5) * vec2(1.0, 0.95);
      float v = 1.0 - clamp(dot(d, d) * 1.15 * grade_fx.z * (1.0 - 0.5 * cave), 0.0, 0.35);
      g *= mix(1.0, v, amt);
    }
    c = max(g, vec3(0.0));
  }

  // HUD intact (voir hud_ref_tex)
  if (has_hud_ref > 0.5) {
    vec3 raw = texture(tex_T0, tex_coord).rgb;
    vec3 ref = texture(hud_ref_tex, tex_coord).rgb;
    vec3 dd = abs(raw - ref);
    // le cadre des lettres (quasi transparent) modifiait a peine les pixels mais suffisait a basculer
    float hud = smoothstep(0.12, 0.35, max(dd.r, max(dd.g, dd.b)));
    // BANDES NOIRES des cinematiques
    float bande = (toon_fx.w > 0.5 && (tex_coord.y < 0.09 || tex_coord.y > 0.91)) ? 1.0 : 0.0;
    hud = max(hud, bande * (1.0 - smoothstep(0.004, 0.02, max(raw.r, max(raw.g, raw.b)))));
    c = mix(max(c + (raw - ref), vec3(0.0)), raw, hud);
  }

  color = vec4(c * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
