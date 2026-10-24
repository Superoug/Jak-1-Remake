#version 410 core

// 17/10 : deux sorties pour le melange a DEUX SOURCES de la surface (bucket 3) :
// final = color + fond * color_t (l'eau FILTRE le fond au lieu de le recouvrir)
layout(location = 0, index = 0) out vec4 color;
layout(location = 0, index = 1) out vec4 color_t;

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

// 17/10 (v0.0.3) : retour au TURQUOISE des artistes — le bleu roi uni ne collait pas a
// l'esprit de Jak and Daxter. Les reflets (post) et la transparence de rivage restent.
// Ancien reglage (bleu franc, valide en aout) :
//   float l = dot(c, vec3(0.299, 0.587, 0.114));
//   vec3 tinted = c * kSeaFilm;
//   vec3 deep = kSeaDeep * (0.42 + 0.88 * l);
//   return mix(tinted, deep, 0.86);
vec3 sea_blue(vec3 c) {
  return c;
}

// 17/10 : brume de l'eau posee ICI, au dessin de la surface (voir CommonOceanRenderer.cpp).
uniform sampler2D water_depth;  // profondeur de la scene juste avant l'eau
uniform mat4 water_inv_camera;  // meme reconstruction que le post (world_from_depth)
uniform vec4 water_cam;
uniform vec4 water_murk;  // x = active, y = densite par metre, zw = taille de la cible
// couleur de sommet moyenne de la PLEINE MER (meme echelle que fragment_color au bucket 3)
uniform vec4 water_deep;

vec3 water_world(vec2 uv, float d) {
  vec4 p = water_inv_camera * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  if (abs(p.w) < 1e-8) {
    return water_cam.xyz;
  }
  return p.xyz / p.w + water_cam.xyz;
}

// PC-REMAKE : CARTES DU RIVAGE (distance signee au trait de cote, m, > 0 en mer ; outils/rivages.py).
// La mer d'origine monte et descend de quelques dizaines de centimetres : sur la plage, ses
// polygones depassaient alors sur le sable en marches d'escalier. La surface s'efface sur la
// terre ; la nappe des vagues (post) dessine l'eau sur le sable.
uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
uniform vec4 riv_box[2];
uniform int riv_n;
float riv_dist(vec2 xz_m) {
  float d = 99.0;
  if (riv_n > 0) {
    vec2 uv = (xz_m - riv_box[0].xy) * riv_box[0].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = texture(riv_tex0, uv).r;
    }
  }
  if (riv_n > 1) {
    vec2 uv = (xz_m - riv_box[1].xy) * riv_box[1].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = min(d, texture(riv_tex1, uv).r);
    }
  }
  return d;
}
// part d'eau de ce point de la surface (1 = mer, 0 = sur la terre)
float water_on_land() {
  if (riv_n == 0) {
    return 1.0;
  }
  vec3 ps = water_world(gl_FragCoord.xy / water_murk.zw, gl_FragCoord.z);
  float d = riv_dist(ps.xz / 4096.0);
  return d > 90.0 ? 1.0 : smoothstep(-0.6, 0.4, d);
}

// metres d'eau traverses par le regard, de la surface au fond (rien derriere = tres profond)
// 17/10 v2 : borne a 2 x la profondeur VERTICALE — vu en rasant, le trajet devenait tres long
// (2 m de fond = 20 m de trajet) et la brume noyait les hauts-fonds (rochers peints sans texture)
// (v1 : return length(fond - surface) / 4096.0;)
float water_column_m() {
  vec2 uv = gl_FragCoord.xy / water_murk.zw;
  float db = texture(water_depth, uv).r;
  if (db < 1e-6) {
    return 1000.0;
  }
  vec3 pb = water_world(uv, db);
  vec3 ps = water_world(uv, gl_FragCoord.z);
  float path = length(pb - ps) / 4096.0;
  float depth = max(ps.y - pb.y, 0.0) / 4096.0;
  return min(path, depth * 2.0);
}

// 22/10 : TRANSMISSION ADOUCIE. Sous l'eau, le terrain d'origine a des marches nettes (le banc
// de sable clair s'arrete d'un coup sur un fond de 6 m) : vue a travers l'eau, cette arete
// faisait une nappe claire a bords droits. La transmission (ce qui passe du fond, par canal) est
// moyennee sur un disque d'environ 4 m autour du point (2 anneaux de 8 tirages, rayon a l'ecran
// selon la distance). Le centre pese double.
vec3 water_transmission_soft(vec3 k) {
  vec2 uv0 = gl_FragCoord.xy / water_murk.zw;
  vec3 ps = water_world(uv0, gl_FragCoord.z);
  float dist = max(length(ps - water_cam.xyz) / 4096.0, 1.0);
  // ~4 m a l'ecran (champ vertical d'environ 60 degres)
  float rpx = clamp(4.0 / dist * water_murk.w * 0.95, 0.0, 260.0);
  vec3 acc = 2.0 * exp(-water_column_m() * k);
  float wsum = 2.0;
  for (int k2 = 0; k2 < 16; k2++) {
    float a = 0.7854 * float(k2) + (k2 < 8 ? 0.39 : 0.0);
    float r = k2 < 8 ? 1.0 : 0.5;
    vec2 uv = uv0 + vec2(cos(a), sin(a)) * (rpx * r) / water_murk.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      continue;
    }
    float db = texture(water_depth, uv).r;
    float col = 1000.0;
    if (db > 1e-6) {
      vec3 pb = water_world(uv, db);
      // un objet AU-DESSUS de l'eau (rocher emerge, Jak) ne compte pas
      if (pb.y > ps.y) {
        continue;
      }
      // le fond voisin, ramene sous CE point de la surface (profondeur verticale x 2)
      col = max(ps.y - pb.y, 0.0) / 4096.0 * 2.0;
    }
    acc += exp(-col * k);
    wsum += 1.0;
  }
  return acc / wsum;
}

