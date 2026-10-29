#version 410 core
// PC-REMAKE 27/10 — BLOOM LARGE, STABILISATION dans le temps. Le halo de cette image est
// melange a celui de l'image precedente, recale sur le mouvement de la camera (profondeur de
// la scene). Une lueur qui ne dure qu'une image (eclair de soudure, etincelle, petite lampe
// qui saute d'un pixel en tournant) ne fait plus pulser tout son halo ; la moyenne, donc la
// clarte, ne change pas.

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;           // unite 0 : bloom de cette image (niveau 1/2)
uniform sampler2D hist_tex;          // unite 6 : bloom stabilise de l'image precedente
uniform sampler2D depth_tex;         // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform mat4 inv_camera;             // ecran -> monde relatif a la camera (cette image)
uniform mat4 prev_clip_from_world;   // monde relatif a la camera -> ecran (image precedente)
uniform vec3 cam_delta;              // camera de cette image - camera precedente
uniform float stab_w;                // poids de cette image (1 = sans historique)

void main() {
  vec3 cur = texture(src_tex, tex_coord).rgb;
  if (stab_w >= 0.999) {
    color = vec4(cur, 1.0);
    return;
  }
  float dz = texture(depth_tex, tex_coord).r;
  bool ciel = dz <= 1e-5;
  vec4 q = inv_camera * vec4(tex_coord * 2.0 - 1.0, (ciel ? 1e-3 : dz) * 2.0 - 1.0, 1.0);
  if (abs(q.w) < 1e-8) {
    color = vec4(cur, 1.0);
    return;
  }
  vec3 p = q.xyz / q.w;
  // ciel : une direction (a 1 km, la translation de la camera n'y change rien)
  p = ciel ? normalize(p) * (4096.0 * 1000.0) : p + cam_delta;
  vec4 c = prev_clip_from_world * vec4(p, 1.0);
  if (c.w <= 1e-4) {
    color = vec4(cur, 1.0);
    return;
  }
  vec2 puv = (c.xy / c.w) * 0.5 + 0.5;
  if (puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) {
    color = vec4(cur, 1.0);
    return;
  }
  vec3 hist = texture(hist_tex, puv).rgb;
  color = vec4(mix(hist, cur, stab_w), 1.0);
}
