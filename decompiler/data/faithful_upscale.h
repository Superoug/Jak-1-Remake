#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace decompiler {

// PC-REMAKE 14/10 : AGRANDISSEMENT FIDELE des textures, calcule a l'EXTRACTION a
// partir du jeu du joueur (aucune image du jeu n'est distribuee avec le mod).
// Portage C++ de upscale_fidele.py (principe : l'original, juste mis en
// HD, rien d'invente) :
//   - facteur x4 si <= 128 px, x2 au-dela, plafond max_side (1024) ;
//   - peu de couleurs (<= 32) ou <= 16 px : AdvMAME2x repete (seules les couleurs
//     deja presentes) ;
//   - sinon Lanczos-3 sur la texture TUILEE 3x3 (raccords preserves), leger masque
//     flou (48 %, rayon 1,2, seuil 2), puis chaque pixel borne au min/max de son
//     voisinage 3x3 d'origine ;
//   - alpha : AdvMAME2x si masque binaire, sinon Lanczos borne ;
//   - controle : re-reduite, la texture doit retomber a moins de 6/255 de
//     l'original sur les texels visibles, sinon repli Lanczos simple borne.
struct FaithfulUpscale {
  int w = 0;
  int h = 0;
  std::vector<u32> rgba;  // RGBA8 (r = octet de poids faible), comme TextureDB
};

// nullopt = pas d'agrandissement (texture deja grande)
std::optional<FaithfulUpscale> faithful_upscale(const u32* rgba, int w, int h, int max_side = 1024);

// iris des yeux : agrandissement dedie (contour lisse, couleurs d'origine) — voir le .cpp
bool faithful_iris_texture(const std::string& tex_name);
std::optional<FaithfulUpscale> faithful_iris_upscale(const u32* rgba, int w, int h);

// textures a taille IMPOSEE par le moteur (ciel, nuages, yeux) : jamais agrandies
bool faithful_upscale_frozen(const std::string& tex_name);

}  // namespace decompiler
