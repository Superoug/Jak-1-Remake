#version 410 core
// CIEL VIVANT

in vec2 tex_coord;
out vec4 color;

uniform sampler3D bruit3d;  // r = forme (Perlin-Worley), g, b, a = Worley 4, 8, 16
uniform sampler2D meteo;  // r = couverture, g = hauteur des sommets
uniform sampler2D hist_tex;  // nuages de l'image precedente
uniform mat4 inv_camera;  // ecran -> monde relatif a la camera
uniform mat4 prev_clip_from_world;
uniform vec4 cam_m;  // camera (m), w = temps (s)
uniform vec4 lum_dir;  // vers la lumiere (soleil ou lune), w = soleil (1) ou lune (0)
uniform vec3 lum_soleil;  // couleur de la lumiere
uniform vec3 lum_ciel;  // ambiance du ciel
uniform vec4 nuage_p;  // x = couverture, y = poids de l'image, z = gigue, w = libre
uniform sampler2D scene_tex;  // scene sans HUD : la couleur du ciel derriere le nuage
uniform sampler2D depth_tex;  // profondeur (0 = ciel)

const float NUAGE_BAS = 1100.0;
const float NUAGE_HAUT = 2300.0;
const float DIST_MAX = 22000.0;
const vec2 VENT = vec2(7.0, 3.0);  // m/s

float remap(float x, float a, float b, float c, float d) {
  return c + (x - a) / max(b - a, 1e-4) * (d - c);
}

float densite(vec3 p, float t, bool detail) {
  float h = (p.y - NUAGE_BAS) / (NUAGE_HAUT - NUAGE_BAS);
  if (h < 0.0 || h > 1.0) {
    return 0.0;
  }
  vec2 m = texture(meteo, (p.xz + VENT * t) / 22000.0).rg;
  float couv = clamp(remap(m.r, 0.3, 0.7, 0.0, 1.0) * nuage_p.x * 2.0, 0.0, 1.0);
  if (couv < 0.01) {
    return 0.0;
  }
  // profil vertical
  float sommet = mix(0.35, 1.0, m.g);
  float profil = smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(sommet * 0.45, sommet, h));
  vec3 q = p + vec3(VENT.x, 0.0, VENT.y) * t;
  vec4 n = texture(bruit3d, q / 2000.0);
  float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
  float forme = remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0);
  float d = remap(forme * profil, 1.0 - couv, 1.0, 0.0, 1.0);
  if (detail && d > 0.0) {
    // erosion fine des bords
    vec4 e = texture(bruit3d, q / 560.0 + vec3(0.0, t * 0.004, 0.0));
    float fe = e.g * 0.625 + e.b * 0.25 + e.a * 0.125;
    d = remap(d, fe * mix(0.45, 0.2, h), 1.0, 0.0, 1.0);
  }
  return clamp(d * 1.6, 0.0, 1.0);
}

float hg(float c, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (12.566 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-3), 1.5));
}

void main() {
  vec4 q = inv_camera * vec4(tex_coord * 2.0 - 1.0, 1e-3 * 2.0 - 1.0, 1.0);
  vec3 dir = normalize(q.xyz / q.w);
  vec3 cam = cam_m.xyz;
  float t = cam_m.w;

  vec4 res = vec4(0.0);
  if (dir.y > 0.005 && cam.y < NUAGE_BAS) {
    float t0 = (NUAGE_BAS - cam.y) / dir.y;
    float t1 = min((NUAGE_HAUT - cam.y) / dir.y, t0 + 5000.0);
    if (t0 < DIST_MAX) {
      t1 = min(t1, DIST_MAX);
      const int N = 56;
      float dt = (t1 - t0) / float(N);
      // decalage du depart
      uint hh = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u +
                uint(nuage_p.z * 65536.0) * 26699u;
      hh = (hh ^ (hh >> 16u)) * 0x45d9f3bu;
      hh = (hh ^ (hh >> 16u)) * 0x45d9f3bu;
      hh ^= hh >> 16u;
      float bruit = float(hh & 0xffffffu) / 16777216.0;
      float s = t0 + dt * bruit;
      vec3 L = normalize(lum_dir.xyz);
      float c = dot(dir, L);
      float T = 1.0;
      vec3 col = vec3(0.0);
      // ambiance : la lumiere du ciel (sa couleur derriere le nuage, plus claire) melee a celle de l'heure
      vec3 amb = lum_ciel;
      vec2 haut = tex_coord + vec2(0.0, 0.02);
      {
        vec3 somme = vec3(0.0);
        float n = 0.0;
        for (int iy = 0; iy < 3; iy++) {
          for (int ix = -1; ix <= 1; ix++) {
            vec2 q = tex_coord + vec2(float(ix) * 0.012, float(iy) * 0.02);
            if (texture(depth_tex, q).r <= 1e-5) {
              somme += textureLod(scene_tex, q, 2.0).rgb;
              n += 1.0;
            }
          }
        }
        if (n > 0.5) {
          amb = mix(lum_ciel, (somme / n) * 1.25 + 0.08, 0.7 * smoothstep(0.5, 4.5, n));
        }
      }
      const float SIGMA = 0.06;  // extinction par metre a densite 1
      for (int i = 0; i < N; i++) {
        vec3 p = cam + dir * s;
        float d = densite(p, t, true);
        if (d > 0.002) {
          // lumiere : densite cumulee vers le soleil (4 pas qui s'allongent)
          float dl = 0.0;
          float pas = 90.0;
          vec3 pl = p;
          for (int k = 0; k < 4; k++) {
            pl += L * pas;
            dl += densite(pl, t, k < 2) * pas;
            pas *= 1.9;
          }
          // diffusion multiple
          float ms = 0.0;
          float oa = 1.0, ob = 1.0, oc = 1.0;
          for (int o = 0; o < 3; o++) {
            ms += oa * exp(-dl * SIGMA * 0.8 * oc) * mix(hg(c, 0.6 * ob), hg(c, -0.25 * ob), 0.3);
            oa *= 0.6;
            ob *= 0.5;
            oc *= 0.4;
          }
          float powder = 1.0 - exp(-d * 4.0);
          float h = clamp((p.y - NUAGE_BAS) / (NUAGE_HAUT - NUAGE_BAS), 0.0, 1.0);
          vec3 S = lum_soleil * ms * 12.566 * mix(0.6, 1.0, powder) + amb * mix(0.55, 1.15, h);
          float a = exp(-d * SIGMA * dt);
          col += T * S * (1.0 - a);
          T *= a;
          if (T < 0.02) {
            break;
          }
        }
        s += dt;
      }
      // au loin, les nuages se fondent dans le ciel
      float loin = exp(-t0 / 9000.0) * smoothstep(0.02, 0.09, dir.y);
      res = vec4(col * loin, (1.0 - T) * loin);
    }
  }

  // melange avec l'image precedente
  if (nuage_p.y < 0.999) {
    vec4 c = prev_clip_from_world * vec4(dir * (4096.0 * 1000.0), 1.0);
    if (c.w > 1e-4) {
      vec2 puv = (c.xy / c.w) * 0.5 + 0.5;
      if (puv.x > 0.0 && puv.x < 1.0 && puv.y > 0.0 && puv.y < 1.0) {
        res = mix(texture(hist_tex, puv), res, nuage_p.y);
      }
    }
  }
  color = res;
}
