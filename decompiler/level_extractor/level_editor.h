#pragma once

// EDITEUR DE NIVEAUX

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

namespace tfrag3 {
struct Level;
}

namespace decompiler {
struct TextureDB;

namespace level_editor {

constexpr float kMeter = 4096.f;

struct AddEdit {
  int id = 0;
  std::string kind;  // "shrub" | "tie"
  std::string proto;  // nom du prototype
  std::string from_level;  // niveau d'origine de la geometrie (vide = ce niveau)
  math::Vector3f pos_m = math::Vector3f::zero();  // metres
  float rot_y_deg = 0.f;
  float scale = 1.f;
};

struct RemoveEdit {
  std::string kind;
  std::string proto;
  math::Vector3f pos_m = math::Vector3f::zero();  // metres
};

struct LevelEdits {
  std::vector<AddEdit> adds;
  std::vector<RemoveEdit> removes;
  bool empty() const { return adds.empty() && removes.empty(); }
};

// Lit custom_assets/jak1/level_edits/<level>.json (vide si absent)
LevelEdits load_edits(const std::string& level);

// L'instance (position en unites du jeu) est-elle supprimee par l'editeur ?
bool is_removed(const LevelEdits& edits,
                const std::string& kind,
                const std::string& proto,
                const math::Vector4f& translation_game_units);

// Matrice d'une instance ajoutee
std::array<math::Vector4f, 4> add_matrix(const AddEdit& a);

// --- export pour l'app
struct ExportPart {
  std::string tex_page;
  std::string tex_name;
  std::vector<float> pos;  // triangles : 9 floats par triangle, metres
  std::vector<float> uv;  // 6 floats par triangle
  std::vector<u8> rgb;  // 9 octets par triangle (128 = neutre)
};

// tout ce qui suit est par thread (une extraction de niveau = un thread)
void begin_level(const std::string& level, const std::string& dgo);
const std::string& current_level();
// kind "shrub" | "tie"
int add_proto(const std::string& kind,
              const std::string& name,
              const std::string& from_level,
              std::vector<ExportPart>&& parts);
// src : "jeu" | "touffe-auto" | "tapis-auto" | "ajout"
void add_instance(const std::string& kind,
                  int proto,
                  const std::array<math::Vector4f, 4>& mat_game_units,
                  const char* src,
                  int add_id);
// sol (tfrag) + ecriture des fichiers
void write_level(const tfrag3::Level& lev, const TextureDB& tdb);

// chemins
std::string edits_dir();
std::string export_dir();
std::string library_dir();

}  // namespace level_editor
}  // namespace decompiler
