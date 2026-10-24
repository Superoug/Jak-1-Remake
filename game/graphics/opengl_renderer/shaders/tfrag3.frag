#version 410 core

out vec4 color;
// PC-REMAKE 21/09 : G-buffer de normales (attachement 1) — rgb = normale monde
// *0.5+0.5, a = 1 quand le fragment en porte une (0 = le post reconstruit)
layout (location = 1) out vec4 out_normal;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
// PC-REMAKE : >0 = surface auto-lumineuse (lave, braises, lueurs précurseurs)
uniform float emissive_boost;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// PC-REMAKE sun shadows
in vec4 sun_light_pos;
in vec3 w_nor;
flat in float v_foliage;
uniform vec3 sun_dir_w;  // direction normalisée VERS la surface (des ombres)
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;
// 22/10 : dose du relief des surfaces (voir plus bas ; 0 = rien)
uniform float relief_amt;
// 22/10 : LAVE ANIMEE (voir pc_lava) — 0 = rien, 1 = croute craquelee, 2 = lave en fusion
uniform float lava_kind;
uniform float wind_time;  // horloge (s), posee par TFragment pour tout le programme

// PC-REMAKE "Spyro water": x = time (s), y = strength (1 on WATER trees)
uniform vec4 water_fx;
// 21/09 : detail intrinseque des sols (0 = rien, 1 = plein), fondu par distance
uniform float detail_amt;
in float v_dist;
in vec3 v_pos_w;
// PC-REMAKE : SCULPTURE DU DECOR — carte de hauteur par texture (outil sculpture_decor.py) : joints
// creuses, planches, pierres et rivets en volume, par parallaxe (relief par pixel) ; la normale,
// l'assombrissement des creux et l'ombre propre en viennent aussi
uniform sampler2D sculpt_tex;
// PC-REMAKE : COUCHE DE SOL EN TRANSPARENCE (herbe posee sur le sable) : sa forme vient de l'alpha des
// sommets du terrain (un tous les metres environ) -> taches a bords en escalier de pres. Le bord
// suit maintenant le dessin de la texture (touffes) : net, organique, sans rien changer au plein.
uniform float sol_melange;
// PC-REMAKE : 1 sol (relief adouci), 2 sol d'herbe : vert plus profond, moins jaune, moins eblouissant
uniform float sol_kind;
uniform float fronde_pli_f;  // PC-REMAKE : relief des folioles (feuilles du terrain), 0 = rien
uniform float sculpt_amp;  // hauteur maxi en fraction d'une repetition de texture (0 = sans carte)
in vec3 v_vue;

// bruit de valeur (hash) — grain de sable/terre a petite echelle, ancre au monde
float pc_hash21(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float pc_vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = pc_hash21(i);
  float b = pc_hash21(i + vec2(1.0, 0.0));
  float c = pc_hash21(i + vec2(0.0, 1.0));
  float d = pc_hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) - 0.5;
}

