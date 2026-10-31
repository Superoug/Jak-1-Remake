#version 410 core

// deux sorties pour le melange a DEUX SOURCES de la surface (bucket 3)
layout(location = 0, index = 0) out vec4 color;
layout(location = 0, index = 1) out vec4 color_t;

in vec4 fragment_color;
in vec3 tex_coord;
uniform float color_mult;
uniform float alpha_mult;

uniform vec4 fog_color;
uniform int bucket;

in float fog;

uniform sampler2D tex_T0;

// "Spyro water": x = time (s), y = strength (0 disables)
uniform vec4 water_fx;

// mer bleue cartoon
const vec3 kSeaDeep = vec3(0.04, 0.34, 0.86);  // bleu profond du large
const vec3 kSeaFilm = vec3(0.40, 0.76, 1.02);  // filtre : coupe le rouge

// (v0.0.3) : retour au TURQUOISE des artistes
vec3 sea_blue(vec3 c) {
  return c;
}

// brume de l'eau posee ICI, au dessin de la surface (voir CommonOceanRenderer.cpp)
uniform sampler2D water_depth;  // profondeur de la scene juste avant l'eau
uniform mat4 water_inv_camera;  // meme reconstruction que le post (world_from_depth)
uniform vec4 water_cam;
uniform vec4 water_murk;  // x = active, y = densite par metre, zw = taille de la cible
// couleur de sommet moyenne de la PLEINE MER
uniform vec4 water_deep;

vec3 water_world(vec2 uv, float d) {
  vec4 p = water_inv_camera * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  if (abs(p.w) < 1e-8) {
    return water_cam.xyz;
  }
  return p.xyz / p.w + water_cam.xyz;
}

