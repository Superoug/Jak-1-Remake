#version 410 core
// CLARTE MOYENNE DE L'IMAGE (reflets du metal, post_processing.frag)

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
