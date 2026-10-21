#version 410 core
// PC-REMAKE 20/10 — RAYONS DE SOLEIL VOLUMETRIQUES (jungle), calcules au QUART de la
// resolution puis adoucis (shafts_blur.frag) avant d'etre ajoutes par le post : sans grain.
// Le regard est suivi de la camera au pixel (70 m au plus) ; a chaque pas, la carte d'ombres
// du soleil dit si le soleil y passe, et un point voisin (~3 m) s'il est a l'ombre. Seul
// compte l'air ECLAIRE A COTE DE L'OMBRE : les colonnes de lumiere des trouees du feuillage
// (une clairiere en plein soleil ne voile pas l'image).

in vec2 tex_coord;
out vec4 color;

uniform sampler2D depth_tex;        // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform sampler2DShadow sun_shadow_tex;  // unite 14
uniform mat4 sun_light_vp;
uniform mat4 inv_camera;
uniform vec4 cam_trans;
uniform vec4 sun_dir;               // vers le soleil, w = 1 si valide
uniform float shaft_indoor;         // 0 = dehors (jungle), 1 = interieur (cabane de Samos)

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
  // epaisseur d'air : 40 m dehors, 6 m dans une piece (sinon les rayons d'une petite piece
  // restaient a 20 % de leur force)
  float sdepth = 1.0 - exp(-slen / (4096.0 * mix(40.0, 6.0, shaft_indoor)));
  color = vec4(sshaft * sdepth * sphase * 5.0, 0.0, 0.0, 1.0);
}
