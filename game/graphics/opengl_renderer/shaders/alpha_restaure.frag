#version 410 core
// rend au canal alpha de l'image sa valeur d'avant les particules
in vec2 tex_coord;
out vec4 color;
uniform sampler2D scene_avant;
void main() {
  color = vec4(0.0, 0.0, 0.0, texture(scene_avant, tex_coord).a);
}
