#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;  // (index A, index B) : milieux de subdivision = moyenne
layout (location = 3) in vec4 normal_in;  // normales lissées (déjà dans le VAO)
layout (location = 6) in uint color3_in;  // PC-REMAKE : 3e couleur + poids (prototypes remodeles)

uniform vec4 hvdf_offset;
uniform mat4 camera;
// PC-REMAKE: same combined matrix WITHOUT the wind bend, plus the local y range
// of this draw. Low vertices stick to the static matrix (trunk planted), high
// vertices take the full wind (crown sways) — like the grass, but per tree.
uniform mat4 camera_static;
uniform vec2 wind_yrange;
uniform vec2 wind_yrange_local;  // étendue du draw seul (couronnes de penchés)
uniform float wind_time;
uniform float wind_phase;  // per instance, camera-independent
uniform vec3 wind_xzc;     // xz centre + rayon du draw : identifie chaque fronde
uniform vec2 palm_scales;  // x = échelle houle, y = échelle battement (REPL)
uniform float trunk_pin;   // 1 = draw de tronc : repos absolu (détachement)
uniform float graft_boost; // ×3 sur les couronnes greffées (span crown-local minuscule)
uniform float palm_debug;  // 1 = peindre les couronnes greffées en violet (test identité)
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 w_nor;  // PC-REMAKE 21/09 : G-buffer de normales
// PC-REMAKE : PALMES PLIEES — une palme est une carte plate ; ici chaque demi-palme se releve en V le
// long de sa nervure (la nervure est le bord s = 0 de la texture, les folioles vont vers s = 1),
// les folioles retombent un peu vers la pointe. Hauteur proportionnelle a l'arbre. 0 = carte
// d'origine (troncs, autres dessins, /tmp/qa_no_fronde).
uniform float fronde_pli;
out vec3 v_pos_p;  // PC-REMAKE : position dans l'espace du modele (relief par pixel)

