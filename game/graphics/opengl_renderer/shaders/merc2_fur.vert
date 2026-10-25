#version 410 core
// PC-REMAKE 21/10 — FOURRURE EN COUCHES des personnages (copie de merc2.vert) : chaque instance
// est une couche posee au-dessus de la peau, le long de la normale, legerement couchee vers le
// bas ; merc2.frag (FUR) y dessine des brins.

// merc vertex definition
layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 weights_in;
layout (location = 3) in vec2 st_in;
layout (location = 4) in vec4 rgba;
layout (location = 5) in uvec3 mats;

// light control
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;
uniform vec3 light_dir2;
uniform vec4 light_col0;
uniform vec4 light_col1;
uniform vec4 light_col2;
uniform vec4 light_ambient;

// camera control
uniform vec4 hvdf_offset;
uniform vec4 fog_constants;

uniform mat4 perspective_matrix;

// PC-REMAKE: view-space -> sun shadow clip space (see background_common.cpp)
uniform mat4 sun_light_from_cam;
// PC-REMAKE 21/09 : normale merc (espace des os) -> monde pour le G-buffer
uniform mat3 world_nrm_from_merc;
uniform float write_normals;
out vec3 w_nor;

// output
out vec4 vtx_color;
out vec2 vtx_st;
out vec4 sun_light_pos;
// PC-REMAKE 22/09 : ECLAIRAGE PAR PIXEL (Phong) — la normale interpolee et la
// couleur brute du sommet partent au fragment, qui refait le calcul de lumiere
// exact de la VU1 par pixel. Le Gouraud d'origine (lumiere calculee aux sommets
// puis interpolee) dessinait les aretes des polygones sur les visages en
// cinematique (lignes des polygones trop visibles, 21/09).
out vec3 v_nrm;
out vec4 v_rgba;
out vec3 v_pos;  // 21/10 : position (espace camera du moteur) — matieres des personnages
out vec3 v_ref;  // 21/10 : position de reference du modele (detail fin des matieres)
out vec3 v_refn;  // 21/10 v3 : normale de reference (projection des cartes de detail)
out vec3 v_lava_w;  // 26/10 : lave (merc2.frag), sans objet ici
out float v_couche;
out vec3 v_racine;
uniform vec4 fourrure;  // x = longueur, y = taille d'un brin, z = nombre de couches

out float fog;

struct MercMatrixData {
  mat4 X;
  mat3 R;
  vec4 pad;
};

layout (std140) uniform ub_bones {
  MercMatrixData bones[128];
};


/*
The inputs are in registers 8, 10, 12, 25, and the outputs are 9, 11, 13, 26. The output is written over the input.

```
mula.xyzw ACC, vf15, vf08
maddz.xyzw vf09, vf16, vf08
mula.xyzw ACC, vf15, vf10
maddz.xyzw vf11, vf16, vf10
mula.xyzw ACC, vf15, vf12
maddz.xyzw vf13, vf16, vf12
addax.xyzw vf20, vf00
madda.xyzw ACC, vf27, vf25
maddz.xyzw vf26, vf28, vf25
```
*/
void main() {
  vec4 p = vec4(position_in, 1);
  vec4 vtx_pos = -bones[mats[0]].X * p * weights_in[0];
  vec3 rotated_nrm = bones[mats[0]].R * normal_in * weights_in[0];

  // game may send garbage bones if the weight is 0, don't let NaNs sneak in.
  if (weights_in[1] > 0) {
    vtx_pos += -bones[mats[1]].X * p * weights_in[1];
    rotated_nrm += bones[mats[1]].R * normal_in * weights_in[1];
  }
  if (weights_in[2] > 0) {
    vtx_pos += -bones[mats[2]].X * p * weights_in[2];
    rotated_nrm += bones[mats[2]].R * normal_in * weights_in[2];
  }

  // couche : decalage le long de la normale, pointe couchee vers le bas du monde
  {
    float c = float(gl_InstanceID + 1) / max(fourrure.z, 1.0);
    vec3 nn = normalize(rotated_nrm);
    vec3 bas = transpose(world_nrm_from_merc) * vec3(0.0, -1.0, 0.0);
    // dans l'espace du moteur, la normale des personnages pointe a l'inverse des positions : la
    // couche part vers -n (vers +n, elle s'enfoncait dans le corps : fourrure invisible)
    vtx_pos.xyz += -nn * (c * fourrure.x) + bas * (c * c * fourrure.x * 0.45);
    v_couche = c;
    v_racine = position_in;
  }

  vec4 transformed = perspective_matrix * vtx_pos;

  // PC-REMAKE: sun shadow lookup for characters
  sun_light_pos = sun_light_from_cam * vtx_pos;

  rotated_nrm = normalize(rotated_nrm);
  w_nor = (write_normals > 0.5) ? normalize(world_nrm_from_merc * rotated_nrm) : vec3(0.0);
  vec3 light_intensity = light_dir0_fade.xyz * rotated_nrm.x + light_dir1_fade_en.xyz * rotated_nrm.y + light_dir2 * rotated_nrm.z;
  light_intensity = max(light_intensity, vec3(0, 0, 0));

  vec4 light_color = light_ambient
                   + light_intensity.x * light_col0
                   + light_intensity.y * light_col1
                   + light_intensity.z * light_col2;



  float Q = fog_constants.x / transformed[3];
  fog = 255 - clamp(-transformed.w + hvdf_offset.w, fog_constants.y, fog_constants.z);

  transformed.xyz *= Q;
  transformed.xyz += hvdf_offset.xyz;
  transformed.xy -= (2048.);
  transformed.z /= (8388608);
  transformed.z -= 1;
  transformed.x /= (256);
  transformed.y /= -(128);
  transformed.xyz *= transformed.w;
  transformed.y *= SCISSOR_ADJUST * HEIGHT_SCALE;
  gl_Position = transformed;


  vtx_color = rgba * light_color;  // (Gouraud d'origine, garde pour reference / repli)
  v_nrm = rotated_nrm;
  v_rgba = rgba;
  v_pos = vtx_pos.xyz;
  v_ref = position_in;
  v_refn = normal_in;
  v_lava_w = vec3(0.0);
  vtx_st = st_in;
}
