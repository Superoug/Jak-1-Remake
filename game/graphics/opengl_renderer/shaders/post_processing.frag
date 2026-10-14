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
// ni nuit, ni etalonnage — « APPUIE SUR O » n'est plus orange dans la hutte)
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
// xyz = normalized direction TOWARDS the sun (world), w = 1 when it is up
uniform vec4 sun_dir;

// World position is rebuilt from the depth buffer with the inverse of the
// game's own camera matrix, which is what makes local lights possible at all:
// nothing in the original pipeline hands the post pass a world position.
uniform mat4 inv_camera;
uniform mat4 clip_from_world;  // the forward transform, for screen-space ray marching
uniform vec4 cam_trans;

#define MAX_LAMPS 48
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

vec3 world_from_depth(vec2 uv, float d) {
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 p = inv_camera * ndc;
  if (abs(p.w) < 1e-8) {
    return cam_trans.xyz;
  }
  return p.xyz / p.w + cam_trans.xyz;
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
  vec3 tx = abs(dxp - d) < abs(dxm - d) ? world_from_depth(uv + vec2(px.x, 0.0), dxp) - P
                                        : P - world_from_depth(uv - vec2(px.x, 0.0), dxm);

  float dyp = texture(depth_tex, uv + vec2(0.0, px.y)).r;
  float dym = texture(depth_tex, uv - vec2(0.0, px.y)).r;
  vec3 ty = abs(dyp - d) < abs(dym - d) ? world_from_depth(uv + vec2(0.0, px.y), dyp) - P
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
    float behind = length(world_from_depth(uv, sd) - cam_trans.xyz) - length(sp - cam_trans.xyz);
    // smooth entry and exit of the thickness window, so the shadow fades in
    float hit = smoothstep(4096.0 * 0.05, 4096.0 * 0.35, behind) *
                (1.0 - smoothstep(4096.0 * 4.0, 4096.0 * 7.0, behind));
    occ += hit * w;
  }
  return weight > 0.0 ? clamp(occ / weight * 2.4, 0.0, 1.0) : 0.0;
}

