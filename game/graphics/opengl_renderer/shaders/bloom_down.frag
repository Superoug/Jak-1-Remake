#version 410 core
// BLOOM LARGE (rendu remake), passe de REDUCTION

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;  // unite 0 : niveau precedent (ou la scene)
uniform sampler2D depth_tex;  // unite 15 : profondeur resolue (ciel = 0)
uniform sampler2D normal_tex;  // unite 13 : G-buffer (alpha = drapeaux)
uniform sampler2D pre_tex;  // unite 12 : scene AVANT les sprites (particules absentes)
uniform vec4 bloom_p;  // x = premier niveau, y = profondeur, z = normales, w = pre_tex
uniform vec2 src_texel;  // 1 / taille de la source
uniform float bloom_night;  // part de nuit (0..1)

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 bright(vec2 uv) {
  // texel EXACT
  ivec2 sz = textureSize(src_tex, 0);
  ivec2 ip = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - ivec2(1));
  vec3 c = texelFetch(src_tex, ip, 0).rgb;
  float maxc = max(c.r, max(c.g, c.b));
  float minc = min(c.r, min(c.g, c.b));
  float sat = maxc > 1e-4 ? (maxc - minc) / maxc : 0.0;
  float em = 0.0;
  if (bloom_p.z > 0.5) {
    float a = texture(normal_tex, uv).a;
    em = (a > 0.6 && a < 0.9) ? 1.0 : 0.0;
  }
  float sky = 0.0;
  if (bloom_p.y > 0.5) {
    sky = texture(depth_tex, uv).r < 1e-5 ? 1.0 : 0.0;
  }
  float part = 0.0;
  vec3 pre = vec3(0.0);
  if (bloom_p.w > 0.5) {
    pre = texelFetch(pre_tex, ip, 0).rgb;
    vec3 dd = abs(c - pre);
    part = smoothstep(0.02, 0.10, max(dd.r, max(dd.g, dd.b)));
  }
  part *= 1.0 - em;
  vec3 e = c * c * 2.2 * em;
  // fumee et vapeur (particules peu colorees)
  vec3 p = max(c - max(vec3(0.50), pre), vec3(0.0)) * 0.7 * mix(0.15, 1.0, sat) * part;
  // decor ordinaire = seuil sur la LUMINANCE
  float lc = dot(c, vec3(0.299, 0.587, 0.114));
  vec3 r = (lc > 1e-4 ? c * (max(lc - 0.86, 0.0) / lc) : vec3(0.0)) * 0.8 * (1.0 - em) * (1.0 - part);
  // la nuit, les surfaces emissives et les particules colorees
  e *= 1.0 + 1.2 * bloom_night;
  p *= 1.0 + 3.0 * bloom_night;
  // compression PAR PIXEL (voir main)
  r *= 1.0 / (1.0 + 0.75 * luma(r));
  vec3 ep = e + p;
  float lep = luma(ep);
  ep *= lep > 3.0 ? (3.0 + (lep - 3.0) / (1.0 + 0.5 * (lep - 3.0))) / lep : 1.0;
  return (ep + r) * (1.0 - sky);
}

vec3 tap(vec2 uv) {
  return texture(src_tex, uv).rgb;
}

void main() {
  vec2 t = src_texel;
  vec2 uv = tex_coord;
  vec3 o;
  if (bloom_p.x > 0.5) {
    // PREMIER NIVEAU (perf 13 points x 4 textures = 52 lectures par pixel, 2,6 ms)
    vec3 b0 = bright(uv + t * vec2(-0.5, -0.5));
    vec3 b1 = bright(uv + t * vec2(0.5, -0.5));
    vec3 b2 = bright(uv + t * vec2(-0.5, 0.5));
    vec3 b3 = bright(uv + t * vec2(0.5, 0.5));
    o = (b0 + b1 + b2 + b3) * 0.25;
  } else {
    vec3 a = tap(uv + t * vec2(-2.0, -2.0));
    vec3 b = tap(uv + t * vec2(0.0, -2.0));
    vec3 c = tap(uv + t * vec2(2.0, -2.0));
    vec3 d = tap(uv + t * vec2(-2.0, 0.0));
    vec3 e = tap(uv);
    vec3 f = tap(uv + t * vec2(2.0, 0.0));
    vec3 g = tap(uv + t * vec2(-2.0, 2.0));
    vec3 h = tap(uv + t * vec2(0.0, 2.0));
    vec3 i = tap(uv + t * vec2(2.0, 2.0));
    vec3 j = tap(uv + t * vec2(-1.0, -1.0));
    vec3 k = tap(uv + t * vec2(1.0, -1.0));
    vec3 l = tap(uv + t * vec2(-1.0, 1.0));
    vec3 m = tap(uv + t * vec2(1.0, 1.0));
    o = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
  }
  color = vec4(max(o, vec3(0.0)), 1.0);
}
