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
// 21/10 : 1 = orbe de precurseur — reflet beaucoup plus fort, bord miroir, eclat net
uniform int reflet_orbe;
// reflet des orbes calcule dans le MONDE : normale merc -> monde (la meme matrice que le G-buffer
// de normales), soleil en monde ; la vue est +position dans la convention des normales merc
uniform mat3 world_nrm_from_merc;
uniform vec4 sun_dir_world;  // vers le soleil, w = force


void main() {
  if (gfx_hack_no_tex == 0) {
    vec3 n = normalize(v_nrm);
    // (original : vec4 T0 = texture(tex_T0, vtx_st);)
    vec4 T0 = texture(tex_T0, emerc_uv(n));

    color.a = T0.a;
    color.rgb = T0.rgb * vtx_color;
    color *= 2;
    // 20/10 : reflets precurseur renforces, comme le decor (etie.frag) : x2.2 avec une epaule
    // pour les reflets forts (original : x1)
    color.rgb *= 2.2 / (1.0 + 1.1 * dot(color.rgb, vec3(0.299, 0.587, 0.114)));
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
    if (reflet_orbe == 1) {
      // ORBE : surface polie de metal precieux. Le ciel et le sol se refletent (ciel clair en
      // haut, sol sombre en bas — le contraste fait le miroir), teintes par l'or de l'orbe, plus
      // forts sur les bords (Fresnel), et un eclat du soleil net. La couleur propre de l'orbe
      // est assombrie dans la passe de base (merc2.frag) pour que le reflet se lise.
      // (la matrice ramene la normale merc retournee : le post la remet face a la camera)
      vec3 Nw = -normalize(world_nrm_from_merc * n);
      vec3 Vw = -normalize(world_nrm_from_merc * v_pos);  // vers la camera
      float nv = clamp(abs(dot(Nw, Vw)), 0.0, 1.0);
      float fres = 0.45 + 0.55 * pow(1.0 - nv, 3.0);
      vec3 R = reflect(-Vw, Nw);
      float h = R.y;
      // horizon NET (c'est lui qui fait le miroir) : ciel clair au-dessus, sol sombre juste dessous
      vec3 haut = mix(vec3(1.25, 1.18, 1.05), vec3(0.62, 0.78, 1.05), smoothstep(0.0, 0.6, h));
      vec3 bas = mix(vec3(0.30, 0.24, 0.18), vec3(0.08, 0.06, 0.05), smoothstep(0.0, 0.5, -h));
      vec3 ciel = mix(bas, haut, smoothstep(-0.025, 0.025, h));
      float jour = 0.35 + 0.65 * clamp(glint_strength / 0.85, 0.0, 1.0);
      vec3 or_ = vec3(1.0, 0.74, 0.45);
      color.rgb *= 0.6;  // le reflet flou d'origine s'efface devant le vrai reflet
      color.rgb += ciel * or_ * (fres * 1.25 * jour);
      float nh = max(dot(Nw, normalize(sun_dir_world.xyz + Vw)), 0.0);
      color.rgb += vec3(1.0, 0.96, 0.88) * (pow(nh, 200.0) * 3.0 + pow(nh, 30.0) * 0.3) *
                   sun_dir_world.w * max(glint_strength, 0.25);

    }
  } else {
    color.rgb = vtx_color;
    color.a = 1;
  }
}
