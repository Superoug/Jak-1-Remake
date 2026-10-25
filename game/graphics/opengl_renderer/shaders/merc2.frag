#version 410 core

out vec4 color;
// PC-REMAKE 21/09 : G-buffer de normales (attachement 1) — rgb = normale monde
// *0.5+0.5, a = 1 quand le fragment en porte une (0 = le post reconstruit)
layout (location = 1) out vec4 out_normal;
in vec3 w_nor;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;
in vec3 v_nrm;
in vec4 v_rgba;
in vec3 v_pos;
in vec3 v_ref;
in vec3 v_refn;
uniform sampler2DArray remake_details;  // 21/10 v3 : cartes de detail (couche = famille)

// PC-REMAKE 21/10 : PERSONNAGES REMODELES — matiere de la texture (peau, tissu, cuir, metal,
// cheveux, fourrure, oeil, plumes, brillant), relief tire de la texture, reflet de la lumiere
// principale, lustre des tissus et des poils, contre-jour. 0 = rendu d'origine.
uniform float remake_perso;
uniform int remake_famille;
uniform float remake_force;
uniform float remake_split;  // essai : matieres seulement a droite de cette colonne (0 = partout)
uniform int reflet_orbe;     // 21/10 : orbe de precurseur — couleur propre assombrie (miroir)
// metal et verre (pc_reflect_texture_kind) : 0 rien, 0.5 metal, 1 metal poli, 2 verre
uniform float reflet_kind;
const int F_PEAU = 1, F_TISSU = 2, F_CUIR = 3, F_METAL = 4, F_CHEVEUX = 5, F_FOURRURE = 6,
          F_OEIL = 7, F_PLUME = 8, F_BRILLANT = 9, F_BOIS = 10, F_ECAILLE = 11, F_VEGETAL = 12,
          F_PIERRE = 13, F_CORNE = 14, F_CORDE = 15, F_GLUANT = 16;
