#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;  // (index A, index B) : milieux de subdivision = moyenne
layout (location = 3) in vec4 normal_in;  // normales lissées (subdiv Phong)
layout (location = 4) in vec4 crown_in;   // squelette de couronne : fr, az, hn, id

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;
// PC-REMAKE : feuillages fondus dans le terrain (palmiers penchés). Leurs
// sommets portent s+100 ; on retire l'offset et on fait onduler en espace monde.
uniform float wind_time;
uniform float tfrag_wind_amp;  // amplitude réglable au REPL, 0 = figé
uniform float palm_debug;      // 1 = draw ciblé par la chirurgie : peint en violet

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
// PC-REMAKE sun shadows: vertex position in light clip space
uniform mat4 sun_light_vp;
out vec4 sun_light_pos;
// normale monde vers le fragment : biais d'ombre proportionnel à la pente
// (les falaises frémissaient sous le soleil fluide — 17/08)
out vec3 w_nor;
// 21/09 : distance camera->sommet (unites jeu) pour le fondu du detail des sols
out float v_dist;
out vec3 v_pos_w;  // position monde (grain procedural des sols, ancre au monde)

void main() {
  // old system:
  // - load vf12
  // - itof0 vf12
  // - multiply with camera matrix (add trans)
  // - let Q = fogx / vf12.w
  // - xyz *= Q
  // - xyzw += hvdf_offset
  // - clip w.
  // - ftoi4 vf12
  // use in gs.
  // gs is 12.4 fixed point, set up with 2048.0 as the center.

  // the itof0 is done in the preprocessing step.  now we have floats.


  // Step 3, the camera transform
  // feuilles marquées (s > 50) : houle douce en espace monde, tronc immobile
  vec3 pos_w = position_in;
  vec3 tc_in = tex_coord_in;
  if (tc_in.x > 50.0) {
    tc_in.x -= 100.0;
    // Palmiers fondus dans le terrain : MÊME recette que les palmiers TIE
    // validés — battement feuille à feuille le long de la NORMALE (phases
    // courtes entre feuilles) + houle de couronne légère. Amplitude pilotée
    // au REPL (pc-set-palm-wind! z).
    if (tfrag_wind_amp > 0.5) {
      // CODE DÉDIÉ AUX PALMIERS PENCHÉS : la couronne SE BALANCE EN BLOC
      // autour de son centre (champ de ROTATION — continu par construction,
      // sin/cos de l'angle ne sautent jamais, donc zéro déchirure possible)
      // + léger affaissement des pointes. C'est le mouvement validé des
      // palmiers TIE, écrit sur le squelette de couronne.
      float fr = crown_in.x;
      float az = crown_in.y * 6.2831853;
      float ph = crown_in.w * 15.0;  // phase propre à la couronne
      float t = wind_time;
      // angle de balancement de la couronne (radians, doux)
      float rock = sin(t * 1.5 + ph) + 0.45 * sin(t * 2.6 + ph * 1.7 + 1.3);
      // tangentielle = rotation autour de l'axe : continue partout
      vec2 tangent = vec2(-sin(az), cos(az)) * fr;
      float amp = tfrag_wind_amp * 0.55;
      pos_w.xz += tangent * (rock * amp);
      // affaissement des pointes, déphasé
      pos_w.y -= (0.5 + 0.5 * sin(t * 2.1 + ph * 2.1)) * fr * fr * amp * 0.35;
    }
  }

  vec3 vert = pos_w - cam_trans.xyz;
  v_dist = length(vert);
  v_pos_w = pos_w;
  vec4 transformed = -pc_camera[3];
  transformed.w = 0;
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;

  // do fog!
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;
  // PC-REMAKE sun shadows
  sun_light_pos = sun_light_vp * vec4(pos_w, 1.0);
  w_nor = normal_in.xyz;

  // time of day lookup
  fragment_color = (0.5 * (texelFetch(tex_T10, time_of_day_index.x, 0) + texelFetch(tex_T10, time_of_day_index.y, 0)));
  // color adjustment
  fragment_color *= 2;
  fragment_color.a *= 2;

  if (decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from fragment.
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  // test d'identité des penchés : draws terrain ciblés peints en violet
  if (palm_debug > 0.5) {
    fragment_color.rgb = vec3(6.0, 0.3, 6.0);
  }

  tex_coord = tc_in;
}
