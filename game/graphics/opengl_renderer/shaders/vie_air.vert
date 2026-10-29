#version 410 core
// PC-REMAKE — VIE DANS L'AIR : de petites choses flottent dans le monde autour de la camera,
// dessinees en points par-dessus la passe d'eclairage du post (avant le bloom et l'etalonnage).
// Chaque particule a une place fixe dans le MONDE (tirage par numero) qui derive avec le vent,
// repetee tous les BOITE metres : on dessine l'exemplaire le plus proche de la camera (les
// particules ne suivent pas la camera). Sortes (vie_sorte) :
//   0 : poussieres et pollen (s'allument au soleil, surtout a contre-jour) ; au-dessus des vagues
//       qui deferlent, des embruns (27/10 : plus dessinee, voir pc_vie_run) ;
//   1 : feuilles qui tombent en tournoyant (jungle, sous les arbres seulement) ;
//   2 : lucioles (la nuit, en foret et au village autour des lampes) : boite plus grande
//       (36 x 14 x 36 m), de petites lueurs qui s'allument et s'eteignent, avec un halo.

uniform mat4 clip_from_world;  // relatif a la camera
uniform vec4 cam_trans;
uniform vec4 vie_fx;     // x = dose, y = jungle (0..1), z = temps (s), w = taille d'un pixel (rad)
uniform vec4 vie_ciel;   // x = jour (0..1), y = nuit, z = interieur, w = soleil present
uniform vec4 sun_dir;    // vers le soleil
uniform int vie_sorte;
uniform sampler2DShadow sun_shadow_tex;
uniform mat4 sun_light_vp;
uniform float sun_map_ok;
// mer : x = niveau (unites du jeu), y = 1 si mer, z = temps des vagues (water_glitter.w)
uniform vec4 vie_mer;
uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
uniform vec4 riv_box[2];
uniform int riv_n;
// lucioles : x = lieu (0 rien, 1 village, 2 foret), y = nombre de lampes ; positions (m)
uniform vec4 vie_zone;
uniform vec4 vie_lampes[32];
uniform vec2 vie_ecran;  // taille de la cible (px), comme vie_air.frag

out vec4 v_couleur;  // rgb, a = opacite (feuilles) ou dose (points lumineux)
out vec3 v_forme;    // x = angle de la feuille, y = aplatissement, z = halo (lucioles)
out float v_dist;    // distance a la camera (m), pour le test de profondeur

const float M = 4096.0;

vec3 h3(float n) {
  vec3 p = fract(vec3(n) * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yzx + 33.33);
  return fract((p.xxy + p.yzz) * p.zyx);
}
float sh_hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float sh_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(sh_hash(i), sh_hash(i + vec2(1.0, 0.0)), f.x),
             mix(sh_hash(i + vec2(0.0, 1.0)), sh_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// meme phase des vagues que le post (post_processing.frag, sh_phase)
float sh_phase(vec2 xz_m, float t) {
  float n = sh_noise(xz_m * 0.045) * 1.7 + sh_noise(xz_m * 0.13 + 3.7) * 0.35;
  return fract(t / 7.0 + n);
}
float riv_dist(vec2 xz_m) {
  float d = 99.0;
  if (riv_n > 0) {
    vec2 uv = (xz_m - riv_box[0].xy) * riv_box[0].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = textureLod(riv_tex0, uv, 0.0).r;
    }
  }
  if (riv_n > 1) {
    vec2 uv = (xz_m - riv_box[1].xy) * riv_box[1].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = min(d, textureLod(riv_tex1, uv, 0.0).r);
    }
  }
  return d;
}
float soleil(vec3 q) {
  if (sun_map_ok < 0.5) {
    return 1.0;
  }
  vec3 sp = (sun_light_vp * vec4(q, 1.0)).xyz * 0.5 + 0.5;
  if (sp.x < 0.01 || sp.x > 0.99 || sp.y < 0.01 || sp.y > 0.99 || sp.z > 1.0) {
    return 1.0;
  }
  return texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
}

