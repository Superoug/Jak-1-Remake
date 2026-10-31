#version 410 core
// METEO ENRICHIE (voir meteo.vert)

in vec4 v_couleur;
in vec3 v_forme;
in float v_dist;
out vec4 color;

uniform sampler2D depth_tex;  // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform sampler2D scene_tex;  // unite 12 : scene sans HUD
uniform mat4 inv_camera;
uniform int meteo_sorte;
uniform vec2 meteo_ecran;  // taille de la cible (px)

void main() {
  vec2 uv = gl_FragCoord.xy / meteo_ecran;
  float dz = texture(depth_tex, uv).r;
  float doux = 1.0;
  if (dz > 1e-5) {
    vec4 q = inv_camera * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
    float scene = abs(q.w) > 1e-8 ? length(q.xyz / q.w) / 4096.0 : 1e9;
    doux = smoothstep(0.02, 0.25, scene - v_dist);
    if (doux <= 0.0) {
      discard;  // derriere le decor
    }
  }
  float a;
  if (meteo_sorte == 0) {
    // travers : couverture du pixel (trait d'une demi-largeur v_forme.z)
    float travers = clamp(v_forme.z + 0.5 - abs(v_forme.x), 0.0, 1.0);
    float y = v_forme.y;
    float lng = smoothstep(0.0, 0.12, y) * 0.35 + 0.65;
    lng *= 1.0 - smoothstep(0.35, 1.0, y);
    a = travers * lng;
  } else {
    float r = length(gl_PointCoord * 2.0 - 1.0);
    a = 1.0 - smoothstep(0.3, 1.0, r);
  }
  a *= v_couleur.a * doux;
  if (a < 0.002) {
    discard;
  }
  vec3 c = v_couleur.rgb;
  if (meteo_sorte == 1) {
    // un flocon n'assombrit jamais le fond
    c = max(c, texture(scene_tex, uv).rgb);
  }
  color = vec4(c, a);
}
