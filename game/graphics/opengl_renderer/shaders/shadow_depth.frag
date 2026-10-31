#version 410 core

// sun shadow pass: depth only

in vec2 uv;
uniform sampler2D tex_T0;
uniform float alpha_min;  // 0 = opaque caster, >0 = alpha-tested caster

void main() {
  if (alpha_min > 0.001) {
    if (texture(tex_T0, uv).a < alpha_min) {
      discard;
    }
  }
}
