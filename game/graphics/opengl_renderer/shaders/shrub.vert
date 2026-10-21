#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in vec3 rgba_base;
layout (location = 3) in int time_of_day_index;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform float fog_min;
uniform float fog_max;
uniform int decal;
uniform vec4 cam_trans;
uniform mat4 pc_camera;
// PC-REMAKE grass interaction: xyz = player world pos (1m = 4096 units),
// w = push strength 0..1. Zero strength disables the effect entirely.
uniform vec4 grass_push;
// 20/10 : CINEMATIQUES (pile d'energie, scenes) : l'herbe autour de Jak se couche au sol pour
// ne plus masquer la scene. xyz = pieds de Jak, w = dose 0..1 (fondu cote jeu).
uniform vec4 grass_clear;
// PC-REMAKE ambient wind: x = time (seconds), y = amplitude (world units at
// blade tip), zw unused. Gives every tuft a gentle, phase-shifted sway.
uniform vec4 grass_wind;
// projecteur violet : draw désigné par la spec de /tmp/palm_debug
uniform float palm_debug;
uniform sampler1D tex_T10; // note, sampled in the vertex shader on purpose.

out vec4 fragment_color;
out vec3 tex_coord;
out float fogginess;
// PC-REMAKE 13/10 : position relative a la camera (normale de facette du G-buffer)
out vec3 v_rel;
// PC-REMAKE sun shadows
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

  // PC-REMAKE grass interaction: bend vegetation vertices radially away from
  // the player. Falloff is radial in the XZ plane; the bend is weighted by the
  // vertex height above the player's feet so blade tips move more than roots,
  // and the top of the tuft is pressed down slightly (trampling).
  vec3 world_pos = position_in;

  // PC-REMAKE ambient wind sway: two offset sine waves per world position give
  // an organic ripple; weight grows with height so roots stay planted.
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
    const float PUSH_RADIUS = 7372.8;   // 1.8 m
    const float PUSH_AMOUNT = 1843.2;   // 0.45 m max horizontal bend
    vec2 to_vert = world_pos.xz - grass_push.xz;
    float dist_xz = length(to_vert);
    float dy = world_pos.y - grass_push.y;
    // only affect vertices near the player and within a sane height band
    if (dist_xz < PUSH_RADIUS && dy > -2048.0 && dy < 8192.0) {
      float fall = 1.0 - dist_xz / PUSH_RADIUS;
      fall = fall * fall;                                  // smooth falloff
      float height_w = clamp(dy / 4096.0 + 0.35, 0.0, 1.0); // roots stay put
      vec2 dir = dist_xz > 1.0 ? to_vert / dist_xz : vec2(0.0, 1.0);
      float amount = grass_push.w * fall * height_w * PUSH_AMOUNT;
      world_pos.xz += dir * amount;
      world_pos.y -= 0.35 * amount;                        // slight squash
    }
  }

  if (grass_clear.w > 0.001) {
    const float CLEAR_IN = 4096.0 * 3.5;   // couchee a plat
    const float CLEAR_OUT = 4096.0 * 7.0;  // fondu : intacte au-dela
    vec2 to_c = world_pos.xz - grass_clear.xz;
    float dc = length(to_c);
    float dyc = world_pos.y - grass_clear.y;
    if (dc < CLEAR_OUT && dyc > -4096.0 * 3.0 && dyc < 4096.0 * 6.0) {
      float k = grass_clear.w * (1.0 - smoothstep(CLEAR_IN, CLEAR_OUT, dc));
      // hauteur au-dessus du sol de Jak : les brins se couchent vers l'exterieur
      float above = max(dyc + 4096.0 * 0.3, 0.0);
      vec2 dir_c = dc > 1.0 ? to_c / dc : vec2(0.0, 1.0);
      world_pos.y -= above * 0.85 * k;
      world_pos.xz += dir_c * (above * 0.5 * k);
    }
  }

  // PC-REMAKE sun shadows (after grass-push deformation so shadows track bent blades)
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
