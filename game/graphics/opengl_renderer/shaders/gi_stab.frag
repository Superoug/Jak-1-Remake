#version 410 core
// PC-REMAKE 27/10 — LUMIERE REBONDIE, STABILISATION dans le temps. Le motif d'echantillonnage
// de gi_rebond.frag est fixe a l'ecran : en tournant la camera, le decor glisse dessous et la
// lumiere rebondie fourmillait autour des lampes. Chaque image est melangee a la precedente,
// recalee sur le mouvement de la camera (profondeur) ; un point qui vient d'etre decouvert (sa
// distance ne correspond pas a celle retenue) repart de l'image actuelle. Alpha = distance a la
// camera (m) du point retenu.

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;           // unite 0 : rebond de cette image (flou)
uniform sampler2D hist_tex;          // unite 6 : rebond stabilise de l'image precedente
uniform sampler2D depth_tex;         // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform mat4 inv_camera;             // ecran -> monde relatif a la camera (cette image)
uniform mat4 prev_clip_from_world;   // monde relatif a la camera -> ecran (image precedente)
uniform vec3 cam_delta;              // camera de cette image - camera precedente
uniform float stab_w;                // poids de cette image (1 = sans historique)

void main() {
  vec3 cur = texture(src_tex, tex_coord).rgb;
  float dz = texture(depth_tex, tex_coord).r;
  if (dz <= 1e-5) {
    color = vec4(cur, 0.0);
    return;
  }
  vec4 q = inv_camera * vec4(tex_coord * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
  if (abs(q.w) < 1e-8) {
    color = vec4(cur, 0.0);
    return;
  }
  vec3 p = q.xyz / q.w;
  float dist = length(p) / 4096.0;
  if (stab_w >= 0.999) {
    color = vec4(cur, dist);
    return;
  }
  vec3 pp = p + cam_delta;
  vec4 c = prev_clip_from_world * vec4(pp, 1.0);
  if (c.w <= 1e-4) {
    color = vec4(cur, dist);
    return;
  }
  vec2 puv = (c.xy / c.w) * 0.5 + 0.5;
  if (puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) {
    color = vec4(cur, dist);
    return;
  }
  vec4 hist = texture(hist_tex, puv);
  float attendu = length(pp) / 4096.0;
  // meme point ? (sinon : decouvert, ou bord d'objet)
  float meme = 1.0 - smoothstep(0.03, 0.08, abs(hist.a - attendu) / max(attendu, 0.5));
  float w = mix(1.0, stab_w, meme);
  color = vec4(mix(hist.rgb, cur, w), dist);
}
