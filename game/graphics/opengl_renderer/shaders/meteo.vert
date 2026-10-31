#version 410 core
// METEO ENRICHIE

uniform mat4 clip_from_world;  // relatif a la camera
uniform vec4 cam_trans;
uniform vec4 meteo_fx;  // x = dose, y = temps (s, fige en pause), z = taille d'un pixel (rad)
uniform vec4 meteo_ciel;  // x = jour (0..1), y = nuit, z = soleil present
uniform vec4 sun_dir;  // vers le soleil
uniform int meteo_sorte;
uniform vec2 meteo_ecran;  // taille de la cible (px)
uniform sampler2DShadow sun_shadow_tex;
uniform mat4 sun_light_vp;
uniform float sun_map_ok;

out vec4 v_couleur;  // rgb, a = opacite
out vec3 v_forme;  // pluie : x = travers (px, signe), y = long
out float v_dist;  // distance a la camera (m), pour le test de profondeur

const float M = 4096.0;

vec3 h3(float n) {
  vec3 p = fract(vec3(n) * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yzx + 33.33);
  return fract((p.xxy + p.yzz) * p.zyx);
}
float soleil(vec3 q) {
  if (sun_map_ok < 0.5) {
    return 1.0;
  }
  vec3 sp = (sun_light_vp * vec4(q, 1.0)).xyz * 0.5 + 0.5;
  if (sp.x < 0.01 || sp.x > 0.99 || sp.y < 0.01 || sp.y > 0.99 || sp.z > 1.0) {
    return 1.0;
  }
  return texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
}

void rien() {
  gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  v_couleur = vec4(0.0);
  v_forme = vec3(0.0);
  v_dist = 0.0;
}

void main() {
  float t = meteo_fx.y;
  vec3 cam = cam_trans.xyz / M;
  float jour = meteo_ciel.x, nuit = meteo_ciel.y;

  if (meteo_sorte == 0) {
    // --- PLUIE
    const vec3 BOITE = vec3(20.0, 14.0, 20.0);
    float id = float(gl_VertexID / 6);
    int coin = gl_VertexID % 6;
    vec3 h = h3(id + 501.0);
    vec3 g = h3(id + 77.0);
    vec3 vel = vec3(0.9, -(7.5 + 2.5 * g.x), 0.5);  // m/s (vent leger)
    vec3 wp = h * BOITE + vel * t;
    vec3 p = wp - BOITE * floor((wp - cam) / BOITE + 0.5);
    // tete en p, trainee au-dessus (le trait d'une goutte qui tombe)
    vec3 q = p - normalize(vel) * (0.45 + 0.35 * g.y);
    vec3 v = p - cam;
    float d = length(v);
    vec3 rel = abs(v) / BOITE;
    float bord = 1.0 - smoothstep(0.32, 0.48, max(rel.x, max(rel.y, rel.z)));
    float proche = smoothstep(0.6, 1.6, d);
    vec4 ca = clip_from_world * vec4(p * M - cam_trans.xyz, 1.0);
    vec4 cb = clip_from_world * vec4(q * M - cam_trans.xyz, 1.0);
    if (ca.w < 0.05 || cb.w < 0.05 || bord * proche < 1e-3) {
      rien();
      return;
    }
    vec2 dir = (cb.xy / cb.w - ca.xy / ca.w) * meteo_ecran;
    float lpx = length(dir);
    dir = lpx > 1e-3 ? dir / lpx : vec2(0.0, 1.0);
    vec2 perp = vec2(-dir.y, dir.x);
    float pix_m = max(d * meteo_fx.z, 1e-5);
    // demi-largeur
    float lm = 0.0035 / pix_m;
    float larg = max(lm, 0.5);
    float energie = max(lm / larg, 0.25);
    // coins : (0 tete, 1 queue) x (cote -1, +1)
    const vec2 C[6] = vec2[6](vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(1.0, -1.0), vec2(1.0, -1.0),
                              vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 c = C[coin];
    vec4 cc = c.x < 0.5 ? ca : cb;
    float off = (larg + 1.0) * c.y;
    cc.xy += perp * off / meteo_ecran * 2.0 * cc.w;
    gl_Position = cc;
    v_forme = vec3(off, c.x, larg);
    v_dist = d;
    // la pluie se voit par la lumiere du ciel qu'elle renvoie
    float contre = meteo_ciel.z > 0.5 ? pow(max(dot(normalize(v), sun_dir.xyz), 0.0), 6.0) : 0.0;
    vec3 coul = vec3(0.80, 0.85, 0.86) * (0.10 + 0.55 * jour) + vec3(1.0, 0.95, 0.85) * contre * jour * 0.6;
    v_couleur = vec4(coul, 0.36 * energie * bord * proche * meteo_fx.x);
  } else {
    // --- NEIGE
    const vec3 BOITE = vec3(28.0, 18.0, 28.0);
    float id = float(gl_VertexID);
    vec3 h = h3(id + 9001.0);
    vec3 g = h3(id + 313.0);
    vec3 vel = vec3(0.35, -(0.8 + 0.6 * g.x), 0.18);  // m/s
    vec3 wp = h * BOITE + vel * t;
    wp += 0.35 * vec3(sin(t * (0.6 + g.y) + g.z * 6.28), 0.0, cos(t * (0.5 + g.z) + g.x * 6.28));
    vec3 p = wp - BOITE * floor((wp - cam) / BOITE + 0.5);
    vec3 v = p - cam;
    float d = length(v);
    vec3 rel = abs(v) / BOITE;
    float bord = 1.0 - smoothstep(0.32, 0.48, max(rel.x, max(rel.y, rel.z)));
    float proche = smoothstep(0.35, 1.1, d);
    vec4 cc = clip_from_world * vec4(p * M - cam_trans.xyz, 1.0);
    if (cc.w < 0.05 || bord * proche < 1e-3) {
      rien();
      return;
    }
    gl_Position = cc;
    float pix_m = max(d * meteo_fx.z, 1e-5);
    float r = 0.013 + 0.017 * g.z;  // rayon du flocon (m)
    float rr = max(r, pix_m * 0.9);
    float energie = max((r * r) / (rr * rr), 0.2);
    gl_PointSize = min(2.0 * rr / pix_m + 1.0, 40.0);
    v_forme = vec3(0.0);
    v_dist = d;
    // eclaire par le ciel, et par le soleil hors de l'ombre
    float sol = meteo_ciel.z > 0.5 ? soleil(p * M) : 0.0;
    vec3 coul = vec3(0.93, 0.96, 1.0) * (0.12 + jour * (0.42 + 0.40 * sol)) +
                vec3(0.55, 0.65, 0.90) * nuit * 0.10;
    v_couleur = vec4(coul, 0.85 * energie * bord * proche * meteo_fx.x);
  }
}
