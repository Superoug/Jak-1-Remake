#version 410 core
// PERSONNAGES DECOUPES, passe des reflets (etape )
layout (triangles, fractional_odd_spacing, ccw) in;

in vec4 tc_pos[];
in vec3 tc_nrm[];
in vec3 tc_weld[];
in float tc_vif[];
in vec2 tc_st[];
in vec4 tc_rgba[];
in vec3 tc_ref[];
// matiere du dessin
uniform int remake_famille;
// bruit de valeur 3D
float pc_h3(vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
float pc_bruit(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(pc_h3(i), pc_h3(i + vec3(1, 0, 0)), f.x),
                 mix(pc_h3(i + vec3(0, 1, 0)), pc_h3(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(pc_h3(i + vec3(0, 0, 1)), pc_h3(i + vec3(1, 0, 1)), f.x),
                 mix(pc_h3(i + vec3(0, 1, 1)), pc_h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// BOIS : forme irreguliere d'un rondin, d'un baton
float pc_bois_relief(vec3 r) {
  return (pc_bruit(r / 820.0) - 0.5) * 70.0 + (pc_bruit(r / 210.0) - 0.5) * 28.0;
}

uniform vec4 hvdf_offset;
uniform vec4 fog_constants;

uniform mat4 perspective_matrix;

uniform vec4 fade;

// output
out vec3 vtx_color;
out vec2 vtx_st;

out float fog;
// tight sun glint for metal materials
out float vtx_glint;
// xyz = sun direction in this shader's space, w = enable
uniform vec4 sun_dir_view;
// reflet et eclat calcules PAR PIXEL (voir emerc.frag)
out vec3 v_nrm;
out vec3 v_pos;
out vec4 v_unp;  // position deperspectivee (vf09 du microcode)
out vec4 v_unpk;  // constantes de deperspective (vf23)

// point de controle d'une arete (i -> j), vu depuis i
vec3 bord(vec3 pi, vec3 pj, vec3 ni, float vif) {
  precise vec3 r = 2.0 * pi + pj;
  if (vif < 0.5) {
    r -= dot(pj - pi, ni) * ni;
  }
  return r / 3.0;
}

// normale intermediaire d'une arete (quadratique PN)
vec3 nmilieu(vec3 pi, vec3 pj, vec3 ni, vec3 nj) {
  vec3 d = pj - pi;
  float dd = max(dot(d, d), 1e-8);
  return normalize(ni + nj - 2.0 * dot(d, ni + nj) / dd * d);
}

void main() {
  float u = gl_TessCoord.x, v = gl_TessCoord.y, w = gl_TessCoord.z;
  vec3 p1 = tc_pos[0].xyz, p2 = tc_pos[1].xyz, p3 = tc_pos[2].xyz;
  vec3 n1 = tc_weld[0], n2 = tc_weld[1], n3 = tc_weld[2];
  float v12 = max(tc_vif[0], tc_vif[1]);
  float v23 = max(tc_vif[1], tc_vif[2]);
  float v31 = max(tc_vif[2], tc_vif[0]);
  precise vec3 b210 = bord(p1, p2, n1, v12), b120 = bord(p2, p1, n2, v12);
  precise vec3 b021 = bord(p2, p3, n2, v23), b012 = bord(p3, p2, n3, v23);
  precise vec3 b102 = bord(p3, p1, n3, v31), b201 = bord(p1, p3, n1, v31);
  precise vec3 E = (b210 + b120 + b021 + b012 + b102 + b201) / 6.0;
  precise vec3 V = (p1 + p2 + p3) / 3.0;
  precise vec3 b111 = E + (E - V) * 0.5;
  precise vec3 pos = p1 * (u * u * u) + p2 * (v * v * v) + p3 * (w * w * w)
                   + b210 * (3.0 * u * u * v) + b120 * (3.0 * u * v * v)
                   + b201 * (3.0 * u * u * w) + b102 * (3.0 * u * w * w)
                   + b021 * (3.0 * v * v * w) + b012 * (3.0 * v * w * w)
                   + b111 * (6.0 * u * v * w);
  vec3 ref = u * tc_ref[0] + v * tc_ref[1] + w * tc_ref[2];
  if (remake_famille == 10) {
    // bois : bosses et creux le long de la normale soudee (meme champ des deux cotes d'une couture)
    vec3 nw = normalize(u * tc_weld[0] + v * tc_weld[1] + w * tc_weld[2]);
    pos += nw * pc_bois_relief(ref);
  }
  precise vec4 vtx_pos = vec4(pos, u * tc_pos[0].w + v * tc_pos[1].w + w * tc_pos[2].w);

  // normale d'eclairage
  vec3 q1 = tc_nrm[0], q2 = tc_nrm[1], q3 = tc_nrm[2];
  vec3 m12 = v12 > 0.5 ? normalize(q1 + q2) : nmilieu(p1, p2, q1, q2);
  vec3 m23 = v23 > 0.5 ? normalize(q2 + q3) : nmilieu(p2, p3, q2, q3);
  vec3 m31 = v31 > 0.5 ? normalize(q3 + q1) : nmilieu(p3, p1, q3, q1);
  vec3 rotated_nrm = normalize(q1 * (u * u) + q2 * (v * v) + q3 * (w * w)
                             + m12 * (u * v) + m23 * (v * w) + m31 * (w * u));

  vec2 st_in = u * tc_st[0] + v * tc_st[1] + w * tc_st[2];
  vec4 transformed = perspective_matrix * vtx_pos;

  v_nrm = rotated_nrm;
  v_pos = vtx_pos.xyz;

  // sun glint. The view direction in this space is the vertex direction itself; a Blinn-style
  {
    // this shader has no light uniforms, so the glint is built from the sun direction supplied by the PC
    vec3 V = normalize(-vtx_pos.xyz);
    vec3 L = normalize(sun_dir_view.xyz);
    vec3 H = normalize(L + V);
    float nh = max(dot(rotated_nrm, H), 0.0);
    vtx_glint = pow(nh, 30.0) * sun_dir_view.w;
  }

  float Q = fog_constants.x / transformed[3];
  fog = 255 - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);

  // emerc
  vec2 st_mod = st_in;
  {
    vec4 vf10 = vec4(rotated_nrm, 1);  // ??
    // vf08 = transformed
    vec4 vf08 = transformed;
    // vf23 = unperspect unperspect (1/P(0, 0), 1/P(1, 1), 0.5, 1/P(2, 3))
    vec4 vf23 = vec4(1. / perspective_matrix[0][0],
                     1. / perspective_matrix[1][1],
                     0.5,
                     1. / perspective_matrix[2][3]);
    // vf14 = rgba-fade
    vec4 vf14 = fade;
    // vf24 = normal st mul.xyzw vf09, vf08, vf23 ;; do unperspect
    vec4 vf09 = vf08 * vf23;
    v_unp = vf09;
    v_unpk = vf23;
    // subw.z vf10, vf10, vf00 ;; subtract 1 from z
    vf10.z -= 1;
    // addw.z vf09, vf00, vf09 ;; xyww the unperspected thing
    vf09.z = vf09.w;
    vec3 vf15 = vf09.xyz * vf10.xyz;
    // adday.xyzw vf15, vf15 maddz.x vf15, vf21, vf15
    float vf15_x = vf15.x + vf15.y + vf15.z;
    // div Q, vf15.x, vf10.z
    float qq = vf15_x / vf10.z;
    // mulaw.xyzw ACC, vf09, vf00
    vec4 ACC = vf09;
    // mul.xyzw vf09, vf08, vf23
    vf09 = vf08 * vf23;
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qq;
    // eleng.xyz P, vf10
    float P = length(vf10.xyz);
    // mfp.w vf10, P div Q, vf23.z, vf10.w
    float qqq = vf23.z / P;
    // addaz.xyzw vf00, vf23
    ACC = vec4(vf23.z, vf23.z, vf23.z, vf23.z + 1.);
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qqq;
    st_mod = vf10.xy;

    // this is required to make jak 1's envmapping look right otherwise it behaves like the envmap texture
    st_mod.x = 1 - vf10.x;
    st_mod.y = 1 - vf10.y;

    // mulz.xy vf24, vf10, vf24 ;; mul tex by q
  }

  transformed.xyz *= Q;
  transformed.xyz += hvdf_offset.xyz;
  transformed.xy -= (2048.);
  transformed.z /= (8388608);
  transformed.z -= 1;
  transformed.x /= (256);
  transformed.y /= -(128);
  transformed.xyz *= transformed.w;
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;


  vtx_color = fade.xyz;
  vtx_st = st_mod;
}
