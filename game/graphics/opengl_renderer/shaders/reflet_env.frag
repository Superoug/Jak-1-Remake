#version 410 core
// PC-REMAKE — CARTE D'ENVIRONNEMENT DES REFLETS (metal, verre ; post_processing.frag).
// Panorama latitude-longitude autour de la camera, rempli par ce que l'ecran montre : chaque
// texel est une direction ; si elle est a l'ecran, la couleur de la scene y est melangee
// (moyenne glissante, couverture dans l'alpha), sinon le texel garde ce qu'il a deja vu. En
// tournant, la carte se remplit de tout l'entourage (ciel, decor, murs d'une piece) : une
// surface qui fait face a la camera y trouve ce qui est derriere celle-ci.
// Melange : rgb = src * a + dst * (1 - a), a = a + dst.a * (1 - a) ; la couleur se lit rgb / a.

in vec2 tex_coord;
out vec4 color;

uniform sampler2D scene_tex;     // scene sans HUD
uniform sampler2D depth_tex;     // profondeur resolue (inversee : 0 = ciel)
uniform mat4 clip_from_world;    // relatif a la camera
uniform mat4 inv_camera;
uniform vec4 cam_trans;
uniform float env_mix;           // part de l'image actuelle

const float PI = 3.14159265;

void main() {
  float lon = (tex_coord.x * 2.0 - 1.0) * PI;
  float lat = (tex_coord.y - 0.5) * PI;
  vec3 d = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
  vec4 c = clip_from_world * vec4(d * (4096.0 * 1000.0), 1.0);
  if (c.w <= 1e-4) {
    discard;
  }
  vec2 uv = (c.xy / c.w) * 0.5 + 0.5;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    discard;
  }
  // le texel couvre ~1,4 degre : moyenne de 5 points de l'ecran (pas de scintillement)
  vec2 o = vec2(0.006, 0.010);
  vec3 s = texture(scene_tex, uv).rgb * 2.0;
  s += texture(scene_tex, clamp(uv + vec2(o.x, 0.0), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv - vec2(o.x, 0.0), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv + vec2(0.0, o.y), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv - vec2(0.0, o.y), 0.0, 1.0)).rgb;
  s /= 6.0;
  // ce qui est tout pres de la camera (Jak, le decor frole) n'est pas l'environnement
  float w = 1.0;
  float dz = texture(depth_tex, uv).r;
  if (dz > 1e-5) {
    vec4 p = inv_camera * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
    float dist = abs(p.w) > 1e-8 ? length(p.xyz / p.w) : 1e9;
    w = smoothstep(4096.0 * 4.0, 4096.0 * 10.0, dist);
  }
  vec2 b = min(uv, 1.0 - uv);
  w *= smoothstep(0.0, 0.05, min(b.x, b.y));
  color = vec4(s, env_mix * w);
}