float compute_ssao(vec2 uv, float d, vec3 P, vec3 N) {
  vec2 px = 1.0 / vec2(textureSize(depth_tex, 0));
  const float R = 4096.0 * 2.2;  // 2.2 m hemisphere
  float occ = 0.0;
  // 8 taps au lieu de 12 (angle d'or, couverture equivalente) — coût -33 %
  for (int i = 0; i < 8; i++) {
    // (original : a = 0.7853982 * i + 0.37 ; jitter = 0.35 + 0.65 * fract(i * 0.6180339) ;
    //  suv = uv + vec2(cos(a), sin(a)) * 11.0 * px * jitter)
    vec2 suv = uv + kSsao8[i] * 11.0 * px;
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
  return clamp(occ / 3.4, 0.0, 1.0);
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
#ifdef POST_PASS2
  // reprise de la passe 1 : couleur eclairee, aretes (a >= 0) ou ciel (a < 0)
  vec4 lit = texelFetch(lit_tex, ivec2(tex_coord * vec2(textureSize(lit_tex, 0))), 0);
  vec3 c = lit.rgb;
  bool sky_pixel = lit.a < -0.5;
  float edge_amt = max(lit.a, 0.0);
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
  if (!sky_pixel) {
    P = world_from_depth(tex_coord, center_d);
    view_dist = length(P - cam_trans.xyz);
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
    if (has_normal_tex > 0.5) {
      vec4 gn = texture(normal_tex, tex_coord);
      if (gn.a > 0.6 && gn.a < 0.9) {
        emissive_px = 1.0;
      }
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
      if (dot(N, cam_trans.xyz - P) < 0.0) {
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
    // (contraste local divise par 2, mesure) -> aspect laiteux, « transparent ».
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
  // avec les facettes de la FALAISE derriere elle : de la « roche blanche sans
  // texture ». Couverture des particules = ecart entre la scene d'avant sprites
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
  // FONDU CHAMP LOINTAIN (21/09, « treillis du lointain » ouvert depuis le 26/08,
  // cause PROUVEE par mesure : relumiere solaire x3 et ambiante hemispherique
  // x2.2 d'energie haute frequence sur les collines lointaines, SSAO/spec/god
  // rays innocents). Au loin, la profondeur perd sa precision et la normale
  // reconstruite dessine des lignes de niveau (« empreintes digitales »). Le
  // modele directionnel s'efface donc entre 180 et 320 m — comme near_flat,
  // il rend la main aux couleurs des artistes sans toucher a l'exposition.
  // (piste non verifiee 21/09 : avec une vraie normale du G-buffer le fondu
  //  lointain pourrait etre supprime — `(sky_pixel || has_true_normal) ? 0.0 : …`
  //  — a tester a l'image ; l'etat ci-dessous est celui mesure et valide.)
  float far_flat = sky_pixel ? 0.0 : smoothstep(4096.0 * 180.0, 4096.0 * 320.0, view_dist);
  float flat_amt = max(near_flat, far_flat);

  // ---- depth-aware: SSAO (contact shading in crevices) ----
  if (depth_fx.x > 0.001 && !sky_pixel) {
    float ao = compute_ssao(tex_coord, center_d, P, N);
    // tint the occlusion slightly cool: shaded crevices lose the warm sun first
    ao *= 1.0 - edge_amt;  // pas d'anneau d'occlusion derrière les objets fins
    ao *= 1.0 - near_flat * 0.9;  // champ proche/cinematique : ne pas cerner les facettes
    ao *= (1.0 - emissive_px);  // 21/09 : pas d'occlusion sur une source
    c *= mix(vec3(1.0), vec3(0.88, 0.92, 1.02), ao * depth_fx.x) * (1.0 - ao * depth_fx.x * 0.85);
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
        float cs = contact_shadow(P, L, view_dist) * clamp(1.0 - view_dist / (4096.0 * 90.0), 0.0, 1.0);
        lit *= 1.0 - cs * 0.85 * (1.0 - near_flat);
      }
      // champ proche : le contraste directionnel s'efface (0.62 = valeur moyenne
      // du wrap sur l'hemisphere — l'exposition ne bouge pas, le facettage oui)
      // (original : lit = mix(lit, 0.62, near_flat);) — lointain inclus
      lit = mix(lit, 0.62, flat_amt);
      vec3 lighting = mix(shade_col * 0.78, sun_col, lit);
      // specular sheen so surfaces catch the sun instead of staying matte
      if (light_fx.w > 0.001) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(Nr, H), 0.0), 42.0) * ndl;
        // (original : * (1.0 - near_flat)) — le reflet s'eteint aussi au loin
        lighting += vec3(1.0, 0.95, 0.85) * spec * light_fx.w * 1.6 * (1.0 - flat_amt);
      }
      // (original : * day) — 21/09 : une source n'est pas relumee
      c *= mix(vec3(1.0), lighting, clamp(light_fx.y, 0.0, 1.0) * day * (1.0 - emissive_px));
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
      amb = mix(amb, vec3(0.74, 0.66, 0.56), clamp(grade_amt2.w, 0.0, 1.0));
      // (original : * day) — 21/09 : idem pour l'hemispherique
      c *= mix(vec3(1.0), amb * 1.55, clamp(light_fx.z, 0.0, 1.0) * day * (1.0 - emissive_px));
    }

    // ============================================================
    // ECLAIRAGE UNIFIE (reecriture 19/08) : la nuit, l'ancienne lumiere
    // n'eclairait pas, elle ternissait l'image. Objectif : un eclairage reel,
    // jour et nuit, avec les couleurs des ecos.
    //
    // Principe : l'image du jeu est l'albedo eclaire plein jour. On ne
    // l'assombrit plus pour ensuite la re-multiplier (multiplier du noir
    // redonne du noir — c'etait le « terni »). On compose UN facteur
    // d'eclairage lineaire, applique UNE fois :
    //
    //     c_final = c * ( ambiant(heure) + somme(lampes) )
    //
    // La nuit, l'ambiant est un clair de lune bleu sombre ; une lampe a
    // pleine force REDONNE a la surface ses couleurs de jour, teintees par la
    // couleur de la lampe — c'est la definition d'« eclairer reellement ».
    // Une lampe d'eco verte rend l'herbe vert-vif, une bleue rend tout
    // bleute : physique, sans astuce de teinte.
    // ============================================================
    float night = clamp(night_fx.x, 0.0, 1.0);
    // 21/09 : NIVEAU INTERIEUR (ground_color.w, pose par time-of-day.gc : grottes,
    // cite engloutie, temple, citadelle). Pas de soleil la-dedans : les luminaires
    // du decor (tubes chauds, lueurs precurseur, cristaux, plaques d'eco) brillent
    // a pleine force a toute heure — de « jour » ils etaient a 30 %.
    float interior = clamp(ground_color.w, 0.0, 1.0);
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
      ambient *= mix(1.0, 0.80, clamp(grade_amt2.w, 0.0, 1.0));

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
            reach_mult = mix(reach_mult, 1.0, clamp(grade_amt2.w, 0.0, 1.0));
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
          if (!world_emissive) {
            vec3 H = normalize(L + V);
            L_sum += lamp_color[i].rgb * pow(max(dot(N, H), 0.0), 24.0) * fall * 0.35 *
                     presence * (1.0 - near_flat);
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
      if (cy < water_glitter.x - 4096.0 * 3.5) {
        // camera immergee : du regard jusqu'au pixel (ou a la sortie de l'eau)
        float t_exit = (t_surf > 0.0 && t_surf < 1.0) ? t_surf : 1.0;
        col_m = pix_dist * t_exit / 4096.0;
      } else if (t_surf > 0.0 && t_surf < 1.04) {
        // camera en l'air : seul le segment sous la surface compte
        col_m = pix_dist * max(0.0, 1.0 - min(t_surf, 1.0)) / 4096.0;
      }
      if (col_m > 0.02) {
        float day_w = clamp(1.0 - night_fx.x, 0.0, 1.0);
        vec3 murk_shallow = mix(vec3(0.05, 0.18, 0.38), vec3(0.12, 0.48, 0.88), day_w);
        vec3 murk_deep = mix(vec3(0.015, 0.07, 0.18), vec3(0.03, 0.22, 0.55), day_w);
        vec3 murk = mix(murk_shallow, murk_deep, clamp(col_m / 9.0, 0.0, 1.0));
        float f = 1.0 - exp(-col_m * 0.50);
        c = mix(c, murk, clamp(f, 0.0, 0.94));
      }
      // le rayon traverse la surface entre la caméra et la géométrie touchée
      // (petite marge au-delà de 1.0 : la surface au ras de la géométrie)
      if (t_surf > 0.0 && t_surf < 1.04) {
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
        vec3 Nr = normalize(vec3(Nw.x * 0.14, 1.0, Nw.z * 0.14));
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
              hit_w = 1.0;
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
        if (hit_w > 0.5) {
          c = mix(c, refl_col, clamp(fresR * 0.55 * strength * shore, 0.0, 0.65));
        }
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
          // la lave RESPIRE : battement lent, déphasé par source — la
          // sensation « jeu moderne »
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
      // dose « legere » : le fantome le plus fort ajoute ~0,15 a gain 1
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
  // Tout est neutre a zero et pilote par le menu Remake (« RENDU REMAKE »).
  if (grade_fx.x > 0.001) {
    float amt = clamp(grade_fx.x, 0.0, 2.0);
    vec3 g = c;
    // NIVEAUX INTERIEURS hors huttes (grottes, cite engloutie, temple, citadelle :
    // ground_color.w sans la part de hutte grade_amt2.w) : scenes deja sombres,
    // l'exposition et le S les assombrissaient de 19 % (cite engloutie mesuree
    // 12/10) — exposition neutre et S a 40 %, leurs lumieres validees restent
    float cave = clamp(ground_color.w - grade_amt2.w, 0.0, 1.0);
    // 1. exposition (legere : la vivacite des couleurs du jeu est gardee)
    // (original : g *= mix(1.0, grade_amt2.x, amt);)
    g *= mix(1.0, mix(grade_amt2.x, 1.0, cave), amt);
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
      float k = mix(1.0, grade_amt2.y, amt * (sky_pixel ? 0.4 : 1.0) * (1.0 - 0.6 * cave));
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
      g += vec3(0.046, 0.050, 0.060) * t * amt * (1.0 - clamp(night_fx.x, 0.0, 1.0)) *
           (1.0 - clamp(ground_color.w, 0.0, 1.0));
    }
    // 4. saturation
    {
      float lu = luma(g);
      g = mix(vec3(lu), g, mix(1.0, grade_amt2.z, amt));
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
