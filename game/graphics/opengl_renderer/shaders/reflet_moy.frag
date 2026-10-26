#version 410 core
// PC-REMAKE — CLARTE MOYENNE DE L'IMAGE (reflets du metal, post_processing.frag) : 64 points de
// la scene sans HUD, dans une texture de 1 x 1. Le metal module sa clarte autour de cette
// moyenne, et le reflet de secours (direction jamais vue) prend la clarte de ce qu'on voit.

out vec4 color;
uniform sampler2D scene_tex;

void main() {
  vec3 acc = vec3(0.0);
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      acc += textureLod(scene_tex, (vec2(x, y) + 0.5) / 8.0, 3.0).rgb;
    }
  }
  color = vec4(acc / 64.0, 1.0);
}
