#version 410 core

uniform sampler2D tex_T0;
out vec4 color;
in vec2 tex_coord;

uniform vec4 color_mult;
uniform vec4 color_add;

// PC-REMAKE post effects, all neutral at zero (uniform default):
//   x = bloom intensity   (0 = off, ~0.8 = Spyro-like glow)
//   y = bloom threshold   (offset from 0.6 base, usually 0)
//   z = saturation offset (0 = unchanged, +0.25 = vivid)
//   w = warmth            (0 = unchanged, +0.4 = golden sunlight)
uniform vec4 post_fx;

// PC-REMAKE toon / cel-shading, neutral at zero:
//   x = posterize strength (0..1), y = edge-line strength (0..1),
//   z = tone levels (defaults to 6 when <= 0), w = unused
uniform vec4 toon_fx;

// PC-REMAKE depth-aware post, neutral at zero:
//   x = SSAO strength, y = aerial fog density, z = far DOF strength,
//   w = filmic tonemap amount. PS2 depth convention: NEAR is LARGE.
uniform vec4 depth_fx;
uniform sampler2D depth_tex;
// PC-REMAKE 21/09 : G-buffer de normales (attachement 1 resolu) — rgb = normale
// monde *0.5+0.5, a = 1 quand le pixel en porte une. has_normal_tex = 0 si absent.
uniform sampler2D normal_tex;
uniform float has_normal_tex;
// [QA] 1 = afficher les normales (G-buffer en couleur, repli en gris) — env QA_SHOW_NORMALS
uniform float qa_show_normals;

// PC-REMAKE rebuilt lighting rig
//   light_fx  : x = god rays, y = sun relight, z = hemisphere ambient, w = spec
//   sun_screen: xy = sun position in screen UV, z = on-screen flag, w = altitude
//   sky_color / ground_color: the two halves of the hemisphere ambient
//   night_fx  : x = night amount (0 day .. 1 night), y = lamp reach (metres),
//               z = lamp brightness, w = number of active lamps
uniform vec4 light_fx;
// RENDU REMAKE : x = dose globale, y = nettete, z = vignette, w = etalonnage couleur
uniform vec4 grade_fx;
// x = exposition, y = contraste, z = saturation, w = libre
uniform vec4 grade_amt2;
// 22/09 : la scene AVANT sprites/HUD/textes (reflets d'eau et rayons sans les infos ecran)
uniform sampler2D scene_tex;
uniform float has_scene_tex;
// 12/10 : BLOOM LARGE (chaine de mips 1/2 -> 1/32, C++ pc_bloom_run), unite 11
uniform sampler2D bloom_tex;
uniform float has_bloom_tex;
// 12/10 : la meme scene SANS HUD (prise avant les sprites d'interface) : un pixel
// qui en differe appartient au HUD et garde sa couleur d'origine (ni relumiere,
// ni nuit, ni etalonnage — APPUIE SUR O n'est plus orange dans la hutte)
uniform sampler2D hud_ref_tex;
// PERF 14/10 : POST EN DEUX PASSES. Le shader entier (1 300 lignes) est limite par
// l'occupation du GPU (registres) : compile deux fois par le C++, POST_PASS1 calcule
// l'eclairage reconstruit et ecrit (couleur, aretes / ciel) dans une texture en
// flottant 32 bits ; POST_PASS2 reprend de la (halos, soleil, rayons, bloom large,
// tonemap, etalonnage, HUD). Sans drapeau : passe unique d'origine.
#ifdef POST_PASS2
uniform sampler2D lit_tex;
#endif
uniform float has_hud_ref;
// 22/09 : x = reflets (0-2), y = halos, z = contact, w = gain des LENS FLARES
uniform vec4 fx_quality;
uniform vec4 sun_screen;
uniform vec4 sky_color;
uniform vec4 ground_color;
uniform vec4 night_fx;
// PC-REMAKE : scintillement de l'eau — x = niveau de la mer (unités monde),
// y = force jour, z = force nuit, w = temps (s)
uniform vec4 water_glitter;
// 17/10 : 1 = la surface de l'eau a pose la brume (ocean_common.frag) — le post ne la
// repose pas au-dessus de l'eau (il la garde pour la camera sous l'eau)
uniform float water_murk_ocean;
// xyz = normalized direction TOWARDS the sun (world), w = 1 when it is up
uniform vec4 sun_dir;
uniform float foliage_transl;  // PC-REMAKE : translucidite du feuillage (0 = coupee)
// reflets du metal et du verre : x = dose (0 = coupes, /tmp/qa_no_reflet), y = 1 : drapeaux en
// couleur (QA : orange = metal, jaune = metal poli, cyan = verre)
uniform vec4 reflet_fx;
// panorama de l'entourage vu par la camera (reflet_env.frag) : rgb / a = couleur, a = couverture
uniform sampler2D reflet_env_tex;
uniform float has_reflet_env;

// World position is rebuilt from the depth buffer with the inverse of the
// game's own camera matrix, which is what makes local lights possible at all:
// nothing in the original pipeline hands the post pass a world position.
uniform mat4 inv_camera;
uniform mat4 clip_from_world;  // the forward transform, for screen-space ray marching
uniform vec4 cam_trans;

