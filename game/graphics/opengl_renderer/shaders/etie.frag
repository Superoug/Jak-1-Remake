#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
in vec3 v_env_nrm;
in vec3 v_env_pos;

// 13/10 : le reflet (passe ADDITIVE) ne recevait pas l'ombre du soleil : complet
// sur toute la surface, il effacait les ombres des palmes sur les objets
// precurseurs. Meme carte d'ombres que le decor (tfrag3.frag), 3x3 PCF.
in vec4 sun_light_pos;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;
float env_sun_shadow() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px * 3.0, p.z - 0.004));
    }
  }
  vec2 edge = min(p.xy, 1.0 - p.xy);
  float rim = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

// PC-REMAKE 13/10 (reflets corrects mais POINTS parasites sur les objets
// precurseurs) : la coordonnee du reflet (sphere d'environnement)
// etait calculee aux SOMMETS puis etiree entre eux — sur un modele de 2001 a gros
// triangles, chaque sommet piochait une tache claire de la sphere et
// l'interpolation dessinait une grille de losanges et de points. Meme calcul que
// le microcode (etie.vert), mais PAR PIXEL : le reflet glisse en douceur.
vec2 envmap_uv(vec3 nrm, vec3 pt) {
  vec3 n = nrm;
  n.z -= 1.0;
  float nrm_dot = dot(pt, n);
  vec3 rfl = pt * n.z;
  float q = -0.5 / max(length(rfl), 1e-6);
  n.xy *= nrm_dot;
  n.xy += rfl.xy;
  return n.xy * q + 0.5;
}
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    // coordonnee PAR SOMMET (comme le jeu) : par pixel, les normales des modeles
    // de 2001 (non partagees entre triangles) faisaient des facettes sombres
    // (essai 13/10). Les points venaient de la teinte des milieux de
    // subdivision, corrigee dans Tfrag3Data.cpp.
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
    // 20/10 : reflets precurseur renforces (murs du temple de la jungle, objets) — la passe
    // de reflet est additive : x2.2 pour les reflets faibles, avec une epaule pour les forts
    // (les sols clairs viraient au blanc) (original : x1)
    color.rgb *= 2.2 / (1.0 + 1.1 * dot(color.rgb, vec3(0.299, 0.587, 0.114)));
    // a l'ombre il reste le reflet du ciel (30 %)
    color.rgb *= 1.0 - 0.7 * env_sun_shadow();
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // PC-REMAKE 22/09 : ALPHA AFFUTE pour l'alpha-to-coverage (MSAA). Les alphas
  // doux des textures (feuillage, herbe) devenaient une COUVERTURE partielle =
  // feuilles et touffes semi-transparentes laissant voir le decor derriere
  //. Interieur opaque, transition anti-aliasee sur ~1 px au seuil.
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
