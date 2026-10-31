#version 410 core
// triangle plein ecran sans tampon de sommets (alpha_restaure.frag)
out vec2 tex_coord;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  tex_coord = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
