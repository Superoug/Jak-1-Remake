#version 410 core
// CIEL DE NUIT

in vec2 tex_coord;
out vec4 color;

uniform sampler2D depth_tex;  // unite 15 : profondeur (0 = ciel)
uniform mat4 inv_camera;
uniform vec4 lune;  // xyz = direction de la lune, w = presence (0..1)
uniform vec4 nuit_p;  // x = part de nuit, y = heure (0..24), z = temps (s), w = taille d'un pixel
// CIEL DERRIERE LES PARTICULES
uniform sampler2D scene_tex;
uniform sampler2D hud_ref_tex;
uniform float has_part;
float couverture_particules() {
  if (has_part < 0.5) {
    return 0.0;
  }
  ivec2 ip = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
  vec3 dd = abs(texelFetch(hud_ref_tex, ip, 0).rgb - texelFetch(scene_tex, ip, 0).rgb);
  return smoothstep(0.04, 0.30, max(dd.r, max(dd.g, dd.b)));
}

const float PI = 3.14159265;

uint hache(uvec3 v) {
  uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
  h ^= h >> 16u;
  h *= 0x7feb352du;
  h ^= h >> 15u;
  h *= 0x846ca68bu;
  h ^= h >> 16u;
  return h;
}
float h01(uint h) {
  return float(h & 0xffffffu) / 16777216.0;
}

// etoiles d'une couche
vec3 etoiles(vec3 d, float res, float densite, float t, float px, uint graine) {
  vec3 a = abs(d);
  int face;
  vec2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0.0 ? 0 : 1;
    uv = d.yz / a.x;
  } else if (a.y >= a.z) {
    face = d.y > 0.0 ? 2 : 3;
    uv = d.xz / a.y;
  } else {
    face = d.z > 0.0 ? 4 : 5;
    uv = d.xy / a.z;
  }
  vec2 g = (uv * 0.5 + 0.5) * res;
  vec3 somme = vec3(0.0);
  ivec2 c0 = ivec2(floor(g));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      ivec2 c = c0 + ivec2(dx, dy);
      uint h = hache(uvec3(uvec2(c + 4096), uint(face) * 7919u + graine));
      if (h01(h) > densite) {
        continue;
      }
      vec2 pos = vec2(c) + vec2(h01(h * 747796405u + 1u), h01(h * 2891336453u + 2u));
      // distance angulaire approchee (case = 2 / res radians environ)
      float dist = length(g - pos) * (2.0 / res);
      // magnitude : beaucoup de faibles, quelques brillantes
      float m = h01(h * 1664525u + 3u);
      float b = pow(m, 6.0) * 2.4 + 0.10;
      // jamais plus fine qu'un pixel, a energie constante
      float sc0 = max(px * 0.75, 0.0004);
      float sc = max(px * 1.1, 0.0004);
      float k = exp(-(dist * dist) / (sc * sc)) * (sc0 * sc0) / (sc * sc);
      // scintillement
      float ph = h01(h * 22695477u + 5u) * 6.2831;
      // scintillement plus doux et plus lent
      b *= 0.86 + 0.14 * sin(t * (0.6 + 1.2 * m) + ph);
      // teinte : de bleu-blanc a jaune pale
      float tt = h01(h * 134775813u + 7u);
      vec3 col = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.92, 0.78), tt);
      somme += col * b * k;
    }
  }
  return somme;
}

float bruit3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  uvec3 u = uvec3(ivec3(i) + 1024);
  float a = h01(hache(u)), b = h01(hache(u + uvec3(1, 0, 0)));
  float c = h01(hache(u + uvec3(0, 1, 0))), e = h01(hache(u + uvec3(1, 1, 0)));
  float a2 = h01(hache(u + uvec3(0, 0, 1))), b2 = h01(hache(u + uvec3(1, 0, 1)));
  float c2 = h01(hache(u + uvec3(0, 1, 1))), e2 = h01(hache(u + uvec3(1, 1, 1)));
  return mix(mix(mix(a, b, f.x), mix(c, e, f.x), f.y), mix(mix(a2, b2, f.x), mix(c2, e2, f.x), f.y), f.z);
}

