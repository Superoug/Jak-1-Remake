#version 410 core
// CIEL VIVANT

in vec2 tex_coord;
out vec4 color;

uniform sampler3D bruit3d;  // comme nuages.frag
uniform sampler2D meteo;
uniform sampler2D depth_tex;  // unite 15 : profondeur (0 = ciel)
uniform sampler2D normal_tex;  // unite 13 : G-buffer (a > 0.5 : surface du decor)
uniform float has_normal_tex;
uniform mat4 inv_camera;  // ecran -> monde relatif a la camera
uniform vec4 cam_m;  // camera (m), w = temps (s)
uniform vec4 lum_dir;  // vers le soleil
uniform vec4 nuage_p;  // x = couverture, y = force de l'ombre (0 = rien)
uniform vec4 mer;  // x = niveau de la mer (m), y = 1 si mer

const float NUAGE_BAS = 1100.0;
const float NUAGE_HAUT = 2300.0;
const vec2 VENT = vec2(7.0, 3.0);  // m/s

float remap(float x, float a, float b, float c, float d) {
  return c + (x - a) / max(b - a, 1e-4) * (d - c);
}

// EXACTEMENT densite de nuages.frag (erosion des bords comprise)
float densite(vec3 p, float t) {
  float h = (p.y - NUAGE_BAS) / (NUAGE_HAUT - NUAGE_BAS);
  if (h < 0.0 || h > 1.0) {
    return 0.0;
  }
  vec2 m = texture(meteo, (p.xz + VENT * t) / 22000.0).rg;
  float couv = clamp(remap(m.r, 0.3, 0.7, 0.0, 1.0) * nuage_p.x * 2.0, 0.0, 1.0);
  if (couv < 0.01) {
    return 0.0;
  }
  float sommet = mix(0.35, 1.0, m.g);
  float profil = smoothstep(0.0, 0.07, h) * (1.0 - smoothstep(sommet * 0.45, sommet, h));
  vec3 q = p + vec3(VENT.x, 0.0, VENT.y) * t;
  vec4 n = texture(bruit3d, q / 2000.0);
  float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
  float forme = remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0);
  float d = remap(forme * profil, 1.0 - couv, 1.0, 0.0, 1.0);
  if (d > 0.0) {
    vec4 e = texture(bruit3d, q / 560.0 + vec3(0.0, t * 0.004, 0.0));
    float fe = e.g * 0.625 + e.b * 0.25 + e.a * 0.125;
    d = remap(d, fe * mix(0.45, 0.2, h), 1.0, 0.0, 1.0);
  }
  return clamp(d * 1.6, 0.0, 1.0);
}

void main() {
  color = vec4(0.0);
  float dz = texture(depth_tex, tex_coord).r;
  vec4 q = inv_camera * vec4(tex_coord * 2.0 - 1.0, max(dz, 1e-3) * 2.0 - 1.0, 1.0);
  if (abs(q.w) < 1e-8) {
    return;
  }
  vec3 rel = q.xyz / q.w;  // relatif a la camera (unites du jeu)
  vec3 cam = cam_m.xyz;
  vec3 P = rel / 4096.0 + cam;
  bool ciel = dz <= 1e-5;
  bool sur_mer = false;
  // direction du regard, independante de la profondeur
  vec4 qa = inv_camera * vec4(tex_coord * 2.0 - 1.0, 0.0, 1.0);
  vec4 qb = inv_camera * vec4(tex_coord * 2.0 - 1.0, 0.98, 1.0);
  vec3 I = normalize(qa.xyz / qa.w - qb.xyz / qb.w);
  // la surface de la mer (et la mer du large, dont la profondeur n'est pas exploitable) n'ecrit pas
  bool sans_normale = has_normal_tex > 0.5 && texture(normal_tex, tex_coord).a < 0.5;
  // vu a travers la mer (fond, ou rien derriere la surface)
  if (mer.y > 0.5 && cam.y > mer.x && (ciel || sans_normale || P.y < mer.x - 0.3)) {
    if (I.y < -0.002) {
      P = cam + I * ((mer.x - cam.y) / I.y);
      sur_mer = true;
    }
  }
  if (ciel && !sur_mer) {
    return;
  }
  vec3 L = normalize(lum_dir.xyz);
  if (L.y < 0.03) {
    return;
  }
  // la colonne de nuage qui se trouve sur le trajet du soleil (a mi-hauteur de la couche)
  vec3 B = P + L * ((0.5 * (NUAGE_BAS + NUAGE_HAUT) - P.y) / L.y);
  const int N = 8;
  float dt = (NUAGE_HAUT - NUAGE_BAS) / float(N);
  uint hh = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u;
  hh = (hh ^ (hh >> 16u)) * 0x45d9f3bu;
  hh = (hh ^ (hh >> 16u)) * 0x45d9f3bu;
  float j = float((hh ^ (hh >> 16u)) & 0xffffu) / 65536.0;
  float od = 0.0;
  for (int i = 0; i < N; i++) {
    od += densite(vec3(B.x, NUAGE_BAS + dt * (float(i) + j), B.z), cam_m.w) * dt;
  }
  // un voile fin (le soleil se voit au travers) ne fait qu'une ombre legere
  float ombre = 1.0 - exp(-od * 0.005);
  // sur l'EAU, l'ombre d'un nuage se voit a peine
  float d = length(P - cam);
  if (mer.y > 0.5 && (sur_mer || P.y < mer.x + 3.0)) {
    ombre *= 0.15 * (1.0 - smoothstep(150.0, 700.0, d));
  }
  // au tres loin, le decor se fond dans la brume
  ombre *= 1.0 - smoothstep(1500.0, 3000.0, d);
  // et quand le regard rase l'horizon (1,7 a 7 degres)
  ombre *= smoothstep(0.03, 0.12, -I.y);
  color = vec4(ombre * nuage_p.y, 0.0, 0.0, 1.0);
}
