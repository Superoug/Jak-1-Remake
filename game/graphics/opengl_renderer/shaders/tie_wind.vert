#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;  // (index A, index B) : milieux de subdivision = moyenne
layout (location = 3) in vec4 normal_in;  // normales lissées (déjà dans le VAO)
layout (location = 6) in uint color3_in;  // 3e couleur + poids (prototypes remodeles)

uniform vec4 hvdf_offset;
uniform mat4 camera;
// same combined matrix WITHOUT the wind bend, plus the local y range of this draw
uniform mat4 camera_static;
uniform vec2 wind_yrange;
uniform vec2 wind_yrange_local;  // étendue du draw seul (couronnes de penchés)
uniform float wind_time;
uniform float wind_phase;  // per instance, camera-independent
uniform vec3 wind_xzc;  // xz centre + rayon du draw : identifie chaque fronde
uniform vec2 palm_scales;  // x = échelle houle, y = échelle battement (REPL)
uniform float trunk_pin;  // 1 = draw de tronc : repos absolu (détachement)
uniform float graft_boost;  // ×3 sur les couronnes greffées (span crown-local minuscule)
uniform float palm_debug;  // 1 = peindre les couronnes greffées en violet (test identité)
uniform float hang_sway;  // 1 = objet suspendu (poissons) : balancement rigide
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
out vec3 w_nor;  // G-buffer de normales
// PALMES PLIEES
uniform float fronde_pli;
out vec3 v_pos_p;  // position dans l'espace du modele (relief par pixel)

void main() {
  float span = max(wind_yrange.y - wind_yrange.x, 1.0);
  float hn_raw = clamp((position_in.y - wind_yrange.x) / span, 0.0, 1.0);
  // PALMIERS PENCHÉS
  float lean = step(6144.0, length(wind_xzc.xy));
  float span_l = max(wind_yrange_local.y - wind_yrange_local.x, 1.0);
  float hn_raw_l = clamp((position_in.y - wind_yrange_local.x) / span_l, 0.0, 1.0);
  hn_raw = mix(hn_raw, hn_raw_l, lean);
  // GRANDS arbres
  float small_tree = 1.0 - step(12288.0, span);
  // penchés : courbe adoucie sur la couronne
  float hn_curve = mix(2.8, 1.4, lean);
  float hn = mix(pow(hn_raw, hn_curve), smoothstep(0.5, 1.0, hn_raw), small_tree);
  // DÉTACHEMENT feuilles/tronc
  hn *= 1.0 - trunk_pin;

  // houle sinusoïdale continue, comme l'herbe
  float ph = wind_phase;
  float s1 = sin(wind_time * 1.5 + ph);
  float s2 = sin(wind_time * 2.6 + ph * 1.7 + 1.3);
  float size_gain = clamp(0.8 + span / 49152.0, 0.8, 1.6);
  // houle d'ensemble elle aussi bornée par la taille du proto penchés
  float amp = min(380.0 * size_gain, span * 0.028) * hn * hn * palm_scales.x * graft_boost *
              (1.0 + 0.6 * lean);
  vec3 p_in = position_in;
  // houle d'ensemble douce (comme l'herbe)
  p_in.x += (s1 + 0.45 * s2) * amp;
  p_in.z += (cos(wind_time * 1.15 + ph) + 0.45 * s2) * amp * 0.7;
  p_in.y += s2 * amp * 0.18;

  // chaque FEUILLE ondule individuellement
  vec2 rel = position_in.xz - wind_xzc.xy;
  float fr = clamp(length(rel) / max(wind_xzc.z, 1.0), 0.0, 1.0);
  float az = atan(rel.y, rel.x);
  // hn_RAW ici, pas le hn ancré
  float leaf_w = pow(hn_raw, 1.1) * pow(fr, 1.35) * (1.0 - trunk_pin);
  float flap = sin(wind_time * 2.0 + az * 2.0 + fr * 2.2 + ph)
             + 0.5 * sin(wind_time * 3.4 + az * 3.0 + ph * 1.3);
  // amplitude PROPORTIONNELLE à la taille (span*0.045)
  float leaf_amp = clamp(span * 0.045, 220.0, 1800.0) * palm_scales.y * graft_boost;
  // battement LE LONG DE LA NORMALE de chaque feuille
  vec3 leaf_dir = normalize(normal_in.xyz + vec3(0.0, 0.25, 0.0));
  p_in += leaf_dir * (flap * leaf_w * leaf_amp * 0.8);
  if (fronde_pli > 0.0) {
    float sp = clamp(tex_coord_in.x, 0.0, 1.0);
    // 22 cm reels au bord des folioles
    float pli = 0.22 * 4096.0 * fronde_pli;
    // V : monte avec l'eloignement de la nervure
    p_in.y += pli * (1.6 * sp - 0.9 * sp * sp);
  }
  // OBJET SUSPENDU (poissons du village de pecheurs)
  if (hang_sway > 0.5) {
    float lg = max(wind_yrange.y - position_in.y, 0.0);
    float t = wind_time;
    // tres leger va-et-vient (~1 degre), sur un seul axe
    float ax = 0.017 * sin(t * 1.1 + ph);
    float az2 = 0.0;
    p_in = position_in;
    p_in.x += lg * ax;
    p_in.z += lg * az2;
    p_in.y += lg * 0.5 * (ax * ax + az2 * az2);  // le bas remonte un peu sur l'arc
  }
  v_pos_p = p_in;

  vec4 t_wind = -camera[3];
  t_wind -= camera[0] * p_in.x;
  t_wind -= camera[1] * p_in.y;
  t_wind -= camera[2] * p_in.z;

  // height-weighted native gusts
  vec4 t_stat = -camera_static[3];
  t_stat -= camera_static[0] * p_in.x;
  t_stat -= camera_static[1] * p_in.y;
  t_stat -= camera_static[2] * p_in.z;

  // penchés : ZÉRO rafale native
  float wind_w = pow(hn, 2.8) * (1.0 - lean) * (1.0 - hang_sway);
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
  // normale d'origine pour un objet suspendu
  w_nor = normalize(normal_in.xyz + vec3(0.0, 0.25 * (1.0 - hang_sway), 0.0));

  // time of day lookup
  // prototype remodele
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

  // projecteur violet
  if (palm_debug > 0.5) {
    fragment_color.rgb = vec3(6.0, 0.3, 6.0);
  }

  tex_coord = tex_coord_in;
}