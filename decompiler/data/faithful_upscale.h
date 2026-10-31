#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace decompiler {

// AGRANDISSEMENT FIDELE des textures, calcule a l'EXTRACTION a partir du jeu du joueur
struct FaithfulUpscale {
  int w = 0;
  int h = 0;
  std::vector<u32> rgba;  // RGBA8 (r = octet de poids faible), comme TextureDB
};

// nullopt = pas d'agrandissement (texture deja grande)
std::optional<FaithfulUpscale> faithful_upscale(const u32* rgba, int w, int h, int max_side = 1024);

// iris des yeux
bool faithful_iris_texture(const std::string& tex_name);
std::optional<FaithfulUpscale> faithful_iris_upscale(const u32* rgba, int w, int h);

// textures a taille IMPOSEE par le moteur (ciel, nuages, yeux)
bool faithful_upscale_frozen(const std::string& tex_name);

}  // namespace decompiler
