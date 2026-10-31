#version 410 core

out vec4 color;
// G-buffer de normales (attachement 1)
layout (location = 1) out vec4 out_normal;

in vec4 fragment_color;
in vec3 tex_coord;
in float fogginess;
uniform sampler2D tex_T0;

uniform float alpha_min;
uniform float force_opaque;
// >0 = surface auto-lumineuse (lave, braises, lueurs précurseurs)
uniform float emissive_boost;
uniform float alpha_max;
uniform vec4 fog_color;

uniform int gfx_hack_no_tex;

// sun shadows
in vec4 sun_light_pos;
in vec3 w_nor;
flat in float v_foliage;
uniform vec3 sun_dir_w;  // direction normalisée VERS la surface (des ombres)
uniform sampler2DShadow sun_shadow_tex;
uniform float sun_shadow_strength;
// dose du relief des surfaces
uniform float relief_amt;
// LAVE ANIMEE (voir pc_lava)
uniform float lava_kind;
// metal et verre (pc_reflect_texture_kind)
uniform float reflet_kind;
// verre : panorama de l'entourage
uniform sampler2D reflet_env_tex;
uniform vec4 reflet_fwd;
uniform float wind_time;  // horloge (s), posee par TFragment pour tout le programme

// "Spyro water": x = time (s), y = strength (1 on WATER trees)
uniform vec4 water_fx;
// detail intrinseque des sols (0 = rien, 1 = plein), fondu par distance
uniform float detail_amt;
in float v_dist;
in vec3 v_pos_w;
// SCULPTURE DU DECOR
uniform sampler2D sculpt_tex;
// COUCHE DE SOL EN TRANSPARENCE (herbe posee sur le sable)
uniform float sol_melange;
// 1 sol (relief adouci), 2 sol d'herbe
uniform float sol_kind;
uniform float fronde_pli_f;  // relief des folioles (feuilles du terrain), 0 = rien
uniform float sculpt_amp;  // hauteur maxi en fraction d'une repetition de texture (0 = sans carte)
// carte de normales du sol
uniform sampler2D nrm_tex;
uniform float nrm_amt;  // 0 = pas de carte
// couche de detail des sols
uniform sampler2DArray det_tex;
uniform int det_kind;
in vec3 v_vue;

// bruit de valeur (hash)
float pc_hash21(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float pc_vnoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = pc_hash21(i);
  float b = pc_hash21(i + vec2(1.0, 0.0));
  float c = pc_hash21(i + vec2(0.0, 1.0));
  float d = pc_hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) - 0.5;
}