void main() {
  // direction du regard
  vec4 qa = inv_camera * vec4(tex_coord * 2.0 - 1.0, 0.0, 1.0);
  vec4 qb = inv_camera * vec4(tex_coord * 2.0 - 1.0, 0.98, 1.0);
  vec3 d = normalize(qa.xyz / qa.w - qb.xyz / qb.w);
  // taille angulaire d'un pixel
  float px = max(length(fwidth(d)), 1e-5);
  if (texture(depth_tex, tex_coord).r > 1e-5 || nuit_p.x < 0.02 || d.y < -0.02) {
    discard;
  }
  float nuit = smoothstep(0.15, 0.85, nuit_p.x);
  vec3 col = vec3(0.0);

  // 1. la LUNE
  float pres = lune.w;
  float pres_l = 0.0;
  if (pres > 0.001) {
    vec3 L = normalize(lune.xyz);
    float ca = clamp(dot(d, L), -1.0, 1.0);
    float ang = acos(ca);
    const float R = 0.022;  // rayon apparent (rad)
    if (ang < R + px * 2.0) {
      // repere du disque
      vec3 ax = normalize(cross(L, vec3(0.0, 1.0, 0.0)) + vec3(1e-4, 0.0, 0.0));
      vec3 ay = cross(ax, L);
      vec2 q = vec2(dot(d, ax), dot(d, ay)) / R;
      float r = length(q);
      float bord = 1.0 - smoothstep(1.0 - px * 1.5 / R, 1.0 + px * 1.5 / R, r);
      float mers = smoothstep(0.42, 0.62, bruit3(vec3(q * 2.2, 3.7)) * 0.65 + bruit3(vec3(q * 5.0, 9.1)) * 0.35);
      float grain = bruit3(vec3(q * 14.0, 1.3));
      float limbe = pow(max(1.0 - r * r, 0.0), 0.25);
      vec3 disque = vec3(1.0, 0.98, 0.94) * mix(1.0, 0.62, mers) * (0.9 + 0.1 * grain) * mix(0.75, 1.0, limbe);
      col += disque * 1.6 * bord * pres;
      pres_l = bord;
    }
    // halo (couronne douce, et grande lueur bleutee)
    col += vec3(0.75, 0.82, 1.0) * (exp(-ang / 0.035) * 0.22 + exp(-ang / 0.25) * 0.06) * pres;
  }

  // 2. les ETOILES
  float rot = nuit_p.y / 24.0 * 2.0 * PI;
  vec3 axe = normalize(vec3(0.0, 0.62, 0.78));
  vec3 ds = d * cos(rot) + cross(axe, d) * sin(rot) + axe * dot(axe, d) * (1.0 - cos(rot));
  // voie lactee
  vec3 nmw = normalize(vec3(0.42, 0.55, -0.72));
  float bande = exp(-pow(dot(ds, nmw) / 0.20, 2.0));
  float nuee = bruit3(ds * 6.0) * 0.6 + bruit3(ds * 14.0) * 0.3 + bruit3(ds * 30.0) * 0.1;
  float poussiere = smoothstep(0.45, 0.75, bruit3(ds * 9.0 + 4.2));
  vec3 lactee = vec3(0.62, 0.70, 0.95) * bande * smoothstep(0.3, 0.8, nuee) * (1.0 - 0.7 * poussiere) * 0.07;
  vec3 et = etoiles(ds, 260.0, 0.22 + 0.35 * bande, nuit_p.z, px, 11u) +
            etoiles(ds, 90.0, 0.10, nuit_p.z, px, 23u) * 1.4;
  // pres de l'horizon
  float horizon = smoothstep(0.0, 0.22, d.y);
  float eclat_lune = pres > 0.0 ? 1.0 - 0.75 * exp(-acos(clamp(dot(d, normalize(lune.xyz)), -1.0, 1.0)) / 0.3) * pres : 1.0;
  col += (et + lactee) * horizon * eclat_lune * (1.0 - pres_l);

  color = vec4(col * nuit * (1.0 - couverture_particules()), 0.0);
}
