#version 410 core

out vec4 color;

flat in vec4 fragment_color;
in vec3 tex_coord;
flat in uvec2 tex_info;

uniform sampler2D tex_T0;
uniform float alpha_min;
uniform float alpha_max;
// PC-REMAKE : 0.5 sur les sprites ADDITIFS (fontaines d'éco, lueurs) — ils
// saturaient en boule blanche sous le pipeline moderne
uniform float additive_scale;

void main() {
  vec4 T0 = texture(tex_T0, tex_coord.xy);
  if (tex_info.y == 0) {
    T0.w = 1.0;
  }
  color = fragment_color * T0;

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  color.rgb *= additive_scale;
}
