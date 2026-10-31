#version 410 core
// ANTICRENELAGE de l'image finale (type FXAA)

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;  // image du jeu apres le post (resolution du jeu, filtre lineaire)
uniform vec2 texel;  // 1 / resolution du jeu

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec3 cM = texture(src_tex, tex_coord).rgb;
  float lM = luma(cM);
  float lNW = luma(texture(src_tex, tex_coord + vec2(-0.5, -0.5) * texel).rgb);
  float lNE = luma(texture(src_tex, tex_coord + vec2(0.5, -0.5) * texel).rgb);
  float lSW = luma(texture(src_tex, tex_coord + vec2(-0.5, 0.5) * texel).rgb);
  float lSE = luma(texture(src_tex, tex_coord + vec2(0.5, 0.5) * texel).rgb);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  // pas de bord (contraste faible, relatif a la clarte)
  if (lMax - lMin < max(0.04, lMax * 0.14)) {
    color = vec4(cM, 1.0);
    return;
  }
  // direction du bord (perpendiculaire au gradient)
  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
  float reduc = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
  float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduc);
  dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * texel;
  vec3 cA = 0.5 * (texture(src_tex, tex_coord + dir * (1.0 / 3.0 - 0.5)).rgb +
                   texture(src_tex, tex_coord + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 cB = cA * 0.5 + 0.25 * (texture(src_tex, tex_coord - dir * 0.5).rgb +
                               texture(src_tex, tex_coord + dir * 0.5).rgb);
  float lB = luma(cB);
  // le moyennage large ne doit pas sortir de la plage locale
  color = vec4((lB < lMin || lB > lMax) ? cA : cB, 1.0);
}
