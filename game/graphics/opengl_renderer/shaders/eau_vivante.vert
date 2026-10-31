#version 410 core
// EAU VIVANTE

layout (location = 0) in vec2 grille;  // [-1, 1]^2

out vec4 fragment_color;
out vec3 tex_coord;
out float fog;

uniform mat4 clip_from_world;  // relatif a la camera
uniform vec4 water_cam;  // camera (unites du jeu)
uniform vec4 eau_grille;  // x, z = centre (m), y = niveau de la mer (m), w = rayon (m)
uniform vec4 eau_houle;  // x = temps (s), y = dose de la houle, z = dose des ondes,
                               // w = dose des vagues de rivage
uniform sampler2D eau_couleurs;  // couleurs des sommets de la mer d'origine (RGBA8)
uniform vec4 eau_couleurs_box;  // x0, z0 (m), 1 / largeur, 1 / profondeur
uniform sampler2D eau_sim;  // r = hauteur des ondes (m)
uniform vec4 eau_sim_box;  // x0, z0 (m), 1 / cote, 1 si active

// hauteur des ondes (m) en un point de la grille de simulation
float eau_sim_h(vec2 su) {
  ivec2 sz = textureSize(eau_sim, 0);
  vec2 t = su * vec2(sz) - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  ivec2 m = sz - 1;
  float a = texelFetch(eau_sim, clamp(i, ivec2(0), m), 0).r;
  float b = texelFetch(eau_sim, clamp(i + ivec2(1, 0), ivec2(0), m), 0).r;
  float c = texelFetch(eau_sim, clamp(i + ivec2(0, 1), ivec2(0), m), 0).r;
  float d = texelFetch(eau_sim, clamp(i + ivec2(1, 1), ivec2(0), m), 0).r;
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
uniform vec4 riv_box[2];
uniform int riv_n;

const float M = 4096.0;

float riv_dist_v(vec2 xz_m) {
  float d = 99.0;
  if (riv_n > 0) {
    vec2 uv = (xz_m - riv_box[0].xy) * riv_box[0].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = textureLod(riv_tex0, uv, 0.0).r;
    }
  }
  if (riv_n > 1) {
    vec2 uv = (xz_m - riv_box[1].xy) * riv_box[1].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = min(d, textureLod(riv_tex1, uv, 0.0).r);
    }
  }
  return d;
}

// HOULE : 6 vagues (longueur d'onde en m, direction en rad, amplitude en m)
const int NV = 6;
const float L[NV] = float[NV](8.5, 13.0, 19.0, 28.0, 41.0, 63.0);
const float DIR[NV] = float[NV](0.95, 0.35, 0.70, 0.15, 0.55, 0.42);
const float AMP[NV] = float[NV](0.035, 0.05, 0.07, 0.09, 0.11, 0.13);
const float PHI[NV] = float[NV](0.0, 1.7, 4.1, 2.6, 5.3, 3.4);
const float Q = 0.55;  // finesse des cretes (Gerstner)

vec3 houle(vec2 p, float t) {
  vec3 d = vec3(0.0);
  for (int i = 0; i < NV; i++) {
    float k = 6.2831853 / L[i];
    float w = sqrt(9.81 * k);
    vec2 D = vec2(cos(DIR[i]), sin(DIR[i]));
    float ph = k * dot(D, p) - w * t + PHI[i];
    float c = cos(ph), s = sin(ph);
    d.xz += Q * AMP[i] * D * c;
    d.y += AMP[i] * s;
  }
  return d;
}

// --- VAGUES DE RIVAGE en relief
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
// hauteur (m) ajoutee par les vagues de rivage a dter m du bord
float vague_rivage(vec2 p, float dter, float t) {
  if (dter > 22.0 || dter < -1.0) {
    return 0.0;
  }
  float ph = sh_phase(p, t);
  float force = 0.25 + 0.75 * sh_noise(p * 0.16 + vec2(t * 0.021, t * 0.013));
  float h = 0.0;
  if (ph < 0.55) {
    float sp = ph / 0.55;
    float xc = 18.0 * pow(1.0 - sp, 1.25);
    float dx = dter - xc;
    // dos long cote large, front court cote plage
    float bosse = dx > 0.0 ? exp(-dx * dx / 6.0) : exp(-dx * dx / 0.7);
    float gonfle = smoothstep(18.0, 6.0, xc);  // la vague se gonfle en approchant
    float casse = smoothstep(5.5, 2.5, xc) * force;  // puis deferle et s'aplatit
    h += bosse * gonfle * (1.0 - 0.75 * casse) * (0.10 + 0.22 * force);
    // creux devant la vague
    h -= exp(-pow(dx + 2.2, 2.0) / 1.6) * gonfle * 0.05 * force;
    // l'eau deferlee pousse vers le bord
    h += casse * (dx > 0.0 ? exp(-dx / 3.0) : exp(dx / 0.6)) * 0.07;
  } else {
    // la nappe monte sur le sable puis redescend
    float s = (ph - 0.55) / 0.45;
    float monte = s < 0.3 ? sin(s / 0.3 * 1.5708) : pow(1.0 - (s - 0.3) / 0.7, 1.4);
    h += monte * 0.10 * (1.0 - smoothstep(0.0, 5.0, dter));
  }
  // s'efface tres pres du sable (le bord doux de la surface fait la suite) et loin du bord
  return h * smoothstep(-1.0, 0.3, dter);
}