#define MAX_LAMPS 48
// 20/10 : faisceaux d'eco (2 extremites monde par faisceau) — lumiere en tube la nuit
uniform vec4 beam_seg[24];
uniform float beam_n;
// 20/10 : rayons de soleil volumetriques (jungle) — carte d'ombres du soleil (unite 14)
uniform sampler2DShadow sun_shadow_tex;
uniform mat4 sun_light_vp;
uniform float shaft_amt;
// 20/10 : traces dans la neige (unite 7) — R relief (m), G hauteur du sol (m), B instant (s) ;
// snow_trail = (active, x et z de Jak en unites du jeu, instant courant en s)
uniform sampler2D snow_trail_tex;
uniform vec4 snow_trail;
// 25/10 : empreintes dans le sable (background_common.cpp, SandPrints) : unite 4 ; sable = (active,
// x et z de Jak en unites du jeu, instant courant en s)
uniform sampler2D sable_tex;
uniform vec4 sable;
uniform float snow_geom;  // 1 = le sol est vraiment creuse (geometrie decoupee) : pas de parallaxe
// ---- TRACE DANS LA NEIGE : profil du sillon (meme code dans tfrag3_snow.tese et
// post_processing.frag). La carte (unite 7) garde, par texel de 10 cm : R = distance au chemin
// de Jak (m), G = hauteur de ses pieds, B = instant du passage, A = profondeur tassee.
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
// relief (m) au point pm (m) de la surface d'origine : < 0 sillon, > 0 bourrelet, 0 hors trace.
// Filtrage bilineaire fait a la main (texture flottante non filtree par le pilote Apple) ;
// lineaire pur : une distance s'interpole exactement (le lissage en S faisait onduler la paroi).
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
  // hors de portee du sillon le plus large (r 0,7 m x 1,95, bord irregulier compris)
  if (wv < 1e-3 || fill < 1e-3 || dist > 1.7) return 0.0;
  float dyg = pm.y - feet / wv;
  if (dyg < -1.5 || dyg > 2.6) return 0.0;
  float thick = snow_thick(dyg);
  // largeur : x1 en neige fine, jusqu'a x1,75 en neige epaisse ; bord irregulier (bruit lisse
  // fixe dans le monde, pas de dents de scie)
  float r = 0.40 * (1.0 + 0.30 * (thick - 1.0));
  // (v1 : octave a 9 /m — dents de 11 cm sur le bord, effet pixelise sur les parois)
  float jag = (snow_vnoise(pm.xz * 1.3) - 0.5) * 0.22 + (snow_vnoise(pm.xz * 3.5) - 0.5) * 0.10;
  float d = dist * (1.0 + jag);
  if (d > 1.95 * r) return 0.0;
  // profondeur : en neige epaisse, le sillon descend jusqu'aux pieds de Jak (ses jambes ne
  // disparaissent plus dans une neige intacte) ; en neige fine, tassement x epaisseur
  // (v1 : tassement x epaisseur seul — 40 cm au centre des etendues, Jak enfonce de 1 m)
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
// 20/10 v3 : rayons au quart de resolution, adoucis (unite 8)
uniform sampler2D shaft_tex;
uniform float has_shaft_tex;
// ---- 22/10 : FAISCEAUX DE CANOPEE (jungle). Des colonnes de lumiere descendent du feuillage,
// meme la ou le vrai soleil ne passe pas : elles tombent d'en haut (axe L presque vertical,
// penche de ~20 degres vers le soleil). Une colonne par cellule de 7 m, une cellule sur trois
// environ. Meme code dans shafts.frag (l'air des colonnes).
const float CB_CELL = 7.0;
vec3 cb_axis(vec3 to_sun) {
  return normalize(vec3(to_sun.x * 0.36, 1.0, to_sun.z * 0.36));
}
// projection le long de L sur le plan y = 0 (metres) : une colonne y devient un point
vec2 cb_proj(vec3 p_m, vec3 L) {
  return p_m.xz - L.xz / L.y * p_m.y;
}
float cb_hash(vec2 c, float k) {
  return fract(sin(dot(c, vec2(127.1, 311.7)) + k * 74.7) * 43758.5453);
}
// colonne de la cellule : centre (m), rayon (m), poids (0 = pas de colonne)
vec4 cb_beam(vec2 cell, float t) {
  if (cb_hash(cell, 0.0) > 0.24) {
    return vec4(0.0);
  }
  float h = cb_hash(cell, 1.0);
  float r = mix(0.45, 1.3, h);
  vec2 off = vec2(cb_hash(cell, 2.0), cb_hash(cell, 3.0));
  vec2 ctr = cell * CB_CELL + vec2(r) + off * (CB_CELL - 2.0 * r);
  float w = mix(0.55, 1.0, cb_hash(cell, 4.0));
  // le feuillage bouge : la colonne respire doucement
  w *= 0.78 + 0.22 * sin(t * (0.45 + 0.35 * h) + 6.2832 * cb_hash(cell, 5.0));
  return vec4(ctr, r, w);
}
// une colonne n'a de source que s'il y a du ciel au-dessus du feuillage : le vrai soleil
// doit eclairer l'un des points 12 m ou 24 m plus haut le long de L (sous la canopee : oui ;
// dans une salle enterree ou sous un toit : non). Hors de la carte d'ombres : oui.
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
// ---- 22/10 : VAGUES DE RIVAGE. Meme code dans post_processing.frag (nappe qui monte sur le sable, sable mouille)
// et ocean_common.frag (ecume sur l'eau) : une vague par periode
// de 7 s, decalee le long de la cote par un bruit (les fronts n'arrivent pas tous ensemble).
// Phase 0 -> 0.55 : la crete avance de 1,6 m de fond jusqu'au bord ; 0.55 -> 1 : elle monte
// sur le sable puis se retire.
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
// hauteur (m au-dessus de la mer) atteinte par la nappe a la phase ph ; hmax depend du lieu
float sh_runup(float ph, float hmax) {
  if (ph < 0.55) {
    return 0.0;
  }
  float s = (ph - 0.55) / 0.45;
  return s < 0.3 ? hmax * sin(s / 0.3 * 1.5708) : hmax * pow(1.0 - (s - 0.3) / 0.7, 1.4);
}
// dentelle d'ecume (motif ancre au monde, qui derive doucement)
float sh_lace(vec2 xz_m, float t) {
  // PC-REMAKE : ecume plus fine, en filaments (trois echelles, la plus fine etiree)
  // (original : a = noise(xz * 1.7 ...), b = noise(xz * 4.1 ...), smoothstep(0.32, 0.72, 0.62 a + 0.38 b))
  float a = sh_noise(xz_m * 2.6 + vec2(t * 0.11, -t * 0.07));
  float b = sh_noise(xz_m * vec2(7.0, 3.2) - vec2(t * 0.05, t * 0.09));
  float f = sh_noise(xz_m * 13.0 + vec2(t * 0.2, 0.0));
  float v = 0.5 * a + 0.32 * b + 0.18 * f;
  return smoothstep(0.38, 0.66, v) * (0.75 + 0.25 * f);
}
// ---- PC-REMAKE : HOULE. Champ de vagues de la surface de la mer : six trains (longueurs 0,7 a
// 12 m) qui avancent a la vitesse des vagues en eau profonde (omega = sqrt(g k)), tous a peu pres
// dans le sens du vent. Un train s'efface quand il devient plus fin que quelques pixels (pas de
// moire au loin). Rend la pente de la surface (dh/dx, dh/dz), metres et secondes.
vec2 houle_pente(vec2 xz, float t, float pix_m) {
  // directions etalees autour du vent (pas de rangees paralleles), longueurs non harmoniques
  const vec2 D[6] = vec2[6](vec2(0.80, 0.60), vec2(0.93, -0.37), vec2(0.38, 0.93),
                            vec2(0.99, 0.10), vec2(0.55, -0.83), vec2(0.07, 1.00));
  const float L[6] = float[6](12.3, 7.4, 4.6, 2.9, 1.75, 1.05);
  const float K[6] = float[6](0.050, 0.052, 0.050, 0.044, 0.036, 0.028);
  // champ deforme a deux echelles et en groupes de vagues : la surface n'a pas de motif
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
// Eclairage de la surface par la houle (meme mer turquoise, rien n'est blanchi) : les faces
// tournees vers l'horizon refletent plus de ciel (crete un peu plus claire, teinte de l'eau
// gardee), les faces vers la camera laissent voir plus d'eau profonde ; reflets du soleil fins.
vec3 houle_surface(vec3 c, vec3 N, vec3 I, float jour, float fondu) {
  float cv = clamp(dot(-I, N), 0.0, 1.0);
  float cf = clamp(-I.y, 0.0, 1.0);
  float F = 0.02 + 0.98 * pow(1.0 - cv, 5.0);
  float F0 = 0.02 + 0.98 * pow(1.0 - cf, 5.0);
  // contraste doux : la mer garde son grand degrade, les vagues ne font que le nuancer
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
uniform vec4 lamp_pos[MAX_LAMPS];    // xyz = world position, w = radius
uniform vec4 lamp_color[MAX_LAMPS];  // rgb = colour, w = intensity
// PERF 14/10 : position ecran des lampes, calculee une fois par image par le C++
// (xy = uv, z = rayon du halo, w = fondu de distance ; w < 0 = lampe sautee)
uniform vec4 lamp_scr[MAX_LAMPS];
// 14/10 : rang d'origine de chaque lampe (la liste est triee par le C++) — le
// vacillement des feux en depend
uniform float lamp_orig[MAX_LAMPS];
// PERF 14/10 : tuiles de lampes (16 x 9) — masque 48 bits (x = lampes 0-31, y = 32-47)
// des lampes qui peuvent agir dans la tuile (calcule par le C++, conservateur).
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

// PERF 14/10 : directions des echantillons PRECALCULEES (les boucles recalculaient
// cos/sin d'angles constants pour chaque pixel) — memes valeurs a la precision du
// flottant pres. Anneaux du petit bloom (6 et 10), octogone (DOF, clarte, soleil),
// SSAO (angle d'or + gigue deja appliquee).
const vec2 kRing6[6] = vec2[6](vec2(1.00000000, 0.00000000), vec2(0.50000000, 0.86602540), vec2(-0.50000000, 0.86602540), vec2(-1.00000000, 0.00000000), vec2(-0.50000000, -0.86602540), vec2(0.50000000, -0.86602540));
const vec2 kRing10[10] = vec2[10](vec2(0.95105652, 0.30901699), vec2(0.58778525, 0.80901699), vec2(0.00000000, 1.00000000), vec2(-0.58778525, 0.80901699), vec2(-0.95105652, 0.30901699), vec2(-0.95105652, -0.30901699), vec2(-0.58778525, -0.80901699), vec2(-0.00000000, -1.00000000), vec2(0.58778525, -0.80901699), vec2(0.95105652, -0.30901699));
const vec2 kOct8[8] = vec2[8](vec2(1.00000000, 0.00000000), vec2(0.70710690, 0.70710667), vec2(0.00000033, 1.00000000), vec2(-0.70710643, 0.70710713), vec2(-1.00000000, 0.00000065), vec2(-0.70710736, -0.70710620), vec2(-0.00000098, -1.00000000), vec2(0.70710597, -0.70710759));
const vec2 kSsao8[8] = vec2[8](vec2(0.32631457, 0.12656540), vec2(0.30336061, 0.68779238), vec2(-0.18205318, 0.46937466), vec2(-0.82818694, 0.36528355), vec2(-0.61243474, -0.23754098), vec2(-0.16489631, -0.37386024), vec2(0.29302880, -0.75549481), vec2(0.51425480, -0.22681926));

// 17/10 : couleur de l'eau vue a travers une colonne de col_m metres (brume sous-marine)
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

// 26/10 : part d'interieur de la cabane de Samos AU POINT P (unites du jeu), 0..1. Meme axe
// que *remake-hut-volumes* (time-of-day.gc), mais au ras des murs (14,3 a 15,5 m, piliers de
// l'ouverture compris) : le volume de la camera deborde jusqu'a 16 m sur la terrasse. En haut :
// 80 m (le plafond de l'etage monte au-dessus des 64 m du volume de la camera). Les
// reglages d'interieur (taches de soleil, rebond chaud, etalonnage) ne s'appliquent plus qu'a
// ce qui est dedans : le dehors vu par l'ouverture garde son eclairage d'exterieur (avant, il
// s'allumait d'un coup en sortant, jusqu'a ce que la camera passe les 16 m).
float pc_hut_inside(vec3 P) {
  vec3 q = P * (1.0 / 4096.0);
  vec2 dq = q.xz - vec2(-130.5, 208.4);
  float in_r = clamp((16.4 - length(dq)) / 0.8, 0.0, 1.0);
  float in_y = clamp((q.y - 31.5) * 0.5, 0.0, 1.0) * clamp((80.0 - q.y) * 0.5, 0.0, 1.0);
  return in_r * in_y;
}

// Surface orientation is not available anywhere in the original pipeline, so it
// is rebuilt in WORLD SPACE from the depth buffer: three unprojected points
// give two tangents, and their cross product is the true surface normal. This
// is what lets the pass do real N.L lighting instead of a screen-space guess.
//
// At silhouettes the neighbouring pixel belongs to another surface, which would
// produce a wildly wrong normal, so each axis picks the side (+ or -) whose
// depth is closest to the centre pixel.
// The normal is rebuilt from depth, which yields the GEOMETRIC normal — flat
// per polygon. Lighting with it makes every facet of every model visible, which
// the original vertex-colour shading never did (the models carry smoothed
// normals). Sampling over a wider baseline averages across several facets and
// gives back a smooth surface; a narrow estimate is blended in so genuine
// creases and small props do not turn to mush.
//
// Each axis picks the side whose depth is closest to the centre pixel, so a
// silhouette never drags a neighbouring surface into the estimate.
vec3 raw_normal(vec2 uv, float d, vec3 P, float radius) {
  vec2 px = radius / vec2(textureSize(depth_tex, 0));

  float dxp = texture(depth_tex, uv + vec2(px.x, 0.0)).r;
  float dxm = texture(depth_tex, uv - vec2(px.x, 0.0)).r;
  // 25/10 : au bord de l'ecran, le voisin exterieur n'existe pas (il relisait le bord : normale
  // faussee, eclairage different sur quelques pixels) — on prend le voisin interieur
  // (original : abs(dxp - d) < abs(dxm - d) ; idem en y)
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
// vrai si le point monde q, vu a l'ecran, montre la mer (le fond vu a travers est sous le
// niveau de la mer) ; hors ecran : non
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
  // la surface traversee par ce regard doit etre LA (a moins de 1,5 m de q) : vue par-dessus
  // un talus, la mer lointaine derriere lui ne compte pas
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
// distance horizontale (m) du point P a la mer (vers_mer) ou a la terre ferme, au plus 4,5 m
// (99 = rien) : dans chacune des 8 directions, premier anneau touche (1,2 / 2,6 / 4,5 m) puis
// dichotomie ; on garde le minimum — une mesure continue quand P se deplace (prendre seulement
// la premiere direction touchee faisait des bords droits)
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

// PC-REMAKE : CARTES DU RIVAGE (une par niveau cotier, outils/rivages.py) : distance signee au trait de
// cote en metres, > 0 en mer, < 0 sur la terre, bornee a 32 m. 99 = aucune carte ici (on garde
// alors la recherche a l'ecran ci-dessus).
uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
// 25/10 : ondes autour de Jak dans l'eau (OpenGLRenderer.cpp) : x, z (unites), instant (s), force
uniform vec4 ondes[12];
uniform vec4 riv_box[2];  // x0, z0 (m), 1/largeur, 1/profondeur
uniform int riv_n;
// 25/10 : EAU INTERIEURE (rivieres, chenaux etroits : +40 dans la carte, shore_map.cpp) — ni mer
// ni rivage : -95 (terre lointaine : ni nappe, ni vagues, ni houle). Teste sur les 4 cellules
// voisines (sans filtrage : le fondu vers la berge ferait une fausse bande de mer).
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
      // (original : d = texture(riv_tex0, uv).r; idem pour la 2e carte)
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

// crude screen-space AO from depth alone: neighbors closer than us occlude
// Ambient occlusion done properly: for each neighbour, unproject it to a world
// position and ask whether it sits inside the hemisphere above this surface.
// That is what puts real contact shading under roof overhangs, in corners and
// where props meet the ground — the depth-difference hack it replaces could
// only ever guess.
// Screen-space contact shadows. The sun shadow map handles the big picture,
// but it can never resolve the small stuff — the line under a roof overhang,
// the dark seam where a crate meets the ground, a character's own shadow on
// itself. Here the depth buffer is marched towards the sun: if anything the
// camera can see stands between this pixel and the light, the pixel is shaded.
float contact_shadow(vec3 P, vec3 L, float pix_dist) {
  // 9 pas au lieu de 16 : mêmes portée/fenêtre, pas plus longs — la moitié du
  // coût pour un rendu quasi identique (optimisation FPS)
  // PERF 25/08 : 9 -> 7, meme portee (le dither ordonne lisse la difference)
  const int STEPS = 7;
  float step_len = 4096.0 * (0.36 + 0.50 * clamp(pix_dist / (4096.0 * 60.0), 0.0, 1.0));
  // Ordered dither instead of a hash: a hash gives every pixel an independent
  // offset, which is exactly what made these shadows look like noise. An
  // ordered pattern varies smoothly over a 4x4 block, so neighbouring pixels
  // sample almost the same thing.
  ivec2 ip = ivec2(gl_FragCoord.xy) & 3;
  const float kDither[16] =
      float[16](0.0625, 0.5625, 0.1875, 0.6875, 0.8125, 0.3125, 0.9375, 0.4375, 0.25, 0.75, 0.125,
                0.625, 1.0, 0.5, 0.875, 0.375);
  float jitter = kDither[ip.y * 4 + ip.x];

  // Accumulate partial occlusion rather than stopping at the first hit: a
  // hard stop turns a smooth surface into speckle.
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
    // PC-REMAKE : un echantillon est dans l'ombre s'il est CACHE a la camera (la surface visible est
    // plus proche que lui). Le signe inverse ombrait le sol degage et epargnait l'ombre de Jak :
    // halo clair sous le personnage, sable marbre.
    // (original : float behind = length(world_from_depth(uv, sd) - cam_trans.xyz) -
    //                            length(sp - cam_trans.xyz);)
    float behind = length(sp - cam_trans.xyz) - length(world_from_depth(uv, sd) - cam_trans.xyz);
    // smooth entry and exit of the thickness window, so the shadow fades in
    // PC-REMAKE : epaisseur d'obstacle bornee a ~1 m : un personnage (quelques dizaines de cm)
    // ne projette plus un pave d'ombre de plusieurs metres derriere lui
    // (original : smoothstep(0.05, 0.35 m) * (1 - smoothstep(4 m, 7 m)))
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
  // 25/10 : les tirages hors de l'ecran sont ignores (ils relisaient le bord de l'image : une
  // bande de ~11 px plus claire ou plus sombre le long des bords) ; le total est remis a
  // l'echelle des tirages valides
  float valides = 0.0;
  // 8 taps au lieu de 12 (angle d'or, couverture equivalente) — coût -33 %
  for (int i = 0; i < 8; i++) {
    // (original : a = 0.7853982 * i + 0.37 ; jitter = 0.35 + 0.65 * fract(i * 0.6180339) ;
    //  suv = uv + vec2(cos(a), sin(a)) * 11.0 * px * jitter)
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
  // (original : return clamp(occ / 3.4, 0.0, 1.0);)
  return clamp(occ * (8.0 / max(valides, 1.0)) / 3.4, 0.0, 1.0);
}

vec3 filmic_aces(vec3 x) {
  // Narkowicz ACES approximation
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

// 13/10 : contribution d'un echantillon au petit bloom — seuil sur la luminance,
// couleurs vives (herbe, feuillage) attenuees ; la teinte de l'echantillon est gardee
vec3 bloom_tap(vec3 s, float thresh) {
  float l = luma(s);
  float mx = max(s.r, max(s.g, s.b));
  float mn = min(s.r, min(s.g, s.b));
  float sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
  float exces = max(l - thresh, 0.0) * mix(1.0, 0.25, smoothstep(0.35, 0.75, sat));
  return l > 1e-4 ? s * (exces / l) : vec3(0.0);
}

void main() {
  bool pc_feuillage = false;  // PC-REMAKE : pixel de feuillage (translucidite, bloc soleil)
#ifdef POST_PASS2
  // reprise de la passe 1 : couleur eclairee, aretes (a >= 0) ou ciel (a < 0)
  vec4 lit = texelFetch(lit_tex, ivec2(tex_coord * vec2(textureSize(lit_tex, 0))), 0);
  vec3 c = lit.rgb;
  bool sky_pixel = lit.a < -0.5;
  float edge_amt = max(lit.a, 0.0);
  // 26/10 : part d'interieur de cabane du pixel (voir pc_hut_inside)
  float hut_px = 0.0;
  if (grade_amt2.w > 0.001 && !sky_pixel) {
    hut_px = clamp(grade_amt2.w, 0.0, 1.0) *
             pc_hut_inside(world_from_depth(tex_coord, scene_depth(tex_coord)));
  }
#else
  vec3 base = texture(tex_T0, tex_coord).rgb;

  // Rebuild the geometry of this pixel once: depth -> world position -> normal.
  // Every effect below (occlusion, aerial haze, sun, lamps) is built on it.
  float center_d = scene_depth(tex_coord);
  bool sky_pixel = center_d < 1e-5;
  vec3 P = cam_trans.xyz;
  vec3 N = vec3(0.0, 1.0, 0.0);
  float view_dist = 1e9;
  // SILHOUETTES (18/08, contours persistants) : bande de discontinuité de
  // profondeur autour des objets fins (Jak, herbes). Calculée UNE fois ici,
  // elle assainit la normale, le SSAO et les lampes.
  float edge_amt = 0.0;
  bool has_true_normal = false;
  // 21/09 : 1.0 = pixel d'une surface EMISSIVE (alpha 0.75 du G-buffer, tfrag3.frag)
  // -> source de lumiere, pas receveur : ni relumiere, ni lampes, ni SSAO dessus
  float emissive_px = 0.0;
  // metal et verre (drapeau 250 a 254/255 du G-buffer, tfrag3.frag) : part de metal (1 = poli,
  // ~0,5 = ordinaire, moins sur un bord MSAA melange avec un solide), metal poli, verre
  float metal_px = 0.0;
  float metal_poli = 0.0;
  float verre_px = 0.0;
  // 26/10 : part d'interieur de cabane du pixel (voir pc_hut_inside) ; le ciel : 0
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
    // PC-REMAKE 21/09 : VRAIE normale (lissee, par sommet) quand le G-buffer en
    // a une — terrain, TIE, personnages. La reconstruction depuis la profondeur
    // (geometrique, plate par facette) ne reste qu'en repli : sprites, eau,
    // shrubs, rendu PS2 direct.
    bool got_gbuffer_n = false;
    bool foliage_px = false;
    if (has_normal_tex > 0.5) {
      vec4 gn = texture(normal_tex, tex_coord);
      if (gn.a > 0.6 && gn.a < 0.9) {
        emissive_px = 1.0;
      }
      // 17/10 : alpha 0.95 = feuillage (tie_wind, shrub, feuilles tfrag)
      foliage_px = gn.a > 0.92 && gn.a < 0.98;
      {
        float a8 = gn.a * 255.0;
        if (a8 > 249.5 && a8 < 253.5) {
          // 250 poli : 1 ; 252 ordinaire : 0,8 ; 253 = bord MSAA d'un metal ordinaire : 0,4
          // (v1 : mix(1.0, 0.3, (a8 - 250.0) / 3.0) — l'acier restait trop discret)
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
      // une surface visible fait face a la camera : garde-fou contre une
      // normale d'artiste retournee (dos de feuillage, interpolation rasante).
      // 22/09 : seuil -0.15 -> 0.0. Une surface OPAQUE visible a forcement
      // N.V > 0 ; une normale inversee donne N.V < 0, meme en incidence rasante
      // (c'etait le cas des pans sombres du sable de la plage : N.V entre -0.15
      // et 0, jamais retournes -> sol eclaire comme s'il regardait le bas).
      // (original : < -0.15)
      // 17/10 : sauf le FEUILLAGE (feuilles fines, double face) : retourner sa normale
      // assombrissait d'un coup toutes les feuilles situees au-dessus de la camera — une
      // ligne nette a hauteur des yeux sur les couronnes des palmiers, a l'horizon.
      // 22/10 : sauf aussi une normale EXACTEMENT verticale : c'est la convention des dessins
      // feuillage / a decoupe du decor (Tfrag3Data, phong_subdivide_impl : normale
      // vers le haut), pas un vrai sol — un sol vu par-dessus ne tourne jamais le dos a la
      // camera. La retourner assombrissait d'un coup ces objets au-dessus de la hauteur des
      // yeux : une ligne nette a l'horizon (souches de la jungle).
      // (original : if (!foliage_px && dot(N, cam_trans.xyz - P) < 0.0) {)
      if (!foliage_px && N.y < 0.995 && dot(N, cam_trans.xyz - P) < 0.0) {
        N = -N;
      }
    } else {
      // au bord : la normale LARGE (4 px) contamine — bascule vers l'estimation
      // serrée seule
      vec3 near_n = raw_normal(tex_coord, center_d, P, 1.0);
      vec3 wide_n = raw_normal(tex_coord, center_d, P, 4.0);
      N = normalize(mix(near_n * 0.30 + wide_n * 0.70, near_n, edge_amt));
      if (dot(N, cam_trans.xyz - P) < 0.0) {
        N = -N;
      }
    }
  }

  if (qa_show_normals > 0.5) {
    // couleur = normale monde (x rouge, y vert, z bleu) ; G-buffer en couleur
    // pleine, reconstruction en demi-intensite pour distinguer les deux
    vec3 nc = N * 0.5 + 0.5;
    color = vec4(has_true_normal ? nc : nc * 0.5, 1.0);
    // 21/09 : les surfaces emissives (tag alpha 0.75) en magenta
    if (emissive_px > 0.5) {
      color = vec4(1.0, 0.0, 1.0, 1.0);
    }
    return;
  }

  // ---- far DOF (soft blur of distant scenery) ----
  if (depth_fx.z > 0.001) {
    float farness = clamp((view_dist - 4096.0 * 60.0) / (4096.0 * 140.0), 0.0, 1.0);
    if (farness > 0.01) {
      vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
      vec3 blur = base;
      for (int i = 0; i < 8; i++) {
        // (original : a = 0.785398 * i ; vec2(cos(a), sin(a)))
        blur += texture(tex_T0, tex_coord + kOct8[i] * 2.5 * px).rgb;
      }
      blur /= 9.0;
      base = mix(base, blur, farness * depth_fx.z);
    }
  }

  // ---- bloom: two spiral rings of taps, soft threshold ----
  vec3 bloom = vec3(0.0);
  if (post_fx.x > 0.001) {
    float thresh = 0.60 + post_fx.y;
    vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
    float r1 = 6.0;   // inner ring radius in pixels
    float r2 = 14.0;  // outer ring radius in pixels
    vec3 acc = vec3(0.0);
    // PERF 25/08 : 24 -> 16 taps a ENERGIE CONSTANTE — poids recalcules pour
    // conserver exactement la somme d'origine (8x1.0=8 ; 16x0.55=8.8)
    // inner ring: 6 taps (8/6 = 1.3333 chacun)
    // 13/10 (herbe trop transparente) : l'herbe au
    // soleil (vert vif, canal vert ~0.95) depassait le seuil PAR CANAL et toute
    // la prairie rayonnait : la lueur comblait les creux entre les brins
    // (contraste local divise par 2, mesure) -> aspect laiteux, transparent.
    // Desormais seuil sur la LUMINANCE et poids reduit pour les couleurs vives :
    // les lumieres claires (lampes, reflets, ciel, metal) brillent toujours.
    // (original : acc += max(vec3(0.0), s - vec3(thresh)) * poids;)
    for (int i = 0; i < 6; i++) {
      // (original : a = 6.2831853 * i / 6.0 ; vec2(cos(a), sin(a)))
      vec3 s = texture(tex_T0, tex_coord + kRing6[i] * r1 * px).rgb;
      acc += bloom_tap(s, thresh) * 1.3333;
    }
    // outer ring: 10 taps (8.8/10 = 0.88 chacun)
    for (int i = 0; i < 10; i++) {
      // (original : a = 6.2831853 * (i + 0.5) / 10.0 ; vec2(cos(a), sin(a)))
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

  // ---- grade: saturation + warmth, both offsets from neutral ----
  float sat = 1.0 + post_fx.z;
  c = mix(vec3(luma(c)), c, sat);
  c.r *= 1.0 + 0.055 * post_fx.w;
  c.g *= 1.0 + 0.012 * post_fx.w;
  c.b *= 1.0 - 0.045 * post_fx.w;

  // gentle highlight rolloff to avoid clipping when bloom is on
  if (post_fx.x > 0.001) {
    c = c / (1.0 + 0.12 * max(vec3(0.0), c - vec3(1.0)));
  }

  // FONDU CHAMP PROCHE (25/08, polygones trop visibles en cinematique) :
  // les normales reconstruites de la profondeur sont GEOMETRIQUES — plates par
  // facette. En gros plan (cinematiques : visages a 1-4 m de la camera) chaque
  // facette accroche le soleil et les lampes. Sous ~8 m, le modele directionnel
  // reconstruit s'efface au profit de l'eclairage d'origine des artistes (leurs
  // couleurs par sommet, deja dans l'image) ; le SOL (normale verticale) est
  // epargne pour garder les ombres de contact et le modele en gameplay.
  float near_flat = 0.0;
  if (!sky_pixel) {
    near_flat = 1.0 - smoothstep(4096.0 * 3.0, 4096.0 * 8.0, view_dist);
    float upn = clamp(N.y, 0.0, 1.0);
    near_flat *= 1.0 - upn * upn;
    // CINEMATIQUE (toon_fx.w = movie?) : aplatissement TOTAL, toute distance,
    // sol compris — les ombres portees de la shadow map restent, elles.
    near_flat = max(near_flat, clamp(toon_fx.w, 0.0, 1.0));
    // 21/09 : avec une VRAIE normale lissee, les facettes que near_flat cachait
    // n'existent plus — le modele directionnel revient a 60 % en champ proche et
    // en cinematique (40 % de la douceur validee conservee).
    // (original : pas de reduction)
    if (has_true_normal) {
      near_flat *= 0.4;
    }
  }
  // PARTICULES (13/10, texture absente derriere l'eau qui tombe,
  // cascade de la plage) : une particule n'ecrit ni profondeur ni
  // normale — le post relumait la cascade (voile blanc de centaines de sprites)
  // avec les facettes de la FALAISE derriere elle (roche blanche sans
  // texture). Couverture des particules = ecart entre la scene d'avant sprites
  // (scene_tex) et celle d'avant HUD (hud_ref_tex) ; sur ces pixels l'eclairage
  // devient UNIFORME (meme mecanisme que le champ proche) : jour, nuit et lampes
  // restent, les facettes, l'occlusion et les ombres de contact disparaissent.
  // A/B : PART_FLAT = 0.0 puis touch /tmp/pc_reload_post (rechargement a chaud).
  // (original : pas de cas particulier)
  const float PART_FLAT = 1.0;
  if (!sky_pixel && PART_FLAT > 0.5 && has_scene_tex > 0.5 && has_hud_ref > 0.5) {
    ivec2 ip = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
    vec3 pre_sprites = texelFetch(scene_tex, ip, 0).rgb;
    vec3 with_sprites = texelFetch(hud_ref_tex, ip, 0).rgb;
    vec3 dd = abs(with_sprites - pre_sprites);
    near_flat = max(near_flat, smoothstep(0.04, 0.30, max(dd.r, max(dd.g, dd.b))));
  }
  // FONDU CHAMP LOINTAIN (21/09, treillis sur les collines lointaines,
  // cause PROUVEE par mesure : relumiere solaire x3 et ambiante hemispherique
  // x2.2 d'energie haute frequence sur les collines lointaines, SSAO/spec/god
  // rays innocents). Au loin, la profondeur perd sa precision et la normale
  // reconstruite dessine des lignes de niveau (empreintes digitales). Le
  // modele directionnel s'efface donc entre 180 et 320 m — comme near_flat,
  // il rend la main aux couleurs des artistes sans toucher a l'exposition.
  // (piste non verifiee 21/09 : avec une vraie normale du G-buffer le fondu
  //  lointain pourrait etre supprime — `(sky_pixel || has_true_normal) ? 0.0 : …`
  //  — a tester a l'image ; l'etat ci-dessous est celui mesure et valide.)
  float far_flat = sky_pixel ? 0.0 : smoothstep(4096.0 * 180.0, 4096.0 * 320.0, view_dist);
  float flat_amt = max(near_flat, far_flat);
  // 22/10 : SOUS L'EAU, ECLAIRAGE RECONSTRUIT EN DEGRADE. La mer ne grave pas de profondeur
  // (elle est transparente) : pour un pixel vu a travers l'eau, P et N sont ceux du FOND. La
  // pleine mer (pas de fond dessine derriere) n'etait jamais relumee, l'eau au-dessus d'un
  // haut-fond l'etait (soleil, ciel) : la limite du fond dessinait une nappe claire a bords
  // droits, en polygones (village). Le haut-fond garde sa clarte, mais dosee par la part de
  // haut-fond (fond a moins de 4,5 m) sur un disque de 3 m autour du point de la surface : elle
  // s'estompe en degrade vers le large. Occlusion et ombre de contact du fond : retirees.
  // 22/10 : NEIGE CREUSEE (traces de Jak, geometrie decoupee). Le post relumait les parois du
  // sillon selon leur pente, avec la lumiere CHAUDE du soleil, sans savoir qu'elles etaient a
  // l'ombre des falaises : les parois tournees vers le soleil viraient au beige-brun. La neige
  // creusee est desormais eclairee comme la neige qui l'entoure (normale de la surface
  // d'origine, verticale, pour tout l'eclairage) : elle reste blanche ; son modele vient de
  // l'occlusion, de l'ombre du bord et de la teinte de neige tassee (bloc des traces, plus bas).
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
  if (!sky_pixel && (water_glitter.y > 0.001 || water_glitter.z > 0.001) &&
      cam_trans.y > water_glitter.x && P.y < water_glitter.x) {
    float ts = (water_glitter.x - cam_trans.y) / (P.y - cam_trans.y);
    float col = view_dist * (1.0 - clamp(ts, 0.0, 1.0)) / 4096.0;
    // (des 25 cm d'eau : une transition sur 1 m suivait la profondeur, donc la marche du banc)
    sous_eau = smoothstep(0.02, 0.25, col);
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

  // ---- depth-aware: SSAO (contact shading in crevices) ----
  float ao_px = 0.0;  // occlusion du pixel, gardee pour les reflets (le ciel ne se reflete pas
                      // au fond d'un recoin)
  if (depth_fx.x > 0.001 && !sky_pixel) {
    float ao = compute_ssao(tex_coord, center_d, P, N);
    // tint the occlusion slightly cool: shaded crevices lose the warm sun first
    ao *= 1.0 - edge_amt;  // pas d'anneau d'occlusion derrière les objets fins
    ao *= 1.0 - near_flat * 0.9;  // champ proche/cinematique : ne pas cerner les facettes
    ao *= (1.0 - emissive_px);  // 21/09 : pas d'occlusion sur une source
    ao *= 1.0 - sous_eau;  // 22/10 : pas d'occlusion du fond vu a travers l'eau
    c *= mix(vec3(1.0), vec3(0.88, 0.92, 1.02), ao * depth_fx.x) * (1.0 - ao * depth_fx.x * 0.85);
    ao_px = clamp(ao * depth_fx.x, 0.0, 1.0);
  }

  // ---- perspective atmosphérique du LOINTAIN (v2 25/08) ----
  if (depth_fx.y > 0.001) {
    // v1 partait de 70 m et plafonnait a 0.28 : trop tot (voile lavande
    // partout) et trop faible pour cacher les MAILLAGES-DECOR
    // de 2001 (montagnes vista quasi sans texture, tramage d'epoque, quasi
    // sans texture au loin). v2 : RIEN sous
    // ~250 m — aucun voile de gameplay — puis fondu franc : a 700 m et plus le
    // decor est presque de l'air, comme une vraie montagne lointaine.
    float far_m = view_dist / 4096.0;
    float haze = smoothstep(400.0, 900.0, far_m) * clamp(depth_fx.y * 4.0, 0.0, 0.92);
    float nt = clamp(night_fx.x, 0.0, 1.0);
    vec3 haze_color = mix(vec3(0.70, 0.80, 0.96), vec3(0.06, 0.09, 0.22), nt);
    c = mix(c, haze_color, haze);
  }

  // ================= rebuilt lighting =================
  // Everything below runs as a deferred pass: the world position and the true
  // surface normal are rebuilt from the depth buffer, so the sun, the sky and
  // the street lamps are computed with real N.L maths instead of screen-space
  // approximations. The original game had none of this — its lighting was
  // baked flat into vertex colours, which is why it reads as 2002.
  // 22/10 : VAGUES DE RIVAGE SUR LE SABLE (l'ecume sur l'eau est dans ocean_common.frag). Sur
  // le terrain bas qui borde la mer, une fine nappe d'eau monte puis se retire a chaque vague
  // (jusqu'a 1,6 a 3,2 m du bord), avec un front d'ecume ; le sable qu'elle couvre reste
  // mouille (plus sombre) et seche entre deux vagues. La distance au bord de l'eau est cherchee
  // a l'ecran (sh_distance) : rien sans mer visible a moins de 4,5 m.
  if (!sky_pixel && water_glitter.y > 0.001 && view_dist < 4096.0 * 90.0) {
    float hy = (P.y - water_glitter.x) / 4096.0;
    // PC-REMAKE : avec une carte du rivage, la nappe monte aussi sur le haut de plage (la mer
    // d'origine monte et descend : le sable du bord est jusqu'a 1 m au-dessus du plan)
    // (original : hy < 0.9 ; fondu 0.55 -> 0.9)
    float drv = riv_dist(P.xz / 4096.0);
    float hmax = drv < 90.0 ? 1.5 : 0.9;
    // pentes douces seulement (plage) : sur un talus raide la nappe ne monte pas
    if (hy > -0.02 && hy < hmax && N.y > 0.72) {
      // PC-REMAKE : distance a la mer lue dans la carte du rivage (original : sh_distance(P, true))
      float dmer = drv < 90.0 ? max(-drv, 0.0) : sh_distance(P, true);
      if (dmer < 90.0) {
        vec2 xz = P.xz / 4096.0;
        float t = water_glitter.w;
        float ph = sh_phase(xz, t);
        float fmax = 1.6 + 1.6 * sh_noise(xz * 0.08 + 1.3);
        float front = sh_runup(ph, fmax);
        float jour = clamp(1.0 - night_fx.x, 0.25, 1.0);
        float loin = (1.0 - smoothstep(50.0, 90.0, view_dist / 4096.0)) *
                     (1.0 - smoothstep(hmax * 0.6, hmax, hy)) * smoothstep(0.72, 0.86, N.y);
        // sable mouille : la zone que les vagues atteignent, qui seche entre deux vagues ; le
        // bord de l'eau reste toujours trempe
        float zone = 1.0 - smoothstep(fmax * 0.75, fmax * 1.05, dmer);
        float seche = ph < 0.55 ? ph / 0.55 : 0.0;
        float mouille = max(zone * (1.0 - 0.45 * seche), 1.0 - smoothstep(0.0, 0.35, dmer)) * loin;
        c = mix(c, c * vec3(0.72, 0.74, 0.78), mouille * 0.75);
        // nappe : fine pellicule d'eau claire, plus epaisse loin du front
        if (front > 0.02) {
          float nappe = (1.0 - smoothstep(front - 0.10, front + 0.02, dmer)) * loin;
          float epais = smoothstep(0.0, 0.9, front - dmer);
          // sous la nappe : sable trempe (plus sombre, a peine bleute) + reflet du ciel, plus
          // fort en rasant (Fresnel) et quand l'eau est plus epaisse
          vec3 Vn = normalize(cam_trans.xyz - P);
          float fres = 0.04 + 0.96 * pow(1.0 - clamp(Vn.y, 0.0, 1.0), 5.0);
          // PC-REMAKE : nappe plus lisible : sable trempe plus sombre et teinte d'eau, reflet du ciel
          // (original : sous = c * (0.80, 0.90, 0.95), film = mix(sous, ciel, fres * (0.35 + 0.45 epais)
          //  borne 0.6), dose nappe * (0.55 + 0.35 epais))
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
      // COTE MER : le pixel montre l'eau (fond sous la surface). Au point ou le regard traverse
      // la surface, distance a la terre ferme (meme recherche) ; les cretes des vagues y
      // avancent vers le bord et y arrivent quand la nappe commence a monter sur le sable.
      vec3 ps = cam_trans.xyz + (P - cam_trans.xyz) * ((water_glitter.x - cam_trans.y) / (P.y - cam_trans.y));
      float dps = length(ps - cam_trans.xyz) / 4096.0;
      // PC-REMAKE : avec une carte du rivage, les vagues se voient de plus loin (original : dps < 45.0)
      float drs = riv_dist(ps.xz / 4096.0);
      // 25/10 : pas de vagues la ou la carte dit terre ou eau interieure (tour du temple, rivieres)
      // (original : if (dps < (drs < 90.0 ? 90.0 : 45.0)) {)
      if (dps < (drs < 90.0 ? 90.0 : 45.0) && drs > -0.5) {
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
          if (drs < 90.0) {
            // PC-REMAKE : VAGUE QUI ARRIVE DU LARGE. La houle part de 18 m, ralentit et se creuse en
            // approchant (dos en pente douce cote large, front raide cote plage) ; sa levre
            // laisse passer la lumiere (bande turquoise claire). Vers 4 m elle deferle : ecume
            // sur la crete, puis un tapis d'ecume qui la suit et se dechire.
            if (ph < 0.55) {
              // chaque vague a sa force, qui varie aussi le long de la cote : certaines cassent
              // franchement, d'autres arrivent presque sans ecume
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
              // la crete n'est jamais un trait continu : elle se dechire comme le tapis
              ecume = crete * smoothstep(0.05, 0.55, lace) + tapis * smoothstep(0.25, 0.85, lace) * 0.6;
            }
            // ressac : la nappe redescend du sable, ecume dechiree pres du bord
            float retrait = ph > 0.55 ? (1.0 - smoothstep(0.0, 2.2, dter)) * (1.0 - (ph - 0.55) / 0.45) : 0.0;
            float lisere = 1.0 - smoothstep(0.0, 0.35 + 0.2 * sin(ph * 6.2832), dter);
            ecume = max(ecume, (lisere * 0.7 + retrait * 0.5) * (0.25 + 0.75 * lace));
            ecume = clamp(ecume, 0.0, 1.0) * loin;
            rouleau *= loin;
          } else {
            // (sans carte : version du 22/10)
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
          // levre de la vague : l'eau mince s'eclaire en turquoise (la lumiere la traverse)
          c = mix(c, c * 1.16 + vec3(0.02, 0.07, 0.06) * jour, rouleau * 0.5);
          vec3 blanc = c + (vec3(0.95, 0.98, 1.0) - c) * 0.85 * jour;
          c = mix(c, blanc, ecume * 0.9);
        }
      }
    }
  }

  // 22/10 : neige creusee eclairee comme la surface intacte qui l'entoure (voir neige_creux)
  N = normalize(mix(N, vec3(0.0, 1.0, 0.0), neige_creux));
  // 25/10 : EMPREINTES DANS LE SABLE. Le relief des pas (carte de 2,5 cm autour de Jak) incline la
  // normale : le soleil dessine le creux et son petit bourrelet ; le sable tasse au fond est un peu
  // plus sombre. Elles se comblent en 2 a 4 minutes.
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
    // champ proche / cinematique / LOINTAIN : l'hemispherique cesse de facetter
    // (original : up = mix(up, 0.5, near_flat);)
    up = mix(up, 0.5, flat_amt);
    float day = clamp(1.0 - night_fx.x, 0.0, 1.0);

    // ---- sun: real directional light ----
    if (light_fx.y > 0.001 && sun_dir.w > 0.5) {
      // PERF 25/08 : sun_dir arrive NORMALISE du C++ (OpenGLRenderer ~1805)
      vec3 L = sun_dir.xyz;
      // 21/09 (ombres parasites omnipresentes sur le sable de la plage,
      // soleil bas) : chaque triangle du terrain prenait sa propre teinte — des
      // facettes sombres a bords droits sur tout le sable. Le relief a grande
      // echelle du SOL est deja peint par les artistes (palettes par heure) : pour
      // les surfaces quasi horizontales la normale de relumiere est ramenee vers la
      // verticale et l'enveloppement elargi ; falaises, murs, personnages gardent
      // leur contraste plein.
      // (original : float ndl = max(dot(N, L), 0.0);
      //             float lit = clamp((dot(N, L) + 0.35) / 1.35, 0.0, 1.0);)
      float ground_w = smoothstep(0.55, 0.9, N.y);
      vec3 Nr = normalize(mix(N, vec3(0.0, 1.0, 0.0), ground_w * 0.7));
      float wrap = mix(0.35, 1.0, ground_w);
      float ndl = max(dot(Nr, L), 0.0);
      // wrapped diffuse: keeps the shadow side readable instead of pure black,
      // the way bounced light behaves outdoors
      float lit = clamp((dot(Nr, L) + wrap) / (1.0 + wrap), 0.0, 1.0);
      // warm direct sun vs cool sky-lit shade — this contrast IS the lighting
      vec3 sun_col = vec3(1.32, 1.16, 0.86);
      vec3 shade_col = vec3(0.74, 0.82, 1.04);
      // small-scale occlusion of the sun, marched through the depth buffer
      // PERF 25/08 : au-dela de 90 m le fondu vaut exactement 0 — on saute la marche
      if (depth_fx.z >= 0.0 && light_fx.w >= 0.0 && ndl > 0.001 && view_dist < 4096.0 * 90.0) {
        float cs = contact_shadow(P, L, view_dist) * clamp(1.0 - view_dist / (4096.0 * 90.0), 0.0, 1.0) *
                   (1.0 - sous_eau);
        lit *= 1.0 - cs * 0.85 * (1.0 - near_flat);
      }
      // champ proche : le contraste directionnel s'efface (0.62 = valeur moyenne
      // du wrap sur l'hemisphere — l'exposition ne bouge pas, le facettage oui)
      // (original : lit = mix(lit, 0.62, near_flat);) — lointain inclus
      lit = mix(lit, 0.62, flat_amt);
      vec3 lighting = mix(shade_col * 0.78, sun_col, lit);
      // PC-REMAKE : TRANSLUCIDITE DU FEUILLAGE (rendu recent). Herbe, palmes et feuilles sont des
      // limbes fins : contre le soleil, la lumiere les traverse et ils s'allument, teintes par
      // leur propre couleur (vert-jaune), au lieu de rester des cartes plates et sombres.
      // Seulement de jour ; /tmp/qa_no_transl = coupe (A/B, foliage_transl a 0).
      if (pc_feuillage && foliage_transl > 0.001) {
        float contre = pow(clamp(dot(-V, L), 0.0, 1.0), 3.0);
        // de dos (on voit la face non eclairee) : la transmission domine
        float dos = clamp(-dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
        lighting += vec3(1.10, 1.12, 0.62) * contre * (0.35 + 0.65 * dos) * 0.75 * foliage_transl *
                    (1.0 - flat_amt * 0.6);
      }
      // specular sheen so surfaces catch the sun instead of staying matte
      if (light_fx.w > 0.001) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(Nr, H), 0.0), 42.0) * ndl;
        // (original : * (1.0 - near_flat)) — le reflet s'eteint aussi au loin
        lighting += vec3(1.0, 0.95, 0.85) * spec * light_fx.w * 1.6 * (1.0 - flat_amt);
      }
      // (original : * day) — 21/09 : une source n'est pas relumee
      // 22/10 : sous l'eau, dose par la part de haut-fond (voir sous_eau) — (original : sans ce facteur)
      c *= mix(vec3(1.0), lighting, clamp(light_fx.y, 0.0, 1.0) * day * (1.0 - emissive_px) * mix(1.0, haut_fond, sous_eau));
    }

    // ---- sky ambient: cool light from the sky above, warm bounce from below ----
    if (light_fx.z > 0.001) {
      vec3 amb = mix(ground_color.rgb, sky_color.rgb, up);
      // 12/10 : dans une HUTTE (grade_amt2.w, volumes de time-of-day.gc), pas de
      // ciel bleu sur les murs : un rebond chaud de bois et de lampes (les murs de
      // la hutte de Samos etaient gris-bleu en plein jour). Grottes, cite
      // engloutie, temple : inchanges (w = 0 hors village1).
      // v2 : (0.78, 0.62, 0.44) noyait la piece en orange — rebond plus neutre,
      // les murs lavande des artistes restent lavande
      // 26/10 : par pixel (original : clamp(grade_amt2.w, 0.0, 1.0))
      amb = mix(amb, vec3(0.74, 0.66, 0.56), hut_px);
      // (original : * day) — 21/09 : idem pour l'hemispherique
      // 22/10 : sous l'eau, dose par la part de haut-fond (voir sous_eau) — (original : sans ce facteur)
      c *= mix(vec3(1.0), amb * 1.55, clamp(light_fx.z, 0.0, 1.0) * day * (1.0 - emissive_px) * mix(1.0, haut_fond, sous_eau));
    }

    // ============================================================
    // ECLAIRAGE UNIFIE (reecriture 19/08) : la nuit, l'ancienne lumiere
    // n'eclairait pas, elle ternissait l'image. Objectif : un eclairage reel,
    // jour et nuit, avec les couleurs des ecos.
    //
    // Principe : l'image du jeu est l'albedo eclaire plein jour. On ne
    // l'assombrit plus pour ensuite la re-multiplier (multiplier du noir
    // redonne du noir — d'ou l'image ternie). On compose UN facteur
    // d'eclairage lineaire, applique UNE fois :
    //
    //     c_final = c * ( ambiant(heure) + somme(lampes) )
    //
    // La nuit, l'ambiant est un clair de lune bleu sombre ; une lampe a
    // pleine force REDONNE a la surface ses couleurs de jour, teintees par la
    // couleur de la lampe : elle eclaire reellement.
    // Une lampe d'eco verte rend l'herbe vert-vif, une bleue rend tout
    // bleute : physique, sans astuce de teinte.
    // ============================================================
    float night = clamp(night_fx.x, 0.0, 1.0);
    // 21/09 : NIVEAU INTERIEUR (ground_color.w, pose par time-of-day.gc : grottes,
    // cite engloutie, temple, citadelle). Pas de soleil la-dedans : les luminaires
    // du decor (tubes chauds, lueurs precurseur, cristaux, plaques d'eco) brillent
    // a pleine force a toute heure — de jour ils etaient a 30 %.
    // 26/10 : la part de cabane est prise au pixel (ground_color.w = max(niveau interieur,
    // cabane) ; hors village1 la cabane vaut 0 et rien ne change)
    // (original : float interior = clamp(ground_color.w, 0.0, 1.0);)
    float interior = clamp(ground_color.w - grade_amt2.w, 0.0, 1.0) + hut_px;
    float lamp_hour = max(night, interior);
    {
      // ---- 1. ambiant selon l'heure ----
      vec3 ambient = vec3(1.0);
      if (night > 0.001) {
        // PERF 25/08 : normalize(vec3(0.35,0.86,-0.36)) plie a la main
        vec3 moon_dir = vec3(0.3515, 0.8636, -0.3615);
        float mndl = clamp(dot(N, moon_dir) * 0.5 + 0.5, 0.0, 1.0);
        float moon = pow(mndl, 1.7);  // face eclairee par la lune / face sombre
        moon = mix(moon, 0.55, near_flat);  // champ proche : pas de facettes lunaires
        vec3 moon_col = vec3(0.36, 0.52, 1.00);  // la lune est BLEUE
        vec3 amb_col = vec3(0.16, 0.22, 0.42);   // ambiante profonde
        vec3 night_light = moon_col * moon * 0.36 + amb_col * (0.30 + 0.16 * up);
        ambient = mix(vec3(1.0), night_light, night);
        // 13/10 (horizon de l'eau casse : noir au loin, ligne a travers
        // les objets) : au loin, le jeu a DEJA assombri le decor de
        // nuit avec son brouillard nocturne — la nuit refaite l'assombrissait une
        // 2e fois : une bande noire entre la mer et le ciel. Au-dela de 250 m le
        // decor garde sa couleur de nuit d'origine (rien au-dela de 600 m).
        ambient = mix(ambient, vec3(1.0), smoothstep(4096.0 * 250.0, 4096.0 * 600.0, view_dist));
      }
      // 12/10 : HUTTE — l'interieur est plus sombre que le dehors (les palettes de
      // jour des artistes, sans l'ombre du toit, donnaient un aplat clair) ; les
      // lampions y creusent ensuite leurs flaques chaudes
      // 26/10 : par pixel (original : clamp(grade_amt2.w, 0.0, 1.0))
      ambient *= mix(1.0, 0.80, hut_px);

      // ---- 2. somme LINEAIRE des lampes ----
      int n = int(night_fx.w);
      vec3 L_sum = vec3(0.0);
      // 26/08 : depot ADDITIF des lumieres d'eco — le multiplicatif seul ne
      // peut pas bleuir un sable jaune (albedo bleu quasi nul) : la couleur
      // de l'eco se depose aussi directement sur la surface.
      vec3 L_eco_add = vec3(0.0);
      if (n > 0) {
        // PERF 14/10 : seules les lampes de la tuile (original : toutes, i = 0..n-1)
        uvec2 tmask = pc_tile_mask();
        for (int it = 0; it < MAX_LAMPS; it++) {
          int i = pc_next_lamp(tmask);
          if (i < 0 || i >= n) {
            break;
          }
          // PERF 25/08 : test de portee AU CARRE d'abord — la majorite des 48
          // lampes sort ici sans payer decodage ni sin (image identique :
          // l'ancien test dist >= r faisait le meme continue, juste apres)
          vec3 dv = lamp_pos[i].xyz - P;
          float d2 = dot(dv, dv);
          float reach_mult = (lamp_color[i].w < 0.0) ? 1.0 : max(night_fx.y, 0.05);
          // 12/10 : dans une HUTTE, un lampion eclaire SON coin (portee propre,
          // 12 m) au lieu de baigner toute la piece (x2.6 = 31 m : la hutte de
          // Samos devenait un aplat orange) — des flaques de lumiere, du contraste
          if (lamp_color[i].w >= 0.0) {
            // 26/10 : par pixel (original : clamp(grade_amt2.w, 0.0, 1.0))
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
            // luminaires du monde : lampes, lueurs precurseurs, cristaux
            inten = -inten - 64.0;
            // (original : presence = mix(0.30, 1.0, night);) — interieur = pleine force
            presence = mix(0.30, 1.0, lamp_hour);
          } else if (world_emissive) {
            // VRAI FEU (lave, braises) : le soleil ne l'eteint pas ; il respire
            inten = (-inten - 16.0) * 1.35;
            // CREPITEMENT (25/08, torches qui crépitent) :
            // trois fréquences non harmoniques + de brefs sursauts quand les
            // phases s'alignent — un vrai feu, plus une respiration régulière.
            {
              float fw = water_glitter.w;
              float ph = lamp_orig[i] * 7.0;  // (original : float(i) * 7.0)
              float fl = 0.84 + 0.09 * sin(fw * 2.3 + ph) + 0.06 * sin(fw * 7.1 + ph * 1.7) +
                         0.04 * sin(fw * 13.7 + ph * 2.3);
              fl += 0.12 * max(0.0, sin(fw * 5.3 + ph) * sin(fw * 8.9 + ph * 0.6) - 0.55);
              inten *= fl;
            }
            presence = 1.0;
          } else if (inten < 0.0) {
            // ecos ramassables : leur couleur PLEINE, jour et nuit
            // 26/08 (boules eteintes de
            // JOUR) : presence diurne 0.55 -> 0.85 — l'eco est de l'energie,
            // le soleil ne l'eteint pas
            is_eco = true;
            inten = -inten;
            // 26/08 A/B decisif (aucun effet visible) : les lampadaires
            // recoivent x night_fx.z (lamp-power, ~2.5) et pas les ecos — la
            // branche eco eclairait 2,5x moins a reglage egal. On aligne :
            // l'eco est une lampe comme les autres (plancher 1.2 pour rester
            // visible meme si lamp-power tombe de jour).
            // 26/08 v2 (trop blanc et trop fort : adouci) : la part
            // MULTIPLICATIVE reduite (elle blanchissait le centre en cumulant
            // avec le depot colore) — la couleur vient du depot, la clarte du
            // multiplicatif, chacun sa dose.
            inten *= max(night_fx.z, 1.2) * 0.45;
            // (original : presence = mix(0.85, 1.0, night);)
            presence = mix(0.85, 1.0, lamp_hour);
          } else {
            // lampadaires classiques (GOAL) : creatures de la nuit
            // 12/10 : ... et des INTERIEURS (lampions de la hutte de Samos en plein jour)
            // (original : presence = night;)
            presence = lamp_hour;
            inten *= max(night_fx.z, 0.0);
          }
          if (presence * inten < 0.001) {
            continue;
          }
          float dist = sqrt(d2);
          vec3 L = dv / max(dist, 1e-4);
          // fenetre douce sur l'inverse-carre : valeur ET pente nulles au bord
          float wnd = clamp(1.0 - (d2 * d2) / (r2 * r2), 0.0, 1.0);
          // 12/10 : lampion de hutte = flaque plus serree (12 -> 20)
          float fall = (wnd * wnd) / (1.0 + (world_emissive ? 6.0 : (is_eco ? 8.0 :
                       mix(12.0, 20.0, clamp(grade_amt2.w, 0.0, 1.0)))) * d2 / r2);
          // terminateur doux (demi-Lambert au carre) + enveloppement par famille
          float ndl = dot(N, L) * 0.5 + 0.5;
          ndl = ndl * ndl;
          // 26/08 : l'eco est une source VOLUMIQUE (une sphere d'energie) —
          // elle enveloppe comme le feu, pas comme un lampadaire
          float wrap = is_eco ? 0.55 : (em_fixture ? 0.14 : (world_emissive ? 0.45 : 0.18));
          float diff = mix(ndl, 1.0, wrap);
          diff = mix(diff, 1.0, near_flat);  // champ proche : la lampe eclaire sans facetter
          // silhouettes : on aplatit, on ne coupe jamais (une coupe = lisere)
          diff = mix(diff, 0.55, edge_amt * 0.6);
          float lamp_contrib = inten * presence * fall * diff;
          L_sum += lamp_color[i].rgb * lamp_contrib;
          if (is_eco) {
            L_eco_add += lamp_color[i].rgb * lamp_contrib;
          }
          // reflet speculaire discret (depend de N : pas pour le feu-surface)
          // metal et verre : reflet de la lampe net et fort (une lampe se mire dans le metal)
          // (original : pow(max(dot(N, H), 0.0), 24.0) * fall * 0.35 * ...)
          if (!world_emissive) {
            vec3 H = normalize(L + V);
            float miroir_l = max(metal_px, verre_px) * reflet_fx.x;
            float sp_pw = mix(24.0, mix(90.0, 260.0, max(metal_poli, verre_px)), min(miroir_l, 1.0));
            float sp_k = 0.35 * mix(1.0, 2.0 + sp_pw / 70.0, min(miroir_l, 1.0));
            L_sum += lamp_color[i].rgb * pow(max(dot(N, H), 0.0), sp_pw) * fall * sp_k *
                     presence * (1.0 - near_flat);
          }
        }
      }

      // ---- 2b. 20/10 : les FAISCEAUX D'ECO eclairent (la nuit) ----
      // Lumiere en tube : distance du pixel au segment, portee ~32 m, bleu eco. Eclaire tours,
      // canopee et sol le long du rayon, comme une vraie source continue.
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

      // ---- 3. application unique + genou doux anti-clip ----
      vec3 lighting = ambient + L_sum;
      // 21/09 : une surface emissive garde sa propre clarte (ni nuit, ni lampes)
      lighting = mix(lighting, vec3(1.0), emissive_px);
      L_eco_add *= (1.0 - emissive_px);
      float ll = luma(lighting);
      if (ll > 1.0) {
        // seule l'EXCEDENT au-dela du plein jour est compresse : sous une
        // lampe on retrouve le plein jour, on ne crame jamais au blanc
        lighting *= (1.0 + (ll - 1.0) / (1.0 + 0.85 * (ll - 1.0))) / ll;
      }
      c *= lighting;
      // depot colore des ecos, avec epaule anti-cramage
      // naturel : 12 % de blanc dans le depot (une vraie lumiere coloree n'est
      // jamais saturee pure) et dose 0.30
      vec3 eco_dep = mix(L_eco_add, vec3(luma(L_eco_add)) * 1.4, 0.12);
      c += (eco_dep / (1.0 + 0.6 * luma(eco_dep))) * 0.30;
    }

    // ---- eau : reflets du soleil (jour) / de la lune (nuit) + étincelles ----
    // v2 : la détection par profondeur créait une GRILLE
    // (les tuiles d'eau proches, transparentes, n'écrivent pas de profondeur).
    // On calcule désormais le point où le REGARD TRAVERSE le plan de la mer —
    // uniforme sur toutes les tuiles ; et les étincelles sont plus grosses,
    // plus rares et plus douces (fini le grain).
    if (water_glitter.y > 0.001 || water_glitter.z > 0.001) {
      float cy = cam_trans.y;
      float dy_ray = P.y - cy;
      float t_surf = (abs(dy_ray) > 1.0) ? (water_glitter.x - cy) / dy_ray : -1.0;
      // 25/10 : la ou la carte du rivage dit TERRE ou EAU INTERIEURE, le plan de la mer n'est pas
      // la mer : interieur de la tour du temple de la jungle (50 m sous la mer), rivieres — brume,
      // reflets, houle et ecume y etaient peints sur une mer qui n'existe pas. Hors carte : inchange.
      bool mer_ici = true;
      if (t_surf > 0.0 && t_surf < 1.04) {
        mer_ici = riv_dist(mix(cam_trans.xyz, P, min(t_surf, 1.0)).xz / 4096.0) > -0.5;
      } else if (cy < water_glitter.x - 4096.0 * 3.5) {
        mer_ici = riv_dist(cam_trans.xz / 4096.0) > -0.5;
      }
      // ---- BRUME SOUS-MARINE v3 (24/08) : par COLONNE D'EAU TRAVERSEE ----
      // L'ancienne brume se dosait par la profondeur du PIXEL sous la mer :
      // vu du dessus, le lagon entier devenait un aplat bleu opaque qui
      // cachait fonds ET orbes precurseurs (le bleu de l'eau cachait tout).
      // Desormais seul compte le trajet du regard SOUS l'eau : 1 m au-dessus
      // d'un orbe = voile leger, le large profond = bleu plein (les fonds nus
      // de 2001 restent caches).
      // PERF 25/08 : identique a view_dist (deja calcule) — un sqrt de moins
      float pix_dist = view_dist;
      float col_m = 0.0;
      // 25/08 (eau qui clignote / disparait) : le plan de mer publie 0 alors
      // que la ligne d'eau reelle est ~2,7 m plus bas — en jouant pres du bord,
      // la camera franchissait le faux plan et la brume basculait mode immerge
      // <-> mode surface d'une frame a l'autre. Le mode immerge ne s'arme que
      // 3,5 m sous le plan (une vraie plongee), plus jamais en surface.
      // (25/10 original : if (cy < water_glitter.x - 4096.0 * 3.5) { ... } else if (t_surf > 0.0
      //  && t_surf < 1.04 && water_murk_ocean < 0.5) { ... } ; idem pour la surface plus bas)
      if (cy < water_glitter.x - 4096.0 * 3.5 && mer_ici) {
        // camera immergee : du regard jusqu'au pixel (ou a la sortie de l'eau)
        float t_exit = (t_surf > 0.0 && t_surf < 1.0) ? t_surf : 1.0;
        col_m = pix_dist * t_exit / 4096.0;
      } else if (t_surf > 0.0 && t_surf < 1.04 && water_murk_ocean < 0.5 && mer_ici) {
        // camera en l'air : seul le segment sous la surface compte
        // (17/10 : sauf si la surface de l'eau a deja pose la brume ;
        //  original : } else if (t_surf > 0.0 && t_surf < 1.04) {)
        col_m = pix_dist * max(0.0, 1.0 - min(t_surf, 1.0)) / 4096.0;
      }
      // 17/10 (v0.0.3) : couleurs passees du bleu roi au TURQUOISE des artistes (mesure de la
      // surface d'origine : ~0.15 0.47 0.51 en plein jour). La brume reste appliquee a toutes
      // les distances : elle cache le fond marin et le vide au-dela (essai de fondu au loin
      // retire — il montrait le fond, son bord et du noir a l'horizon).
      // (original : murk_shallow = mix(vec3(0.05, 0.18, 0.38), vec3(0.12, 0.48, 0.88), day_w);
      //             murk_deep = mix(vec3(0.015, 0.07, 0.18), vec3(0.03, 0.22, 0.55), day_w);)
      if (col_m > 0.02) {
        c = mix(c, sea_murk(col_m), clamp(1.0 - exp(-col_m * 0.50), 0.0, 0.94));
      }
      // le rayon traverse la surface entre la caméra et la géométrie touchée
      // (petite marge au-delà de 1.0 : la surface au ras de la géométrie)
      if (t_surf > 0.0 && t_surf < 1.04 && mer_ici) {
        vec3 Psurf = mix(cam_trans.xyz, P, min(t_surf, 1.0));
        float t = water_glitter.w;
        vec2 wuv = Psurf.xz * (0.55 / 4096.0);
        float n1 = sin(dot(wuv, vec2(3.1, 1.7)) + t * 1.6);
        float n2 = sin(dot(wuv, vec2(-1.3, 3.7)) + t * 2.2 + 1.3);
        float n3 = sin(dot(wuv, vec2(5.3, -2.9)) - t * 1.1);
        vec3 Nw = normalize(vec3(n1 + 0.6 * n3, 16.0, n2 - 0.6 * n3));
        float day = clamp(1.0 - night_fx.x, 0.0, 1.0);
        // (la colonne de lumière et le voile de ciel ont été retirés — la mer
        // bleue validée est la base, seuls les reflets d'objets s'y posent)
        // 2. VRAIE RÉFLEXION (v6, même principe que le système d'ombres) :
        //    lancer de rayon en espace écran contre la
        //    profondeur — falaises, palmiers et huttes se renversent RÉELLEMENT
        //    dans l'eau ; le ciel n'est que le repli quand le rayon sort de
        //    l'écran. Mêmes idiomes que contact_shadow (clip_from_world relatif
        //    caméra, scene_depth, world_from_depth).
        vec3 sky_day = vec3(0.55, 0.72, 1.00);
        vec3 sky_night = vec3(0.10, 0.16, 0.34);
        vec3 sky_ref = mix(sky_night, sky_day, day);
        vec3 I = normalize(Psurf - cam_trans.xyz);
        // miroir presque plan (0.14) : les traits blanchâtres venaient de
        // l'ondulation du reflet du ciel
        // PC-REMAKE : la houle deforme aussi le miroir (reflets qui ondulent avec les vagues)
        // (original : vec3 Nr = normalize(vec3(Nw.x * 0.14, 1.0, Nw.z * 0.14));)
        float surf_m = length(Psurf - cam_trans.xyz) / 4096.0;
        vec2 hp = houle_pente(Psurf.xz / 4096.0, t, surf_m * 0.0011 / max(abs(I.y), 0.04));
        // 25/10 : ONDES AUTOUR DE JAK (emetteurs : OpenGLRenderer.cpp, 12 au plus). Des anneaux
        // partent de lui et s'elargissent (0,9 m/s) : ils deforment la surface (reflets, houle)
        // et portent une fine ecume sur leur crete.
        float ondes_ec = 0.0;
        if (surf_m < 40.0) {
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
        // perf (saccades) : 10 pas au lieu de 22, et marche seulement
        // quand la surface est assez proche pour que le miroir montre la scène
        float surf_dist = length(Psurf - cam_trans.xyz);
        if (Rdir.y > 0.02 && surf_dist < 4096.0 * 70.0) {
          float rstep = 4096.0 * 2.6;
          for (int i = 1; i <= 10; i++) {
            float rt = rstep * float(i) * (1.0 + 0.22 * float(i));
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
              continue;  // ciel : rien à toucher
            }
            float scene_dist = length(world_from_depth(ruv, sd) - cam_trans.xyz);
            float ray_dist = length(Pw - cam_trans.xyz);
            float behind = ray_dist - scene_dist;
            if (behind > 4096.0 * 0.10 && behind < 4096.0 * 20.0) {
              // 22/09 : la copie sans HUD — sinon les informations a l'ecran
              // se refletent dans l'eau
              refl_col = (has_scene_tex > 0.5) ? texture(scene_tex, ruv).rgb : texture(tex_T0, ruv).rgb;
              // 25/10 : le reflet s'efface en approchant du bord de l'ecran et en fin de course
              // du rayon (avant : coupe net la ou l'objet reflete sortait de l'image)
              // (original : hit_w = 1.0;)
              vec2 bord = min(ruv, 1.0 - ruv);
              hit_w = smoothstep(0.0, 0.10, bord.x) * smoothstep(0.0, 0.10, bord.y) *
                      (1.0 - smoothstep(8.0, 10.5, float(i)));
              break;
            }
          }
        }
        // PHILOSOPHIE FINALE : la mer
        // bleue cartoon VALIDÉE reste la base intégrale — AUCUN voile de ciel
        // (c'était lui, le blanchâtre strié), AUCUNE colonne. Seuls les OBJETS
        // réellement touchés par le rayon se reflètent, discrètement plus forts
        // au rasant. Rien touché = la mer garde sa couleur, point.
        float fresR = 0.35 + 0.65 * pow(1.0 - clamp(dot(-I, Nr), 0.0, 1.0), 2.5);
        float strength = mix(water_glitter.z * 0.7, water_glitter.y, day);
        float shore = smoothstep(0.0, 0.06, t_surf);
        // 24/08 : composition VALIDEE restauree a l'identique — aucun dosage
        // par profondeur ici (il eteignait le miroir au pied des falaises, la
        // ou la roche immergee est juste sous la surface). La visibilite des
        // orbes est assuree par la brume-colonne seule.
        // (25/10 original : if (hit_w > 0.5) { ... clamp(fresR * 0.55 * strength * shore, 0.0, 0.65)
        //  ... })
        if (hit_w > 0.001) {
          c = mix(c, refl_col, clamp(fresR * 0.55 * strength * shore, 0.0, 0.65) * hit_w);
        }
        // PC-REMAKE : HOULE (eclairage de la surface et reflets du soleil), s'efface au loin
        // hauts-fonds (fond clair a moins de 2 m) : la houle y est plus douce ; au ras du bord
        // (moins de 15 cm d'eau) elle s'efface
        float prof_m = max(water_glitter.x - P.y, 0.0) / 4096.0;
        c = houle_surface(c, Nh, I, day, (1.0 - smoothstep(120.0, 260.0, surf_m)) *
                                             mix(0.35, 1.0, smoothstep(0.5, 2.5, prof_m)) *
                                             smoothstep(0.0, 0.15, prof_m));
        // 25/10 : ecume fine sur la crete des ondes autour de Jak
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

  // REFLETS DU METAL ET DU VERRE. Les textures de metal (bronze precurseur, or, laiton, acier,
  // toles, tuyaux, armures) et de verre (vitres, lentilles) portent un drapeau dans le G-buffer
  // (pc_reflect_texture_kind). Comme pour la mer : lancer de rayon en espace ecran contre la
  // profondeur — le decor visible (et Jak) se reflete vraiment ; hors de l'ecran, le ciel tel
  // qu'il est affiche, sinon ciel et sol avec un horizon net (comme les orbes). Le metal teinte
  // son reflet de sa propre couleur et garde sa texture lisible ; le verre reflete surtout en
  // incidence rasante. Plus l'eclat du soleil, la ou il eclaire. Reflets dans un recoin, de nuit
  // et a l'interieur (grottes, cabane) : attenues.
  if (!sky_pixel && (metal_px > 0.001 || verre_px > 0.001) && reflet_fx.x > 0.001 &&
      view_dist < 4096.0 * 150.0) {
    const float M = 4096.0;
    if (reflet_fx.y > 0.5) {
      vec3 qa = verre_px > 0.5 ? vec3(0.1, 0.9, 1.0)
                               : mix(vec3(1.0, 0.45, 0.0), vec3(1.0, 1.0, 0.1), metal_poli);
      c = mix(c, qa * max(metal_px, verre_px), 0.75);
    } else {
      vec3 I = normalize(P - cam_trans.xyz);
      float nv = clamp(dot(-I, N), 0.0, 1.0);
      vec3 R = reflect(I, N);
      float jour = clamp(1.0 - night_fx.x, 0.0, 1.0);
      float interieur = clamp(clamp(ground_color.w - grade_amt2.w, 0.0, 1.0) + hut_px, 0.0, 1.0);
      float loin = 1.0 - smoothstep(90.0 * M, 150.0 * M, view_dist);

      // 1. ENVIRONNEMENT (repli). Le ciel affiche a l'ecran dans la direction R, s'il y est ;
      //    sinon ciel clair au-dessus de l'horizon, sol sombre dessous, horizon net.
      float h = R.y;
      // (le sol reflete est nettement plus sombre que le ciel : c'est ce contraste, et l'horizon
      // net entre les deux, qui fait lire une surface comme du metal poli)
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
      // a l'interieur, pas de ciel : la lumiere ambiante du lieu (un peu plus claire en haut)
      env = mix(env, mix(ground_color.rgb * 0.16, sky_color.rgb * 0.34, smoothstep(-0.6, 0.6, h)),
                interieur);
      // l'entourage reellement vu par la camera dans la direction R (panorama rempli image par
      // image) : ce qui est derriere la camera se reflete aussi ; flou selon le poli
      if (has_reflet_env > 0.5) {
        vec2 euv = vec2(atan(R.x, R.z) * (0.5 / 3.14159265) + 0.5,
                        asin(clamp(R.y, -1.0, 1.0)) / 3.14159265 + 0.5);
        float lod = verre_px > 0.5 ? 0.8 : mix(2.0, 1.3, metal_poli);
        vec4 e = textureLod(reflet_env_tex, euv, lod);
        if (e.a > 0.02) {
          env = mix(env, e.rgb / e.a, smoothstep(0.05, 0.45, e.a));
        }
      }
      // recoin : le ciel n'arrive pas jusque-la
      env *= 1.0 - 0.75 * ao_px;

      // 2. VRAI REFLET : marche du rayon reflechi contre la profondeur, affinee par dichotomie
      vec3 refl = env;
      // un rayon qui revient vers la camera part vers ce que l'ecran ne montre pas : le reflet
      // de l'ecran s'efface, l'environnement prend le relais (sinon : trainees et fantomes)
      float vers_scene = smoothstep(-0.10, 0.25, dot(R, I));
      if (view_dist < 90.0 * M && vers_scene > 0.001) {
        vec3 P0 = P + N * (0.03 * M + view_dist * 0.003);
        float pas = max(0.12 * M, view_dist * 0.03);
        float t_prev = 0.0;
        for (int i = 1; i <= 16; i++) {
          float rt = pas * float(i) * (1.0 + 0.2 * float(i));
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
            for (int j = 0; j < 4; j++) {
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
            // metal ordinaire : reflet un peu flou (surface moins lisse)
            float fl = (1.0 - metal_poli) * (1.0 - verre_px) * 0.0025;
            if (has_scene_tex > 0.5) {
              hit = texture(scene_tex, huv).rgb;
              if (fl > 1e-5) {
                hit = (hit * 2.0 + texture(scene_tex, huv + vec2(fl, 0.0)).rgb +
                       texture(scene_tex, huv - vec2(fl, 0.0)).rgb +
                       texture(scene_tex, huv + vec2(0.0, fl)).rgb +
                       texture(scene_tex, huv - vec2(0.0, fl)).rgb) / 6.0;
              }
            } else {
              hit = texture(tex_T0, huv).rgb;
            }
            vec2 bord = min(huv, 1.0 - huv);
            float hw = smoothstep(0.0, 0.08, bord.x) * smoothstep(0.0, 0.08, bord.y) *
                       (1.0 - smoothstep(12.0, 16.5, float(i))) * vers_scene;
            // surface touchee par l'envers (le rayon passe derriere un objet) : refusee
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
      // 3. METAL : le reflet prend la couleur du metal (bronze, or, acier), la couleur propre
      //    s'efface un peu devant lui (un metal renvoie peu de lumiere diffuse)
      if (metal_px > 0.001) {
        float cm = max(c.r, max(c.g, c.b));
        vec3 teinte = cm > 1e-3 ? c / cm : vec3(1.0);
        teinte = mix(vec3(1.0), teinte, 0.85);
        float dose = metal_px * reflet_fx.x * loin;
        float F0 = mix(0.55, 0.75, metal_poli);
        float k = (F0 + (1.0 - F0) * fres) * dose;
        // le metal MULTIPLIE ce qu'il reflete par sa propre couleur : sa texture reste entiere
        // (motifs, usure, parties sombres et claires) et le reflet module sa clarte — plus clair
        // face au ciel, plus sombre face au sol, l'horizon net entre les deux ; un ciel bleu
        // devient dore sur le bronze (reflet a demi desature). Un peu de reflet neutre en plus
        // pour que le metal sombre brille aussi.
        // (v1 : couleur propre x(1 - 0,58) + reflet teinte ajoute — delavait le bronze sombre)
        // La clarte est prise RELATIVEMENT a la clarte moyenne de l'entourage (dernier niveau du
        // panorama) : en moyenne le metal garde sa clarte d'origine, de jour comme dans une
        // grotte ; le reflet n'y ajoute que du contraste. Couleur du reflet au quart (le bleu du
        // ciel sur un metal jaune virait au vert).
        // (v1 : c *= (1 - 0,58 dose) + reflet adouci * k — assombrissait le metal des interieurs)
        float moy = 0.45;
        if (has_reflet_env > 0.5) {
          vec4 em = textureLod(reflet_env_tex, vec2(0.5), 10.0);
          if (em.a > 0.05) {
            moy = luma(em.rgb / em.a);
          }
        }
        float lr_ = luma(refl);
        float rapport = lr_ / max(moy, 0.03);
        float rs = 2.0 * rapport / (1.0 + rapport);
        vec3 refl_n = refl / max(lr_, 1e-3);
        c *= mix(vec3(1.0), mix(vec3(1.0), refl_n, 0.25) * rs, k * 0.85);
        c += refl * teinte * (k * 0.06);
        // lustre large du soleil sur le metal poli (les surfaces courbes prennent un degrade)
        if (sun_dir.w > 0.5) {
          float lr = max(dot(R, sun_dir.xyz), 0.0);
          c += teinte * vec3(1.0, 0.95, 0.85) * (pow(lr, 12.0) * 0.30 * jour * (1.0 - interieur)) *
               metal_poli * dose * (1.0 - 0.75 * ao_px);
        }
      }
      // 4. VERRE : peu de reflet de face, presque un miroir au rasant (Fresnel)
      if (verre_px > 0.001) {
        float k = (0.14 + 0.86 * fres) * verre_px * reflet_fx.x * loin;
        c = mix(c, refl, clamp(k, 0.0, 0.85));
      }
      // 5. ECLAT DU SOLEIL : petit et intense sur le poli et le verre, plus large sur le metal
      //    ordinaire ; seulement la ou le soleil eclaire (carte d'ombre)
      if (sun_dir.w > 0.5 && jour > 0.01) {
        float rl = max(dot(R, sun_dir.xyz), 0.0);
        if (rl > 0.9) {
          float soleil = 1.0;
          vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
          if (sp.x > 0.01 && sp.x < 0.99 && sp.y > 0.01 && sp.y < 0.99 && sp.z < 1.0) {
            soleil = texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
          }
          float pw = verre_px > 0.5 ? 900.0 : mix(140.0, 650.0, metal_poli);
          float eclat = pow(rl, pw) * (0.4 + pw / 260.0);
          c += vec3(1.0, 0.95, 0.85) * eclat * soleil * jour * (1.0 - interieur) *
               max(metal_px, verre_px) * reflet_fx.x * loin;
        }
      }
    }
  }

  // 17/10 : LA MER JUSQU'A L'HORIZON. Au-dela du fond marin il n'y a plus de geometrie : la
  // surface de la mer (transparente, sans profondeur) se pose sur le CIEL et ces pixels
  // echappaient a la brume ci-dessus — une ligne nette la ou s'arretait le fond, puis une
  // bande plus claire jusqu'a l'horizon. Ils recoivent la brume d'une eau tres profonde :
  // une seule mer, continue jusqu'a l'horizon.
  // (17/10 : inutile quand la surface pose la brume elle-meme — le vide derriere l'eau y
  //  compte comme une eau tres profonde ; original sans la condition water_murk_ocean)
  // PC-REMAKE : la houle aussi sur la mer au-dela du fond (pas de geometrie derriere la surface)
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
    // direction du regard (profondeur inversee : 1 = proche, 0 = loin)
    vec3 Dv = world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99);
    if (Dv.y < 0.0) {
      c = mix(c, sea_murk(100.0), 0.94);
    }
  }

  // 20/10 : LA NUIT, LES LUMIERES BRILLENT. (1) Surfaces emissives (anneaux bleus des
  // tours de la jungle, lueurs precurseur, cristaux) : eclat renforce avec la nuit.
  // (2) Particules qui AJOUTENT une lumiere COLOREE (faisceau d'eco, ecos, etincelles) :
  // leur apport (scene avant HUD - scene avant sprites) est redonne, renforce ; la fumee
  // (qui assombrit) et la vapeur blanche (peu saturee) n'en recoivent pas.
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
        // v2 : coeur plus clair (une lumiere vive tire vers le blanc), dose relevee
        // (v1 : c += add * 1.2 * nb * smoothstep(0.25, 0.60, asat);)
        float gp = nb * smoothstep(0.25, 0.60, asat);
        c += (add * 1.6 + vec3(luma(add)) * 0.9) * gp;
      }
    }
  }

  // 20/10 : TRACES DANS LA NEIGE (neige qui se tasse). Quand le sol de neige est vraiment
  // creuse (snow_geom : geometrie decoupee, tfrag3_snow.tese), le post ne fait plus que la neige
  // tassee du fond (plus dense : plus sombre, bleutee) et l'ombre du bord dans le creux. Sinon
  // (carte graphique sans decoupage), parallaxe : le regard descend dans le sillon et la normale
  // suit la pente. Les traces se comblent en 2 a 4 minutes.
  // (v2 : relief pre-calcule dans la carte, bord en dents de scie, parallaxe seule)
  if (!sky_pixel && snow_trail.x > 0.5 && N.y > (snow_geom > 0.5 ? 0.0 : 0.45)) {
    const float SM = 4096.0;
    vec3 pm = P / SM;
    if (length(pm.xz - snow_trail.yz / SM) < 49.0) {
      float now = snow_trail.w;
      vec3 q = pm;
      float py0 = pm.y;  // hauteur de la surface d'origine (avant tassement)
      float h;
      if (snow_geom > 0.5) {
        // P est deja au fond du creux : surface d'origine = P.y - relief (2 iterations)
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
          // normale du relief au point vu (la geometrie decoupee l'a deja dans le G-buffer)
          const float e = 0.06;
          vec3 qb = vec3(q.x, pm.y, q.z);
          float hx = snow_relief(qb + vec3(e, 0.0, 0.0), now) - snow_relief(qb - vec3(e, 0.0, 0.0), now);
          float hz = snow_relief(qb + vec3(0.0, 0.0, e), now) - snow_relief(qb - vec3(0.0, 0.0, e), now);
          vec3 tn = normalize(N + vec3(-hx, 0.0, -hz) / (2.0 * e));
          float l0 = max(dot(N, Ls), 0.0) * 0.75 + 0.25;
          float l1 = max(dot(tn, Ls), 0.0) * 0.75 + 0.25;
          lit = clamp(l1 / max(l0, 0.05), 0.45, 1.4);
        }
        // ombre du bord dans le creux : du point vu vers le soleil, le relief cache-t-il ?
        float shadow = 0.0;
        if (h < -0.01) {
          vec3 sp = vec3(q.x, py0 + h, q.z);
          for (int k = 1; k <= 8; k++) {
            vec3 s2 = sp + Ls * (max(0.08, -h / 5.0) * float(k));  // pas allonge si creux profond
            // ombre douce : penetration du rayon dans le relief (pas de bandes)
            float pen = snow_relief(vec3(s2.x, py0, s2.z), now) - (s2.y - py0);
            shadow = max(shadow, clamp(pen / 0.06, 0.0, 1.0) * (1.0 - float(k - 1) / 8.0));
          }
        }
        // 22/10 : neige creusee BLANCHE — ombre du bord et teinte de neige tassee adoucies
        // (original : mix(1.0, 0.62, shadow) et vec3(0.74, 0.82, 0.95))
        c *= lit * mix(1.0, 0.78, shadow);
        // neige tassee (a peine plus dense : legerement bleutee) ; bourrelet de poudreuse plus clair
        float trough = clamp(-h / 0.35, 0.0, 1.0);
        float bump = clamp(h / 0.08, 0.0, 1.0);
        c *= mix(vec3(1.0), vec3(0.90, 0.94, 0.99), trough);
        c *= 1.0 + 0.10 * bump;
      }
    }
  }

  // 20/10 : RAYONS DE SOLEIL A TRAVERS LE FEUILLAGE (jungle, de jour). v3 : calcules au QUART
  // de la resolution puis adoucis (shafts.frag + shafts_blur.frag, OpenGLRenderer
  // pc_shaft_run) — la v2, calculee ici pixel par pixel (16 pas avec gigue), donnait une image
  // granuleuse. Ici : la dose adoucie, avec une epaule (face au soleil, pas de voile).
  if (shaft_amt > 0.001 && has_shaft_tex > 0.5) {
    float sadd = texture(shaft_tex, tex_coord).r;
    c += vec3(1.0, 0.90, 0.68) * (sadd / (1.0 + 2.0 * sadd)) * shaft_amt;
  }
  // 22/10 : FAISCEAUX DE CANOPEE (jungle, de jour ; voir shafts.frag). L'air des colonnes brille
  // (canal vert, adouci) et ce qu'une colonne touche (sol, rocher, tronc, Jak) recoit sa tache
  // de lumiere chaude, a l'ombre du feuillage seulement (en plein soleil, rien ne change).
  if (shaft_amt > 0.001 && has_shaft_tex > 0.5 && grade_amt2.w < 0.01) {
    float cba = texture(shaft_tex, tex_coord).g;
    c += vec3(1.0, 0.86, 0.56) * (cba / (1.0 + 1.2 * cba)) * shaft_amt * 0.40;
    // poussieres en suspension dans les colonnes : grains de 1 a 2 pixels qui derivent et
    // scintillent. Grille 3D de 0,4 m ancree au monde, parcourue le long du regard dans la
    // traversee de chaque colonne (seulement la ou un faisceau brille deja).
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
        // la tache s'etale un peu plus que la colonne (lumiere diffusee), bord doux, et
        // tachetee par l'ombre des feuilles
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
  // 20/10 : TACHES DE SOLEIL dans la cabane de Samos (toit ouvert) : la ou la carte d'ombres
  // dit que le soleil arrive (par le toit, la porte, la fenetre), le sol et les murs recoivent
  // une lumiere chaude. Ailleurs dans la piece, rien ne change (pas d'ombres portees dedans).
  // 26/10 : par pixel — le dehors vu par l'ouverture ne prend plus ces taches (il s'allumait
  // en entier : tout y est au soleil) (original : ... && grade_amt2.w > 0.01))
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
      // tache franche : le soleil qui entre par les ouvertures du toit et la fenetre
      // (original : * clamp(grade_amt2.w, 0.0, 1.0))
      c += c * vec3(1.0, 0.84, 0.56) * (slitp * sndl * 3.2 * hut_px);
    }
  }

  // 20/10 : FAISCEAU DE LUMIERE MIS EN SCENE dans la cabane de Samos (etage). Le toit est ferme
  // dans le modele : le vrai soleil n'y entre presque pas. Un cone de lumiere chaude descend du
  // haut de la coupole vers le plancher (effet artistique, de jour) : l'air du cone brille, avec
  // un leger mouvement, et ce qu'il touche (plancher, Jak) recoit sa lumiere.
  {
    float hb = clamp(grade_amt2.w, 0.0, 1.0) * clamp(1.0 - 1.4 * night_fx.x, 0.0, 1.0);
    if (hb > 0.001) {
      const float M = 4096.0;
      // 20/10 v3 : le faisceau tombe de l'OUVERTURE DU TOIT au-dessus de l'arbre (bord du trou
      // mesure vers (-123, 60, 205) m) et descend a gauche de l'arbre
      // (v1 : vec3(-126.0, 63.0, 214.0), plafond ; v2 : vec3(-134.6, 50.8, 216.2), fenetre ronde)
      vec3 b0 = vec3(-123.5, 61.5, 204.5) * M;  // ouverture du toit
      vec3 b1 = vec3(-129.0, 46.2, 201.5) * M;  // plancher, a gauche de l'arbre
      vec3 bax = b1 - b0;
      float blen = length(bax);
      vec3 bdir = bax / blen;
      float r0 = 0.75 * M, r1 = 1.5 * M;
      // 1. lumiere RECUE par la surface (dans le cone, et un peu sous la tache)
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
      // poussiere en suspension : bandes douces qui descendent lentement dans le faisceau
      float shimmer = 0.80 + 0.20 * sin(sa * 23.0 + t * 0.9) * sin(sa * 9.0 - t * 0.5 + dist / M * 2.0);
      float glow = core * ends * shimmer * min(cross_len, 1.7);
      c += vec3(1.0, 0.88, 0.62) * (glow * 0.9 * hb);
    }
  }

#ifdef POST_PASS1
  color = vec4(c, sky_pixel ? -1.0 : edge_amt);
  return;
#endif
#endif  // POST_PASS2 (fin de la premiere moitie)

  // ---- the lamps as visible SOURCES ----
  // Lighting the surfaces around a lamp is not enough: the lamp itself has to
  // read as a light from across the village. Each lamp is projected to screen
  // and given a halo whose size falls off with distance but never disappears,
  // so a street lamp still marks its spot from far away. The halo is hidden if
  // something the camera can see stands in front of the lamp.
  {
    int n = int(night_fx.w);
    float night_glow = clamp(night_fx.x, 0.0, 1.0);
    // PERF 25/08 : facteur final des halos classiques — s'il vaut 0 (plein
    // jour), ces lampes n'apportent exactement rien : on les saute d'entree
    // 12/10 : dans une HUTTE les lampions ont leur aura de jour aussi
    // (original : float glow_gate = night_glow * clamp(night_fx.z, 0.0, 2.0);)
    float glow_gate = max(night_glow, clamp(grade_amt2.w, 0.0, 1.0)) * clamp(night_fx.z, 0.0, 2.0);
    if (n > 0) {
      vec2 ts = vec2(textureSize(tex_T0, 0));
      float aspect = ts.x / ts.y;
      vec3 glow = vec3(0.0);
      vec3 glow_em = vec3(0.0);  // auras des émissives (jour et nuit)
      // PERF 14/10 : seules les lampes de la tuile (original : toutes, i = 0..n-1)
      uvec2 hmask = pc_tile_mask();
      for (int it = 0; it < MAX_LAMPS; it++) {
        int i = pc_next_lamp(hmask);
        if (i < 0 || i >= n) {
          break;
        }
        vec3 lp = lamp_pos[i].xyz;
        // PERF 25/08 : famille decodee EN TETE — halo classique nul de jour
        float iw = lamp_color[i].w;
        bool em_h = iw < -8.0;
        bool fix_h = iw < -40.0;
        // 26/08 : les ECOS ont leur propre halo, jour et nuit (les
        // ecos bleus etaient peu visibles) — la boule elle-meme doit briller
        bool eco_h = iw < 0.0 && iw >= -8.0;
        if (!em_h && !eco_h && glow_gate <= 0.0) {
          continue;
        }
        // PERF 14/10 : projection, cadre, rayon et fondu viennent de lamp_scr (C++,
        // une fois par image) — memes formules que le code d'origine garde ci-dessous.
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
        // In this projection w is NEGATIVE in front of the camera. Testing only
        // |w| let lamps BEHIND the camera through: they project mirrored, and
        // their halo appeared as a row of yellow dots floating in the sky.
        // 22/09 : SIGNE CORRIGE. Dans cette matrice (clip_from_world), w est
        // POSITIF devant la camera — c'est ce que font deja le reflet d'eau
        // (rclip.w < 1e-4 -> break) et la projection du soleil (C++, cw > 0).
        // Le test inverse du 25/08 rejetait TOUTES les lampes devant la camera :
        // aucun halo ni fantome de lampe ne se dessinait depuis (les points
        // jaunes dans le ciel d'alors etaient les lampes DERRIERE, projetees en
        // miroir ; avec le bon signe elles sont bien rejetees).
        // (original : if (lclip.w > -1e-5) continue;)
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
        // PC-REMAKE 18/08 : les halos ecran s'EFFACENT avec la distance. Vus
        // depuis la falaise d'en face, les dizaines de lampes du village de
        // pierre se projetaient dans une petite zone de l'image et leurs halos
        // s'additionnaient en une nappe jaune geante (de loin, la portee de
        // lumiere etait beaucoup trop grande). De pres : inchange.
        float halo_fade = 1.0 - smoothstep(4096.0 * 30.0, 4096.0 * 85.0, ldist);
        if (halo_fade < 0.01) {
          continue;
        }
        */
        // décode l'intensité (feu -(i+16), luminaire -(i+64)) — sinon
        // l'encodage ferait des halos x18 ou x66
        float inten_h = fix_h ? (-iw - 64.0) : (em_h ? (-iw - 16.0) : abs(iw));
        // 22/09 (flare aussi sur les lampes la nuit) :
        // deux petits fantomes d'objectif par lampe VISIBLE, sur l'axe lampe ->
        // centre de l'image, de la couleur de la lampe ; discret, la nuit et pour
        // les sources (lave, ecos) ; une seule sonde de profondeur pour l'occlusion.
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
            // PERF 14/10 : hors du disque du fantome la contribution vaut exactement 0
            // (pow(0, 1.5)) — on saute le pow (quasi tous les pixels)
            if (gd < 1.0) {
              gsum += pow(1.0 - gd, 1.5) * ((gi == 0) ? 0.6 : 0.4);
            }
          }
          if (gsum > 0.001) {
            float cd0 = scene_depth(clamp(luv, 0.001, 0.999));
            bool seen = (cd0 >= 1e-5) &&
                        (length(world_from_depth(clamp(luv, 0.001, 0.999), cd0) - cam_trans.xyz) >=
                         ldist - 4096.0 * 1.5);
            if (seen) {
              float ng = fix_h ? mix(0.18, 1.0, night_glow) : 1.0;
              glow_em += lamp_color[i].rgb * gsum * inten_h * ng * halo_fade * fx_quality.w * 2.5;
            }
          }
        }
        // PERF 25/08 : test du DISQUE avant l'occlusion — un halo couvre ~2 %
        // de l'ecran : ~98 % des pixels sortent ici sans payer les 6 taps de
        // profondeur ni les 6 unprojections. Si t == 0, la contribution etait
        // exactement nulle de toute facon (core = halo = 0) : image identique.
        vec2 dd = (tex_coord - luv) * vec2(aspect, 1.0);
        float t = clamp(1.0 - length(dd) / rad, 0.0, 1.0);
        if (t <= 0.0) {
          continue;
        }
        // Soft occlusion: five taps around the lamp instead of one. A lamp
        // hanging under a hut roof is blocked dead centre but still spills
        // light around the edge — a hard test would switch it off entirely.
        // A light you can SEE has to have something rendered where it is — its
        // own fixture. If the sky is showing at the lamp's screen position then
        // nothing is drawn there, and a halo floating in the sky is exactly the
        // artefact that produces. So the centre tap is strict.
        float cd = scene_depth(luv);
        if (cd < 1e-5) {
          continue;
        }
        float d_center = length(world_from_depth(luv, cd) - cam_trans.xyz);
        if (d_center < ldist - 4096.0 * 1.5) {
          continue;  // something clearly stands in front of it
        }
        // the neighbours only soften the edge; sky counts as nothing, not as
        // clear line of sight
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
        if (vis < 0.05) {
          continue;
        }
        float core = pow(t, 3.0);
        float halo = pow(t, 2.0) * 0.12;
        // l'aura d'un luminaire s'efface aussi en plein jour
        if (fix_h) {
          // (original : mix(0.18, 1.0, night)) — 21/09 : pleine aura en interieur
          inten_h *= mix(0.18, 1.0, max(clamp(night_fx.x, 0.0, 1.0), clamp(ground_color.w, 0.0, 1.0)));
        }
        if (eco_h) {
          // coeur eclatant sur la boule + aura de sa couleur, a toute heure
          glow_em += lamp_color[i].rgb * (core * 0.55 + halo * 2.6) * inten_h * vis * halo_fade;
        } else if (em_h) {
          // la lave RESPIRE : battement lent, déphasé par source
          float fw = water_glitter.w;
          float ph = lamp_orig[i] * 7.0;  // (original : float(i) * 7.0)
          float fl = 0.84 + 0.09 * sin(fw * 2.3 + ph) + 0.06 * sin(fw * 7.1 + ph * 1.7) +
                     0.04 * sin(fw * 13.7 + ph * 2.3);
          fl += 0.12 * max(0.0, sin(fw * 5.3 + ph) * sin(fw * 8.9 + ph * 0.6) - 0.55);
          // cœur réduit : l'aura ne doit pas NOYER l'objet source
          glow_em += lamp_color[i].rgb * (core * 0.08 + halo * 2.2) * inten_h * vis * fl * halo_fade;
        } else {
          // cœur écrasé (0.50 → 0.18) : il peignait un disque éblouissant SUR
          // l'objet source — la pile d'énergie illisible
          glow += lamp_color[i].rgb * (core * 0.18 + halo) * inten_h * vis * halo_fade;
        }
      }
      // epaule sur la SOMME : dix halos qui se chevauchent ne doivent pas
      // s'additionner lineairement jusqu'au blanc
      glow = glow / (1.0 + 0.9 * glow);
      glow_em = glow_em / (1.0 + 0.9 * glow_em);
      c += glow * glow_gate * 0.12;
      // les auras émissives vivent jour ET nuit (un peu plus fortes la nuit)
      // (original : (0.7 + 0.5 * night_glow)) — 21/09 : interieur compte comme la nuit
      c += glow_em * 0.10 * (0.7 + 0.5 * max(night_glow, clamp(ground_color.w, 0.0, 1.0)));
    }
  }

  // ---- the sun itself: disc, glare and rays when the camera looks at it ----
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
      // 22/09 : le disque du post ne se dessinait JAMAIS avant (sun_screen faux) ;
      // le jeu a deja son propre soleil (sprite du ciel) — le post n'ajoute
      // qu'un eclat doux par-dessus (original : disc * 2.6 + glare * 0.75)
      c += sun_col * (disc * 0.9 + glare * 0.35) * light_fx.x;
    }
    // warm veiling glare across the whole frame, strongest near the sun
    // (original : 0.16)
    float veil = pow(clamp(1.0 - rs / 1.15, 0.0, 1.0), 2.5);
    c += vec3(1.0, 0.88, 0.66) * veil * 0.08 * light_fx.x * up;
  }

  // ---- god rays: light streaming from the sun's on-screen position ----
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

  // ---- LENS FLARES (22/09, effet léger) ----
  // Reflets d'objectif : quelques fantômes doux alignés sur l'axe soleil -> centre
  // de l'image (de l'autre côté du centre), un fin anneau irisé et une lueur
  // chromatique près du soleil. Tout est additif, faible, et s'éteint quand le
  // soleil est caché (visibilité mesurée dans la profondeur autour du soleil) ou
  // sort du cadre.
  if (fx_quality.w > 0.001 && sun_screen.z > 0.5 && sun_screen.w > 0.02 && light_fx.x > 0.001) {
    vec2 asp = vec2(textureSize(tex_T0, 0));
    float ar = asp.x / asp.y;
    // 1. visibilité du soleil : 8 sondes de profondeur autour de sa position
    float vis = 0.0;
    for (int i = 0; i < 8; i++) {
      // (original : a = 0.785398 * i ; vec2(cos(a) / ar, sin(a)) * 0.018)
      vec2 o = vec2(kOct8[i].x / ar, kOct8[i].y) * 0.018;
      vis += (scene_depth(clamp(sun_screen.xy + o, 0.001, 0.999)) < 1e-5) ? 1.0 : 0.0;
    }
    vis /= 8.0;
    // 2. fondu quand le soleil s'approche du bord / sort du cadre
    vec2 sn = sun_screen.xy * 2.0 - 1.0;
    float in_frame = 1.0 - smoothstep(0.95, 1.12, max(abs(sn.x), abs(sn.y)));
    // (la hauteur du soleil ne module pas l'intensite : un soleil rasant fait
    // autant de reflets qu'un soleil haut — seuil doux juste au-dessus de l'horizon)
    float gain = fx_quality.w * light_fx.x * smoothstep(0.02, 0.12, sun_screen.w) * vis * in_frame;
    if (gain > 0.001) {
      vec2 center = vec2(0.5);
      vec2 axis = (center - sun_screen.xy);  // du soleil vers le centre
      vec2 pix = (tex_coord - center) * vec2(ar, 1.0);
      vec3 flare = vec3(0.0);
      // 3. fantômes : (position le long de l'axe, rayon, couleur, poids)
      for (int i = 0; i < 5; i++) {
        float k; float rad; vec3 col; float w;
        if (i == 0)      { k = 0.35; rad = 0.045; col = vec3(1.00, 0.72, 0.40); w = 0.55; }
        else if (i == 1) { k = 0.62; rad = 0.075; col = vec3(0.55, 0.80, 1.00); w = 0.45; }
        else if (i == 2) { k = 0.95; rad = 0.028; col = vec3(1.00, 0.55, 0.75); w = 0.55; }
        else if (i == 3) { k = 1.30; rad = 0.110; col = vec3(0.60, 0.95, 0.80); w = 0.30; }
        else             { k = 1.70; rad = 0.060; col = vec3(1.00, 0.90, 0.65); w = 0.40; }
        vec2 gp = (center + axis * k - center) * vec2(ar, 1.0);
        float d = length(pix - gp) / rad;
        // disque doux avec un bord un peu plus dense (comme un vrai reflet de lentille)
        float disc = pow(clamp(1.0 - d, 0.0, 1.0), 1.6) * 0.7 + smoothstep(1.0, 0.85, d) * 0.3;
        flare += col * disc * w;
      }
      // 4. anneau irisé (rayon par canal : rouge dehors, bleu dedans)
      {
        vec2 rp = (center + axis * 0.55 - center) * vec2(ar, 1.0);
        float d = length(pix - rp);
        vec3 ring = vec3(smoothstep(0.014, 0.0, abs(d - 0.190)),
                         smoothstep(0.014, 0.0, abs(d - 0.180)),
                         smoothstep(0.014, 0.0, abs(d - 0.170)));
        flare += ring * vec3(1.0, 0.85, 0.9) * 0.22;
      }
      // 5. lueur chromatique douce autour du soleil (halo large, teinté vers les bords)
      {
        vec2 sp = (sun_screen.xy - center) * vec2(ar, 1.0);
        float d = length(pix - sp);
        float halo = pow(clamp(1.0 - d / 0.42, 0.0, 1.0), 3.0);
        flare += vec3(1.0, 0.82, 0.55) * halo * 0.35;
      }
      // dose legere : le fantome le plus fort ajoute ~0,15 a gain 1
      c += flare * gain * 0.50;
    }
  }

  // ---- BLOOM LARGE (12/10, rendu remake) ----
  // Le grand halo doux des sources (portail, lueurs, lave, ecos, lampes) : de la
  // LUMIERE ajoutee apres l'eclairage (elle n'est ni relumee ni assombrie par la
  // nuit), avant le tonemap qui l'adoucit. Dose = curseur Bloom (post_fx.x).
  if (has_bloom_tex > 0.5 && post_fx.x > 0.001) {
    vec3 wide = texture(bloom_tex, tex_coord).rgb;
    // v2 : 0.55 -> 1.0 (a l'etage de Samos le halo restait timide face a la reference)
    // v3 : epaule — une grande surface lumineuse (cascade) ne peut plus voiler
    // tout l'ecran ; une petite source garde son halo
    // (original : c += wide * post_fx.x * 1.0;)
    vec3 wb = wide * post_fx.x;
    c += wb / (1.0 + 1.5 * luma(wb));
  }

  // ---- filmic tonemap (gentle, blended) ----
  if (depth_fx.w > 0.001) {
    vec3 filmic = filmic_aces(c * 1.05);
    // keep blacks from crushing: slight lift
    filmic = filmic * 0.97 + 0.015;
    c = mix(c, filmic, clamp(depth_fx.w, 0.0, 1.0));
  }

  // ---- toon / cel-shading pass ----
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

  // ======================= RENDU REMAKE (12/10) =======================
  // Objectif : un rendu moderne, a partir d'images de reference.
  // Ce qu'on peut viser SANS toucher aux modeles : l'EXPOSITION et la MATIERE de
  // l'image. Mesure des references : luma 0.41-0.51, p95 0.75-0.88 (jamais
  // sature), contraste 0.19, saturation 0.46, ombres chaudes-sombres.
  // Le jeu avant : luma 0.69, p95 0.95 (hautes lumieres ECRASEES a blanc : le sol
  // perdait toute sa matiere), saturation 0.64. D'ou, dans l'ordre :
  //   1. exposition (grade_fx.x), 2. courbe filmique a vraie epaule (les hautes
  //   lumieres se compressent au lieu de clipper), 3. contraste en S autour du
  //   gris moyen (grade_fx.y), 4. saturation (grade_fx.z), 5. teinte ombres
  //   fraiches / hautes lumieres chaudes (grade_fx.w), 6. nettete locale.
  // Tout est neutre a zero et pilote par le menu Remake (RENDU REMAKE).
  if (grade_fx.x > 0.001) {
    float amt = clamp(grade_fx.x, 0.0, 2.0);
    vec3 g = c;
    // NIVEAUX INTERIEURS hors huttes (grottes, cite engloutie, temple, citadelle :
    // ground_color.w sans la part de hutte grade_amt2.w) : scenes deja sombres,
    // l'exposition et le S les assombrissaient de 19 % (cite engloutie mesuree
    // 12/10) — exposition neutre et S a 40 %, leurs lumieres validees restent
    float cave = clamp(ground_color.w - grade_amt2.w, 0.0, 1.0);
    // 26/10 : etalonnage de cabane PAR PIXEL. time-of-day.gc envoie celui de l'exterieur ; ce
    // qui est dans la cabane tend vers exposition 0.95, contraste 1.6, saturation 1.15 (les
    // valeurs de la cabane, avant appliquees a tout l'ecran) ; le dehors vu par l'ouverture
    // garde le sien.
    vec3 g_px = mix(grade_amt2.xyz, vec3(0.95, 1.6, 1.15), hut_px);
    // 1. exposition (legere : la vivacite des couleurs du jeu est gardee)
    // (original : g *= mix(1.0, grade_amt2.x, amt);)
    // (26/10 original : g *= mix(1.0, mix(grade_amt2.x, 1.0, cave), amt);)
    g *= mix(1.0, mix(g_px.x, 1.0, cave), amt);
    // 2. EPAULE DES HAUTES LUMIERES SEULEMENT (v2 : la courbe filmique globale
    //    ternissait tout — ciel gris-cyan, lointain laiteux). On ne touche qu'a ce
    //    qui depasse le genou : le sol en plein soleil (sature a 61 % des pixels)
    //    retrouve sa matiere ; ombres et tons moyens restent ceux des artistes.
    //    Compression sur le MAX des canaux : la teinte est conservee.
    {
      float m = max(g.r, max(g.g, g.b));
      // v3 (12/10, plage) : 0.70 -> 0.80 — le sable en plein soleil restait terne
      // (p95 0.75 contre 0.88 dans la reference) ; le village ne recrame pas
      const float knee = 0.80;
      if (m > knee) {
        float ex = m - knee;
        float room = 1.0 - knee;
        float m2 = knee + room * (ex / (ex + room));
        g *= mix(1.0, m2 / m, amt);
      }
    }
    // 3. contraste doux en S (autour de 0.5 affiche) — ciel a demi-dose : le S et
    //    la nettete y transformaient les nuages basse definition en aplats durs
    {
      // (original : amt * (sky_pixel ? 0.4 : 1.0))
      // (26/10 original : mix(1.0, grade_amt2.y, ...))
      float k = mix(1.0, g_px.y, amt * (sky_pixel ? 0.4 : 1.0) * (1.0 - 0.6 * cave));
      vec3 x = clamp(g, 0.0, 1.0);
      vec3 s_curve = x * x * (3.0 - 2.0 * x);  // smoothstep
      g = mix(g, mix(x, s_curve, clamp(k - 1.0, 0.0, 1.0)), step(0.0, k - 1.0));
    }
    // 3b. OMBRES RELEVEES (12/10, plage) : les falaises a l'ombre tombaient au noir
    //     (p5 0.03 contre 0.21 dans la reference) — un peu de lumiere du ciel,
    //     legerement froide, dans les plus sombres. De jour seulement : les nuits
    //     validees restent intactes.
    if (!sky_pixel) {
      float lu = luma(g);
      float t = 1.0 - smoothstep(0.0, 0.24, lu);
      // ni dans les grottes ni en interieur (pas de ciel : ground_color.w)
      // 26/10 : interieur pris au pixel (original : (1.0 - clamp(ground_color.w, 0.0, 1.0)))
      g += vec3(0.046, 0.050, 0.060) * t * amt * (1.0 - clamp(night_fx.x, 0.0, 1.0)) *
           (1.0 - clamp(cave + hut_px, 0.0, 1.0));
    }
    // 4. saturation
    {
      float lu = luma(g);
      // (26/10 original : mix(1.0, grade_amt2.z, amt))
      g = mix(vec3(lu), g, mix(1.0, g_px.z, amt));
    }
    // 4b. HERBE MOINS FLUO : mesure, l'herbe des references est vert-jaune mais
    //     moderee (saturation 0.59) ; la notre etait a 0.83 (neon). On comprime la
    //     saturation des verts-jaunes au-dela de 0.60, valeur (HSV) conservee ; le
    //     ciel, l'eau, la peau ne sont pas touches.
    {
      float mx = max(g.r, max(g.g, g.b));
      float mn = min(g.r, min(g.g, g.b));
      float sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
      // v3 : vert AU MOINS egal au rouge — le sable (rouge > vert) n'est plus pris
      // pour de l'herbe (il devenait terne et gris)
      // (original : ... * step(g.b, g.g);)
      float yg = smoothstep(0.02, 0.15, min(g.r, g.g) - g.b) * step(g.b, g.g) *
                 smoothstep(0.92, 0.98, g.g / max(g.r, 1e-4));
      // v2 (12/10, mesure sur le sol du village contre la jungle de reference :
      // saturation 0.68 contre 0.58, valeur 0.69 contre 0.57) : compression des
      // 0.50, et les verts-jaunes TRES clairs (plein soleil) perdent jusqu'a 12 %
      // de clarte — l'herbe au soleil n'est plus un aplat citron.
      // (original : if (sat > 0.60 ...) k = (0.60 + (sat - 0.60) * 0.30) / sat;)
      if (sat > 0.50 && yg > 0.0) {
        float k = (0.50 + (sat - 0.50) * 0.35) / sat;
        g = mix(g, mix(vec3(mx), g, k), yg * clamp(amt, 0.0, 1.0));
        g *= mix(1.0, 0.88, yg * smoothstep(0.55, 0.85, mx) * clamp(amt, 0.0, 1.0));
      }
    }
    // 5. etalonnage : hautes lumieres neutres (le jaune venait du soleil chaud x
    //    l'herbe jaune-vert), ombres tres legerement fraiches
    {
      float lu = clamp(luma(g), 0.0, 1.0);
      vec3 shadow_tint = vec3(0.97, 0.99, 1.04);
      vec3 high_tint = vec3(0.99, 1.0, 1.03);
      vec3 tint = mix(shadow_tint, high_tint, smoothstep(0.25, 0.75, lu));
      g *= mix(vec3(1.0), tint, amt * clamp(grade_fx.w, 0.0, 2.0));
    }
    // 6. nettete locale (masque flou leger) — jamais sur le ciel
    if (grade_fx.y > 0.001 && !sky_pixel) {
      vec2 px = 1.0 / vec2(textureSize(tex_T0, 0));
      vec3 blur = texture(tex_T0, tex_coord + vec2(px.x, 0.0)).rgb +
                  texture(tex_T0, tex_coord - vec2(px.x, 0.0)).rgb +
                  texture(tex_T0, tex_coord + vec2(0.0, px.y)).rgb +
                  texture(tex_T0, tex_coord - vec2(0.0, px.y)).rgb;
      float hp = luma(texture(tex_T0, tex_coord).rgb) - luma(blur * 0.25);
      g += clamp(hp, -0.10, 0.10) * grade_fx.y * amt;
    }
    // 6b. CLARTE (13/10, herbe trop transparente) : l'epaule
    //     des hautes lumieres et l'herbe moins fluo compriment la prairie au
    //     soleil — les brins et leurs creux se confondaient (contraste local
    //     mesure : 0.16 dans la scene brute, 0.07 a la sortie). Le relief LOCAL
    //     de la scene brute (rayon 2 px) est reinjecte en rapport de luminance :
    //     la teinte et l'exposition restent celles de l'etalonnage.
    if (!sky_pixel) {
      vec2 px2 = 2.0 / vec2(textureSize(tex_T0, 0));
      float l0 = luma(texture(tex_T0, tex_coord).rgb);
      float lb = 0.0;
      for (int i = 0; i < 8; i++) {
        // (original : a = 0.785398 * i ; vec2(cos(a), sin(a)))
        lb += luma(texture(tex_T0, tex_coord + kOct8[i] * px2).rgb);
      }
      lb *= 0.125;
      float ratio = clamp((l0 + 0.03) / (lb + 0.03), 0.75, 1.3);
      g *= mix(1.0, ratio, 0.6 * clamp(amt, 0.0, 1.0) * (1.0 - edge_amt));
    }
    // 7. vignette tres douce
    if (grade_fx.z > 0.001) {
      vec2 d = (tex_coord - 0.5) * vec2(1.0, 0.95);
      // (original : * grade_fx.z) — vignette a moitie dans les niveaux interieurs
      float v = 1.0 - clamp(dot(d, d) * 1.15 * grade_fx.z * (1.0 - 0.5 * cave), 0.0, 0.35);
      g *= mix(1.0, v, amt);
    }
    c = max(g, vec3(0.0));
  }

  // 12/10 : HUD intact (voir hud_ref_tex). Texte opaque = couleur brute du jeu ;
  // un voile d'interface tres leger garde le traitement de la scene.
  if (has_hud_ref > 0.5) {
    vec3 raw = texture(tex_T0, tex_coord).rgb;
    vec3 ref = texture(hud_ref_tex, tex_coord).rgb;
    vec3 dd = abs(raw - ref);
    // v2 (13/10, textes affiches dans un rectangle transparent) :
    // le cadre des lettres (quasi transparent) modifiait a peine les pixels mais
    // suffisait a basculer vers la scene BRUTE (non eclairee, non etalonnee) : un
    // rectangle visible autour de chaque texte. Desormais seules les lettres
    // franches prennent la couleur brute ; ailleurs l'apport du HUD (raw - ref)
    // est AJOUTE a la scene traitee — un cadre invisible le reste.
    // (original : hud = smoothstep(0.03, 0.20, ...); c = mix(c, raw, hud);)
    float hud = smoothstep(0.12, 0.35, max(dd.r, max(dd.g, dd.b)));
    c = mix(max(c + (raw - ref), vec3(0.0)), raw, hud);
  }

  color = vec4(c * color_mult.rgb * color_mult.a, 1.0) + color_add;
}