// CARTES DU RIVAGE
uniform sampler2D riv_tex0;
uniform sampler2D riv_tex1;
uniform vec4 riv_box[2];
uniform int riv_n;
float riv_dist(vec2 xz_m) {
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
// EAU VIVANTE (eau_vivante.vert)
uniform vec4 eau_vivante;
// ondes de la simulation autour de Jak (eau_sim.frag)
uniform sampler2D eau_sim;
uniform vec4 eau_sim_box;
float eau_sim_h(vec2 pm) {
  vec2 su = (pm - eau_sim_box.xy) * eau_sim_box.z;
  if (su.x < 0.0 || su.x > 1.0 || su.y < 0.0 || su.y > 1.0) {
    return 0.0;
  }
  ivec2 sz = textureSize(eau_sim, 0);
  vec2 t = su * vec2(sz) - 0.5;
  ivec2 i = ivec2(floor(t));
  vec2 f = t - vec2(i);
  ivec2 m = sz - 1;
  float a = texelFetch(eau_sim, clamp(i, ivec2(0), m), 0).r;
  float b = texelFetch(eau_sim, clamp(i + ivec2(1, 0), ivec2(0), m), 0).r;
  float c = texelFetch(eau_sim, clamp(i + ivec2(0, 1), ivec2(0), m), 0).r;
  float d = texelFetch(eau_sim, clamp(i + ivec2(1, 1), ivec2(0), m), 0).r;
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float eau_vivante_part() {
  if (eau_vivante.x < 0.5) {
    return 1.0;
  }
  vec2 uv = gl_FragCoord.xy / water_murk.zw;
  vec3 ps = water_world(uv, gl_FragCoord.z);
  float d = length(ps.xz - water_cam.xz) / 4096.0;
  float vive = 1.0 - smoothstep(eau_vivante.y - eau_vivante.z, eau_vivante.y, d);
  return eau_vivante.x < 1.5 ? 1.0 - vive : vive;
}

// part d'eau de ce point de la surface
float water_on_land() {
  if (riv_n == 0) {
    return 1.0;
  }
  vec2 uv = gl_FragCoord.xy / water_murk.zw;
  vec3 ps = water_world(uv, gl_FragCoord.z);
  float d = riv_dist(ps.xz / 4096.0);
  float k = d > 90.0 ? 1.0 : smoothstep(-0.6, 0.4, d);
  if (k < 1.0 && water_murk.x > 0.5) {
    float db = texture(water_depth, uv).r;
    float vide = db < 1e-6 ? 1.0 : smoothstep(40.0, 80.0, length(water_world(uv, db) - ps) / 4096.0);
    k = max(k, vide);
    // FOND PROFOND SOUS LA SURFACE
    if (db > 1e-6 && water_cam.y > ps.y) {
      float prof = (ps.y - water_world(uv, db).y) / 4096.0;
      k = max(k, smoothstep(1.0, 2.0, prof));
    }
  }
  return k;
}

// BORD DOUX. La surface plate de la mer coupe le sable a facettes
float water_bord_m() {
  if (water_murk.x < 0.5) {
    return 100.0;
  }
  vec2 uv = gl_FragCoord.xy / water_murk.zw;
  float db = texture(water_depth, uv).r;
  if (db < 1e-6) {
    return 100.0;
  }
  return max(water_world(uv, gl_FragCoord.z).y - water_world(uv, db).y, 0.0) / 4096.0;
}
float water_bord_doux() {
  return smoothstep(0.0, 0.45, water_bord_m());
}

// metres d'eau traverses par le regard, de la surface au fond (rien derriere = tres profond) borne a
float water_column_m() {
  vec2 uv = gl_FragCoord.xy / water_murk.zw;
  float db = texture(water_depth, uv).r;
  if (db < 1e-6) {
    return 1000.0;
  }
  vec3 pb = water_world(uv, db);
  vec3 ps = water_world(uv, gl_FragCoord.z);
  float path = length(pb - ps) / 4096.0;
  float depth = max(ps.y - pb.y, 0.0) / 4096.0;
  return min(path, depth * 2.0);
}

// profondeur verticale moyenne du fond (m) sur un disque d'environ 6 m autour du point de la surface
float eau_prof_moyenne() {
  vec2 uv0 = gl_FragCoord.xy / water_murk.zw;
  vec3 ps = water_world(uv0, gl_FragCoord.z);
  float dist = max(length(ps - water_cam.xyz) / 4096.0, 1.0);
  float rpx = clamp(6.0 / dist * water_murk.w * 0.95, 0.0, 320.0);
  float acc = 0.0, wsum = 0.0;
  for (int k2 = 0; k2 < 17; k2++) {
    float a = 0.7854 * float(k2) + (k2 < 8 ? 0.39 : 0.0);
    float r = k2 == 16 ? 0.0 : (k2 < 8 ? 1.0 : 0.5);
    vec2 uv = uv0 + vec2(cos(a), sin(a)) * (rpx * r) / water_murk.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      continue;
    }
    float db = texture(water_depth, uv).r;
    float pr = 10.0;
    if (db > 1e-6) {
      vec3 pb = water_world(uv, db);
      if (pb.y > ps.y) {
        continue;  // objet au-dessus de l'eau (rocher emerge, Jak)
      }
      pr = min((ps.y - pb.y) / 4096.0, 10.0);
    }
    float w = k2 == 16 ? 2.0 : 1.0;
    acc += pr * w;
    wsum += w;
  }
  return wsum > 0.0 ? acc / wsum : 10.0;
}

// TRANSMISSION ADOUCIE
vec3 water_transmission_soft(vec3 k) {
  vec2 uv0 = gl_FragCoord.xy / water_murk.zw;
  vec3 ps = water_world(uv0, gl_FragCoord.z);
  float dist = max(length(ps - water_cam.xyz) / 4096.0, 1.0);
  // ~4 m a l'ecran (champ vertical d'environ 60 degres)
  float rpx = clamp(4.0 / dist * water_murk.w * 0.95, 0.0, 260.0);
  vec3 acc = 2.0 * exp(-water_column_m() * k);
  float wsum = 2.0;
  for (int k2 = 0; k2 < 16; k2++) {
    float a = 0.7854 * float(k2) + (k2 < 8 ? 0.39 : 0.0);
    float r = k2 < 8 ? 1.0 : 0.5;
    vec2 uv = uv0 + vec2(cos(a), sin(a)) * (rpx * r) / water_murk.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      continue;
    }
    float db = texture(water_depth, uv).r;
    float col = 1000.0;
    if (db > 1e-6) {
      vec3 pb = water_world(uv, db);
      // un objet AU-DESSUS de l'eau (rocher emerge, Jak) ne compte pas
      if (pb.y > ps.y) {
        continue;
      }
      // le fond voisin, ramene sous CE point de la surface
      col = max(ps.y - pb.y, 0.0) / 4096.0 * 2.0;
    }
    acc += exp(-col * k);
    wsum += 1.0;
  }
  return acc / wsum;
}