// ---- 22/10 : LAVE ANIMEE. La lave du jeu est une texture fixe : un aplat orange. Ici elle coule
// et rougeoie. Ecoulement par carte de flux a deux phases (la texture glisse le long d'un
// champ de directions lent, deux tirages decales d'une demi-periode se relaient : pas
// d'etirement infini) ; chaleur qui respire (bruit ancre au monde qui derive) : les zones
// chaudes jaunissent, les froides tirent vers le rouge sombre. Croute (kind 1) : seuls les
// texels incandescents (orange vif) coulent, la croute reste ; sa bordure recoit la lueur.
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
// part incandescente d'un texel (orange-jaune vif = 1, croute brune ou noire = 0)
float lv_hot(vec3 c) {
  return smoothstep(0.38, 0.75, c.r * 0.9 + c.g * 0.45 - c.b * 1.1);
}
// motif de surface de la lave a un point (m) : x = peau refroidie (sombre), y = filament chaud
vec2 lv_motif(vec2 q) {
  float n1 = lv_noise(q * 0.9) * 0.6 + lv_noise(q * 2.3 + 4.1) * 0.4;
  float peau = smoothstep(0.42, 0.72, n1);
  float r = 1.0 - abs(lv_noise(q * 1.4 + 9.2) * 2.0 - 1.0);
  return vec2(peau, pow(r, 5.0));
}
// eclat final de la lave (applique APRES l'auto-illumination, sinon la lave sature au rouge pur
// et la peau sombre ne se voit pas) : < 1 peau refroidie, > 1 filament chaud
float lv_eclat = 1.0;
vec4 pc_lava(vec4 T0, vec2 uv, vec3 pw, float t, float kind) {
  vec2 m = pw.xz / 4096.0;  // metres
  // champ de flux lent : direction qui tourne doucement d'un endroit a l'autre
  float a = lv_noise(m * 0.05 + vec2(3.1, 7.7)) * 6.2832 * 1.5;
  vec2 dir = vec2(cos(a), sin(a));
  // flux a deux phases (periode 5,5 s, 2 m parcourus par phase)
  float ph0 = fract(t * 0.18);
  float ph1 = fract(t * 0.18 + 0.5);
  float wa = abs(ph0 - 0.5) * 2.0;
  vec2 M0 = mix(lv_motif(m - dir * ph0 * 2.0), lv_motif(m - dir * ph1 * 2.0 + 17.3), wa);
  // la texture elle-meme glisse un peu avec le courant
  vec2 du = dir * 0.02;
  vec4 F = mix(texture(tex_T0, uv - du * ph0), texture(tex_T0, uv - du * ph1), wa);
  // chaleur qui respire (0 froid .. 1 tres chaud)
  float ch = lv_noise(m * 0.2 + vec2(t * 0.05, -t * 0.035)) * 0.65 +
             lv_noise(m * 0.6 - vec2(t * 0.09, t * 0.06)) * 0.35;
  ch = smoothstep(0.2, 0.85, ch);
  float peau = M0.x * (1.0 - 0.6 * ch);
  float veine = M0.y * (1.0 - peau);
  vec4 res = T0;
  float hot = kind < 1.5 ? max(lv_hot(T0.rgb), lv_hot(F.rgb) * 0.85) : 1.0;
  vec3 base = kind < 1.5 ? max(F.rgb, T0.rgb * 0.6) : F.rgb;
  // peau refroidie : rouge sombre ; filaments : jaune presque blanc
  vec3 lave = mix(base, base * vec3(0.80, 0.42, 0.30), peau * 0.85);
  lave = mix(lave, vec3(1.0, 0.76, 0.32) * 1.2, veine * 0.7);
  res.rgb = mix(T0.rgb, lave, hot);
  lv_eclat = mix(1.0, (1.0 - 0.58 * peau) * (0.85 + 0.3 * ch) * (1.0 + 0.35 * veine), hot);
  if (kind < 1.5) {
    // croute : lueur au bord des fissures, qui respire avec la chaleur
    float bord = lv_hot(textureLod(tex_T0, uv, textureQueryLod(tex_T0, uv).x + 2.0).rgb);
    res.rgb += vec3(0.55, 0.16, 0.03) * bord * (1.0 - hot) * (0.2 + 0.4 * ch);
  }
  return res;
}

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz;      // ortho: no perspective divide needed
  p = p * 0.5 + 0.5;               // to [0,1]
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;                    // outside the map: lit
  }
  // BIAIS DE PENTE (17/08, clignotement uniquement sur les ombres des
  // rochers) : les faces raides rasées par le soleil s'auto-ombrent à la
  // limite de précision — le motif frémit quand le soleil fluide avance.
  // Plus la face est rasante, plus on recule la comparaison.
  float slope = 1.0 - clamp(abs(dot(normalize(w_nor), normalize(sun_dir_w))), 0.0, 1.0);
  float bias = 0.0016 + 0.0060 * slope * slope;
  // 3x3 PCF for soft edges
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      // spread x2: at 4096² the texels are finer — wider taps keep the penumbra
      // soft and stop the edge shimmer
      // (original : px * 2.0) — 22/09 : penombre plus large, ombres moins dures
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px * 3.0, p.z - bias));
    }
  }
  // texture() on sampler2DShadow returns visibility (1 = lit); invert
  // FONDU DE BORDURE (17/08, coupure visible) : les ombres s'estompent en douceur
  // vers la limite de la carte au lieu de se couper en ligne nette
  vec2 edge = min(p.xy, 1.0 - p.xy);
  float rim = smoothstep(0.0, 0.08, min(edge.x, edge.y));
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