// bruit de valeur 3D (cellules de l'espace de reference du modele : le detail suit le
// personnage quand il bouge)
float pc_h3(vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
float pc_bruit(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(pc_h3(i), pc_h3(i + vec3(1, 0, 0)), f.x),
                 mix(pc_h3(i + vec3(0, 1, 0)), pc_h3(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(pc_h3(i + vec3(0, 0, 1)), pc_h3(i + vec3(1, 0, 1)), f.x),
                 mix(pc_h3(i + vec3(0, 1, 1)), pc_h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// BOIS : forme irreguliere d'un rondin, d'un baton (unites du jeu, 4096 = 1 m)
float pc_bois_relief(vec3 r) {
  return (pc_bruit(r / 820.0) - 0.5) * 70.0 + (pc_bruit(r / 210.0) - 0.5) * 28.0;
}

#ifdef FUR
// fourrure en couches : couche (0 a la peau, 1 a la pointe), position de reference du sommet
in float v_couche;
in vec3 v_racine;
uniform vec4 fourrure;  // x = longueur, y = taille d'un brin (unites du jeu)
float hash3(vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
#endif

// relief sans tangentes (Mikkelsen) : h = hauteur (unites du jeu) du detail de la matiere
vec3 remake_relief(vec3 n, float h) {
  vec3 dpx = dFdx(v_pos), dpy = dFdy(v_pos);
  float dhx = dFdx(h), dhy = dFdy(h);
  vec3 r1 = cross(dpy, n), r2 = cross(n, dpx);
  float det = dot(dpx, r1);
  if (abs(det) < 1e-12) {
    return n;
  }
  vec3 g = sign(det) * (dhx * r1 + dhy * r2);
  return normalize(abs(det) * n - g);
}

// 21/10 v4 : DETAIL DES MATIERES par CARTES (tissu tisse, grain du cuir, pores, ecailles, metal
// brosse, meches, plumes, fibres du bois...) projetees sur le modele (trois plans, espace de
// reference : le detail suit le personnage). Les cartes sont des textures a mipmaps : nettes de
// pres, fondues proprement au loin. cav = assombrissement des creux.
// (v2 : bruit calcule par pixel — scintillement ; v3 : relief de la texture seul — trop discret)
float remake_carte(float tuile) {
  vec3 w = abs(normalize(v_refn + vec3(1e-4)));
  w = w * w * w * w;
  w /= (w.x + w.y + w.z);
  vec3 p = v_ref / tuile;
  float l = float(remake_famille);
  return w.x * texture(remake_details, vec3(p.yz, l)).r +
         w.y * texture(remake_details, vec3(p.xz, l)).r +
         w.z * texture(remake_details, vec3(p.xy, l)).r;
}
float remake_detail(vec4 tex, out float cav) {
  float lum = dot(tex.rgb, vec3(0.3, 0.59, 0.11));
  // tuile (unites du jeu, 4096 = 1 m), relief (unites du jeu), creux
  vec3 r = remake_famille == F_PEAU ? vec3(400.0, 1.5, 0.05) :
           remake_famille == F_TISSU ? vec3(200.0, 4.0, 0.22) :
           remake_famille == F_CUIR ? vec3(320.0, 4.0, 0.16) :
           remake_famille == F_METAL ? vec3(400.0, 1.2, 0.06) :
           remake_famille == F_CHEVEUX ? vec3(300.0, 3.0, 0.14) :
           remake_famille == F_FOURRURE ? vec3(260.0, 4.0, 0.2) :
           remake_famille == F_PLUME ? vec3(220.0, 3.0, 0.14) :
           remake_famille == F_BOIS ? vec3(700.0, 9.0, 0.22) :
           remake_famille == F_ECAILLE ? vec3(500.0, 6.0, 0.25) :
           remake_famille == F_VEGETAL ? vec3(400.0, 2.5, 0.12) :
           remake_famille == F_PIERRE ? vec3(600.0, 8.0, 0.2) :
           remake_famille == F_CORNE ? vec3(300.0, 2.0, 0.1) :
           remake_famille == F_CORDE ? vec3(300.0, 5.0, 0.25) : vec3(0.0);
  cav = 0.0;
  if (r.x <= 0.0) {
    return 0.0;
  }
  float d = remake_carte(r.x);
  cav = (1.0 - d) * r.z;
  return d * r.y + lum * r.y * 0.6;
}

uniform sampler2D tex_T0;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;
uniform vec3 light_dir2;
uniform vec4 light_col0;
uniform vec4 light_col1;
uniform vec4 light_col2;
uniform vec4 light_ambient;
// PC-REMAKE 22/09 : 1 = eclairage par pixel (Phong), 0 = Gouraud d'origine (A/B)
uniform float per_pixel_light;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

// PC-REMAKE: characters receive sun shadows
in vec4 sun_light_pos;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;

// 26/10 : NAPPE DE LAVE (lacs du village du volcan et de l'ogre) : la lave qui ondule du decor
// (tfrag3.frag), code commun recopie ci-dessous
uniform int lave_merc;
uniform float lava_time;
uniform vec3 lava_cam;
// 25/10 : JAK MOUILLE (Jak et Daxter, Merc2.cpp ; humidite posee par time-of-day.gc) : x = haut
// du corps, y = jambes, z = hauteur mouillee au-dessus des pieds, w = pieds (y monde, unites)
uniform vec4 jak_mouille;
uniform int mouille_ici;
uniform mat4 lava_world_from_merc;
in vec3 v_lava_w;
float lv_hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float lv_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(lv_hash(i), lv_hash(i + vec2(1.0, 0.0)), f.x),
             mix(lv_hash(i + vec2(0.0, 1.0)), lv_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// ---- 26/10 : LAVE QUI ONDULE (kind 2 ; la croute craquelee, kind 1, garde pc_lava). Fidele au
// dessin d'origine : la lave garde SA texture et ses couleurs ; seuls le mouvement et la lumiere
// changent. Une houle lente et lourde (lv_houle, meme code dans tfrag3.vert, ou elle souleve
// vraiment les sommets) ondule la surface : le dessin ondule avec elle, les creux sont un peu
// plus chauds, les cretes un peu plus sombres, et la texture coule lentement.
// metres, secondes -> hauteur (m) : trois trains lents (7 m, 4,3 m, 2,6 m ; 0,2 a 0,35 m/s)
float lv_houle(vec2 m, float t) {
  vec2 w = m + 0.9 * vec2(sin(m.y * 0.23 + t * 0.07), sin(m.x * 0.19 - t * 0.05));
  float h = 0.55 * sin(dot(w, vec2(0.85, 0.53)) * 0.90 - t * 0.30) +
            0.30 * sin(dot(w, vec2(-0.42, 0.91)) * 1.46 - t * 0.42 + 1.7) +
            0.15 * sin(dot(w, vec2(0.30, -0.95)) * 2.42 - t * 0.55 + 4.1);
  return h * 0.09;
}
// coeur commun (meme code dans merc2.frag pour les lacs de lave) : couleur de la texture animee,
// normale monde de la houle
vec3 lv_ondule(vec2 uv, vec3 pw, vec3 vue, float t, out vec3 n) {
  vec2 m = pw.xz / 4096.0;  // metres
  float h0 = lv_houle(m, t);
  vec2 gr = vec2(lv_houle(m + vec2(0.15, 0.0), t) - h0, lv_houle(m + vec2(0.0, 0.15), t) - h0) /
            0.15;
  n = normalize(vec3(-gr.x * 1.8, 1.0, -gr.y * 1.8));
  // coulee lente de la texture (deux phases qui se relaient, comme pc_lava) ; le dessin suit
  // la pente de la houle
  float a = lv_noise(m * 0.05 + vec2(3.1, 7.7)) * 6.2832 * 1.5;
  vec2 du = vec2(cos(a), sin(a)) * 0.03;
  float ph0 = fract(t * 0.12);
  float ph1 = fract(t * 0.12 + 0.5);
  float wa = abs(ph0 - 0.5) * 2.0;
  vec2 ond = gr * 0.035;
  vec3 F = mix(texture(tex_T0, uv - du * ph0 + ond).rgb, texture(tex_T0, uv - du * ph1 + ond).rgb,
               wa);
  // chaleur qui respire ; creux plus chauds, cretes plus sombres ; flancs tournes vers le haut
  // un peu plus clairs (a plat : 1)
  float ch = lv_noise(m * 0.2 + vec2(t * 0.05, -t * 0.035)) * 0.65 +
             lv_noise(m * 0.6 - vec2(t * 0.09, t * 0.06)) * 0.35;
  float k = (0.92 + 0.16 * ch) * (1.0 - h0 * 2.2);
  k *= 1.0 + 1.6 * (dot(n, vec3(0.2265, 0.9061, 0.3171)) - 0.9061);
  vec3 res = F * k;
  // reflet discret d'un liquide brillant, de la couleur de la lave
  vec3 v = normalize(-vue);
  vec3 R = reflect(-v, n);
  float gl = max(dot(R, vec3(0.2265, 0.9061, 0.3171)), 0.0);
  gl = gl * gl * gl * gl;
  gl = gl * gl * gl * gl;  // puissance 16
  res += (F * 0.5 + 0.25) * gl * 0.15;
  return res;
}

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz;
  if (abs(sun_light_pos.w) > 1e-5) {
    p /= sun_light_pos.w;
  }
  p = p * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float bias = 0.0075;  // characters are small: a touch more slack
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      // (original : px * 2.0) — 22/09 : penombre plus large, ombres moins dures
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px * 3.0, p.z - bias));
    }
  }
  // fondu de bordure : cohérent avec le décor (tfrag3.frag)
  vec2 edge = min(p.xy, 1.0 - p.xy);
  float rim = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

void main() {
#ifdef FUR
  // 21/10 v3 : FOURRURE DOUCE — chaque couche est semi-transparente (bord du brin adouci), surtout
  // visible sur les contours (duvet), discrete de face. (v2 : brins decoupes net — pointilles et
  // scintillement)
  float fur_alpha = 0.0;
  {
    // brins : cellules de l'espace de reference (stables quand le personnage bouge) ; un brin
    // s'affine vers la pointe et chacun a sa longueur
    // brin trop petit a l'ecran : pas de couche (sinon scintillement) — la texture suffit
    float px = max(length(fwidth(v_racine)), 1e-3);
    float vis = clamp((fourrure.y / px - 2.0) / 3.0, 0.0, 1.0);
    if (vis <= 0.0) {
      discard;
    }
    // grille tournee et deformee (v2 : grille alignee sur les axes — quadrillage visible de pres)
    const mat3 tourne = mat3(0.36, 0.48, -0.80, -0.80, 0.60, 0.00, 0.48, 0.64, 0.60);
    vec3 q = tourne * (v_racine / fourrure.y);
    q += (vec3(pc_bruit(q * 0.31), pc_bruit(q * 0.31 + 17.0), pc_bruit(q * 0.31 + 41.0)) - 0.5) * 1.6;
    vec3 c = floor(q);
    vec3 f = fract(q) - 0.5;
    float r = hash3(c);
    f += (vec3(hash3(c + 3.7), hash3(c + 9.1), hash3(c + 5.3)) - 0.5) * 0.7;
    float rayon = (1.0 - v_couche / (0.45 + 0.55 * r)) * 0.5;
    if (rayon <= 0.0) {
      discard;
    }
    float d = length(f);
    float aa = max(fwidth(d), 0.04);
    fur_alpha = (1.0 - smoothstep(rayon - aa, rayon + aa, d)) * vis;
    if (fur_alpha < 0.01) {
      discard;
    }
  }
#endif
  // PC-REMAKE 22/09 : le calcul de lumiere de la VU1, refait PAR PIXEL sur la
  // normale interpolee (meme formule que merc2.vert, meme resultat aux sommets)
  vec4 lit_color = vtx_color;
  bool remake = remake_perso > 0.5 && gl_FragCoord.x >= remake_split;
  vec4 Tr = (gfx_hack_no_tex == 0) ? texture(tex_T0, vtx_st) : vec4(0.5);
  vec3 n = normalize(v_nrm);
  float cavite = 0.0;
#ifndef FUR
  if (remake) {
    // 21/10 : relief des matieres (trame, grain, meches, ecorce), en unites du jeu
    float h = remake_detail(Tr, cavite);
    if (h != 0.0) {
      n = remake_relief(n, h * remake_force);
    }
  }
#endif
  if (per_pixel_light > 0.5) {
    vec3 li = light_dir0_fade.xyz * n.x + light_dir1_fade_en.xyz * n.y + light_dir2 * n.z;
    li = max(li, vec3(0.0));
    vec4 lc = light_ambient + li.x * light_col0 + li.y * light_col1 + li.z * light_col2;
    lit_color = v_rgba * lc;
  }
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = Tr;
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = lit_color * T0 * 2;
    } else {
      color = T0;
    }
    color.a *= 2;
  } else {
    color.rgb = lit_color.rgb;

    if (decal_enable == 0) {
      color.a = lit_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  // 21/10 : PERSONNAGES REMODELES — reflet de la lumiere principale, lustre, contre-jour
  if (remake) {
    // direction de la camera, dans la convention de la normale (opposee aux positions du moteur ;
    // v1 : normalize(-v_pos) — contre-jour a pleine force partout, couleurs delavees)
    vec3 V = normalize(v_pos);
    vec3 L0 = vec3(light_dir0_fade.x, light_dir1_fade_en.x, light_dir2.x);
    float l0 = length(L0);
    float nv = max(dot(n, V), 0.0);
    vec3 base = color.rgb;
    vec3 ajout = vec3(0.0);
    if (l0 > 1e-4) {
      L0 /= l0;
      float nl = max(dot(n, L0), 0.0);
      float nh = max(dot(n, normalize(L0 + V)), 0.0);
      float ks = 0.0, sh = 1.0;
      if (remake_famille == F_PEAU) { ks = 0.10; sh = 20.0; }
      else if (remake_famille == F_TISSU) { ks = 0.03; sh = 8.0; }
      else if (remake_famille == F_CUIR) { ks = 0.22; sh = 40.0; }
      else if (remake_famille == F_METAL) { ks = 0.30; sh = 50.0; }
      else if (remake_famille == F_CHEVEUX) { ks = 0.22; sh = 34.0; }
      else if (remake_famille == F_FOURRURE) { ks = 0.05; sh = 10.0; }
      else if (remake_famille == F_OEIL) { ks = 0.90; sh = 300.0; }
      else if (remake_famille == F_PLUME) { ks = 0.08; sh = 16.0; }
      else if (remake_famille == F_BRILLANT) { ks = 0.40; sh = 60.0; }
      else if (remake_famille == F_BOIS) { ks = 0.04; sh = 10.0; }
      else if (remake_famille == F_ECAILLE) { ks = 0.25; sh = 40.0; }
      else if (remake_famille == F_VEGETAL) { ks = 0.10; sh = 18.0; }
      else if (remake_famille == F_PIERRE) { ks = 0.05; sh = 12.0; }
      else if (remake_famille == F_CORNE) { ks = 0.30; sh = 45.0; }
      else if (remake_famille == F_CORDE) { ks = 0.03; sh = 8.0; }
      else if (remake_famille == F_GLUANT) { ks = 0.60; sh = 80.0; }
      ajout += light_col0.rgb * (ks * pow(nh, sh) * nl * 2.0);
    }
    // lustre des tissus, poils et plumes, et contre-jour : dans la COULEUR de la surface
    // (v1 : ajout de gris — couleurs delavees, plus ternes que le jeu d'origine)
    float lustre = remake_famille == F_TISSU ? 0.12 : remake_famille == F_FOURRURE ? 0.25 :
                   remake_famille == F_PLUME ? 0.15 : 0.0;
    float clair = clamp(dot(light_ambient.rgb + light_col0.rgb * 0.5, vec3(0.33)), 0.0, 1.0);
    ajout += base * (clair * (lustre * pow(1.0 - nv, 2.5) + 0.18 * pow(1.0 - nv, 3.0)));
    color.rgb += ajout * remake_force;
    // creux du detail (ecorce, grain, trame) un peu plus sombres
    color.rgb *= 1.0 - cavite * remake_force;
  }

  // 21/10 : orbe — un metal poli renvoie peu de lumiere diffuse : le reflet (emerc) domine
  if (reflet_orbe == 1) {
    color.rgb *= 0.55;
  }

  // 25/10 : JAK MOUILLE. L'eau remplit les pores : le tissu fonce beaucoup (et se sature un peu),
  // la peau un peu, le metal et les yeux pas du tout ; un film d'eau brille (reflet net de la
  // lumiere principale, lustre sur les bords). Sous la hauteur ou ses jambes ont trempe, Jak
  // reste mouille plus longtemps (les jambes sechent apres le haut du corps).
  if (mouille_ici == 1 && (jak_mouille.x > 0.001 || jak_mouille.y > 0.001)) {
    vec4 wh = lava_world_from_merc * vec4(v_pos, -1.0);
    float h_pied = wh.y / wh.w - jak_mouille.w;
    float sous = 1.0 - smoothstep(jak_mouille.z - 0.05 * 4096.0, jak_mouille.z + 0.05 * 4096.0, h_pied);
    float mouille = mix(jak_mouille.x, max(jak_mouille.x, jak_mouille.y), sous);
    int fam = remake ? remake_famille : 0;
    float poro = (fam == F_PEAU) ? 0.30 : (fam == F_TISSU) ? 1.0 : (fam == F_CUIR) ? 0.75 :
                 (fam == F_CHEVEUX) ? 0.85 : (fam == F_FOURRURE) ? 0.95 :
                 (fam == F_METAL || fam == F_OEIL || fam == F_BRILLANT) ? 0.0 : 0.70;
    float k = mouille * poro;
    vec3 gris = vec3(dot(color.rgb, vec3(0.299, 0.587, 0.114)));
    // assombri (jusqu'a -48 % pour un tissu trempe) et un peu plus sature
    color.rgb = mix(color.rgb, mix(gris, color.rgb, 1.18) * 0.52, k);
    // film d'eau (espace de la camera du moteur, comme l'eclairage par pixel) : un lustre large
    // et doux plutot que des points durs
    vec3 nw = normalize(v_nrm);
    vec3 vw = normalize(-v_pos);
    vec3 l0 = normalize(vec3(light_dir0_fade.x, light_dir1_fade_en.x, light_dir2.x));
    float sp = max(dot(reflect(-l0, nw), vw), 0.0);
    sp = sp * sp * sp * sp;  // puissance 4
    sp = sp * sp * sp;       // puissance 12
    float bord = 1.0 - max(dot(nw, vw), 0.0);
    bord = bord * bord * bord;
    float film = mouille * mix(0.45, 0.8, 1.0 - poro * 0.5);
    color.rgb += (light_col0.rgb * sp * 0.35 + vec3(0.14, 0.16, 0.18) * bord * 0.45) * film;
    // gouttes : petites perles brillantes accrochees au personnage (reperes du modele, elles
    // le suivent), seulement quand il est encore bien trempe
    if (mouille > 0.45) {
      vec3 q = v_ref * (1.0 / 90.0);
      vec3 c = floor(q);
      vec3 f = fract(q) - 0.5;
      float r = fract(sin(dot(c, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
      if (r < 0.10) {
        vec3 o = vec3(fract(r * 37.0), fract(r * 91.0), fract(r * 53.0)) - 0.5;
        float d = length(f - o * 0.5);
        float goutte = 1.0 - smoothstep(0.08, 0.16, d);
        float eclat = pow(max(dot(reflect(-l0, nw), vw), 0.0), 6.0);
        color.rgb += vec3(0.9, 0.95, 1.0) * goutte * (0.25 + 0.75 * eclat) * 0.45 *
                     smoothstep(0.45, 0.9, mouille);
      }
    }
  }

  // PC-REMAKE: darken the character when it stands in the sun's shadow
  color.xyz *= 1.0 - sun_shadow_factor();
  // 26/10 : nappe de lave : sa texture d'origine, animee (coulee, houle) — meme eclairage
  vec3 lave_n = vec3(0.0, 1.0, 0.0);
  if (lave_merc == 1 && gfx_hack_no_tex == 0 && decal_enable == 0) {
    vec3 res = lv_ondule(vtx_st, v_lava_w, v_lava_w - lava_cam, lava_time, lave_n);
    color.rgb = lit_color.rgb * res * 2.0 * (1.0 - sun_shadow_factor());
  }
#ifdef FUR
  // racine dans l'ombre des autres poils, pointe eclairee ; couverture : duvet sur les contours
  color.rgb *= mix(0.6, 1.1, v_couche);
  {
    float nvf = abs(dot(normalize(v_nrm), normalize(v_pos)));
    color.a = fur_alpha * mix(0.9, 0.2, nvf);
  }
#endif

   color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
  {
    float nl = length(w_nor);
    // drapeau des reflets du post : 250/255 metal poli, 252/255 metal, 254/255 verre
    // (original : vec4(w_nor / nl * 0.5 + 0.5, 1.0))
    float tag = reflet_kind > 1.5 ? 254.0 / 255.0 : reflet_kind > 0.75 ? 250.0 / 255.0 :
                reflet_kind > 0.25 ? 252.0 / 255.0 : 1.0;
    out_normal = (nl > 0.05) ? vec4(w_nor / nl * 0.5 + 0.5, tag) : vec4(0.0);
    // 26/10 : lave : la normale de la houle (le post eclaire les vagues)
    if (lave_merc == 1) {
      out_normal = vec4(lave_n * 0.5 + 0.5, 1.0);
    }
  }
}
