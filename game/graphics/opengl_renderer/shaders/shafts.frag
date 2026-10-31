#version 410 core
// RAYONS DE SOLEIL VOLUMETRIQUES (jungle), calcules au QUART de la resolution puis adoucis

in vec2 tex_coord;
out vec4 color;

uniform sampler2D depth_tex;  // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform sampler2DShadow sun_shadow_tex;  // unite 14
uniform mat4 sun_light_vp;
uniform mat4 inv_camera;
uniform vec4 cam_trans;
uniform vec4 sun_dir;  // vers le soleil, w = 1 si valide
uniform float shaft_indoor;  // 0 = dehors (jungle), 1 = interieur (cabane de Samos)
uniform float shaft_time;  // temps (s), mouvement du feuillage des faisceaux

// --- FAISCEAUX DE CANOPEE (jungle)
const float CB_CELL = 7.0;
vec3 cb_axis(vec3 to_sun) {
  return normalize(vec3(to_sun.x * 0.36, 1.0, to_sun.z * 0.36));
}
// projection le long de L sur le plan y = 0 (metres)
vec2 cb_proj(vec3 p_m, vec3 L) {
  return p_m.xz - L.xz / L.y * p_m.y;
}
float cb_hash(vec2 c, float k) {
  return fract(sin(dot(c, vec2(127.1, 311.7)) + k * 74.7) * 43758.5453);
}
// colonne de la cellule
vec4 cb_beam(vec2 cell, float t) {
  if (cb_hash(cell, 0.0) > 0.24) {
    return vec4(0.0);
  }
  float h = cb_hash(cell, 1.0);
  float r = mix(0.45, 1.3, h);
  vec2 off = vec2(cb_hash(cell, 2.0), cb_hash(cell, 3.0));
  vec2 ctr = cell * CB_CELL + vec2(r) + off * (CB_CELL - 2.0 * r);
  float w = mix(0.55, 1.0, cb_hash(cell, 4.0));
  // le feuillage bouge
  w *= 0.78 + 0.22 * sin(t * (0.45 + 0.35 * h) + 6.2832 * cb_hash(cell, 5.0));
  return vec4(ctr, r, w);
}
float cb_erf(float x) {
  return tanh(1.202 * x + 0.083 * x * x * x);
}
// une colonne n'a de source que s'il y a du ciel au-dessus du feuillage
float cb_ciel(vec3 q, vec3 L) {
  for (int i = 1; i <= 2; i++) {
    vec3 s = (sun_light_vp * vec4(q + L * (12.0 * 4096.0 * float(i)), 1.0)).xyz * 0.5 + 0.5;
    if (s.x < 0.01 || s.x > 0.99 || s.y < 0.01 || s.y > 0.99 || s.z > 1.0) {
      return 1.0;
    }
    if (texture(sun_shadow_tex, vec3(s.xy, s.z - 0.002)) > 0.5) {
      return 1.0;
    }
  }
  return 0.0;
}

vec3 world_from_depth(vec2 uv, float d) {
  vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  vec4 p = inv_camera * ndc;
  if (abs(p.w) < 1e-8) {
    return cam_trans.xyz;
  }
  return p.xyz / p.w + cam_trans.xyz;
}

