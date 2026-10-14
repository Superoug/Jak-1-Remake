#version 410 core
// PC-REMAKE 12/10 — BLOOM LARGE, passe de REMONTEE : filtre tente 3x3 du niveau
// plus petit, AJOUTE (melange additif) au niveau plus grand. Au bout de la chaine
// le niveau 1/2 porte la somme de tous les rayons : petits halos nets + grand
// voile doux, comme une vraie lentille.

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;  // unite 0 : niveau plus petit
uniform vec2 src_texel;     // 1 / taille de la source
uniform float up_weight;    // poids de ce niveau dans la somme

void main() {
  vec2 t = src_texel;
  vec2 uv = tex_coord;
  vec3 s = texture(src_tex, uv + t * vec2(-1.0, -1.0)).rgb;
  s += texture(src_tex, uv + t * vec2(0.0, -1.0)).rgb * 2.0;
  s += texture(src_tex, uv + t * vec2(1.0, -1.0)).rgb;
  s += texture(src_tex, uv + t * vec2(-1.0, 0.0)).rgb * 2.0;
  s += texture(src_tex, uv).rgb * 4.0;
  s += texture(src_tex, uv + t * vec2(1.0, 0.0)).rgb * 2.0;
  s += texture(src_tex, uv + t * vec2(-1.0, 1.0)).rgb;
  s += texture(src_tex, uv + t * vec2(0.0, 1.0)).rgb * 2.0;
  s += texture(src_tex, uv + t * vec2(1.0, 1.0)).rgb;
  color = vec4(s * (up_weight / 16.0), 1.0);
}