void main() {
  color_t = vec4(0.0);
  vec2 uv = tex_coord.xy / tex_coord.z;
  vec4 T0 = texture(tex_T0, uv);
  // MODE IDENTIFICATION
  const bool kIdentify = false;
  if (kIdentify) {
    vec3 dbg = (bucket == 0)   ? vec3(1.0, 0.1, 0.1)  // rouge
               : (bucket == 1) ? vec3(0.1, 1.0, 0.1)  // vert
               : (bucket == 2) ? vec3(0.15, 0.25, 1.0)  // bleu
               : (bucket == 3) ? vec3(1.0, 1.0, 0.1)  // jaune
                               : vec3(1.0, 0.1, 1.0);  // magenta
    color = vec4(dbg, 1.0);
    if (bucket == 1 || bucket == 2 || bucket == 4) {
      color.a = clamp((fragment_color * T0).a * 2.2, 0.3, 1.0);
    }
    return;
  }
  if (bucket == 0) {
    color.rgb = sea_blue(fragment_color.rgb * T0.rgb);
    // l'alpha d'AUTEUR restaure
    color.a = clamp(fragment_color.a * 0.5 * T0.a * 1.2, 0.0, 1.0);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
    // surface des dalles proches, effacee dans l'eau tres peu profonde
    color.a *= water_on_land() * smoothstep(0.0, 0.9, water_bord_m());
  } else if (bucket == 1) {
    // passe de transparence (tuiles PROCHES)
    color = fragment_color * T0;
    color.rgb = sea_blue(color.rgb);
    color.a = clamp(color.a * 2.2, 0.0, 1.0);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
    // dalles proches dans l'eau tres peu profonde
    color.a *= water_on_land() * smoothstep(0.0, 0.9, water_bord_m());
  } else if (bucket == 2 || bucket == 4) {
    // 2 = reflet du proche, 4 = reflet ADDITIF du large
    color = fragment_color * T0;
    if (bucket == 2) {
      // idem pour le reflet des dalles proches
      color *= water_on_land() * smoothstep(0.0, 0.9, water_bord_m());
    }
  } else if (bucket == 3) {
    color = fragment_color * T0;
    color.rgb = sea_blue(color.rgb);
    // EAU TRANSPARENTE
    float shore_a = clamp(fragment_color.a * 0.5, 0.0, 1.0);
    // la rampe de rivage des artistes est posee par cases de la grille de la mer d'origine
    shore_a = max(shore_a, smoothstep(2.0, 4.0, water_bord_m()));
    float lod = textureQueryLod(tex_T0, uv).y;
    float dist01 = clamp(lod * 0.7, 0.0, 1.0);
    // surface presque transparente pres de la camera (0.40 -> 0.12)
    color.a = shore_a * mix(0.12, 1.0, dist01);
    color.rgb = mix(color.rgb, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
    // le fond vu a travers se perd dans l'eau selon l'epaisseur traversee
    if (water_murk.x > 0.5) {
      float a = color.a;
      // garde : couleur par defaut si le moteur ne l'a pas encore fournie
      vec3 wd = dot(water_deep.rgb, vec3(1.0)) > 0.01 ? water_deep.rgb : vec3(0.30, 0.62, 0.66);
      vec3 deep = sea_blue(wd * T0.rgb);
      deep = mix(deep, fog_color.rgb, clamp(fog_color.a * fog, 0, 1)) * 0.92;
      // l'eau FILTRE le fond, par canal (le rouge s'eteint en premier, le bleu-vert porte loin)
      float col_c = water_column_m();
      vec3 Tc = exp(-col_c * water_murk.y * vec3(1.6, 0.55, 0.45));
      vec3 Ts = water_transmission_soft(water_murk.y * vec3(1.6, 0.55, 0.45));
      vec3 T = clamp(2.0 * Ts - Tc, vec3(0.0), Tc);
      // au ras du rivage (moins de ~60 cm d'eau), la moyenne sur 4 m melangeait le sable sec du bord
      T = mix(Tc, T, smoothstep(0.15, 0.8, col_c));
      // DEMARCATIONS
      T *= 1.0 - smoothstep(1.5, 4.0, eau_prof_moyenne());
      color.rgb = a * color.rgb + (1.0 - a) * deep * (1.0 - T);
      color_t = vec4((1.0 - a) * T, 0.0);
      color.a = 1.0;
      // sur la terre, plus d'eau (couleur nulle, fond transmis entier) et au ras du sable
      if (eau_vivante.x > 1.5 && eau_sim_box.w > 0.5) {
        vec2 uvs = gl_FragCoord.xy / water_murk.zw;
        vec3 pw = water_world(uvs, gl_FragCoord.z) / 4096.0;
        const float E = 0.1;
        float gx = (eau_sim_h(pw.xz + vec2(E, 0.0)) - eau_sim_h(pw.xz - vec2(E, 0.0))) / (2.0 * E);
        float gz = (eau_sim_h(pw.xz + vec2(0.0, E)) - eau_sim_h(pw.xz - vec2(0.0, E))) / (2.0 * E);
        if (abs(gx) + abs(gz) > 1e-4) {
          vec3 Ns = normalize(vec3(-gx * 1.0, 1.0, -gz * 1.0));
          vec3 Iv = normalize(pw - water_cam.xyz / 4096.0);
          float Fv = pow(1.0 - clamp(dot(-Iv, Ns), 0.0, 1.0), 5.0);
          float F0 = pow(1.0 - clamp(-Iv.y, 0.0, 1.0), 5.0);
          float dd = clamp((Fv - F0) * 3.0, -0.2, 0.25);
          color.rgb = dd > 0.0 ? mix(color.rgb, color.rgb * 1.2 + vec3(0.04, 0.07, 0.07), dd * 1.4)
                               : color.rgb * (1.0 + dd * 0.6);
        }
      }
      // x part de la surface vivante / de la mer d'origine
      float k = water_on_land() * water_bord_doux() * eau_vivante_part();
      color.rgb *= k;
      color_t = mix(vec4(1.0, 1.0, 1.0, 0.0), color_t, k);
    }
  } else if (bucket == 5) {
    // depot d'alpha destination (passe B du mid transparent)
    color = vec4(0.0, 0.0, 0.0,
                 clamp(fragment_color.a * T0.a, 0.0, 1.0) * water_on_land() * water_bord_doux() *
                     eau_vivante_part());
  }
}
