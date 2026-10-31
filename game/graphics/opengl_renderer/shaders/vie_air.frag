#version 410 core
// VIE DANS L'AIR (voir vie_air.vert)

in vec4 v_couleur;
in vec3 v_forme;
in float v_dist;
out vec4 color;

uniform sampler2D depth_tex;  // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform sampler2D scene_tex;  // unite 12 : scene sans HUD (clarte du fond)
uniform mat4 inv_camera;
uniform int vie_sorte;
uniform vec2 vie_ecran;  // taille de la cible (px)

void main() {
  vec2 uv = gl_FragCoord.xy / vie_ecran;
  float dz = texture(depth_tex, uv).r;
  // fondu sur les 40 cm devant le decor
  float doux = 1.0;
  if (dz > 1e-5) {
    vec4 q = inv_camera * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
    float scene = abs(q.w) > 1e-8 ? length(q.xyz / q.w) / 4096.0 : 1e9;
    doux = smoothstep(0.03, 0.40, scene - v_dist);
    if (doux <= 0.0) {
      discard;  // derriere le decor
    }
  }
  vec2 pc = gl_PointCoord * 2.0 - 1.0;
  if (vie_sorte == 0 || vie_sorte == 2) {
    float r = length(pc);
    float coeur = 1.0 - smoothstep(v_forme.z * 0.3, v_forme.z, r);
    float halo = v_forme.z < 0.999 ? pow(1.0 - smoothstep(0.0, 1.0, r), 2.0) * 0.35 : 0.0;
    // une poussiere ne se voit que sur un fond plus sombre qu'elle
    float fond = dot(texture(scene_tex, uv).rgb, vec3(0.299, 0.587, 0.114));
    float vu = v_forme.z < 0.999 ? 1.0 : 1.0 - 0.85 * smoothstep(0.35, 0.85, fond);
    if (dz <= 1e-5 && v_forme.z > 0.999) {
      vu *= 0.12;  // devant le ciel : a peine visible
    }
    color = vec4(v_couleur.rgb * (coeur + halo) * vu * doux, 0.0);
  } else {
    float cs = cos(v_forme.x), sn = sin(v_forme.x);
    vec2 q = vec2(cs * pc.x - sn * pc.y, sn * pc.x + cs * pc.y);
    float e = length(q / vec2(1.0, max(0.5 * v_forme.y, 0.08)));
    float a = (1.0 - smoothstep(0.7, 1.0, e)) * v_couleur.a * doux;
    if (a < 0.002) {
      discard;
    }
    color = vec4(v_couleur.rgb, a);
  }
}