void main() {
  vec2 uv = tex_coord.xy;
  // 22/10 : derivees et hauteurs du relief prises AVANT tout discard (voir RELIEF plus bas)
  vec2 rl_dx = dFdx(uv), rl_dy = dFdy(uv);
  vec3 rl_sx = dFdx(v_pos_w), rl_sy = dFdy(v_pos_w);

  // PC-REMAKE : SCULPTURE (parallaxe). Repere de la texture tire des derivees ecran (sans tangentes).
  bool sc_ok = false;
  vec3 sc_N = vec3(0.0);
  float sc_h = 0.0, sc_fondu = 0.0, sc_ombre = 0.0;
  if (sculpt_amp > 0.0 && gfx_hack_no_tex == 0 && alpha_min < 0.45 && v_foliage < 0.5 &&
      emissive_boost < 0.001 && lava_kind < 0.5 && v_dist < 4096.0 * 34.0) {
    float nl = length(w_nor);
    vec3 dp2perp = cross(rl_sy, w_nor), dp1perp = cross(w_nor, rl_sx);
    vec3 T = dp2perp * rl_dx.x + dp1perp * rl_dy.x;
    vec3 B = dp2perp * rl_dx.y + dp1perp * rl_dy.y;
    float lt = length(T), lb = length(B);
    if (nl > 0.05 && lt > 1e-20 && lb > 1e-20) {
      vec3 N = w_nor / nl;
      T /= lt;
      B /= lb;
      sc_fondu = 1.0 - smoothstep(4096.0 * 18.0, 4096.0 * 34.0, v_dist);
      vec3 V = -normalize(v_vue);  // fragment -> camera
      vec3 Vt = vec3(dot(V, T), dot(V, B), max(dot(V, N), 0.12));
      // taille monde d'une unite de texture ; profondeur plafonnee a 5 cm reels (une texture
      // etiree sur dix metres ne se creuse pas de quarante centimetres)
      float aire_uv = abs(rl_dx.x * rl_dy.y - rl_dx.y * rl_dy.x);
      float monde_par_uv = sqrt(length(cross(rl_sx, rl_sy)) / max(aire_uv, 1e-20));
      float prof = min(2.0 * sculpt_amp, 0.05 * 4096.0 / max(monde_par_uv, 1e-6)) * sc_fondu;
      // decalage de parallaxe : 1,2 cm reels au plus et rasant borne (avant : 5 cm, rasant a
      // 0.12 -> 8x) ; les cartes tirees du dessin prennent des fibres peintes pour des joints,
      // un decalage profond froisse les planches vues de pres. Le relief reste dans la normale.
      float prof_px = min(prof, 0.012 * 4096.0 / max(monde_par_uv, 1e-6));
      int n = int(mix(26.0, 8.0, Vt.z));
      vec2 pas = -Vt.xy / max(Vt.z, 0.4) * prof_px / float(n);
      float couche = 1.0 / float(n);
      vec2 p = uv;
      float d = 0.0;
      float hc = 1.0 - textureGrad(sculpt_tex, p, rl_dx, rl_dy).r;  // profondeur sous le sommet
      float dp = 0.0, hp = hc;
      for (int i = 0; i < 26; i++) {
        if (i >= n || d >= hc) {
          break;
        }
        dp = d;
        hp = hc;
        p += pas;
        d += couche;
        hc = 1.0 - textureGrad(sculpt_tex, p, rl_dx, rl_dy).r;
      }
      float apres = hc - d, avant = hp - dp;
      uv = mix(p, p - pas, clamp(apres / (apres - avant - 1e-6), 0.0, 1.0));
      // normale de la sculpture (pente de la hauteur, unites de texture -> monde)
      vec2 e = 1.0 / vec2(textureSize(sculpt_tex, 0));
      float hl = textureGrad(sculpt_tex, uv - vec2(e.x, 0.0), rl_dx, rl_dy).r;
      float hr = textureGrad(sculpt_tex, uv + vec2(e.x, 0.0), rl_dx, rl_dy).r;
      float hd = textureGrad(sculpt_tex, uv - vec2(0.0, e.y), rl_dx, rl_dy).r;
      float hu = textureGrad(sculpt_tex, uv + vec2(0.0, e.y), rl_dx, rl_dy).r;
      float k = min(prof, 0.03 * 4096.0 / max(monde_par_uv, 1e-6));  // avant : prof (5 cm)
      vec2 pente = vec2((hr - hl) / (2.0 * e.x), (hu - hd) / (2.0 * e.y)) * k;
      sc_N = normalize(N - T * pente.x - B * pente.y);
      float hh = textureGrad(sculpt_tex, uv, rl_dx, rl_dy).r;
      sc_h = hh * 2.0 - 1.0;
      // ombre propre vers le soleil : une bosse entre ce point et le soleil le cache
      vec3 L = -normalize(sun_dir_w);
      vec3 Lt = vec3(dot(L, T), dot(L, B), dot(L, N));
      if (Lt.z > 0.05) {
        vec2 pl = uv;
        vec2 pasl = Lt.xy / max(Lt.z, 0.4) * prof_px / 8.0;  // avant : / Lt.z * prof
        float hz = hh;
        for (int i = 1; i <= 8; i++) {
          pl += pasl;
          float hs = textureGrad(sculpt_tex, pl, rl_dx, rl_dy).r;
          sc_ombre = max(sc_ombre, clamp((hs - (hz + float(i) / 8.0)) * 4.0, 0.0, 1.0));
        }
      }
      sc_ok = true;
    }
  }

  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = sc_ok ? textureGrad(tex_T0, uv, rl_dx, rl_dy) : texture(tex_T0, uv);
    // 22/10 : lave animee (avant tout le reste : l'auto-illumination plus bas en profite)
    if (lava_kind > 0.5) {
      T0 = pc_lava(T0, uv, v_pos_w, wind_time, lava_kind);
    }
    color = fragment_color * T0;
    if (sol_kind > 1.5) {
      // herbe : -25 % de lumiere, rouge retire (le jaune), un peu de saturation en plus
      // (v1 : 0.74, 0.90, 0.80, trop clair)
      vec3 c = color.rgb * vec3(0.63, 0.77, 0.68);
      float l = dot(c, vec3(0.299, 0.587, 0.114));
      color.rgb = mix(vec3(l), c, 1.04);
    }
    if (sol_melange > 0.5) {
      const vec3 Ys = vec3(0.299, 0.587, 0.114);
      float h = dot(T0.rgb, Ys) - dot(textureLod(tex_T0, uv, 3.5).rgb, Ys);
      float a = color.a + clamp(h * 2.2, -0.3, 0.3);
      float w = max(fwidth(a) * 0.75, 0.06);
      color.a = smoothstep(0.5 - w, 0.5 + w, a) * step(0.004, color.a);
    }
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

  // 22/10 : RELIEF DES SURFACES (rendu plus recent). Les textures du decor sont plates : le
  // soleil glisse dessus sans accrocher le moindre grain. La luminance de la texture (adoucie
  // d'un niveau de mip) sert de hauteur — clair = bosse, sombre = creux — et la normale est
  // inclinee par sa pente (methode de Mikkelsen, sans tangentes : derivees ecran de la
  // position et de la hauteur). Le soleil eclaire alors un flanc de chaque bosse et laisse
  // l'autre dans l'ombre ; les creux s'assombrissent un peu. Ni feuillage, ni decoupes, ni
  // surfaces emissives ; fondu au loin (le grain y scintillerait).
  // (ombre portee du soleil calculee une seule fois : relief ET assombrissement plus bas)
  float sun_shadow = sun_shadow_factor();
  if (relief_amt > 0.001 && gfx_hack_no_tex == 0 && alpha_min < 0.45 &&
      emissive_boost < 0.001 && v_foliage < 0.5 && v_dist < 4096.0 * 45.0) {
    float nl = length(w_nor);
    if (nl > 0.05) {
      vec3 N = w_nor / nl;
      // niveau de mip pris sur l'uv d'origine (continu) : apres la parallaxe, uv saute aux
      // bords du relief (original : textureQueryLod(tex_T0, uv))
      float lb = textureQueryLod(tex_T0, tex_coord.xy).x + 0.7;
      const vec3 Y = vec3(0.299, 0.587, 0.114);
      float h0 = dot(textureLod(tex_T0, uv, lb).rgb, Y);
      float hx = dot(textureLod(tex_T0, uv + rl_dx, lb).rgb, Y);
      float hy = dot(textureLod(tex_T0, uv + rl_dy, lb).rgb, Y);
      float hb = dot(textureLod(tex_T0, uv, lb + 2.5).rgb, Y);
      const float H = 0.04 * 4096.0;  // 4 cm entre le plus sombre et le plus clair
      vec3 r1 = cross(rl_sy, N), r2 = cross(N, rl_sx);
      float det = dot(rl_sx, r1);
      vec3 grad = sign(det) * ((hx - h0) * H * r1 + (hy - h0) * H * r2);
      vec3 Nb = normalize(abs(det) * N - grad);
      // PC-REMAKE : avec une carte sculptee, la normale vient de la sculpture (le dessin n'invente
      // plus de bosses a chaque tache claire) ; creux = joints creuses
      if (sc_ok) {
        Nb = normalize(mix(N, sc_N, 0.85));
        hb = h0 - sc_h * 0.15;
      }
      vec3 L = -normalize(sun_dir_w);
      // part directe du soleil ici (0 a l'ombre portee, la nuit, en interieur)
      float direct = sun_shadow_strength > 0.001
                         ? clamp(1.0 - sun_shadow / sun_shadow_strength, 0.0, 1.0)
                         : 0.0;
      float flanc = clamp(dot(Nb, L) - dot(N, L), -0.45, 0.45);
      // a l'ombre : la lumiere du ciel vient d'en haut — les bosses tournees vers lui en
      // recoivent un peu plus
      float ciel = clamp(Nb.y - N.y, -0.3, 0.3);
      float creux = clamp((h0 - hb) * 1.6, -0.25, 0.25);
      float fondu = 1.0 - smoothstep(4096.0 * 25.0, 4096.0 * 45.0, v_dist);
      fondu *= (sol_kind > 0.5) ? 0.35 : 1.0;  // PC-REMAKE : sols : relief adouci (scintillement)
      color.rgb *= 1.0 + relief_amt * fondu *
                             (flanc * 1.45 * direct + ciel * 0.6 * (1.0 - direct) + creux * 0.45);
      if (sc_ok) {
        // joints et creux sculptes : plus sombres (occlusion) ; ombre propre au soleil
        // (avant : 0.34 joints, 0.35 ombre)
        color.rgb *= 1.0 - sc_fondu * (0.2 * smoothstep(0.25, 1.0, -sc_h) + 0.25 * sc_ombre * direct);
      }
    }
  }

  // 21/09 (au village, la texture du sol paraissait de basse qualite) :
  // les sols sont des textures 128² etirees sur des metres. DETAIL INTRINSEQUE :
  // la MEME texture re-echantillonnee a 4x la frequence, dont on ne garde que la
  // haute frequence (difference avec son mip flou) -> le grain du sable / de la
  // terre a petite echelle, sans rien inventer ni changer la couleur (luminance
  // seulement). Fondu 4 m -> 18 m : de loin, rien ne change.
  if (detail_amt > 0.001 && gfx_hack_no_tex == 0) {
    float fade = 1.0 - smoothstep(4096.0 * 4.0, 4096.0 * 18.0, v_dist);
    if (fade > 0.001) {
      vec2 duv = uv * 4.0;
      vec3 fine = texture(tex_T0, duv).rgb;
      vec3 coarse = textureLod(tex_T0, duv, 3.0).rgb;
      float hp = dot(fine - coarse, vec3(0.299, 0.587, 0.114));
      // + GRAIN procedural ancre au monde (cellules 4 cm et 1,5 cm) : les
      // textures de sable sont lisses par construction (aucune haute frequence
      // a re-echantillonner) — un grain fin, en luminance, les rend nettes
      // sous les pieds ; il disparait avec la distance comme le reste.
      vec2 gp = v_pos_w.xz / 4096.0;  // metres
      float grain = pc_vnoise(gp * 9.0) * 0.35 + pc_vnoise(gp * 25.0) * 0.4 + pc_vnoise(gp * 66.0) * 0.25;
      // + accentuation (masque flou) du dessin de la texture a sa propre echelle :
      // les taches molles du sable retrouvent des bords
      float hp0 = dot(T0.rgb - textureLod(tex_T0, uv, 2.0).rgb, vec3(0.299, 0.587, 0.114));
      color.rgb *= 1.0 + (hp * 2.2 + hp0 * 1.2 + grain * 0.30) * detail_amt * fade;
    }
  }


  // PC-REMAKE émissives : la surface EST une source (lave, braises, lueurs) —
  // texture à pleine puissance, insensible à la nuit et aux ombres
  if (emissive_boost > 0.001) {
    // auto-normalisant : les texels déjà CLAIRS reçoivent moins de boost — la
    // source brille sans clipper au blanc, sa forme reste lisible (la pile
    // d'énergie était indistincte, 18/08)
    float t_l = dot(T0.rgb, vec3(0.299, 0.587, 0.114));
    float eb = mix(emissive_boost, 1.05, clamp(t_l, 0.0, 1.0));
    color.rgb = max(color.rgb, T0.rgb * eb);
  }

  // PC-REMAKE sun shadows: soft darkening, before fog so distance stays hazy
  // (original : (1.0 - sun_shadow_factor()) — calcule une fois plus haut, 22/10)
  color.rgb *= (emissive_boost > 0.001) ? 1.0 : (1.0 - sun_shadow);
  color.rgb *= lv_eclat;  // 22/10 : lave animee (1 ailleurs)

  float fog_amt = clamp(fogginess * fog_color.a, 0, 1);
  // PC-REMAKE 25/08 (rochers lointains bugges) : au-dela de
  // 85 % de brouillard, ce qui reste de la texture n'est plus du detail mais du
  // bruit SOUS-PIXEL (le decor de vista du village fait quelques pixels de haut
  // a l'ecran) : il se replie en treillis. On laisse le brouillard finir : la
  // silhouette reste, le tissage disparait. Enquete : ni texture, ni filtrage,
  // ni couleurs de sommet, ni test d'alpha, ni profondeur, ni post-effets.
  fog_amt = mix(fog_amt, 1.0, smoothstep(0.85, 0.97, fog_amt));
  color.rgb = mix(color.rgb, fog_color.rgb, fog_amt);
  {
    float nl = length(w_nor);
    // 21/09 : alpha 0.75 = surface EMISSIVE (lave, braises, lueurs) — le post la
    // traite en SOURCE, pas en receveur (ni soleil, ni lampes, ni occlusion :
    // le lac de lave du village volcan s'eclairait lui-meme jusqu'au blanc)
    // 17/10 : alpha 0.95 = feuillage (voir tie_wind.frag)
    float tag = (emissive_boost > 0.001) ? 0.75 : ((v_foliage > 0.5) ? 0.95 : 1.0);
    vec3 n_g = (nl > 0.05) ? w_nor / nl : vec3(0.0, 1.0, 0.0);
    if (sc_ok) {
      n_g = normalize(mix(n_g, sc_N, 0.5));
    }
    // PC-REMAKE : FEUILLES (palmes fondues dans le terrain) : relief des folioles tire du dessin, comme
    // tie_wind.frag — chaque foliole accroche la lumiere du post
    if (fronde_pli_f > 0.0 && nl > 0.05 && gfx_hack_no_tex == 0) {
      const vec3 Yf = vec3(0.299, 0.587, 0.114);
      float Hf = 0.012 * 4096.0;
      float f0 = dot(texture(tex_T0, uv).rgb, Yf) * Hf;
      float fx = dot(texture(tex_T0, uv + rl_dx).rgb, Yf) * Hf;
      float fy = dot(texture(tex_T0, uv + rl_dy).rgb, Yf) * Hf;
      vec3 r1 = cross(rl_sy, n_g), r2 = cross(n_g, rl_sx);
      float det = dot(rl_sx, r1);
      if (abs(det) > 1e-12) {
        n_g = normalize(abs(det) * n_g - sign(det) * ((fx - f0) * r1 + (fy - f0) * r2));
      }
    }
    out_normal = (nl > 0.05) ? vec4(n_g * 0.5 + 0.5, tag) : vec4(0.0);
  }
}
