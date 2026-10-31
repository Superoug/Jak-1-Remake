#version 410 core
// SOL DE NEIGE DEFORMABLE (etape decoupage)
layout (vertices = 3) out;

in vec3 vs_pos[];
in vec3 vs_tc[];
in vec4 vs_color[];
in vec3 vs_nor[];
out vec3 tc_pos[];
out vec3 tc_tc[];
out vec4 tc_color[];
out vec3 tc_nor[];

uniform vec4 snow_trail;  // x = actif, y/z = Jak (unites du jeu), w = instant (s)
uniform vec4 cam_trans;
uniform sampler2D snow_cells;  // instant du dernier pas, par case de 0,8 m (torique, 128 x 128)

const float M = 4096.0;
const float CELL = 0.8;  // m

// une trace recente (moins de 4 min) touche-t-elle une case du rectangle [lo, hi] (m) ?
bool cells_hit(vec2 lo, vec2 hi) {
  ivec2 c0 = ivec2(floor(lo / CELL));
  ivec2 c1 = ivec2(floor(hi / CELL));
  if (c1.x - c0.x > 7 || c1.y - c0.y > 7) {
    return true;  // tres grand triangle : decoupe sans chercher
  }
  for (int z = c0.y; z <= c1.y; z++) {
    for (int x = c0.x; x <= c1.x; x++) {
      float age = snow_trail.w - texelFetch(snow_cells, ivec2(x, z) & 127, 0).r;
      if (age < 0.0) age += 3600.0;
      if (age < 240.0) {
        return true;
      }
    }
  }
  return false;
}

// niveau de decoupage d'une arete
float edge_level(vec3 a, vec3 b) {
  if (snow_trail.x < 0.5) {
    return 1.0;
  }
  vec3 mid = 0.5 * (a + b);
  float len = length(b - a) / M;
  if (length(mid.xz - snow_trail.yz) / M > 50.0 + 0.5 * len) {
    return 1.0;
  }
  if (!cells_hit(min(a.xz, b.xz) / M, max(a.xz, b.xz) / M)) {
    return 1.0;
  }
  float dcam = length(mid - cam_trans.xyz) / M;
  float seg = clamp(0.014 * dcam, 0.07, 0.5);
  return clamp(len / seg, 1.0, 64.0);
}

void main() {
  tc_pos[gl_InvocationID] = vs_pos[gl_InvocationID];
  tc_tc[gl_InvocationID] = vs_tc[gl_InvocationID];
  tc_color[gl_InvocationID] = vs_color[gl_InvocationID];
  tc_nor[gl_InvocationID] = vs_nor[gl_InvocationID];
  if (gl_InvocationID == 0) {
    float e0 = edge_level(vs_pos[1], vs_pos[2]);
    float e1 = edge_level(vs_pos[2], vs_pos[0]);
    float e2 = edge_level(vs_pos[0], vs_pos[1]);
    gl_TessLevelOuter[0] = e0;
    gl_TessLevelOuter[1] = e1;
    gl_TessLevelOuter[2] = e2;
    float inner = max(e0, max(e1, e2));
    // trace au milieu d'un triangle dont aucune arete n'est touchee
    if (inner < 1.5 && snow_trail.x > 0.5) {
      vec3 cen = (vs_pos[0] + vs_pos[1] + vs_pos[2]) / 3.0;
      vec2 lo = min(vs_pos[0].xz, min(vs_pos[1].xz, vs_pos[2].xz)) / M;
      vec2 hi = max(vs_pos[0].xz, max(vs_pos[1].xz, vs_pos[2].xz)) / M;
      if (length(cen.xz - snow_trail.yz) / M < 50.0 && cells_hit(lo, hi)) {
        float dcam = length(cen - cam_trans.xyz) / M;
        inner = clamp(max(hi.x - lo.x, hi.y - lo.y) / clamp(0.014 * dcam, 0.07, 0.5), 1.0, 64.0);
      }
    }
    gl_TessLevelInner[0] = inner;
  }
}
