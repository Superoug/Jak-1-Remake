#version 410 core
// PC-REMAKE — LUMIERE REBONDIE (illumination globale a l'ecran), au QUART de la resolution puis
// adoucie (gi_flou.frag) avant d'etre ajoutee par le post (post_processing.frag). Pour chaque
// point, 16 points visibles autour de lui dans un disque de l'ecran (quelques metres) : ceux qui
// sont au-dessus de sa surface et lui font face lui renvoient leur couleur (la scene sans HUD),
// plus fort de face et de pres. Le sable au soleil eclaire le dessous des rochers, l'herbe verdit le
// bas des murs, la lave teinte ses abords. Le ciel n'en fait pas partie (ambiant du post).

in vec2 tex_coord;
out vec4 color;

uniform sampler2D depth_tex;   // unite 15 : profondeur resolue (inversee : 0 = ciel)
uniform sampler2D normal_tex;  // unite 13 : G-buffer (rgb = normale * 0.5 + 0.5, a > 0.5 si presente)
uniform sampler2D scene_tex;   // unite 12 : scene sans HUD
uniform float has_normal_tex;
uniform mat4 inv_camera;
uniform mat4 clip_from_world;  // relatif a la camera
uniform vec4 cam_trans;

const float M = 4096.0;

vec3 monde(vec2 uv, float d) {
  vec4 p = inv_camera * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  return abs(p.w) < 1e-8 ? cam_trans.xyz : p.xyz / p.w + cam_trans.xyz;
}

vec3 normale(vec2 uv, vec3 P, out bool ok, out bool feuille) {
  ok = false;
  feuille = false;
  if (has_normal_tex > 0.5) {
    vec4 g = texture(normal_tex, uv);
    feuille = g.a > 0.92 && g.a < 0.98;  // feuillage (tie_wind, shrub) : double face
    if (g.a > 0.5) {
      vec3 n = g.rgb * 2.0 - 1.0;
      float l = length(n);
      if (l > 0.3) {
        ok = true;
        n /= l;
        return dot(n, cam_trans.xyz - P) < 0.0 ? -n : n;
      }
    }
  }
  return vec3(0.0, 1.0, 0.0);
}

void main() {
  float d = texture(depth_tex, tex_coord).r;
  if (d < 1e-5) {
    color = vec4(0.0);
    return;
  }
  vec3 P = monde(tex_coord, d);
  float vue = length(P - cam_trans.xyz);
  if (vue > 120.0 * M) {
    color = vec4(0.0);
    return;
  }
  bool nok, nfeuille;
  vec3 N = normale(tex_coord, P, nok, nfeuille);
  if (!nok) {
    // repli : normale de la profondeur (deux voisins)
    vec2 px = 1.0 / vec2(textureSize(depth_tex, 0)) * 3.0;
    float dx = texture(depth_tex, tex_coord + vec2(px.x, 0.0)).r;
    float dy = texture(depth_tex, tex_coord + vec2(0.0, px.y)).r;
    if (dx < 1e-5 || dy < 1e-5) {
      color = vec4(0.0);
      return;
    }
    N = normalize(cross(monde(tex_coord + vec2(px.x, 0.0), dx) - P,
                        monde(tex_coord + vec2(0.0, px.y), dy) - P));
    if (dot(N, cam_trans.xyz - P) < 0.0) {
      N = -N;
    }
  }
  // portee : 1,5 m de pres, jusqu'a 6 m au loin ; son rayon a l'ecran
  float R = clamp(vue * 0.10, 2.5 * M, 7.0 * M);
  vec3 droite = normalize(cross(P - cam_trans.xyz, vec3(0.0, 1.0, 0.0)) + vec3(1e-4, 0.0, 0.0));
  vec4 rc = clip_from_world * vec4(P + droite * R - cam_trans.xyz, 1.0);
  float ruv = rc.w > 1e-4 ? length((rc.xy / rc.w) * 0.5 + 0.5 - tex_coord) : 0.05;
  ruv = clamp(ruv, 0.004, 0.25);
  // rotation propre a chaque pixel (bruit), lissee ensuite par le flou
  float bruit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec3 recu = vec3(0.0);
  float wsum = 0.0;
  const int NS = 16;
  // (v1 : directions de l'hemisphere projetees a l'ecran — elles tombaient presque toutes dans
  //  le vide au-dessus du sol : les surfaces VISIBLES autour du point, dans un disque de
  //  l'ecran, sont les vrais emetteurs)
  for (int k = 0; k < NS; k++) {
    float u = (float(k) + 0.5) / float(NS);
    float phi = (float(k) + bruit) * 2.39996323;  // spirale de Vogel
    vec2 o = vec2(cos(phi), sin(phi)) * sqrt(u) * ruv;
    vec2 quv = tex_coord + o * vec2(1.0, 1.6);
    if (quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) {
      continue;
    }
    float qd = texture(depth_tex, quv).r;
    if (qd < 1e-5) {
      continue;  // ciel : c'est l'ambiant
    }
    vec3 S = monde(quv, qd);
    vec3 v = S - P;
    float dist = length(v);
    if (dist < 0.05 * M || dist > R * 1.6) {
      continue;
    }
    vec3 vd = v / dist;
    float recoit = dot(N, vd);
    if (recoit < 0.05) {
      continue;  // meme plan ou derriere la surface
    }
    // la surface emettrice doit faire face au point ; le feuillage (herbe, palmes : lames fines
    // a normale verticale par convention) renvoie des deux cotes
    // (v1 : clamp(dot(Ns, -vd) * 1.2) partout — les touffes d'herbe rejetaient presque tout)
    float emet = 1.0;
    bool sok, sfeuille;
    vec3 Ns = normale(quv, S, sok, sfeuille);
    if (sok && !sfeuille) {
      emet = clamp(dot(Ns, -vd) * 1.2 + 0.2, 0.0, 1.0);
    } else if (sfeuille) {
      emet = 0.35;  // (v1 : 0.7 — l'herbe verdissait l'herbe, effet fluo)
    }
    float att = 1.0 / (1.0 + 4.0 * (dist / R) * (dist / R));
    float w = recoit * emet * att;
    recu += texture(scene_tex, quv).rgb * w;
    wsum += w;
  }
  // couleur moyenne des emetteurs, dosee par leur couverture (un mur au bord du sol en recoit
  // beaucoup, le milieu d'une plaine presque rien)
  // (v1 : somme / 16 points — la plupart des points sont trop loin ou dans le meme plan : ~0)
  vec3 moyen = wsum > 1e-4 ? recu / wsum : vec3(0.0);
  // (v1 : plein a 15 % — trop rare ; 6 %)
  color = vec4(moyen * clamp(wsum / (float(NS) * 0.06), 0.0, 1.0), 1.0);
}
