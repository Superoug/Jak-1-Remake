#version 410 core
// PERSONNAGES DECOUPES (etape surface courbe)
layout (triangles, fractional_odd_spacing, ccw) in;

in vec4 tc_pos[];
in vec3 tc_nrm[];
in vec3 tc_weld[];
in float tc_vif[];
in vec2 tc_st[];
in vec4 tc_rgba[];
in vec3 tc_ref[];
in vec3 tc_refn[];
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

// light control
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;
uniform vec3 light_dir2;
uniform vec4 light_col0;
uniform vec4 light_col1;
uniform vec4 light_col2;
uniform vec4 light_ambient;

// camera control
uniform vec4 hvdf_offset;
uniform vec4 fog_constants;
uniform mat4 perspective_matrix;
uniform mat4 sun_light_from_cam;
uniform mat3 world_nrm_from_merc;
uniform float write_normals;

out vec3 w_nor;
out vec4 vtx_color;
out vec2 vtx_st;
out vec4 sun_light_pos;
out vec3 v_nrm;
out vec4 v_rgba;
out vec3 v_pos;
out vec3 v_ref;
out vec3 v_refn;
out vec3 v_lava_w;  // lave (merc2.frag), sans objet ici
out float fog;

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

  vec4 transformed = perspective_matrix * vtx_pos;
  sun_light_pos = sun_light_from_cam * vtx_pos;

  w_nor = (write_normals > 0.5) ? normalize(world_nrm_from_merc * rotated_nrm) : vec3(0.0);
  vec3 light_intensity = light_dir0_fade.xyz * rotated_nrm.x + light_dir1_fade_en.xyz * rotated_nrm.y + light_dir2 * rotated_nrm.z;
  light_intensity = max(light_intensity, vec3(0, 0, 0));
  vec4 light_color = light_ambient
                   + light_intensity.x * light_col0
                   + light_intensity.y * light_col1
                   + light_intensity.z * light_col2;

  float Q = fog_constants.x / transformed[3];
  fog = 255 - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);

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

  vec4 rgba = u * tc_rgba[0] + v * tc_rgba[1] + w * tc_rgba[2];
  vtx_color = rgba * light_color;
  v_nrm = rotated_nrm;
  v_rgba = rgba;
  v_pos = vtx_pos.xyz;
  v_ref = ref;
  v_refn = u * tc_refn[0] + v * tc_refn[1] + w * tc_refn[2];
  v_lava_w = vec3(0.0);
  vtx_st = u * tc_st[0] + v * tc_st[1] + w * tc_st[2];
}
