#version 410 core

out vec4 color;
// G-buffer de normales (attachement 1)
layout (location = 1) out vec4 out_normal;
in vec3 w_nor;
// RELIEF PAR PIXEL des palmiers
in vec3 v_pos_p;
uniform sampler2D sculpt_tex;
uniform float sculpt_amp;  // carte sculptee de la texture (0 = sans)
uniform float relief_feuille;  // 1 = relief tire du dessin (palmes), 0 = rien
uniform float hang_sway;  // 1 = objet suspendu (poissons), pas du feuillage

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;


void main() {
  // derivees AVANT tout discard
  vec2 uv0 = tex_coord.xy;
  vec2 duvx = dFdx(uv0), duvy = dFdy(uv0);
  vec3 dpx = dFdx(v_pos_p), dpy = dFdy(v_pos_p);
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }
  // QA draws a decoupe peints en couleur pleine (tfrag=rouge, tie_wind=bleu, etie=jaune)

  // ALPHA AFFUTE pour l'alpha-to-coverage (MSAA)
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0, 1));
  // alpha 0.95 = FEUILLAGE
  vec3 n_out = normalize(w_nor);
  if (gfx_hack_no_tex == 0 && (sculpt_amp > 0.0 || relief_feuille > 0.0)) {
    vec3 N = n_out;
    float aire_uv = abs(duvx.x * duvy.y - duvx.y * duvy.x);
    float monde_par_uv = sqrt(length(cross(dpx, dpy)) / max(aire_uv, 1e-20));
    float h0, hx, hy;
    if (sculpt_amp > 0.0) {
      // ecailles du tronc
      float H = min(2.0 * sculpt_amp * monde_par_uv, 0.04 * 4096.0);
      h0 = (texture(sculpt_tex, uv0).r * 2.0 - 1.0) * H;
      hx = (texture(sculpt_tex, uv0 + duvx).r * 2.0 - 1.0) * H;
      hy = (texture(sculpt_tex, uv0 + duvy).r * 2.0 - 1.0) * H;
    } else {
      // folioles : le dessin clair = foliole en relief, sombre = creux entre elles
      const vec3 Y = vec3(0.299, 0.587, 0.114);
      float H = 0.015 * 4096.0;
      h0 = dot(texture(tex_T0, uv0).rgb, Y) * H;
      hx = dot(texture(tex_T0, uv0 + duvx).rgb, Y) * H;
      hy = dot(texture(tex_T0, uv0 + duvy).rgb, Y) * H;
    }
    vec3 r1 = cross(dpy, N), r2 = cross(N, dpx);
    float det = dot(dpx, r1);
    if (abs(det) > 1e-12) {
      vec3 grad = sign(det) * ((hx - h0) * r1 + (hy - h0) * r2);
      n_out = normalize(abs(det) * N - grad);
    }
  }
  out_normal = vec4(n_out * 0.5 + 0.5, hang_sway > 0.5 ? 1.0 : 0.95);
}