void main() {
  color_t = vec4(0.0);
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
    // passe de transparence (tuiles PROCHES) : même traitement mer bleue
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
    // 17/10 : surface presque transparente pres de la camera (0.40 -> 0.12) : l'eau peu
    // profonde laisse voir le fond (rochers, sable) avec sa texture, filtre par l'epaisseur
    // d'eau (voir plus bas) ; au loin la surface redevient pleine (original : mix(0.40, 1.0, dist01))
    color.a = shore_a * mix(0.12, 1.0, dist01);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
    // 17/10 : le fond vu a travers se perd dans l'eau selon l'epaisseur traversee. L'eau
    // profonde M = la pleine mer des artistes A CET ENDROIT (couleur de sommet du large x
    // texture locale : memes vagues), un peu plus sombre — et non la couleur locale, noire
    // pres des cotes ou le large est transparent. Rendu en une passe :
    //   final = a*S + (1-a)*(f*M + (1-f)*fond)
    // = alpha a' = a + (1-a)*f, couleur (a*S + (1-a)*f*M) / a'.
    // (1re version : M = 0.92*S — eau noire autour du Rocher du Geyser)
    if (water_murk.x > 0.5) {
      float a = color.a;
      // (garde : couleur par defaut si le moteur ne l'a pas encore fournie)
      vec3 wd = dot(water_deep.rgb, vec3(1.0)) > 0.01 ? water_deep.rgb : vec3(0.30, 0.62, 0.66);
      vec3 deep = sea_blue(wd * T0.rgb);
      deep = mix(deep, fog_color.rgb, clamp(fog_color.a * fog, 0, 1)) * 0.92;
      // 17/10 v2 : l'eau FILTRE le fond, par canal (le rouge s'eteint en premier, le
      // bleu-vert porte loin) : T = transmission sur l'epaisseur traversee. Le fond garde
      // sa texture, teinte ; ce qui est absorbe est remplace par l'eau profonde.
      //   final = a*S + (1-a)*(fond*T + deep*(1-T))  — melange a deux sources (C++).
      // (v1, fondu : couleur (a*S + (1-a)*f*deep)/a', alpha a' = a + (1-a)*f — les rochers
      //  sous l'eau n'etaient plus qu'un aplat cyan)
      // 22/10 : le fond vu a travers garde son arete (sable clair contre fond profond) : on
      // l'efface en approchant de la marche — la transmission descend jusqu'a 0 au bord du banc
      // (moyenne = moitie de la locale) et reste entiere a 4 m du bord (moyenne = locale)
      // (original : vec3 T = exp(-water_column_m() * water_murk.y * vec3(1.6, 0.55, 0.45));)
      vec3 Tc = exp(-water_column_m() * water_murk.y * vec3(1.6, 0.55, 0.45));
      vec3 Ts = water_transmission_soft(water_murk.y * vec3(1.6, 0.55, 0.45));
      vec3 T = clamp(2.0 * Ts - Tc, vec3(0.0), Tc);
      color.rgb = a * color.rgb + (1.0 - a) * deep * (1.0 - T);
      color_t = vec4((1.0 - a) * T, 0.0);
      color.a = 1.0;
      // PC-REMAKE : sur la terre, plus d'eau (couleur nulle, fond transmis entier)
      float k = water_on_land();
      color.rgb *= k;
      color_t = mix(vec4(1.0, 1.0, 1.0, 0.0), color_t, k);
    }
  } else if (bucket == 5) {
    // depot d'alpha destination (passe B du mid transparent) : dose du reflet
    // additif bucket 4 — formule d'origine PS2, le MOTIF alphamod pleine force
    // (fragment_color.a arrive x2 du vertex shader : 0.502 x 2 = 1.0 en eau libre)
    // PC-REMAKE : pas de reflet du large sur la terre (original : sans le facteur water_on_land)
    color = vec4(0.0, 0.0, 0.0, clamp(fragment_color.a * T0.a, 0.0, 1.0) * water_on_land());
  }
}
