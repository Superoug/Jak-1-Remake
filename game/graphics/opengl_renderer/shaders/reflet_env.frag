#version 410 core
// PC-REMAKE — CARTE D'ENVIRONNEMENT DES REFLETS (metal, verre ; post_processing.frag).
// Panorama latitude-longitude autour de la camera, rempli par ce que l'ecran montre : chaque
// texel est une direction ; si elle est a l'ecran, la couleur de la scene y est melangee
// (moyenne glissante), sinon le texel garde ce qu'il a deja vu. En tournant, la carte se
// remplit de tout l'entourage (ciel, decor, murs d'une piece) : une surface qui fait face a la
// camera y trouve ce qui est derriere celle-ci.
// Deux sorties (lecture de l'image precedente, ecriture de la nouvelle) :
//   out_env : rgb = couleur multipliee par a, a = confiance ;
//   out_pos : position de la camera (m) quand la direction a ete vue. Une direction vue d'un
//   endroit dont la camera s'est eloignee est perimee (ascenseur, entree dans une grotte) : sa
//   confiance tombe (ici meme, pour que le flou des niveaux de mip reste doux), elle est
//   remplacee des qu'elle revient a l'ecran, et le post s'en detourne.
// (v1 : une seule carte melangee par le GPU, remise a zero seulement sur un saut de 30 m — en
//  descendant le puits du temple, les parois refletaient encore le ciel vu d'en haut)

in vec2 tex_coord;
layout (location = 0) out vec4 out_env;
layout (location = 1) out vec4 out_pos;

uniform sampler2D prev_env;      // panorama de l'image precedente
uniform sampler2D prev_pos;      // positions de capture de l'image precedente
uniform sampler2D scene_tex;     // scene sans HUD
uniform sampler2D depth_tex;     // profondeur resolue (inversee : 0 = ciel)
uniform mat4 clip_from_world;    // relatif a la camera
uniform mat4 inv_camera;
uniform vec4 cam_trans;
uniform float env_mix;           // part de l'image actuelle pour une direction deja connue

const float PI = 3.14159265;

void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 pe = texelFetch(prev_env, ip, 0);
  vec4 pp = texelFetch(prev_pos, ip, 0);
  vec3 cam_m = cam_trans.xyz / 4096.0;
  // confiance de ce qui est deja la : nulle quand la camera s'est eloignee de plus de 20 m
  // (min : ne remonte pas si la camera revient, la direction sera simplement revue)
  float conf = min(pe.a, 1.0 - smoothstep(6.0, 20.0, length(cam_m - pp.xyz)));
  out_env = pe.a > 1e-4 ? pe * (conf / pe.a) : vec4(0.0);
  out_pos = pp;

  float lon = (tex_coord.x * 2.0 - 1.0) * PI;
  float lat = (tex_coord.y - 0.5) * PI;
  vec3 d = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
  vec4 c = clip_from_world * vec4(d * (4096.0 * 1000.0), 1.0);
  if (c.w <= 1e-4) {
    return;
  }
  vec2 uv = (c.xy / c.w) * 0.5 + 0.5;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    return;
  }
  // le texel couvre ~0,7 degre : moyenne de 5 points de l'ecran (pas de scintillement)
  vec2 o = vec2(0.003, 0.005);
  vec3 s = texture(scene_tex, uv).rgb * 2.0;
  s += texture(scene_tex, clamp(uv + vec2(o.x, 0.0), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv - vec2(o.x, 0.0), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv + vec2(0.0, o.y), 0.0, 1.0)).rgb;
  s += texture(scene_tex, clamp(uv - vec2(0.0, o.y), 0.0, 1.0)).rgb;
  s /= 6.0;
  // ce qui est tout pres de la camera (Jak, le decor frole) compte peu, sauf pour remplir une
  // direction vide (sol et murs proches : sinon des trous, refletes par le repli)
  // (v1 : rien sous 4 a 10 m)
  float w = 1.0;
  float dz = texture(depth_tex, uv).r;
  if (dz > 1e-5) {
    vec4 p = inv_camera * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
    float dist = abs(p.w) > 1e-8 ? length(p.xyz / p.w) : 1e9;
    w = mix(smoothstep(4096.0 * 1.5, 4096.0 * 6.0, dist), 1.0, 1.0 - conf);
  }
  vec2 b = min(uv, 1.0 - uv);
  w *= smoothstep(0.0, 0.05, min(b.x, b.y));
  if (w <= 0.0) {
    return;
  }
  // direction inconnue ou perimee : remplacee aussitot ; connue : moyenne glissante
  vec3 prev_col = pe.a > 1e-4 ? pe.rgb / pe.a : s;
  float r = w * max(env_mix, 1.0 - conf);
  vec3 col = mix(prev_col, s, r);
  float a = mix(conf, 1.0, w);
  out_env = vec4(col * a, a);
  out_pos = vec4(mix(pp.xyz, cam_m, max(r, 1.0 - conf)), 1.0);
}