// --- LAVE ANIMEE
float lv_hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float lv_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(lv_hash(i), lv_hash(i + vec2(1.0, 0.0)), f.x),
             mix(lv_hash(i + vec2(0.0, 1.0)), lv_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// part incandescente d'un texel
float lv_hot(vec3 c) {
  return smoothstep(0.38, 0.75, c.r * 0.9 + c.g * 0.45 - c.b * 1.1);
}
// motif de surface de la lave a un point (m)
vec2 lv_motif(vec2 q) {
  float n1 = lv_noise(q * 0.9) * 0.6 + lv_noise(q * 2.3 + 4.1) * 0.4;
  float peau = smoothstep(0.42, 0.72, n1);
  float r = 1.0 - abs(lv_noise(q * 1.4 + 9.2) * 2.0 - 1.0);
  return vec2(peau, pow(r, 5.0));
}
// eclat final de la lave
float lv_eclat = 1.0;
vec4 pc_lava(vec4 T0, vec2 uv, vec3 pw, float t, float kind) {
  vec2 m = pw.xz / 4096.0;  // metres
  // champ de flux lent
  float a = lv_noise(m * 0.05 + vec2(3.1, 7.7)) * 6.2832 * 1.5;
  vec2 dir = vec2(cos(a), sin(a));
  // flux a deux phases (periode 5,5 s, 2 m parcourus par phase)
  float ph0 = fract(t * 0.18);
  float ph1 = fract(t * 0.18 + 0.5);
  float wa = abs(ph0 - 0.5) * 2.0;
  vec2 M0 = mix(lv_motif(m - dir * ph0 * 2.0), lv_motif(m - dir * ph1 * 2.0 + 17.3), wa);
  // la texture elle-meme glisse un peu avec le courant
  vec2 du = dir * 0.02;
  vec4 F = mix(texture(tex_T0, uv - du * ph0), texture(tex_T0, uv - du * ph1), wa);
  // chaleur qui respire
  float ch = lv_noise(m * 0.2 + vec2(t * 0.05, -t * 0.035)) * 0.65 +
             lv_noise(m * 0.6 - vec2(t * 0.09, t * 0.06)) * 0.35;
  ch = smoothstep(0.2, 0.85, ch);
  float peau = M0.x * (1.0 - 0.6 * ch);
  float veine = M0.y * (1.0 - peau);
  vec4 res = T0;
  float hot = kind < 1.5 ? max(lv_hot(T0.rgb), lv_hot(F.rgb) * 0.85) : 1.0;
  vec3 base = kind < 1.5 ? max(F.rgb, T0.rgb * 0.6) : F.rgb;
  // peau refroidie
  vec3 lave = mix(base, base * vec3(0.80, 0.42, 0.30), peau * 0.85);
  lave = mix(lave, vec3(1.0, 0.76, 0.32) * 1.2, veine * 0.7);
  res.rgb = mix(T0.rgb, lave, hot);
  lv_eclat = mix(1.0, (1.0 - 0.58 * peau) * (0.85 + 0.3 * ch) * (1.0 + 0.35 * veine), hot);
  if (kind < 1.5) {
    // croute : lueur au bord des fissures, qui respire avec la chaleur
    float bord = lv_hot(textureLod(tex_T0, uv, textureQueryLod(tex_T0, uv).x + 2.0).rgb);
    res.rgb += vec3(0.55, 0.16, 0.03) * bord * (1.0 - hot) * (0.2 + 0.4 * ch);
  }
  return res;
}

// --- LAVE QUI ONDULE
float lv_houle(vec2 m, float t) {
  vec2 w = m + 0.9 * vec2(sin(m.y * 0.23 + t * 0.07), sin(m.x * 0.19 - t * 0.05));
  float h = 0.55 * sin(dot(w, vec2(0.85, 0.53)) * 0.90 - t * 0.30) +
            0.30 * sin(dot(w, vec2(-0.42, 0.91)) * 1.46 - t * 0.42 + 1.7) +
            0.15 * sin(dot(w, vec2(0.30, -0.95)) * 2.42 - t * 0.55 + 4.1);
  return h * 0.09;
}
// coeur commun (meme code dans merc2.frag pour les lacs de lave)
vec3 lv_ondule(vec2 uv, vec3 pw, vec3 vue, float t, out vec3 n) {
  vec2 m = pw.xz / 4096.0;  // metres
  float h0 = lv_houle(m, t);
  vec2 gr = vec2(lv_houle(m + vec2(0.15, 0.0), t) - h0, lv_houle(m + vec2(0.0, 0.15), t) - h0) /
            0.15;
  n = normalize(vec3(-gr.x * 1.8, 1.0, -gr.y * 1.8));
  // coulee lente de la texture (deux phases qui se relaient, comme pc_lava)
  float a = lv_noise(m * 0.05 + vec2(3.1, 7.7)) * 6.2832 * 1.5;
  vec2 du = vec2(cos(a), sin(a)) * 0.03;
  float ph0 = fract(t * 0.12);
  float ph1 = fract(t * 0.12 + 0.5);
  float wa = abs(ph0 - 0.5) * 2.0;
  vec2 ond = gr * 0.035;
  vec3 F = mix(texture(tex_T0, uv - du * ph0 + ond).rgb, texture(tex_T0, uv - du * ph1 + ond).rgb,
               wa);
  // chaleur qui respire
  float ch = lv_noise(m * 0.2 + vec2(t * 0.05, -t * 0.035)) * 0.65 +
             lv_noise(m * 0.6 - vec2(t * 0.09, t * 0.06)) * 0.35;
  float k = (0.92 + 0.16 * ch) * (1.0 - h0 * 2.2);
  k *= 1.0 + 1.6 * (dot(n, vec3(0.2265, 0.9061, 0.3171)) - 0.9061);
  vec3 res = F * k;
  // reflet discret d'un liquide brillant, de la couleur de la lave
  vec3 v = normalize(-vue);
  vec3 R = reflect(-v, n);
  float gl = max(dot(R, vec3(0.2265, 0.9061, 0.3171)), 0.0);
  gl = gl * gl * gl * gl;
  gl = gl * gl * gl * gl;  // puissance 16
  res += (F * 0.5 + 0.25) * gl * 0.15;
  return res;
}
vec4 pc_lava_ondule(vec4 T0, vec2 uv, vec3 pw, vec3 vue, float t) {
  vec3 n;
  vec3 res = lv_ondule(uv, pw, vue, t, n);
  lv_eclat = 1.0;
  return vec4(res, T0.a);
}
// fin du code commun avec merc2.frag

// PENOMBRE CONTINUE
float pc_ombre_douce(vec2 uv, float zref) {
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  const float R = 2.6;
  float e = texture(sun_shadow_tex, vec3(uv, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(-R, -R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(R, -R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(-R, R) * px, zref));
  e += texture(sun_shadow_tex, vec3(uv + vec2(R, R) * px, zref));
  if (e < 0.001 || e > 4.999) {
    return e / 5.0;
  }
  float acc = 0.0;
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      acc += texture(sun_shadow_tex, vec3(uv + vec2(dx, dy) * (1.3 * px), zref));
    }
  }
  return acc / 25.0;
}

float sun_shadow_factor() {
  if (sun_shadow_strength < 0.001) {
    return 0.0;
  }
  vec3 p = sun_light_pos.xyz;  // ortho: no perspective divide needed
  p = p * 0.5 + 0.5;  // to [0,1]
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) {
    return 0.0;  // outside the map: lit
  }
  // BIAIS DE PENTE (clignotement uniquement sur les ombres des rochers)
  float slope = 1.0 - clamp(abs(dot(normalize(w_nor), normalize(sun_dir_w))), 0.0, 1.0);
  float bias = 0.0016 + 0.0060 * slope * slope;
  // 3x3 PCF for soft edges
  float acc = 0.0;
  vec2 px = 1.0 / vec2(textureSize(sun_shadow_tex, 0));
  // penombre continue (pc_ombre_douce)
  acc = pc_ombre_douce(p.xy, p.z - bias) * 9.0;
  // texture on sampler2DShadow returns visibility (1 = lit); invert FONDU DE BORDURE (coupure visible)
  float rim = 1.0 - smoothstep(0.72, 0.97, length(p.xy - 0.5) * 2.0);
  return (1.0 - acc / 9.0) * sun_shadow_strength * rim;
}

