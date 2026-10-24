#version 410 core
// PC-REMAKE 20/10 — flou gaussien separable (9 points) des rayons de soleil (shafts.frag).
in vec2 tex_coord;
out vec4 color;
uniform sampler2D src_tex;
uniform vec2 blur_step;  // 1 texel dans la direction du passage

void main() {
  // 22/10 : r = rayons du vrai soleil, g = faisceaux de canopee (shafts.frag)
  vec2 acc = texture(src_tex, tex_coord).rg * 0.2270270;
  acc += texture(src_tex, tex_coord + blur_step * 1.3846154).rg * 0.3162162;
  acc += texture(src_tex, tex_coord - blur_step * 1.3846154).rg * 0.3162162;
  acc += texture(src_tex, tex_coord + blur_step * 3.2307692).rg * 0.0702703;
  acc += texture(src_tex, tex_coord - blur_step * 3.2307692).rg * 0.0702703;
  color = vec4(acc, 0.0, 1.0);
}
