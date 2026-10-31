#version 410 core
// CIEL VIVANT

in vec2 tex_coord;
out vec4 color;

uniform sampler2D nuages_tex;  // nuages (couleur premultipliee, alpha = opacite)
uniform sampler2D depth_tex;  // unite 15 : profondeur resolue (inversee : 0 = ciel)
// CIEL DERRIERE LES PARTICULES
uniform sampler2D scene_tex;
uniform sampler2D hud_ref_tex;
uniform float has_part;
// apport (lumiere ajoutee) des particules a ce pixel
vec3 apport_particules() {
  if (has_part < 0.5) {
    return vec3(0.0);
  }
  ivec2 ip = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
  return max(texelFetch(hud_ref_tex, ip, 0).rgb - texelFetch(scene_tex, ip, 0).rgb, vec3(0.0));
}
float couverture_particules() {
  if (has_part < 0.5) {
    return 0.0;
  }
  ivec2 ip = ivec2(tex_coord * vec2(textureSize(scene_tex, 0)));
  vec3 dd = abs(texelFetch(hud_ref_tex, ip, 0).rgb - texelFetch(scene_tex, ip, 0).rgb);
  return smoothstep(0.04, 0.30, max(dd.r, max(dd.g, dd.b)));
}

void main() {
  if (texture(depth_tex, tex_coord).r > 1e-5) {
    discard;
  }
  // filtre tente 3 x 3 du quart de resolution
  vec2 t = 1.0 / vec2(textureSize(nuages_tex, 0));
  vec4 c = texture(nuages_tex, tex_coord) * 4.0;
  c += (texture(nuages_tex, tex_coord + vec2(t.x, 0.0)) + texture(nuages_tex, tex_coord - vec2(t.x, 0.0)) +
        texture(nuages_tex, tex_coord + vec2(0.0, t.y)) + texture(nuages_tex, tex_coord - vec2(0.0, t.y))) * 2.0;
  c += texture(nuages_tex, tex_coord + t) + texture(nuages_tex, tex_coord - t) +
       texture(nuages_tex, tex_coord + vec2(t.x, -t.y)) + texture(nuages_tex, tex_coord + vec2(-t.x, t.y));
  // les particules passent DEVANT le nuage (leur apport est rendu par-dessus) au lieu d'effacer le
  c /= 16.0;
  color = vec4(c.rgb + c.a * apport_particules(), c.a);
}
