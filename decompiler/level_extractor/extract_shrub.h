#pragma once

#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

#include "decompiler/data/TextureDB.h"
#include "decompiler/level_extractor/BspHeader.h"

namespace decompiler {

// PC-REMAKE: a ground triangle (world units) whose collision material is grass,
// used to procedurally seed a continuous grass carpet at extraction time.
struct GrassSeedTri {
  math::Vector3f a, b, c;
  u32 materiau = 0;  // pat-material du jeu (7 = herbe)
};

// PC-REMAKE 17/10 : zones ou l'herbe AUTOMATIQUE du remake (touffes clonees + tapis) n'est
// pas semee : autour des piles d'energie, des orifices d'eco... (sinon elle cachait leurs
// animations et recouvrait leur socle). x y z en unites du jeu, w = rayon (unites du jeu).
// Posees par extract_level juste avant extract_shrub (meme fil).
void set_grass_keep_clear(const std::vector<math::Vector4f>& zones);

// PC-REMAKE 26/10 : tous les triangles de collision du niveau (sol, murs, rochers, maisons).
// L'herbe AUTOMATIQUE (touffes clonees + tapis) y est recalee sur le vrai sol et retiree la ou
// elle flotterait (bord, vide, pente) ou toucherait un mur, un rocher, un surplomb. Posee par
// extract_level juste avant extract_shrub (meme fil).
void set_grass_collision(const std::vector<GrassSeedTri>& tris);

/// <summary>
/// Extract shrubs from the level
/// </summary>
/// <param name="tree"></param>
/// <param name="debug_name"></param>
/// <param name="map"></param>
/// <param name="tex_db"></param>
/// <param name="expected_missing_textures"></param>
/// <param name="out"></param>
/// <param name="dump_level"></param>
void extract_shrub(const level_tools::shrub_types::DrawableTreeInstanceShrub* tree,
                   const std::string& debug_name,
                   const std::vector<level_tools::TextureRemap>& map,
                   const TextureDB& tex_db,
                   const std::vector<std::pair<int, int>>& expected_missing_textures,
                   tfrag3::Level& out,
                   bool dump_level,
                   GameVersion version,
                   const std::vector<GrassSeedTri>& grass_seed_tris = {});

}  // namespace decompiler