void main() {
  vec2 uv = tex_coord.xy;
  // derivees et hauteurs du relief prises AVANT tout discard
  vec2 rl_dx = dFdx(uv), rl_dy = dFdy(uv);
  vec3 rl_sx = dFdx(v_pos_w), rl_sy = dFdy(v_pos_w);

  // SCULPTURE (parallaxe)
  bool sc_ok = false;
  vec3 sc_N = vec3(0.0);
  float sc_h = 0.0, sc_fondu = 0.0, sc_ombre = 0.0;
  if (sculpt_amp > 0.0 && gfx_hack_no_tex == 0 && alpha_min < 0.45 && v_foliage < 0.5 &&
      emissive_boost < 0.001 && lava_kind < 0.5 && v_dist < 4096.0 * 34.0) {
    float nl = length(w_nor);
    vec3 dp2perp = cross(rl_sy, w_nor), dp1perp = cross(w_nor, rl_sx);
    vec3 T = dp2perp * rl_dx.x + dp1perp * rl_dy.x;
    vec3 B = dp2perp * rl_dx.y + dp1perp * rl_dy.y;
    float lt = length(T), lb = length(B);
    if (nl > 0.05 && lt > 1e-20 && lb > 1e-20) {
      vec3 N = w_nor / nl;
      T /= lt;
      B /= lb;
      sc_fondu = 1.0 - smoothstep(4096.0 * 18.0, 4096.0 * 34.0, v_dist);
      vec3 V = -normalize(v_vue);  // fragment -> camera
      vec3 Vt = vec3(dot(V, T), dot(V, B), max(dot(V, N), 0.12));
      // taille monde d'une unite de texture
      float aire_uv = abs(rl_dx.x * rl_dy.y - rl_dx.y * rl_dy.x);
      float monde_par_uv = sqrt(length(cross(rl_sx, rl_sy)) / max(aire_uv, 1e-20));
      float prof = min(2.0 * sculpt_amp, 0.05 * 4096.0 / max(monde_par_uv, 1e-6)) * sc_fondu;
      // decalage de parallaxe
      float prof_px = min(prof, 0.012 * 4096.0 / max(monde_par_uv, 1e-6));
      int n = int(mix(26.0, 8.0, Vt.z));
      vec2 pas = -Vt.xy / max(Vt.z, 0.4) * prof_px / float(n);
      float couche = 1.0 / float(n);
      vec2 p = uv;
      float d = 0.0;
      float hc = 1.0 - textureGrad(sculpt_tex, p, rl_dx, rl_dy).r;  // profondeur sous le sommet
      float dp = 0.0, hp = hc;
      for (int i = 0; i < 26; i++) {
        if (i >= n || d >= hc) {
          break;
        }
        dp = d;
        hp = hc;
        p += pas;
        d += couche;
        hc = 1.0 - textureGrad(sculpt_tex, p, rl_dx, rl_dy).r;
      }
      float apres = hc - d, avant = hp - dp;
      uv = mix(p, p - pas, clamp(apres / (apres - avant - 1e-6), 0.0, 1.0));
      // normale de la sculpture
      vec2 e = 1.0 / vec2(textureSize(sculpt_tex, 0));
      float hl = textureGrad(sculpt_tex, uv - vec2(e.x, 0.0), rl_dx, rl_dy).r;
      float hr = textureGrad(sculpt_tex, uv + vec2(e.x, 0.0), rl_dx, rl_dy).r;
      float hd = textureGrad(sculpt_tex, uv - vec2(0.0, e.y), rl_dx, rl_dy).r;
      float hu = textureGrad(sculpt_tex, uv + vec2(0.0, e.y), rl_dx, rl_dy).r;
      float k = min(prof, 0.03 * 4096.0 / max(monde_par_uv, 1e-6));  // avant : prof (5 cm)
      vec2 pente = vec2((hr - hl) / (2.0 * e.x), (hu - hd) / (2.0 * e.y)) * k;
      sc_N = normalize(N - T * pente.x - B * pente.y);
      float hh = textureGrad(sculpt_tex, uv, rl_dx, rl_dy).r;
      sc_h = hh * 2.0 - 1.0;
      // ombre propre vers le soleil
      vec3 L = -normalize(sun_dir_w);
      vec3 Lt = vec3(dot(L, T), dot(L, B), dot(L, N));
      if (Lt.z > 0.05) {
        vec2 pl = uv;
        vec2 pasl = Lt.xy / max(Lt.z, 0.4) * prof_px / 8.0;  // avant : / Lt.z * prof
        float hz = hh;
        for (int i = 1; i <= 8; i++) {
          pl += pasl;
          float hs = textureGrad(sculpt_tex, pl, rl_dx, rl_dy).r;
          sc_ombre = max(sc_ombre, clamp((hs - (hz + float(i) / 8.0)) * 4.0, 0.0, 1.0));
        }
      }
      sc_ok = true;
    }
  }

  // CARTE DE NORMALES DU SOL
  bool nr_ok = false;
  vec3 nr_N = vec3(0.0);
  float nr_h = 0.5, nr_hb = 0.5;
  if (nrm_amt > 0.0 && !sc_ok && gfx_hack_no_tex == 0 && v_dist < 4096.0 * 60.0) {
    float nl = length(w_nor);
    vec3 dp2perp = cross(rl_sy, w_nor), dp1perp = cross(w_nor, rl_sx);
    vec3 T = dp2perp * rl_dx.x + dp1perp * rl_dy.x;
    vec3 B = dp2perp * rl_dx.y + dp1perp * rl_dy.y;
    float lt = length(T), lb = length(B);
    if (nl > 0.05 && lt > 1e-20 && lb > 1e-20) {
      vec3 N = w_nor / nl;
      vec3 s = textureGrad(nrm_tex, uv, rl_dx, rl_dy).rgb;
      vec2 d = (s.xy * 2.0 - 1.0) * nrm_amt;
      nr_N = normalize(N + (T / lt) * d.x + (B / lb) * d.y);
      nr_h = s.z;
      float lod = textureQueryLod(nrm_tex, tex_coord.xy).x;
      nr_hb = textureLod(nrm_tex, uv, lod + 2.5).b;
      nr_ok = true;
    }
  }

  vec4 T0 = vec4(1.0);
  if (gfx_hack_no_tex == 0) {
    T0 = sc_ok ? textureGrad(tex_T0, uv, rl_dx, rl_dy) : texture(tex_T0, uv);
    // lave animee
    if (lava_kind > 1.5) {
      // a plat seulement
      float plat = smoothstep(0.45, 0.75, abs(normalize(w_nor).y));
      vec4 T_old = T0;
      float e_old = 1.0;
      if (plat < 0.999) {
        T_old = pc_lava(T0, uv, v_pos_w, wind_time, lava_kind);
        e_old = lv_eclat;
      }
      vec4 T_new = T_old;
      float e_new = e_old;
      if (plat > 0.001) {
        T_new = pc_lava_ondule(T0, uv, v_pos_w, v_vue, wind_time);
        e_new = lv_eclat;
      }
      T0 = mix(T_old, T_new, plat);
      lv_eclat = mix(e_old, e_new, plat);
    } else if (lava_kind > 0.5) {
      T0 = pc_lava(T0, uv, v_pos_w, wind_time, lava_kind);
    }
    color = fragment_color * T0;
    if (sol_kind > 1.5) {
      // herbe : -25 % de lumiere, rouge retire (le jaune), un peu de saturation en plus
      vec3 c = color.rgb * vec3(0.63, 0.77, 0.68);
      float l = dot(c, vec3(0.299, 0.587, 0.114));
      color.rgb = mix(vec3(l), c, 1.04);
    }
    if (sol_melange > 0.5) {
      // BORD NATUREL ENTRE DEUX SOLS
      const vec3 Ys = vec3(0.299, 0.587, 0.114);
      float h = dot(T0.rgb, Ys) - dot(textureLod(tex_T0, uv, 3.5).rgb, Ys);
      float a0 = color.a;
      float pix_m = max(length(rl_sx), length(rl_sy)) / 4096.0;
      float ga = (abs(dFdx(a0)) + abs(dFdy(a0))) / max(pix_m, 1e-5);
      float dist_m = clamp((a0 - 0.5) / max(ga, 0.08), -4.0, 4.0);
      vec2 pm = v_pos_w.xz / 4096.0;
      float bord = pc_vnoise(pm * 1.1) * 0.55 + pc_vnoise(pm * 3.7) * 0.24 +
                   pc_vnoise(pm * 11.0) * 0.09 + clamp(h, -0.2, 0.2) * 0.25;
      float w = max(pix_m, 0.012);
      color.a = smoothstep(-w, w, dist_m + bord) * step(0.004, a0);
    }
  } else {
    color = fragment_color/2;
  }


  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  // COUCHE DE DETAIL DES SOLS
  bool dt_ok = false;
  vec3 dt_tilt = vec3(0.0);
  {
    // une tuile de detail couvre 1,6 m
    const float DT_TUILE = 1.0 / (4096.0 * 1.6);
    vec2 dpx = rl_sx.xz * DT_TUILE, dpy = rl_sy.xz * DT_TUILE;
    if (det_kind >= 0 && gfx_hack_no_tex == 0 && v_dist < 4096.0 * 18.0) {
      float nl = length(w_nor);
      vec3 Nd = nl > 0.05 ? w_nor / nl : vec3(0.0, 1.0, 0.0);
      // le sens des normales du terrain d'origine n'est pas fiable
      float f = (1.0 - smoothstep(4096.0 * 5.0, 4096.0 * 18.0, v_dist)) * smoothstep(0.5, 0.8, abs(Nd.y));
      if (f > 0.001) {
        vec2 p = v_pos_w.xz * DT_TUILE;
        const mat2 R = mat2(0.6, 0.8, -0.8, 0.6);
        vec2 p2 = R * p * 1.37 + vec2(0.31, 0.77);
        float layer = float(det_kind);
        vec4 s1 = textureGrad(det_tex, vec3(p, layer), dpx, dpy);
        vec4 s2 = textureGrad(det_tex, vec3(p2, layer), R * dpx * 1.37, R * dpy * 1.37);
        float m = clamp(0.5 + pc_vnoise(p * 1.1) * 1.4 + pc_vnoise(p * 3.3) * 0.4, 0.0, 1.0);
        float t = smoothstep(-0.12, 0.12, (s2.a + m) - (s1.a + 1.0 - m));
        vec2 n1 = s1.gb * 2.0 - 1.0;
        vec2 n2 = transpose(R) * (s2.gb * 2.0 - 1.0);
        float alb = mix(s1.r, s2.r, t);
        vec2 nn = mix(n1, n2, t);
        color.rgb *= 1.0 + (alb - 0.5) * 2.2 * f;
        dt_tilt = vec3(nn.x, 0.0, nn.y) * 1.6 * f * sign(Nd.y);
        dt_ok = true;
      }
    }
  }
  // QA draws a decoupe peints en couleur pleine (tfrag=rouge, tie_wind=bleu, etie=jaune)

  // ALPHA AFFUTE pour l'alpha-to-coverage (MSAA)
  if (alpha_min >= 0.45) {
    color.a = clamp((color.a - alpha_min) / max(fwidth(color.a), 0.0001) + 0.5, 0.0, 1.0);
  }

  // RELIEF DES SURFACES (rendu plus recent)
  float sun_shadow = sun_shadow_factor();
  if (relief_amt > 0.001 && gfx_hack_no_tex == 0 && alpha_min < 0.45 &&
      emissive_boost < 0.001 && v_foliage < 0.5 && v_dist < 4096.0 * 45.0) {
    float nl = length(w_nor);
    if (nl > 0.05) {
      vec3 N = w_nor / nl;
      // niveau de mip pris sur l'uv d'origine (continu)
      float lb = textureQueryLod(tex_T0, tex_coord.xy).x + 0.7;
      const vec3 Y = vec3(0.299, 0.587, 0.114);
      float h0 = dot(textureLod(tex_T0, uv, lb).rgb, Y);
      float hx = dot(textureLod(tex_T0, uv + rl_dx, lb).rgb, Y);
      float hy = dot(textureLod(tex_T0, uv + rl_dy, lb).rgb, Y);
      float hb = dot(textureLod(tex_T0, uv, lb + 2.5).rgb, Y);
      const float H = 0.04 * 4096.0;  // 4 cm entre le plus sombre et le plus clair
      vec3 r1 = cross(rl_sy, N), r2 = cross(N, rl_sx);
      float det = dot(rl_sx, r1);
      vec3 grad = sign(det) * ((hx - h0) * H * r1 + (hy - h0) * H * r2);
      vec3 Nb = normalize(abs(det) * N - grad);
      // avec une carte sculptee, la normale vient de la sculpture
      if (sc_ok) {
        Nb = normalize(mix(N, sc_N, 0.85));
        hb = h0 - sc_h * 0.15;
      }
      // carte de normales du sol
      if (nr_ok) {
        Nb = nr_N;
        h0 = nr_h;
        hb = nr_hb;
      } else if (dt_ok && !sc_ok) {
        // sol sans carte
        Nb = N;
        hb = h0;
      }
      if (dt_ok) {
        Nb = normalize(Nb + dt_tilt);
      }
      vec3 L = -normalize(sun_dir_w);
      // part directe du soleil ici (0 a l'ombre portee, la nuit, en interieur)
      float direct = sun_shadow_strength > 0.001
                         ? clamp(1.0 - sun_shadow / sun_shadow_strength, 0.0, 1.0)
                         : 0.0;
      float flanc = clamp(dot(Nb, L) - dot(N, L), -0.45, 0.45);
      // a l'ombre : la lumiere du ciel vient d'en haut
      float ciel = clamp(Nb.y - N.y, -0.3, 0.3);
      float creux = clamp((h0 - hb) * 1.6, -0.25, 0.25);
      float fondu = 1.0 - smoothstep(4096.0 * 25.0, 4096.0 * 45.0, v_dist);
      // sols : relief adouci (scintillement)
      fondu *= (sol_kind > 0.5 && !nr_ok && !dt_ok) ? 0.35 : 1.0;
      color.rgb *= 1.0 + relief_amt * fondu *
                             (flanc * 1.45 * direct + ciel * 0.6 * (1.0 - direct) + creux * 0.45);
      if (sc_ok) {
        // joints et creux sculptes
        color.rgb *= 1.0 - sc_fondu * (0.2 * smoothstep(0.25, 1.0, -sc_h) + 0.25 * sc_ombre * direct);
      }
    }
  }

  // (au village, la texture du sol paraissait de basse qualite)
  if (detail_amt > 0.001 && gfx_hack_no_tex == 0) {
    float fade = 1.0 - smoothstep(4096.0 * 4.0, 4096.0 * 18.0, v_dist);
    if (fade > 0.001) {
      vec2 duv = uv * 4.0;
      vec3 fine = texture(tex_T0, duv).rgb;
      vec3 coarse = textureLod(tex_T0, duv, 3.0).rgb;
      float hp = dot(fine - coarse, vec3(0.299, 0.587, 0.114));
      // + GRAIN procedural ancre au monde (cellules 4 cm et 1,5 cm)
      vec2 gp = v_pos_w.xz / 4096.0;  // metres
      float grain = pc_vnoise(gp * 9.0) * 0.35 + pc_vnoise(gp * 25.0) * 0.4 + pc_vnoise(gp * 66.0) * 0.25;
      // + accentuation (masque flou) du dessin de la texture a sa propre echelle
      float hp0 = dot(T0.rgb - textureLod(tex_T0, uv, 2.0).rgb, vec3(0.299, 0.587, 0.114));
      color.rgb *= 1.0 + (hp * 2.2 + hp0 * 1.2 + grain * 0.30) * detail_amt * fade;
    }
  }


  // émissives : la surface EST une source (lave, braises, lueurs)
  if (emissive_boost > 0.001) {
    // auto-normalisant
    float t_l = dot(T0.rgb, vec3(0.299, 0.587, 0.114));
    float eb = mix(emissive_boost, 1.05, clamp(t_l, 0.0, 1.0));
    // une texture lumineuse en GRIS (lueurs precurseur du temple de la jungle) tient sa couleur des
    float t_mx = max(T0.r, max(T0.g, T0.b));
    float t_sat = t_mx > 1e-4 ? (t_mx - min(T0.r, min(T0.g, T0.b))) / t_mx : 0.0;
    float f_mx = max(fragment_color.r, max(fragment_color.g, fragment_color.b));
    vec3 teinte = f_mx > 1e-4 ? fragment_color.rgb / f_mx : vec3(1.0);
    teinte = mix(teinte, vec3(1.0), smoothstep(0.12, 0.30, t_sat));
    color.rgb = max(color.rgb, T0.rgb * teinte * eb);
  }

  // sun shadows: soft darkening, before fog so distance stays hazy
  color.rgb *= (emissive_boost > 0.001) ? 1.0 : (1.0 - sun_shadow);
  color.rgb *= lv_eclat;  // lave animee (1 ailleurs)

  // VERRE (vitres, lentilles)
  if (reflet_kind > 1.5 && reflet_fwd.x > 0.001 && gfx_hack_no_tex == 0) {
    float nlv = length(w_nor);
    if (nlv > 0.05) {
      vec3 Nv = w_nor / nlv;
      vec3 Iv = normalize(v_vue);
      if (dot(Nv, Iv) > 0.0) {
        Nv = -Nv;
      }
      // EAU (marais)
      if (reflet_kind > 2.5) {
        vec2 pw = v_pos_w.xz / 4096.0;
        float t = wind_time;
        vec2 g = vec2(0.0);
        g += vec2(0.8, 0.6) * cos(dot(pw, vec2(0.8, 0.6)) * 1.9 + t * 1.1) * 0.035;
        g += vec2(-0.5, 0.86) * cos(dot(pw, vec2(-0.5, 0.86)) * 3.1 + t * 1.7) * 0.022;
        g += vec2(0.95, -0.3) * cos(dot(pw, vec2(0.95, -0.3)) * 5.3 + t * 2.3) * 0.012;
        if (reflet_fwd.w > 0.5) {
          // une goutte par case de 0,7 m, a un instant propre
          vec2 cs = pw / 0.7;
          vec2 ci = floor(cs);
          for (int oy = -1; oy <= 1; oy++) {
            for (int ox = -1; ox <= 1; ox++) {
              vec2 c = ci + vec2(ox, oy);
              vec2 hsh = fract(sin(vec2(dot(c, vec2(127.1, 311.7)), dot(c, vec2(269.5, 183.3)))) * 43758.5453);
              float age = fract(t * 0.9 + hsh.x * 7.0);
              vec2 cen = (c + 0.2 + 0.6 * hsh) * 0.7;
              vec2 dv = pw - cen;
              float r = length(dv);
              float rr = age * 0.45;
              float onde = exp(-pow((r - rr) / 0.03, 2.0)) * (1.0 - age) * (1.0 - age);
              g += (r > 1e-4 ? dv / r : vec2(0.0)) * onde * 0.35;
            }
          }
        }
        float loin = 1.0 - smoothstep(25.0, 70.0, length(v_vue) / 4096.0);
        Nv = normalize(Nv + vec3(-g.x, 0.0, -g.y) * loin);
      }
      float nv = clamp(-dot(Nv, Iv), 0.0, 1.0);
      vec3 Rv = reflect(Iv, Nv);
      vec3 env = mix(vec3(0.20, 0.19, 0.17), vec3(0.80, 0.88, 1.0), smoothstep(-0.04, 0.20, Rv.y));
      if (reflet_fwd.y > 0.5) {
        vec2 euv = vec2(atan(Rv.x, Rv.z) * (0.5 / 3.14159265) + 0.5,
                        asin(clamp(Rv.y, -1.0, 1.0)) / 3.14159265 + 0.5);
        // alpha = confiance
        vec4 e = textureLod(reflet_env_tex, euv, 1.4);
        if (e.a > 0.02) {
          env = mix(env, e.rgb / e.a, smoothstep(0.05, 0.45, e.a));
        }
      }
      float Fv = clamp((0.14 + 0.86 * pow(1.0 - nv, 5.0)) * reflet_fwd.x, 0.0, 0.85);
      if (reflet_kind > 2.5) {
        // eau : Fresnel de l'eau (2 % de face, miroir au rasant)
        Fv = clamp((0.02 + 0.98 * pow(1.0 - nv, 5.0)) * reflet_fwd.x, 0.0, 0.55);
        Fv = max(Fv, 0.10 * reflet_fwd.x);
        env *= 0.85;
        color.rgb *= mix(0.85, 1.0, Fv);
      }
      color.rgb = mix(color.rgb, env, Fv);
      color.a = clamp(color.a + Fv * (1.0 - color.a) * 0.6, 0.0, 1.0);
      float rl = max(dot(Rv, -normalize(sun_dir_w)), 0.0);
      color.rgb += vec3(1.0, 0.95, 0.85) * (pow(rl, 900.0) * 3.0) * (1.0 - sun_shadow) * reflet_fwd.z;
    }
  }

  float fog_amt = clamp(fogginess * fog_color.a, 0, 1);
  // (rochers lointains bugges)
  fog_amt = mix(fog_amt, 1.0, smoothstep(0.85, 0.97, fog_amt));
  color.rgb = mix(color.rgb, fog_color.rgb, fog_amt);
  {
    float nl = length(w_nor);
    // alpha 0.75 = surface EMISSIVE (lave, braises, lueurs)
    float tag_reflet = reflet_kind > 1.5 ? 1.0 : reflet_kind > 0.75 ? 250.0 / 255.0 :
                       reflet_kind > 0.25 ? 252.0 / 255.0 : 1.0;
    float tag = (emissive_boost > 0.001) ? 0.75 : ((v_foliage > 0.5) ? 0.95 : tag_reflet);
    vec3 n_g = (nl > 0.05) ? w_nor / nl : vec3(0.0, 1.0, 0.0);
    if (sc_ok) {
      n_g = normalize(mix(n_g, sc_N, 0.5));
    }
    if (nr_ok) {
      n_g = normalize(mix(n_g, nr_N, 0.6));  // l'eclairage du post suit aussi le relief du sol
    }
    if (dt_ok) {
      n_g = normalize(n_g + dt_tilt * 0.8);
    }
    // FEUILLES (palmes fondues dans le terrain)
    if (fronde_pli_f > 0.0 && nl > 0.05 && gfx_hack_no_tex == 0) {
      const vec3 Yf = vec3(0.299, 0.587, 0.114);
      float Hf = 0.012 * 4096.0;
      float f0 = dot(texture(tex_T0, uv).rgb, Yf) * Hf;
      float fx = dot(texture(tex_T0, uv + rl_dx).rgb, Yf) * Hf;
      float fy = dot(texture(tex_T0, uv + rl_dy).rgb, Yf) * Hf;
      vec3 r1 = cross(rl_sy, n_g), r2 = cross(n_g, rl_sx);
      float det = dot(rl_sx, r1);
      if (abs(det) > 1e-12) {
        n_g = normalize(abs(det) * n_g - sign(det) * ((fx - f0) * r1 + (fy - f0) * r2));
      }
    }
    out_normal = (nl > 0.05) ? vec4(n_g * 0.5 + 0.5, tag) : vec4(0.0);
  }
}
