#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in vec3 rgba_base;
layout (location = 3) in int time_of_day_index;
// ecart a l'origine de l'instance (10 bits signes par axe, 2 cm), bit 30 = present
layout (location = 4) in uint origine_in;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform int decal;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
// grass interaction: xyz = player world pos (1m = 4096 units), w = push strength 0..1
uniform vec4 grass_push;
// CINEMATIQUES (pile d'energie, scenes)
uniform vec4 grass_clear;
// ambient wind: x = time (seconds), y = amplitude (world units at blade tip), zw unused
uniform vec4 grass_wind;
// projecteur violet
uniform float palm_debug;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.
// CHAMPIGNONS ELASTIQUES
uniform float champi;
uniform vec4 champi_hist[24];

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
// position relative a la camera (normale de facette du G-buffer)
out vec3 v_rel;
// sun shadows
uniform mat4 sun_light_vp;
out vec4 sun_light_pos;

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

  // grass interaction: bend vegetation vertices radially away from the player
  vec3 world_pos = position_in;

  // ambient wind sway: two offset sine waves per world position give an organic ripple; weight grows
  if (grass_wind.y > 0.001) {
    float t = grass_wind.x;
    float phase = world_pos.x * 0.00012 + world_pos.z * 0.00017;
    float sway = sin(t * 1.9 + phase * 6.2831853) * 0.6
               + sin(t * 3.1 + phase * 12.566371 + 1.7) * 0.4;
    float h = clamp((position_in.y - grass_push.y) / 4096.0 + 0.3, 0.0, 1.0);
    world_pos.x += sway * grass_wind.y * h;
    world_pos.z += sway * grass_wind.y * h * 0.6;
  }

  if (grass_push.w > 0.001) {
    const float PUSH_RADIUS = 7372.8;  // 1.8 m
    const float PUSH_AMOUNT = 1843.2;  // 0.45 m max horizontal bend
    vec2 to_vert = world_pos.xz - grass_push.xz;
    float dist_xz = length(to_vert);
    float dy = world_pos.y - grass_push.y;
    // only affect vertices near the player and within a sane height band
    if (dist_xz < PUSH_RADIUS && dy > -2048.0 && dy < 8192.0) {
      float fall = 1.0 - dist_xz / PUSH_RADIUS;
      fall = fall * fall;  // smooth falloff
      float height_w = clamp(dy / 4096.0 + 0.35, 0.0, 1.0);  // roots stay put
      vec2 dir = dist_xz > 1.0 ? to_vert / dist_xz : vec2(0.0, 1.0);
      float amount = grass_push.w * fall * height_w * PUSH_AMOUNT;
      world_pos.xz += dir * amount;
      world_pos.y -= 0.35 * amount;  // slight squash
    }
  }

  if (grass_clear.w > 0.001) {
    const float CLEAR_IN = 4096.0 * 3.5;  // couchee a plat
    const float CLEAR_OUT = 4096.0 * 7.0;  // fondu : intacte au-dela
    vec2 to_c = world_pos.xz - grass_clear.xz;
    float dc = length(to_c);
    float dyc = world_pos.y - grass_clear.y;
    if (dc < CLEAR_OUT && dyc > -4096.0 * 3.0 && dyc < 4096.0 * 6.0) {
      float k = grass_clear.w * (1.0 - smoothstep(CLEAR_IN, CLEAR_OUT, dc));
      // hauteur au-dessus du sol de Jak
      float above = max(dyc + 4096.0 * 0.3, 0.0);
      vec2 dir_c = dc > 1.0 ? to_c / dc : vec2(0.0, 1.0);
      world_pos.y -= above * 0.85 * k;
      world_pos.xz += dir_c * (above * 0.5 * k);
    }
  }

  if (champi > 0.5 && (origine_in & (1u << 30)) != 0u) {
    // le champignon entier bouge d'un bloc
    const float M = 4096.0;
    const float W0 = 6.2831853 * 3.0;
    const float Z = 0.30;
    float wd = W0 * sqrt(1.0 - Z * Z);
    ivec3 qi = ivec3(int(origine_in & 0x3ffu), int((origine_in >> 10) & 0x3ffu),
                     int((origine_in >> 20) & 0x3ffu));
    qi -= 1024 * ivec3(greaterThan(qi, ivec3(511)));
    vec3 O = world_pos + vec3(qi) * (0.02 * M);
    float x = 0.0;
    float c_prec = -1.0;
    for (int i = 0; i < 24; i++) {
      vec4 hs = champi_hist[i];
      if (hs.w < 0.0) {
        continue;
      }
      float dxz = length(O.xz - hs.xz) / M;
      float dyf = (hs.y - O.y) / M;  // hauteur des pieds au-dessus du pied du champignon
      float c = (1.0 - smoothstep(0.7, 1.6, dxz)) * smoothstep(-0.4, 0.0, dyf) *
                (1.0 - smoothstep(3.0, 4.0, dyf));
      if (c_prec < 0.0) {
        x = c;  // le plus ancien : etat de repos atteint
      } else {
        float tau = hs.w;
        float S = 1.0 - exp(-Z * W0 * tau) * (cos(wd * tau) + (Z / sqrt(1.0 - Z * Z)) * sin(wd * tau));
        x += (c - c_prec) * S;
      }
      c_prec = c;
    }
    if (abs(x) > 0.002) {
      // ecrasement vertical autour du pied (18 % de sa hauteur au plus), il s'evase un peu
      float k = 0.18 * x;
      world_pos.y = O.y + (world_pos.y - O.y) * (1.0 - k);
      world_pos.xz = O.xz + (world_pos.xz - O.xz) * (1.0 + 0.45 * k);
    }
  }

  // sun shadows
  sun_light_pos = sun_light_vp * vec4(world_pos, 1.0);

  // Step 3, the camera transform
  vec3 vert = world_pos - cam_trans.xyz;
  v_rel = vert;
  vec4 transformed = -pc_camera[3];
  transformed -= pc_camera[0] * vert.x;
  transformed -= pc_camera[1] * vert.y;
  transformed -= pc_camera[2] * vert.z;

  // do fog!
  fogginess = 255 - clamp(-transformed.w + hvdf_offset.w, fog_min, fog_max);

  // scissoring area adjust
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;

  // time of day lookup
  // start with the vertex color (only rgb, VIF filled in the 255.)
  fragment_color =  vec4(rgba_base, 1);
  // get the time of day multiplier
  vec4 tod_color = texelFetch(tex_T10, time_of_day_index, 0);
  // combine
  fragment_color *= tod_color * 4;

  if (decal == 1) {
    fragment_color.xyz = vec3(1.0, 1.0, 1.0);
  }

  // projecteur violet
  if (palm_debug > 0.5) {
    fragment_color.rgb = vec3(6.0, 0.3, 6.0);
  }

  tex_coord = tex_coord_in;
  tex_coord.xy /= 4096;
}