// ondes de bord (m) a d m du rivage
float ondes_bord_s(vec2 p, float d, float t) {
  const float K = 6.2831853 / 7.0;
  const float W = 6.2831853 / 5.5;
  float ph = sh_noise(p * 0.025) * 4.0;
  return sin(K * d + W * t + ph);
}
float ondes_bord_env(float d) {
  return smoothstep(-0.3, 1.0, d) * (1.0 - smoothstep(8.0, 20.0, d));
}
float ondes_bord_h(vec2 p, float d, float t) {
  float s = ondes_bord_s(p, d, t);
  s = s > 0.0 ? pow(s, 1.4) : -pow(-s, 0.8) * 0.7;
  return 0.16 * ondes_bord_env(d) * s;
}

void main() {
  // grille : plus dense au centre
  vec2 g = grille;
  vec2 f = sign(g) * (0.3 * abs(g) + 0.7 * abs(g) * abs(g) * abs(g));
  vec2 p = eau_grille.xz + f * eau_grille.w;  // m

  float dist = length(p - water_cam.xz / M);
  float d_riv = riv_dist_v(p);
  // pas de houle sur la terre, pres du bord (elle s'eteint sur 1 a 10 m d'eau)
  float a_riv = (d_riv > 35.0 && d_riv < 45.0) ? 0.0 : smoothstep(0.5, 6.0, d_riv);
  float a_bord = 1.0 - smoothstep(0.55, 0.9, dist / eau_grille.w);
  float a = a_riv * a_bord;

  vec3 d = houle(p, eau_houle.x) * (a * eau_houle.y);
  // VA-ET-VIENT au bord
  if (d_riv < 35.0) {
    float ph_v = sh_noise(p * 0.03) * 3.0 + sh_noise(p * 0.11 + 5.3) * 0.6;
    float vv = sin(eau_houle.x * (6.2831853 / 7.0) + ph_v);
    vv = vv > 0.0 ? pow(vv, 0.8) : -pow(-vv, 1.3);  // monte vite, redescend plus lentement
    d.y += vv * 0.10 * (1.0 - smoothstep(4.0, 16.0, d_riv)) * a_bord;
    // ONDES DE BORD
    d.y += ondes_bord_h(p, d_riv, eau_houle.x) * a_bord;
  }
  // vagues de rivage
  if (d_riv < 35.0) {
    d.y += vague_rivage(p, d_riv, eau_houle.x) * a_bord * eau_houle.w;
  }
  // ondes de la simulation autour de Jak
  if (eau_sim_box.w > 0.5) {
    vec2 su = (p - eau_sim_box.xy) * eau_sim_box.z;
    if (su.x > 0.0 && su.x < 1.0 && su.y > 0.0 && su.y < 1.0) {
      vec2 e = min(su, 1.0 - su);
      float bord_s = smoothstep(0.0, 0.08, min(e.x, e.y));
      d.y += eau_sim_h(su) * eau_houle.z * bord_s * smoothstep(0.0, 1.0, d_riv);
    }
  }
  vec3 w = vec3(p.x + d.x, eau_grille.y + d.y, p.y + d.z);

  // couleur de la mer d'origine a cet endroit
  vec2 cu = (w.xz - eau_couleurs_box.xy) * eau_couleurs_box.zw;
  vec4 rgba = textureLod(eau_couleurs, clamp(cu, vec2(0.0), vec2(1.0)), 0.0);
  fragment_color = vec4(rgba.rgb, rgba.a * 2.0) * 2.0;
  // la crete des ondes de bord s'eclaircit (la lumiere traverse l'eau qui se souleve)
  if (d_riv < 22.0) {
    float sb = ondes_bord_s(p, d_riv, eau_houle.x) * ondes_bord_env(d_riv) * a_bord;
    fragment_color.rgb *= 1.0 + (sb > 0.0 ? 0.22 * sb : 0.10 * sb);
  }
  // texture des vagues
  tex_coord = vec3(p / 96.0, 1.0);
  // brouillard de la mer d'origine
  fog = 255.0 - clamp(255.0 - (dist - 120.0) * 0.06, 25.0, 255.0);
  gl_Position = clip_from_world * vec4(w * M - water_cam.xyz, 1.0);
}
