#version 410 core
// CIEL VIVANT

in vec2 tex_coord;
out vec4 color;

uniform sampler2D ombre_tex;  // r = ombre des nuages (quart de resolution)
uniform sampler2D depth_tex;  // unite 15
uniform sampler2D normal_tex;  // unite 13 : G-buffer (alpha 0.6-0.9 = emissif)
uniform sampler2DShadow sun_shadow_tex;  // unite 14
uniform mat4 sun_light_vp;
uniform float sun_map_ok;
uniform float has_normal_tex;
uniform mat4 inv_camera;
uniform vec4 cam_trans;

void main() {
  vec2 t = 1.0 / vec2(textureSize(ombre_tex, 0));
  float o = texture(ombre_tex, tex_coord).r * 4.0;
  o += (texture(ombre_tex, tex_coord + vec2(t.x, 0.0)).r + texture(ombre_tex, tex_coord - vec2(t.x, 0.0)).r +
        texture(ombre_tex, tex_coord + vec2(0.0, t.y)).r + texture(ombre_tex, tex_coord - vec2(0.0, t.y)).r) * 2.0;
  o += texture(ombre_tex, tex_coord + t).r + texture(ombre_tex, tex_coord - t).r +
       texture(ombre_tex, tex_coord + vec2(t.x, -t.y)).r + texture(ombre_tex, tex_coord + vec2(-t.x, t.y)).r;
  o /= 16.0;
  if (o < 0.003) {
    discard;
  }
  float dz = texture(depth_tex, tex_coord).r;
  if (dz > 1e-5 && has_normal_tex > 0.5) {
    float a = texture(normal_tex, tex_coord).a;
    if (a > 0.6 && a < 0.9) {
      discard;  // source de lumiere (lave, lueurs)
    }
  }
  float vis = 1.0;
  if (dz > 1e-5 && sun_map_ok > 0.5) {
    vec4 q = inv_camera * vec4(tex_coord * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
    vec3 P = q.xyz / q.w + cam_trans.xyz;
    vec3 sp = (sun_light_vp * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    if (sp.x > 0.01 && sp.x < 0.99 && sp.y > 0.01 && sp.y < 0.99 && sp.z < 1.0) {
      vis = texture(sun_shadow_tex, vec3(sp.xy, sp.z - 0.004));
    }
  }
  // ombre douce et un peu bleutee
  color = vec4(mix(vec3(1.0), vec3(0.62, 0.67, 0.78), o * vis), 1.0);
}
