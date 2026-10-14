#version 410 core

out vec4 color;

in vec4 fragment_color;
in vec3 tex_coord;
uniform float color_mult;
uniform float alpha_mult;

uniform vec4 fog_color;
uniform int bucket;

in float fog;

uniform sampler2D tex_T0;

// PC-REMAKE "Spyro water": x = time (s), y = strength (0 disables)
uniform vec4 water_fx;

// PC-REMAKE — mer bleue cartoon.
//
// L'océan ne lit aucune texture statique : sa texture est reconstruite à chaque
// image par le renderer, et sa couleur vient des couleurs de sommets. La seule
// prise, c'est donc ici.
//
// bucket 0 = couleur de surface, 1 = passe de transparence, 2 = reflets,
// 3 = surface proche. On teinte les passes colorées et on rend la mer OPAQUE :
// au large on ne doit plus voir le fond.
const vec3 kSeaDeep = vec3(0.04, 0.34, 0.86);  // bleu profond du large
const vec3 kSeaFilm = vec3(0.40, 0.76, 1.02);  // filtre : coupe le rouge

vec3 sea_blue(vec3 c) {
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  // on garde la lumière d'origine (crêtes, reflets du ciel, heure du jour)
  // et on remplace la teinte par un bleu franc — reglage VALIDE, ne plus y toucher
  vec3 tinted = c * kSeaFilm;
  vec3 deep = kSeaDeep * (0.42 + 0.88 * l);
  return mix(tinted, deep, 0.86);
}

void main() {
  vec2 uv = tex_coord.xy / tex_coord.z;
  vec4 T0 = texture(tex_T0, uv);
  // MODE IDENTIFICATION (17/08) : a rendu son verdict — la zone plate autour
  // du joueur = bucket 4 (magenta). Conservé éteint pour de futures chasses.
  const bool kIdentify = false;
  if (kIdentify) {
    vec3 dbg = (bucket == 0)   ? vec3(1.0, 0.1, 0.1)    // rouge
               : (bucket == 1) ? vec3(0.1, 1.0, 0.1)    // vert
               : (bucket == 2) ? vec3(0.15, 0.25, 1.0)  // bleu
               : (bucket == 3) ? vec3(1.0, 1.0, 0.1)    // jaune
                               : vec3(1.0, 0.1, 1.0);   // magenta
    color = vec4(dbg, 1.0);
    if (bucket == 1 || bucket == 2 || bucket == 4) {
      color.a = clamp((fragment_color * T0).a * 2.2, 0.3, 1.0);
    }
    return;
  }
  if (bucket == 0) {
    color.rgb = sea_blue(fragment_color.rgb * T0.rgb);
    // 19/08 v2 : l'alpha d'AUTEUR restaure — fragment_color.a arrive deja x2
    // du vertex shader, et T0.a porte la transparence de rivage voulue par les
    // artistes (retiree un temps de la formule : l'eau cachait les orbes).
    color.a = clamp(fragment_color.a * 0.5 * T0.a * 1.2, 0.0, 1.0);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  } else if (bucket == 1) {
    // passe de transparence (tuiles PROCHES) : même traitement « mer bleue »
    // que le large — c'était la seule passe sans sea_blue, d'où la frontière
    // en grille autour du joueur (17/08)
    color = fragment_color * T0;
    color.rgb = sea_blue(color.rgb);
    color.a = clamp(color.a * 2.2, 0.0, 1.0);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  } else if (bucket == 2 || bucket == 4) {
    // 2 = reflet du proche, 4 = reflet ADDITIF du large (restauré d'origine
    // après la fausse piste du 17/08)
    color = fragment_color * T0;
  } else if (bucket == 3) {
    color = fragment_color * T0;
    color.rgb = sea_blue(color.rgb);
    // EAU TRANSPARENTE (25/08) : le mid se dessine desormais APRES Jak, en
    // blending — cet alpha est une VRAIE transparence. Rampe de rivage des
    // artistes (fragment_color.a, table ocean-colors : 1 en eau libre, 0 aux
    // cellules cotieres) x rampe de distance sur les mips de T0 (le meme fondu
    // que les artistes utilisaient pour le voile near). Pres de la camera on
    // voit Jak et le fond a travers ; au loin la mer redevient pleine et cache
    // le vide de 2001.
    // (ancienne ligne, mid opaque : color.a = clamp(fragment_color.a * 0.25 * T0.a * 1.2, 0.0, 1.0);)
    float shore_a = clamp(fragment_color.a * 0.5, 0.0, 1.0);
    float lod = textureQueryLod(tex_T0, uv).y;
    float dist01 = clamp(lod * 0.7, 0.0, 1.0);
    color.a = shore_a * mix(0.40, 1.0, dist01);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  } else if (bucket == 5) {
    // depot d'alpha destination (passe B du mid transparent) : dose du reflet
    // additif bucket 4 — formule d'origine PS2, le MOTIF alphamod pleine force
    // (fragment_color.a arrive x2 du vertex shader : 0.502 x 2 = 1.0 en eau libre)
    color = vec4(0.0, 0.0, 0.0, clamp(fragment_color.a * T0.a, 0.0, 1.0));
  }
}
