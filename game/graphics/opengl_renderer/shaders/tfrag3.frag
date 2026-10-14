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
uniform vec3 sun_dir_w;  // direction normalisée VERS la surface (des ombres)
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;

// PC-REMAKE "Spyro water": x = time (s), y = strength (1 on WATER trees)
uniform vec4 water_fx;
// 21/09 : detail intrinseque des sols (0 = rien, 1 = plein), fondu par distance
uniform float detail_amt;
in float v_dist;
in vec3 v_pos_w;

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

  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = texture(tex_T0, uv);
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
  color.rgb *= (emissive_boost > 0.001) ? 1.0 : (1.0 - sun_shadow_factor());

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
    float tag = (emissive_boost > 0.001) ? 0.75 : 1.0;
    out_normal = (nl > 0.05) ? vec4(w_nor / nl * 0.5 + 0.5, tag) : vec4(0.0);
  }
}
