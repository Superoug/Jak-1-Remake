#version 410 core
// triangle plein ecran sans tampon de sommets (anticrenelage.frag)
out vec2 tex_coord;

void main() {
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  tex_coord = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
