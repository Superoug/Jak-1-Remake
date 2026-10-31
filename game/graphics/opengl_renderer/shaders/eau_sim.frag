#version 410 core
// EAU VIVANTE

in vec2 tex_coord;
out vec4 color;

uniform sampler2D sim_prec;  // etat precedent
uniform vec4 sim_decal;  // xy = decalage de la grille depuis l'image precedente (uv)
uniform vec4 sim_box;  // x0, z0 (m), cote (m), texel (uv)
uniform vec4 sim_param;  // x = c^2 (en texels), y = amortissement, z = dt, w = viscosite
// sources : xy = position (uv), z = rayon (uv), w = hauteur ajoutee (m) cette image
uniform vec4 sim_src[4];
uniform int sim_n;

uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
uniform vec4 riv_box[2];
uniform int riv_n;

float riv_dist_s(vec2 xz_m) {
  float d = 99.0;
  if (riv_n > 0) {
    vec2 uv = (xz_m - riv_box[0].xy) * riv_box[0].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = texture(riv_tex0, uv).r;
    }
  }
  if (riv_n > 1) {
    vec2 uv = (xz_m - riv_box[1].xy) * riv_box[1].zw;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
      d = min(d, texture(riv_tex1, uv).r);
    }
  }
  return d;
}

vec2 etat(vec2 uv) {
  vec2 q = uv + sim_decal.xy;
  if (q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0) {
    return vec2(0.0);
  }
  return texture(sim_prec, q).rg;
}

void main() {
  float e = sim_box.w;
  vec2 c = etat(tex_coord);
  float h = c.r, hp = c.g;
  vec2 n0 = etat(tex_coord + vec2(e, 0.0)), n1 = etat(tex_coord - vec2(e, 0.0));
  vec2 n2 = etat(tex_coord + vec2(0.0, e)), n3 = etat(tex_coord - vec2(0.0, e));
  float lap = n0.r + n1.r + n2.r + n3.r - 4.0 * h;
  // viscosite (sim_param.w)
  float v = h - hp;
  float lapv = (n0.r - n0.g) + (n1.r - n1.g) + (n2.r - n2.g) + (n3.r - n3.g) - 4.0 * v;
  float hn = h + v * sim_param.y + sim_param.x * lap + sim_param.w * lapv;
  vec2 pm = sim_box.xy + tex_coord * sim_box.z;
  for (int i = 0; i < 4; i++) {
    if (i >= sim_n) {
      break;
    }
    vec2 d = (tex_coord - sim_src[i].xy) / max(sim_src[i].z, 1e-5);
    hn += sim_src[i].w * exp(-dot(d, d) * 2.0);
  }
  // la terre (et le bord de l'eau) ne bouge pas
  float d_riv = riv_dist_s(pm);
  if (d_riv < 0.0 && d_riv > -90.0) {
    hn = 0.0;
  }
  // bords de la grille
  vec2 b = min(tex_coord, 1.0 - tex_coord);
  hn *= smoothstep(0.0, 0.06, min(b.x, b.y));
  hn = clamp(hn, -0.6, 0.6);
  color = vec4(hn, h, 0.0, 1.0);
}
