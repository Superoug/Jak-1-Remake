#version 410 core

out vec4 color;
// PC-REMAKE 21/09 : G-buffer de normales (attachement 1) — rgb = normale monde
// *0.5+0.5, a = 1 quand le fragment en porte une (0 = le post reconstruit)
layout (location = 1) out vec4 out_normal;
in vec3 w_nor;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// PC-REMAKE 13/10 : ombre du soleil (memes reglages que le decor, tfrag3.frag)
in vec4 sun_light_pos;
uniform vec3 sun_dir_w;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;
float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float nl = length(w_nor);
  float slope = nl > 0.05 ? 1.0 - clamp(abs(dot(w_nor / nl, normalize(sun_dir_w))), 0.0, 1.0) : 0.5;
  float bias = 0.0016 + 0.0060 * slope * slope;
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px * 3.0, p.z - bias));
    }
  }
  vec2 edge = min(p.xy, 1.0 - p.xy);
  float rim = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}


void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // QA 22/09 : draws a decoupe peints en couleur pleine (tfrag=rouge, tie_wind=bleu,
  // etie=jaune) — si le decor se voit A TRAVERS cette couleur pleine, le melange
  // ne vient pas de la couleur du fragment (couverture MSAA ou post)

  // PC-REMAKE 22/09 : ALPHA AFFUTE pour l'alpha-to-coverage (MSAA). Les alphas
  // doux des textures (feuillage, herbe) devenaient une COUVERTURE partielle =
  // feuilles et touffes semi-transparentes laissant voir le decor derriere
  //. Interieur opaque, transition anti-aliasee sur ~1 px au seuil.
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  color.rgb *= 1.0 - sun_shadow_factor();
  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));

  {
    float nl = length(w_nor);
    out_normal = (nl > 0.05) ? vec4(w_nor / nl * 0.5 + 0.5, 1.0) : vec4(0.0);
  }
}
