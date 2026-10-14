#version 410 core
// PC-REMAKE 12/10 — BLOOM LARGE (« rendu remake »), passe de REDUCTION.
// Filtre 13 points (Jimenez 2014) : chaque niveau = la moitie du precedent. Le
// premier niveau trie ce qui RAYONNE vraiment :
//  - surfaces emissives (lave, ecos, cristaux — alpha 0.6-0.9 du G-buffer) :
//    toute leur lumiere ;
//  - PARTICULES (lueurs, spirales vertes, etincelles, portail) : reperees par
//    difference avec la scene d'AVANT les sprites (pre_tex) — leur exces au-dela
//    de 0.35, davantage si elles sont colorees ;
//  - le decor ordinaire : presque rien (v1 : l'herbe du village en plein soleil,
//    aussi claire et saturee qu'une lueur, voilait toute l'image) ;
//  - le ciel : rien (le soleil a son propre disque).
// Moyenne de Karis au premier niveau : un pixel isole tres clair ne scintille pas.

in vec2 tex_coord;
out vec4 color;

uniform sampler2D src_tex;     // unite 0 : niveau precedent (ou la scene)
uniform sampler2D depth_tex;   // unite 15 : profondeur resolue (ciel = 0)
uniform sampler2D normal_tex;  // unite 13 : G-buffer (alpha = drapeaux)
uniform sampler2D pre_tex;     // unite 12 : scene AVANT les sprites (particules absentes)
uniform vec4 bloom_p;          // x = premier niveau, y = profondeur, z = normales, w = pre_tex
uniform vec2 src_texel;        // 1 / taille de la source

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 bright(vec2 uv) {
  // texel EXACT : pre_tex est en NEAREST, la source en LINEAR — lues entre deux
  // texels elles differaient sur tout detail et tout passait pour particule
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
  if (bloom_p.w > 0.5) {
    vec3 dd = abs(c - texelFetch(pre_tex, ip, 0).rgb);
    part = smoothstep(0.02, 0.10, max(dd.r, max(dd.g, dd.b)));
  }
  part *= 1.0 - em;
  vec3 e = c * c * 2.2 * em;
  // fumee et vapeur (particules peu colorees) : halo reduit — un voile rose
  // couvrait la lave du village 3 (v1 : mix(0.35, 1.0, sat))
  // v3 : 1.8 -> 1.2 — les spirales vertes de l'etage de Samos se noyaient dans
  // leur halo (la reference garde des lignes nettes ET une lueur)
  // v4 (13/10, texture absente derriere la cascade) : une
  // cascade = des centaines de particules bleues superposees sur une grande
  // surface — leur halo cumule noyait la falaise dans un voile bleu. Seuil 0.35
  // -> 0.50 et poids 1.2 -> 0.7 ; les petites lueurs gardent leur halo.
  vec3 p = max(c - vec3(0.50), vec3(0.0)) * 0.7 * mix(0.15, 1.0, sat) * part;
  // 13/10 : decor ordinaire = seuil sur la LUMINANCE (l'herbe vert vif, canal
  // vert ~0.95, rayonnait et voilait la prairie)
  // (original : max(c - vec3(0.86), vec3(0.0)) * ...)
  float lc = dot(c, vec3(0.299, 0.587, 0.114));
  vec3 r = (lc > 1e-4 ? c * (max(lc - 0.86, 0.0) / lc) : vec3(0.0)) * 0.8 * (1.0 - em) * (1.0 - part);
  return (e + p + r) * (1.0 - sky);
}

vec3 tap(vec2 uv) {
  return texture(src_tex, uv).rgb;
}

void main() {
  vec2 t = src_texel;
  vec2 uv = tex_coord;
  vec3 o;
  if (bloom_p.x > 0.5) {
    // PREMIER NIVEAU (perf 12/10 : 13 points x 4 textures = 52 lectures par pixel,
    // 2,6 ms) : pre-filtre 2x2 — chaque texel plein ecran lu UNE fois (4 x 4
    // textures), tri de ce qui rayonne, moyenne de Karis contre le scintillement.
    vec3 b0 = bright(uv + t * vec2(-0.5, -0.5));
    vec3 b1 = bright(uv + t * vec2(0.5, -0.5));
    vec3 b2 = bright(uv + t * vec2(-0.5, 0.5));
    vec3 b3 = bright(uv + t * vec2(0.5, 0.5));
    float w0 = 1.0 / (1.0 + luma(b0));
    float w1 = 1.0 / (1.0 + luma(b1));
    float w2 = 1.0 / (1.0 + luma(b2));
    float w3 = 1.0 / (1.0 + luma(b3));
    o = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3) / (w0 + w1 + w2 + w3);
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