void main() {
  color = vec4(0.0);
  if (sun_dir.w < 0.5) {
    return;
  }
  float d = texture(depth_tex, tex_coord).r;
  vec3 sdir;
  float slen;
  if (d < 1e-5) {
    sdir = normalize(world_from_depth(tex_coord, 0.5) - world_from_depth(tex_coord, 0.99));
    slen = 4096.0 * 70.0;
  } else {
    vec3 sdv = world_from_depth(tex_coord, d) - cam_trans.xyz;
    slen = min(length(sdv), 4096.0 * 70.0);
    sdir = sdv / max(length(sdv), 1.0);
  }
  // dans la cabane de Samos, seul compte l'air DE la cabane
  if (shaft_indoor > 0.001) {
    vec3 o = cam_trans.xyz * (1.0 / 4096.0);
    vec2 oc = o.xz - vec2(-130.5, 208.4);
    float a = dot(sdir.xz, sdir.xz);
    float c0 = dot(oc, oc) - 16.4 * 16.4;
    float t_out = 1e9;
    if (a > 1e-6 && c0 < 0.0) {
      float b = dot(sdir.xz, oc);
      t_out = (-b + sqrt(max(b * b - a * c0, 0.0))) / a;
    }
    if (sdir.y > 1e-4) {
      t_out = min(t_out, max(80.0 - o.y, 0.0) / sdir.y);
    } else if (sdir.y < -1e-4) {
      t_out = min(t_out, max(o.y - 31.5, 0.0) / -sdir.y);
    }
    slen = min(slen, t_out * 4096.0);
  }
  float sjit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float sshaft = 0.0;
  const int NS = 32;
  for (int si = 0; si < NS; si++) {
    vec3 sq = cam_trans.xyz + sdir * (slen * (float(si) + sjit) / float(NS));
    vec3 ss = (sun_light_vp * vec4(sq, 1.0)).xyz * 0.5 + 0.5;
    if (ss.x < 0.02 || ss.x > 0.98 || ss.y < 0.02 || ss.y > 0.98 || ss.z > 1.0) {
      continue;
    }
    float sl = texture(sun_shadow_tex, vec3(ss.xy, ss.z - 0.002));
    if (sl > 0.01) {
      float sa = float(si) * 2.39996 + sjit * 6.2832;
      vec2 so = vec2(cos(sa), sin(sa)) * (3.0 / 240.0);
      float sn = texture(sun_shadow_tex, vec3(ss.xy + so, ss.z - 0.002));
      sshaft += sl * (1.0 - sn);
    }
  }
  sshaft /= float(NS);
  float smu = max(dot(sdir, sun_dir.xyz), 0.0);
  float sphase = 0.6 + 1.3 * pow(smu, 6.0);
  // epaisseur d'air
  float sdepth = 1.0 - exp(-slen / (4096.0 * mix(40.0, 6.0, shaft_indoor)));
  // faisceaux de canopee (canal vert), dehors seulement
  float cb = 0.0;
  if (shaft_indoor < 0.5) {
    vec3 L = cb_axis(sun_dir.xyz);
    vec3 ro = cam_trans.xyz / 4096.0;
    float tmax = slen / 4096.0;
    vec2 X0 = cb_proj(ro, L);
    vec2 V = sdir.xz - L.xz / L.y * sdir.y;
    float v2 = max(dot(V, V), 1e-6);
    float vl = sqrt(v2);
    vec2 cell = floor(X0 / CB_CELL);
    vec2 st = vec2(V.x >= 0.0 ? 1.0 : -1.0, V.y >= 0.0 ? 1.0 : -1.0);
    vec2 av = max(abs(V), vec2(1e-5));
    vec2 tdel = CB_CELL / av;
    vec2 tnext = ((cell + max(st, 0.0)) * CB_CELL - X0) * st / av;
    float tin = 0.0;
    for (int k = 0; k < 24; k++) {
      vec4 b = cb_beam(cell, shaft_time);
      if (b.w > 0.0) {
        float tc = dot(b.xy - X0, V) / v2;
        vec2 dv = X0 + V * tc - b.xy;
        float sg = b.z * 0.62;
        float dp2 = dot(dv, dv) / (sg * sg);
        if (dp2 < 9.0) {
          // longueur equivalente (m) de colonne traversee entre la camera et la surface
          float seg = (sg / vl) * 0.8862 *
                      (cb_erf(vl * (tmax - tc) / sg) - cb_erf(vl * (0.0 - tc) / sg));
          seg = min(seg, min(tmax, 6.0));
          vec3 q = (ro + sdir * clamp(tc, 0.0, tmax)) * 4096.0;
          vec3 qs = (sun_light_vp * vec4(q, 1.0)).xyz * 0.5 + 0.5;
          float ombre = 0.0;
          if (qs.x > 0.01 && qs.x < 0.99 && qs.y > 0.01 && qs.y < 0.99 && qs.z < 1.0) {
            // 4 points ecartes (~1 m)
            vec2 o = vec2(1.0 / 240.0, 0.0);
            ombre = 1.0 - 0.25 * (texture(sun_shadow_tex, vec3(qs.xy + o, qs.z - 0.002)) +
                                  texture(sun_shadow_tex, vec3(qs.xy - o, qs.z - 0.002)) +
                                  texture(sun_shadow_tex, vec3(qs.xy + o.yx, qs.z - 0.002)) +
                                  texture(sun_shadow_tex, vec3(qs.xy - o.yx, qs.z - 0.002)));
          }
          if (ombre > 0.01) {
            ombre *= cb_ciel(q, L);
          }
          // au loin, la brume reprend la main
          float tcc = clamp(tc, 0.0, tmax);
          float loin = (1.0 - smoothstep(35.0, 70.0, tcc)) * smoothstep(6.0, 14.0, tcc);
          // stries le long de l'axe
          vec2 ax = normalize(vec2(-V.y, V.x) + vec2(1e-5, 0.0));
          float u = dot(dv, ax) / b.z;
          float stries = 0.55 + 0.45 * sin(u * 9.0 + b.x * 3.1) * sin(u * 4.3 - b.y * 2.3 + 1.7);
          // plus vive en haut, sous le feuillage, plus diffuse en descendant vers le sol
          float haut = mix(0.45, 1.0, smoothstep(-4.0, 9.0, q.y / 4096.0 - ro.y));
          cb += exp(-dp2) * seg * b.w * ombre * loin * stries * haut;
        }
      }
      float tout = min(tnext.x, tnext.y);
      if (tout > tmax) {
        break;
      }
      if (tnext.x < tnext.y) {
        cell.x += st.x;
        tnext.x += tdel.x;
      } else {
        cell.y += st.y;
        tnext.y += tdel.y;
      }
    }
  }
  color = vec4(sshaft * sdepth * sphase * 5.0, cb, 0.0, 1.0);
}
