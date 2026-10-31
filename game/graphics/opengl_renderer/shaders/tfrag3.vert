#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;  // (index A, index B) : milieux de subdivision = moyenne
layout (location = 3) in vec4 normal_in;  // normales lissées (subdiv Phong)
layout (location = 4) in vec4 crown_in;  // squelette de couronne : fr, az, hn, id
// decor affine
layout (location = 6) in uint color3_in;

uniform vec4 hvdf_offset;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
uniform int decal;
// feuillages fondus dans le terrain (palmiers penchés)
uniform float wind_time;
uniform float tfrag_wind_amp;  // amplitude réglable au REPL, 0 = figé
uniform float palm_debug;  // 1 = draw ciblé par la chirurgie : peint en violet
uniform float fronde_pli;  // palmes pliees en V (voir tie_wind.vert), 0 = cartes d'origine

// 2 = lave en fusion
uniform float lava_kind;
float lv_houle(vec2 m, float t) {
  vec2 w = m + 0.9 * vec2(sin(m.y * 0.23 + t * 0.07), sin(m.x * 0.19 - t * 0.05));
  float h = 0.55 * sin(dot(w, vec2(0.85, 0.53)) * 0.90 - t * 0.30) +
            0.30 * sin(dot(w, vec2(-0.42, 0.91)) * 1.46 - t * 0.42 + 1.7) +
            0.15 * sin(dot(w, vec2(0.30, -0.95)) * 2.42 - t * 0.55 + 4.1);
  return h * 0.09;
}

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
// sun shadows: vertex position in light clip space
uniform mat4 sun_light_vp;
out vec4 sun_light_pos;
// normale monde vers le fragment
out vec3 w_nor;
flat out float v_foliage;  // 1 = feuille marquee (s > 50), voir tfrag3.frag
// distance camera->sommet (unites jeu) pour le fondu du detail des sols
out float v_dist;
out vec3 v_pos_w;  // position monde (grain procedural des sols, ancre au monde)
out vec3 v_vue;  // camera -> sommet (parallaxe de la sculpture du decor)

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
  // feuilles marquées (s > 50)
  vec3 pos_w = position_in;
  vec3 tc_in = tex_coord_in;
  v_foliage = 0.0;
  if (tc_in.x > 50.0) {
    tc_in.x -= 100.0;
    v_foliage = 1.0;

    // Palmiers fondus dans le terrain
    if (tfrag_wind_amp > 0.5) {
      // CODE DÉDIÉ AUX PALMIERS PENCHÉS
      float fr = crown_in.x;
      float az = crown_in.y * 6.2831853;
      float ph = crown_in.w * 15.0;  // phase propre à la couronne
      float t = wind_time;
      // angle de balancement de la couronne (radians, doux)
      float rock = sin(t * 1.5 + ph) + 0.45 * sin(t * 2.6 + ph * 1.7 + 1.3);
      // tangentielle = rotation autour de l'axe
      vec2 tangent = vec2(-sin(az), cos(az)) * fr;
      float amp = tfrag_wind_amp * 0.55;
      pos_w.xz += tangent * (rock * amp);
      // affaissement des pointes, déphasé
      pos_w.y -= (0.5 + 0.5 * sin(t * 2.1 + ph * 2.1)) * fr * fr * amp * 0.35;
    }
  }

  // palme pliee en V le long de sa nervure (s = 0), folioles retombantes vers la pointe
  if (fronde_pli > 0.0) {
    float sp = clamp(tc_in.x, 0.0, 1.0);
    pos_w.y += fronde_pli * 0.22 * 4096.0 * (1.6 * sp - 0.9 * sp * sp);
  }
  if (lava_kind > 1.5) {
    pos_w.y += lv_houle(pos_w.xz * (1.0 / 4096.0), wind_time) * 4096.0;
  }
  vec3 vert = pos_w - cam_trans.xyz;
  v_dist = length(vert);
  v_pos_w = pos_w;
  v_vue = vert;
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
  // sun shadows
  sun_light_pos = sun_light_vp * vec4(pos_w, 1.0);
  w_nor = normal_in.xyz;

  // time of day lookup
  uint cw0 = (color3_in >> 16) & 0xffu, cw1 = (color3_in >> 24) & 0xffu;
  if (cw0 + cw1 > 0u) {
    // sommet du decor affine
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

  // test d'identité des penchés
  if (palm_debug > 0.5) {
    fragment_color.rgb = vec3(6.0, 0.3, 6.0);
  }

  tex_coord = tc_in;
}