void main() {
  float span = max(wind_yrange.y - wind_yrange.x, 1.0);
  float hn_raw = clamp((position_in.y - wind_yrange.x) / span, 0.0, 1.0);
  // PALMIERS PENCHÉS (17/08) : leur couronne est sur le CÔTÉ, à mi-hauteur de
  // l'arbre — la normalisation sur l'arbre ENTIER l'écrase (hn^2.8 ≈ 0.25).
  // Signature d'un draw de couronne excentrée : centre xz loin de l'origine du
  // proto (> 1,5 m). Ces draws ondulent sur LEUR PROPRE étendue verticale.
  // Les palmiers droits (couronne centrée) gardent exactement le chemin validé.
  float lean = step(6144.0, length(wind_xzc.xy));
  float span_l = max(wind_yrange_local.y - wind_yrange_local.x, 1.0);
  float hn_raw_l = clamp((position_in.y - wind_yrange_local.x) / span_l, 0.0, 1.0);
  hn_raw = mix(hn_raw, hn_raw_l, lean);
  // GRANDS arbres : la courbe VALIDÉE (hn^2.8), intouchée.
  // TOUT PETITS (< 3 m, les palmplants au tronc qui vibrait) : ancrage dur,
  // rien sous la moitié de la hauteur.
  float small_tree = 1.0 - step(12288.0, span);
  // penchés : courbe adoucie sur la couronne (2.8 écrase le milieu de couronne)
  float hn_curve = mix(2.8, 1.4, lean);
  float hn = mix(pow(hn_raw, hn_curve), smoothstep(0.5, 1.0, hn_raw), small_tree);
  // DÉTACHEMENT feuilles/tronc : un draw de tronc est
  // épinglé au repos absolu — zéro houle, zéro rafale, zéro battement.
  hn *= 1.0 - trunk_pin;

  // PC-REMAKE : houle sinusoïdale continue, comme l'herbe — pas les à-coups des
  // rafales natives. Phase par instance (tirée de la translation de la matrice)
  // pour déphaser les arbres voisins ; amplitude pondérée par la hauteur² et un
  // peu par la taille de l'arbre.
  float ph = wind_phase;
  float s1 = sin(wind_time * 1.5 + ph);
  float s2 = sin(wind_time * 2.6 + ph * 1.7 + 1.3);
  float size_gain = clamp(0.8 + span / 49152.0, 0.8, 1.6);
  // houle d'ensemble elle aussi bornée par la taille du proto
  // penchés : houle renforcée (+60 %) — elle porte seule l'ondulation puisque la
  // rafale native est coupée pour eux
  float amp = min(380.0 * size_gain, span * 0.028) * hn * hn * palm_scales.x * graft_boost *
              (1.0 + 0.6 * lean);
  vec3 p_in = position_in;
  // houle d'ensemble douce (comme l'herbe)
  p_in.x += (s1 + 0.45 * s2) * amp;
  p_in.z += (cos(wind_time * 1.15 + ph) + 0.45 * s2) * amp * 0.7;
  p_in.y += s2 * amp * 0.18;

  // PC-REMAKE : chaque FEUILLE ondule individuellement. L'angle du sommet
  // autour de l'axe du tronc identifie sa fronde (= sa phase), la distance
  // radiale le long de la fronde donne l'amplitude (racine fixe, pointe
  // libre). Une vague court de feuille en feuille autour de la couronne.
  vec2 rel = position_in.xz - wind_xzc.xy;
  float fr = clamp(length(rel) / max(wind_xzc.z, 1.0), 0.0, 1.0);
  float az = atan(rel.y, rel.x);
  // hn_RAW ici, pas le hn ancré : le battement est déjà protégé côté tronc par
  // fr (distance à l'axe) — l'ancrage en hauteur étouffait les feuilles des
  // petits palmiers (feuilles pas assez animées)
  float leaf_w = pow(hn_raw, 1.1) * pow(fr, 1.35) * (1.0 - trunk_pin);
  float flap = sin(wind_time * 2.0 + az * 2.0 + fr * 2.2 + ph)
             + 0.5 * sin(wind_time * 3.4 + az * 3.0 + ph * 1.3);
  // amplitude PROPORTIONNELLE à la taille (span*0.045) : ±44 cm sur un palmier
  // de 10 m (réglage validé), ±10 cm sur un palmplant d'1,50 m — sans ça les
  // petits nouveaux venus battaient beaucoup trop fort
  float leaf_amp = clamp(span * 0.045, 220.0, 1800.0) * palm_scales.y * graft_boost;
  // battement LE LONG DE LA NORMALE de chaque feuille : juste sur les palmiers
  // droits COMME penchés (l'ancien couple vertical+radial partait dans tous
  // les sens sur les couronnes inclinées)
  vec3 leaf_dir = normalize(normal_in.xyz + vec3(0.0, 0.25, 0.0));
  p_in += leaf_dir * (flap * leaf_w * leaf_amp * 0.8);
  if (fronde_pli > 0.0) {
    float sp = clamp(tex_coord_in.x, 0.0, 1.0);
    // 22 cm reels au bord des folioles (l'etendue du dessin ne donne pas la taille de la palme)
    float pli = 0.22 * 4096.0 * fronde_pli;
    // V : monte avec l'eloignement de la nervure ; pointe des folioles retombante
    p_in.y += pli * (1.6 * sp - 0.9 * sp * sp);
  }
  v_pos_p = p_in;

  vec4 t_wind = -camera[3];
  t_wind -= camera[0] * p_in.x;
  t_wind -= camera[1] * p_in.y;
  t_wind -= camera[2] * p_in.z;

  // height-weighted native gusts — base anchored, top free, no extra gain
  vec4 t_stat = -camera_static[3];
  t_stat -= camera_static[0] * p_in.x;
  t_stat -= camera_static[1] * p_in.y;
  t_stat -= camera_static[2] * p_in.z;

  // penchés : ZÉRO rafale native — la rotation d'instance sur une couronne
  // excentrée produit un ballottement vertical faux ; leur mouvement est
  // entièrement porté par la houle + le battement (le système validé), en pur
  float wind_w = pow(hn, 2.8) * (1.0 - lean);
  vec4 transformed = mix(t_stat, t_wind, wind_w);

  float Q = fog_constant / transformed.w;

  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // perspective divide!
  transformed.xyz *= Q;
  // offset
  transformed.xyz += hvdf_offset.xyz;
  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;
  // correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  w_nor = normalize(normal_in.xyz + vec3(0.0, 0.25, 0.0));

  // time of day lookup
  // PC-REMAKE : prototype remodele : melange des couleurs des trois coins du triangle d'origine
  // (original : 0.5 * (A + B) seulement)
  uint cw0 = (color3_in >> 16) & 0xffu, cw1 = (color3_in >> 24) & 0xffu;
  if (cw0 + cw1 > 0u) {
    float a = float(cw0) / 255.0, b = float(cw1) / 255.0;
    fragment_color = a * texelFetch(tex_T10, time_of_day_index.x, 0) +
                     b * texelFetch(tex_T10, time_of_day_index.y, 0) +
                     max(1.0 - a - b, 0.0) * texelFetch(tex_T10, int(color3_in & 0xffffu), 0);
  } else {
    fragment_color = (0.5 * (texelFetch(tex_T10, time_of_day_index.x, 0) + texelFetch(tex_T10, time_of_day_index.y, 0)));
  }
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  // projecteur violet : draw désigné par la spec de /tmp/palm_debug
  if (palm_debug > 0.5) {
    fragment_color.rgb = vec3(6.0, 0.3, 6.0);
  }

  tex_coord = tex_coord_in;
}