#version 410 core

out vec4 color;
// G-BUFFER. Les arbustes (herbe, touffes, feuillages) n'ecrivaient PAS la normale
layout (location = 1) out vec4 out_normal;
in vec3 v_rel;  // position relative a la camera
uniform float shrub_facet;  // 1 = objet rigide : vraie facette ; 0 = verticale

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;
uniform float herbe_vert;  // etalonnage du vert de l'herbe (comme le sol, tfrag3.frag)

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// sun shadows
in vec4 sun_light_pos;
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz * 0.5 + 0.5;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;
  }
  float bias = 0.0022;  // slightly higher: blades are thin and near the ground
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      acc += texture(sun_shadow_tex, vec3(p.xy + vec2(dx, dy) * px, p.z - bias));
    }
  }
  // fondu vers la limite de la carte, comme le sol
  float rim = 1.0 - smoothstep(0.72, 0.97, length(p.xy - 0.5) * 2.0);
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

void main() {
  // derivees AVANT tout discard (flot de controle uniforme)
  vec3 dpx = dFdx(v_rel);
  vec3 dpy = dFdy(v_rel);
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
    if (herbe_vert > 0.5) {
      vec3 c = color.rgb * vec3(0.69, 0.81, 0.72);  // 0.80, 0.94, 0.84
      float l = dot(c, vec3(0.299, 0.587, 0.114));
      color.rgb = mix(vec3(l), c, 1.08);
    }
  } else {
    color = fragment_color;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  {
    vec3 n = vec3(0.0, 1.0, 0.0);
    if (shrub_facet > 0.5) {
      vec3 fn = cross(dpx, dpy);
      float fl = length(fn);
      if (fl > 1e-8) {
        n = fn / fl;
        // double face
        if (dot(n, -v_rel) < 0.0) {
          n = -n;
        }
      }
    }
    // alpha 0.95 = feuillage (voir tie_wind.frag)
    out_normal = vec4(n * 0.5 + 0.5, (shrub_facet > 0.5) ? 1.0 : 0.95);
  }
  // ALPHA AFFUTE pour l'alpha-to-coverage (MSAA)
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  // sun shadows
  color.xyz *= 1.0 - sun_shadow_factor();

  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
}
