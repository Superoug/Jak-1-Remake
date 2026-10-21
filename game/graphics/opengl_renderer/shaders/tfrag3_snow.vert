#version 410 core
// PC-REMAKE 20/10 — SOL DE NEIGE DEFORMABLE (etape 1/3 : sommets). Meme calcul que tfrag3.vert,
// sans la projection : le decoupage (tfrag3_snow.tesc) et la deformation par la carte des
// traces (tfrag3_snow.tese) se font en espace monde. Le pixel reste tfrag3.frag.

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in ivec2 time_of_day_index;
layout (location = 3) in vec4 normal_in;

uniform sampler1D tex_T10;
uniform int decal;
uniform float palm_debug;

out vec3 vs_pos;
out vec3 vs_tc;
out vec4 vs_color;
out vec3 vs_nor;

void main() {
  vs_pos = position_in;
  vs_tc = tex_coord_in;
  vec4 c = 0.5 * (texelFetch(tex_T10, time_of_day_index.x, 0) + texelFetch(tex_T10, time_of_day_index.y, 0));
  c *= 2;
  c.a *= 2;
  if (decal == 1) {
    c.xyz = vec3(1.0, 1.0, 1.0);
  }
  if (palm_debug > 0.5) {
    c.rgb = vec3(6.0, 0.3, 6.0);
  }
  vs_color = c;
  vs_nor = normal_in.xyz;
}