void main() {
  float id = float(gl_VertexID);
  float t = vie_fx.z;
  vec3 h = h3(id + (vie_sorte == 1 ? 7919.0 : vie_sorte == 2 ? 15331.0 : 0.0));
  vec3 cam = cam_trans.xyz / M;
  vec3 BOITE = vie_sorte == 1 ? vec3(30.0) : vie_sorte == 2 ? vec3(36.0, 14.0, 36.0) : vec3(22.0);
  // place dans le monde + derive (vent ; les feuilles tombent) + petit balancement
  vec3 vent = vie_sorte == 1 ? vec3(0.04, -0.32, 0.025)
              : vie_sorte == 2 ? vec3(0.03, 0.0, 0.02) : vec3(0.11, 0.012, 0.07);
  vec3 wp = h * BOITE + vent * t * (0.7 + 0.6 * h3(id + 3.0).x);
  vec3 g = h3(id + 11.0);
  wp += (vie_sorte == 1 ? 0.25 : vie_sorte == 2 ? 0.6 : 0.12) *
        vec3(sin(t * (0.5 + g.x) + g.y * 6.28), sin(t * (0.4 + g.y) + g.z * 6.28) * 0.6,
             cos(t * (0.45 + g.z) + g.x * 6.28));
  // exemplaire le plus proche de la camera
  vec3 p = wp - BOITE * floor((wp - cam) / BOITE + 0.5);
  vec3 v = p - cam;
  float d = length(v);
  v_dist = d;
  // fondu vers le bord de la boite (pas d'apparition brusque quand une particule s'y repete)
  vec3 rel = abs(v) / BOITE;
  float bord = 1.0 - smoothstep(0.30, 0.48, max(rel.x, max(rel.y, rel.z)));
  float proche = smoothstep(0.25, 0.8, d);

  vec4 cc = clip_from_world * vec4(p * M - cam_trans.xyz, 1.0);
  gl_Position = cc;
  float pix_m = max(d * vie_fx.w, 1e-4);  // taille d'un pixel a cette distance (m)

  v_couleur = vec4(0.0);
  v_forme = vec3(0.0);
  float jour = vie_ciel.x, nuit = vie_ciel.y, inter = vie_ciel.z;
  vec3 L = sun_dir.xyz;

  if (vie_sorte == 0) {
    float taille = 0.016;
    vec3 coul = vec3(0.0);
    float halo = 0.0;
    // embruns : au-dessus de l'eau tout pres du bord, quand la vague casse la
    float hm = p.y - vie_mer.x / M;
    bool embrun = false;
    if (vie_mer.y > 0.5 && hm > -0.1 && hm < 1.8 && riv_n > 0) {
      float drm = riv_dist(p.xz);
      if (drm > -1.0 && drm < 5.0) {
        embrun = true;
        float ph = sh_phase(p.xz, vie_mer.z);
        float casse = smoothstep(0.30, 0.45, ph) * (1.0 - smoothstep(0.55, 0.75, ph)) *
                      (1.0 - smoothstep(1.0, 4.5, abs(drm - 1.5))) * (1.0 - smoothstep(0.6, 1.8, hm));
        taille = 0.022;
        coul = vec3(0.95, 0.98, 1.0) * casse * mix(0.25, 1.0, jour) * 1.4;
      }
    }
    if (!embrun) {
      if (h.y < 0.55) {
        // poussiere / pollen : eclairee par le soleil, tres visible a contre-jour
        float vis = vie_ciel.w > 0.5 ? soleil(p * M) : 0.0;
        float contre = pow(max(dot(normalize(v), L), 0.0), 5.0);
        coul = vec3(1.0, 0.94, 0.80) *
               (vis * vie_ciel.w * jour * (1.0 - inter) * (0.45 + 2.2 * contre) + 0.06 * (1.0 - nuit));
      }
    }
    // au moins un pixel et demi ; plus petite, la particule pale (energie conservee)
    float r = max(taille, pix_m * 1.5);
    float energie = max((taille * taille) / (r * r), 0.30);
    gl_PointSize = halo > 0.5 ? min(2.0 * max(0.18, r * 4.0) / pix_m, 48.0) : 2.0 * r / pix_m;
    v_forme = vec3(0.0, 0.0, halo > 0.5 ? r / max(0.18, r * 4.0) : 1.0);
    v_couleur = vec4(coul * energie * bord * proche * vie_fx.x, 1.0);
    if (dot(coul, vec3(1.0)) < 1e-3) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // rien a dessiner
    }
  } else if (vie_sorte == 2) {
    // luciole : lueur qui s'allume et s'eteint lentement, avec un halo. 27/10 (moins nombreuses)
    // : en foret, une sur trois, plutot sous la camera ; au village, autour des lampes
    // (6 m) et quelques-unes ailleurs ; jamais pres de la mer (plage) ni ailleurs.
    // (v1 : partout la nuit, toutes)
    // (27/10 : lueur plus douce, ne s'eteint jamais tout a fait ; v1 : 0.15 + 0.85 * ... ^2)
    float cl = 0.40 + 0.60 * pow(0.5 + 0.5 * sin(t * (0.5 + 0.6 * h.z) + h.x * 40.0), 2.0);
    float bas = 1.0 - smoothstep(-1.0, 2.5, p.y - cam.y);
    float garde = 0.0;
    if (vie_zone.x > 1.5) {
      garde = step(h.z, 0.12) * bas;
    } else if (vie_zone.x > 0.5) {
      float pres = 0.0;
      for (int k = 0; k < 32; k++) {
        if (float(k) >= vie_zone.y) {
          break;
        }
        vec3 dl = p - vie_lampes[k].xyz;
        dl.y *= 1.6;  // autour de la lampe, pas tres haut au-dessus
        pres = max(pres, 1.0 - smoothstep(2.0, 5.5, length(dl)));
      }
      // 27/10 : FONDU (une luciole qui derivait au bord de la zone d'une lampe s'allumait ou
      // s'eteignait d'un coup) ; meme nombre en moyenne
      // (original : max(step(h.z, pres), step(h.z, 0.02) * bas))
      garde = max(clamp((pres - h.z) / 0.12 + 0.5, 0.0, 1.0), step(h.z, 0.02) * bas);
    }
    // plage, bord de l'eau (bas et a moins de 8 m de la mer), et jamais au-dessus de la mer
    // (27/10 : bords fondus ; original : garde = 0 des que
    //  (p.y - mer < 4 et drm > -8) ou (0 < drm < 90))
    if (garde > 0.0 && riv_n > 0) {
      float drm = riv_dist(p.xz);
      float plage = (1.0 - smoothstep(3.5, 4.5, p.y - vie_mer.x / M)) * smoothstep(-9.0, -7.0, drm);
      float mer = smoothstep(-0.5, 0.5, drm) * (1.0 - smoothstep(89.0, 91.0, drm));
      garde *= 1.0 - max(plage, mer);
    }
    vec3 coul = vec3(0.75, 1.0, 0.30) * cl * 5.0 * smoothstep(0.4, 0.8, nuit) * (1.0 - inter) * garde;
    float taille = 0.05;
    float r = max(taille, pix_m * 1.5);
    float rh = max(0.30, r * 5.0);
    gl_PointSize = min(2.0 * rh / pix_m, 64.0);
    v_forme = vec3(0.0, 0.0, r / rh);
    // 27/10 : un point disparait EN ENTIER quand son centre sort de l'ecran — le halo d'une
    // luciole proche (jusqu'a 64 pixels) s'eteignait d'un coup au bord en tournant la camera :
    // fondu des que le halo touche le bord
    float ecran = 1.0;
    if (cc.w > 1e-4) {
      vec2 ndc = abs(cc.xy / cc.w);
      vec2 mr = max(vec2(gl_PointSize) / max(vie_ecran, vec2(1.0)), vec2(1e-3));
      ecran = (1.0 - smoothstep(1.0 - mr.x, 1.0, ndc.x)) * (1.0 - smoothstep(1.0 - mr.y, 1.0, ndc.y));
    }
    v_couleur = vec4(coul * max((taille * taille) / (r * r), 0.30) * bord * proche * vie_fx.x * ecran, 1.0);
    if (dot(coul, vec3(1.0)) < 1e-3 || nuit < 0.4) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
  } else {
    // feuille : seulement sous les arbres (a l'ombre de la canopee), jungle
    float vis = soleil(p * M);
    float sous_bois = 1.0 - smoothstep(0.35, 0.75, vis);
    float a = sous_bois * vie_fx.y * bord * proche * vie_fx.x * 0.9;
    // feuille fine : la lumiere la traverse (vert-jaune vif)
    vec3 coul = mix(vec3(0.46, 0.66, 0.16), vec3(0.80, 0.68, 0.22), h.z) * (0.75 + 0.6 * vis) *
                mix(0.5, 1.0, jour);
    float r = max(0.09, pix_m);
    gl_PointSize = min(2.0 * r / pix_m, 64.0);
    // rotation dans l'image et amincissement (la feuille tourne sur elle-meme)
    v_forme = vec3(t * (1.5 + 2.0 * g.y) + g.z * 6.28,
                   0.25 + 0.75 * abs(sin(t * (2.2 + g.x * 2.0) + g.y * 9.0)), 0.0);
    v_couleur = vec4(coul, a);
    if (a < 1e-3) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
  }
}
