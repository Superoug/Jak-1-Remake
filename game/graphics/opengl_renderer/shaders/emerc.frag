#version 410 core

out vec4 color;
in vec3 vtx_color;
in vec2 vtx_st;
in float fog;
in float vtx_glint;
uniform float glint_strength;


uniform sampler2D tex_T0;

// PC-REMAKE 13/10 (POINTS sur les objets precurseurs) :
// coordonnee de reflet et eclat du soleil calcules PAR PIXEL. Aux sommets, sur des
// modeles a gros triangles, l'eclat (puissance 30) et le reflet faisaient des
// points et des losanges ; par pixel ils glissent sur la surface comme sur du
// metal. Meme math que le microcode (emerc.vert).
in vec3 v_nrm;
in vec3 v_pos;
in vec4 v_unp;
in vec4 v_unpk;
uniform vec4 sun_dir_view;

vec2 emerc_uv(vec3 nrm) {
  vec4 vf10 = vec4(nrm, 1.0);
  vec4 vf09 = v_unp;
  vf10.z -= 1.0;
  vf09.z = vf09.w;
  vec3 vf15 = vf09.xyz * vf10.xyz;
  float qq = (vf15.x + vf15.y + vf15.z) / vf10.z;
  vec4 acc = vf09;
  vf10 = acc + vf10 * qq;
  float P = max(length(vf10.xyz), 1e-6);
  float qqq = v_unpk.z / P;
  acc = vec4(v_unpk.z, v_unpk.z, v_unpk.z, v_unpk.z + 1.0);
  vf10 = acc + vf10 * qqq;
  return vec2(1.0 - vf10.x, 1.0 - vf10.y);
}

uniform vec4 fog_color;
uniform int ignore_alpha;

uniform int decal_enable;

uniform int gfx_hack_no_tex;


void main() {
  if (gfx_hack_no_tex == 0) {
    vec3 n = normalize(v_nrm);
    // (original : vec4 T0 = texture(tex_T0, vtx_st);)
    vec4 T0 = texture(tex_T0, emerc_uv(n));

    color.a = T0.a;
    color.rgb = T0.rgb * vtx_color;
    color *= 2;
    // PC-REMAKE: specular sun highlight on metal — par pixel (13/10)
    // (original : * vtx_glint)
    float glint = 0.0;
    {
      vec3 V = normalize(-v_pos);
      vec3 L = normalize(sun_dir_view.xyz);
      vec3 H = normalize(L + V);
      glint = pow(max(dot(n, H), 0.0), 30.0) * sun_dir_view.w;
    }
    color.rgb += vec3(1.0, 0.97, 0.90) * glint * glint_strength;
  } else {
    color.rgb = vtx_color;
    color.a = 1;
  }
}
