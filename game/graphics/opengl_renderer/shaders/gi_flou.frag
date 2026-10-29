#version 410 core
// PC-REMAKE — flou de la lumiere rebondie (gi_rebond.frag) : gaussien separable sur 9 points,
// qui respecte la profondeur (pas de halo d'un objet sur le decor derriere lui).
in vec2 tex_coord;
out vec4 color;
uniform sampler2D src_tex;
uniform sampler2D depth_tex;  // unite 15
uniform vec2 blur_step;       // 1 texel dans la direction du passage

void main() {
  float dc = texture(depth_tex, tex_coord).r;
  const float wk[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
  vec3 acc = texture(src_tex, tex_coord).rgb * wk[0];
  float ws = wk[0];
  for (int i = 1; i < 5; i++) {
    for (int s = -1; s <= 1; s += 2) {
      vec2 uv = tex_coord + blur_step * float(i * s) * 1.5;
      float d = texture(depth_tex, uv).r;
      float w = wk[i] * (1.0 - smoothstep(0.03, 0.10, abs(d - dc) / max(dc, 1e-6)));
      acc += texture(src_tex, uv).rgb * w;
      ws += w;
    }
  }
  color = vec4(acc / max(ws, 1e-4), 1.0);
}
