#version 410 core
// PERSONNAGES DECOUPES (etape decoupage)
layout (vertices = 3) out;

in vec4 vs_pos[];
in vec3 vs_nrm[];
in vec3 vs_weld[];
in float vs_vif[];
in vec2 vs_st[];
in vec4 vs_rgba[];
in vec3 vs_ref[];
in vec3 vs_refn[];
out vec4 tc_pos[];
out vec3 tc_nrm[];
out vec3 tc_weld[];
out float tc_vif[];
out vec2 tc_st[];
out vec4 tc_rgba[];
out vec3 tc_ref[];
out vec3 tc_refn[];

// x = longueur visee d'un morceau, en fraction de la distance a la camera (0 = pas de decoupage) y =
uniform vec2 decoupe;

float niveau(vec3 a, vec3 b) {
  if (decoupe.x <= 0.0) {
    return 1.0;
  }
  float d = max(length(0.5 * (a + b)), 1.0);
  return clamp(length(b - a) / (d * decoupe.x), 1.0, decoupe.y);
}

void main() {
  tc_pos[gl_InvocationID] = vs_pos[gl_InvocationID];
  tc_nrm[gl_InvocationID] = vs_nrm[gl_InvocationID];
  tc_weld[gl_InvocationID] = vs_weld[gl_InvocationID];
  tc_vif[gl_InvocationID] = vs_vif[gl_InvocationID];
  tc_st[gl_InvocationID] = vs_st[gl_InvocationID];
  tc_rgba[gl_InvocationID] = vs_rgba[gl_InvocationID];
  tc_ref[gl_InvocationID] = vs_ref[gl_InvocationID];
  tc_refn[gl_InvocationID] = vs_refn[gl_InvocationID];
  if (gl_InvocationID == 0) {
    float e0 = niveau(vs_pos[1].xyz, vs_pos[2].xyz);
    float e1 = niveau(vs_pos[2].xyz, vs_pos[0].xyz);
    float e2 = niveau(vs_pos[0].xyz, vs_pos[1].xyz);
    gl_TessLevelOuter[0] = e0;
    gl_TessLevelOuter[1] = e1;
    gl_TessLevelOuter[2] = e2;
    gl_TessLevelInner[0] = max(e0, max(e1, e2));
  }
}
