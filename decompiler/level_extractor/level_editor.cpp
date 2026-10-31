#include "level_editor.h"

#include <cmath>
#include <cstring>
#include <mutex>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "decompiler/data/TextureDB.h"

#include "fmt/format.h"
#include "third-party/json.hpp"

namespace decompiler {
namespace level_editor {

std::string edits_dir() {
  return (file_util::get_jak_project_dir() / "custom_assets" / "jak1" / "level_edits").string();
}
std::string export_dir() {
  return (file_util::get_jak_project_dir() / "decompiler_out" / "jak1" / "editor").string();
}
std::string library_dir() {
  return (file_util::get_jak_project_dir() / "decompiler_out" / "jak1" / "editor" / "library")
      .string();
}

LevelEdits load_edits(const std::string& level) {
  LevelEdits result;
  auto path = fs::path(edits_dir()) / fmt::format("{}.json", level);
  if (!fs::exists(path)) {
    return result;
  }
  try {
    auto j = nlohmann::json::parse(file_util::read_text_file(path));
    auto read_pos = [](const nlohmann::json& p) {
      return math::Vector3f(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
    };
    if (j.contains("add")) {
      for (auto& a : j.at("add")) {
        AddEdit e;
        e.id = a.value("id", 0);
        e.kind = a.at("kind").get<std::string>();
        e.proto = a.at("proto").get<std::string>();
        e.from_level = a.value("from", std::string());
        if (e.from_level == level) {
          e.from_level.clear();
        }
        e.pos_m = read_pos(a.at("pos"));
        e.rot_y_deg = a.value("rot", 0.f);
        e.scale = a.value("scale", 1.f);
        result.adds.push_back(e);
      }
    }
    if (j.contains("remove")) {
      for (auto& r : j.at("remove")) {
        RemoveEdit e;
        e.kind = r.at("kind").get<std::string>();
        e.proto = r.at("proto").get<std::string>();
        e.pos_m = read_pos(r.at("pos"));
        result.removes.push_back(e);
      }
    }
    lg::info("PC-REMAKE editeur [{}] : {} ajout(s), {} suppression(s)", level, result.adds.size(),
             result.removes.size());
  } catch (std::exception& ex) {
    lg::error("PC-REMAKE editeur [{}] : fichier de modifications illisible ({}) — ignore", level,
              ex.what());
    return LevelEdits{};
  }
  return result;
}

bool is_removed(const LevelEdits& edits,
                const std::string& kind,
                const std::string& proto,
                const math::Vector4f& t) {
  // 5 cm de tolerance
  constexpr float kTol = 0.05f;
  for (const auto& r : edits.removes) {
    if (r.kind != kind || r.proto != proto) {
      continue;
    }
    if (std::fabs(r.pos_m.x() - t.x() / kMeter) < kTol &&
        std::fabs(r.pos_m.y() - t.y() / kMeter) < kTol &&
        std::fabs(r.pos_m.z() - t.z() / kMeter) < kTol) {
      return true;
    }
  }
  return false;
}

std::array<math::Vector4f, 4> add_matrix(const AddEdit& a) {
  const float ang = a.rot_y_deg * 3.14159265f / 180.f;
  const float s = a.scale > 0.01f ? a.scale : 1.f;
  const float ca = std::cos(ang) * s;
  const float sa = std::sin(ang) * s;
  std::array<math::Vector4f, 4> m;
  // meme convention que le tapis d'herbe (extract_shrub.cpp)
  m[0] = math::Vector4f(ca, 0.f, -sa, 0.f);
  m[1] = math::Vector4f(0.f, s, 0.f, 0.f);
  m[2] = math::Vector4f(sa, 0.f, ca, 0.f);
  m[3] = math::Vector4f(a.pos_m.x() * kMeter, a.pos_m.y() * kMeter, a.pos_m.z() * kMeter, 1.f);
  return m;
}

namespace {
struct ExportProto {
  std::string kind, name, from_level;
  std::vector<ExportPart> parts;
};
struct ExportInstance {
  std::string kind;
  int proto = 0;
  std::array<float, 12> m;  // 3 colonnes + translation, metres pour la translation
  std::string src;
  int add_id = 0;
};
struct LevelExport {
  std::string level, dgo;
  std::vector<ExportProto> protos;
  std::vector<ExportInstance> instances;
};
thread_local LevelExport t_export;

// binaire : blocs alignes sur 4 octets
struct BinWriter {
  std::vector<u8> data;
  u32 put(const void* p, size_t bytes) {
    u32 off = data.size();
    data.resize(off + bytes);
    if (bytes) {
      memcpy(data.data() + off, p, bytes);
    }
    while (data.size() % 4) {
      data.push_back(0);
    }
    return off;
  }
};

nlohmann::json write_part(BinWriter& bin, const ExportPart& p) {
  nlohmann::json j;
  j["tpage"] = p.tex_page;
  j["tex"] = p.tex_name;
  j["tris"] = p.pos.size() / 9;
  j["pos"] = bin.put(p.pos.data(), p.pos.size() * sizeof(float));
  j["uv"] = bin.put(p.uv.data(), p.uv.size() * sizeof(float));
  j["rgb"] = bin.put(p.rgb.data(), p.rgb.size());
  return j;
}

// sommet d'un arbre tfrag -> part (triangles)
void append_tri(ExportPart& part,
                const tfrag3::TfragTree& tree,
                const tfrag3::PreloadedVertex& a,
                const tfrag3::PreloadedVertex& b,
                const tfrag3::PreloadedVertex& c) {
  for (const auto* v : {&a, &b, &c}) {
    part.pos.push_back(v->x / kMeter);
    part.pos.push_back(v->y / kMeter);
    part.pos.push_back(v->z / kMeter);
    part.uv.push_back(v->s);
    part.uv.push_back(v->t);
    // palette 2 (journee)
    u8 rgb[3] = {128, 128, 128};
    if (tree.colors.color_count && v->color_index < tree.colors.color_count) {
      for (int ch = 0; ch < 3; ch++) {
        rgb[ch] = tree.colors.read(v->color_index, 2, ch);
      }
    }
    part.rgb.insert(part.rgb.end(), rgb, rgb + 3);
  }
}

void strip_to_tris(const std::vector<u32>& strip,
                   const tfrag3::TfragTree& tree,
                   ExportPart& part) {
  for (size_t i = 2; i < strip.size(); i++) {
    const auto& va = tree.unpacked.vertices.at(strip[i - 2]);
    const auto& vb = tree.unpacked.vertices.at(strip[i - 1]);
    const auto& vc = tree.unpacked.vertices.at(strip[i]);
    append_tri(part, tree, va, vb, vc);
  }
}
}  // namespace

void begin_level(const std::string& level, const std::string& dgo) {
  t_export = LevelExport{};
  t_export.level = level;
  t_export.dgo = dgo;
}

const std::string& current_level() {
  return t_export.level;
}

int add_proto(const std::string& kind,
              const std::string& name,
              const std::string& from_level,
              std::vector<ExportPart>&& parts) {
  auto& p = t_export.protos.emplace_back();
  p.kind = kind;
  p.name = name;
  p.from_level = from_level;
  p.parts = std::move(parts);
  return (int)t_export.protos.size() - 1;
}

void add_instance(const std::string& kind,
                  int proto,
                  const std::array<math::Vector4f, 4>& m,
                  const char* src,
                  int add_id) {
  auto& inst = t_export.instances.emplace_back();
  inst.kind = kind;
  inst.proto = proto;
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      inst.m[c * 3 + r] = m[c][r];
    }
  }
  for (int r = 0; r < 3; r++) {
    // arrondi au mm
    inst.m[9 + r] = std::round(m[3][r] / kMeter * 1000.f) / 1000.f;
  }
  inst.src = src;
  inst.add_id = add_id;
}

void write_level(const tfrag3::Level& lev, const TextureDB& tdb) {
  (void)tdb;
  auto& ex = t_export;
  if (ex.level.empty()) {
    return;
  }
  BinWriter bin;
  nlohmann::json j;
  j["level"] = ex.level;
  j["dgo"] = ex.dgo;
  j["version"] = 1;

  // --- sol : arbres tfrag de plus haut detail
  {
    std::map<std::string, ExportPart> by_tex;
    for (const auto& tree_in : lev.tfrag_trees[0]) {
      if (tree_in.kind == tfrag3::TFragmentTreeKind::LOWRES ||
          tree_in.kind == tfrag3::TFragmentTreeKind::LOWRES_TRANS ||
          tree_in.kind == tfrag3::TFragmentTreeKind::INVALID) {
        continue;
      }
      tfrag3::TfragTree tree = tree_in;  // copie : unpack ne touche pas aux donnees ecrites
      tree.unpack();
      for (const auto& draw : tree.draws) {
        std::string key = "?";
        std::string page, name;
        if (draw.tree_tex_id >= 0 && draw.tree_tex_id < (int)lev.textures.size()) {
          page = lev.textures[draw.tree_tex_id].debug_tpage_name;
          name = lev.textures[draw.tree_tex_id].debug_name;
          key = page + "/" + name;
        }
        if (tree.kind == tfrag3::TFragmentTreeKind::WATER) {
          key = "@eau/" + key;
        }
        auto& part = by_tex[key];
        part.tex_page = tree.kind == tfrag3::TFragmentTreeKind::WATER ? "@eau" : page;
        part.tex_name = name;
        std::vector<u32> strip;
        for (const auto& run : draw.runs) {
          strip.clear();
          for (u32 ri = 0; ri < run.length; ri++) {
            strip.push_back(run.vertex0 + ri);
          }
          if (tree.use_strips) {
            strip_to_tris(strip, tree, part);
          } else {
            for (size_t i = 0; i + 2 < strip.size(); i += 3) {
              append_tri(part, tree, tree.unpacked.vertices.at(strip[i]),
                         tree.unpacked.vertices.at(strip[i + 1]),
                         tree.unpacked.vertices.at(strip[i + 2]));
            }
          }
        }
        if (tree.use_strips) {
          strip.clear();
          for (u32 idx : draw.plain_indices) {
            if (idx == UINT32_MAX) {
              strip_to_tris(strip, tree, part);
              strip.clear();
            } else {
              strip.push_back(idx);
            }
          }
          strip_to_tris(strip, tree, part);
        } else {
          const auto& pi = draw.plain_indices;
          for (size_t i = 0; i + 2 < pi.size(); i += 3) {
            append_tri(part, tree, tree.unpacked.vertices.at(pi[i]),
                       tree.unpacked.vertices.at(pi[i + 1]), tree.unpacked.vertices.at(pi[i + 2]));
          }
        }
      }
    }
    nlohmann::json terrain = nlohmann::json::array();
    for (auto& [key, part] : by_tex) {
      if (!part.pos.empty()) {
        terrain.push_back(write_part(bin, part));
      }
    }
    j["terrain"] = terrain;
  }

  // --- objets (geometrie locale)
  nlohmann::json protos = nlohmann::json::array();
  for (const auto& p : ex.protos) {
    nlohmann::json pj;
    pj["kind"] = p.kind;
    pj["name"] = p.name;
    if (!p.from_level.empty()) {
      pj["from"] = p.from_level;
    }
    nlohmann::json parts = nlohmann::json::array();
    float r2 = 0.f;
    for (const auto& part : p.parts) {
      if (part.pos.empty()) {
        continue;
      }
      for (size_t i = 0; i + 2 < part.pos.size(); i += 3) {
        float d = part.pos[i] * part.pos[i] + part.pos[i + 1] * part.pos[i + 1] +
                  part.pos[i + 2] * part.pos[i + 2];
        r2 = std::max(r2, d);
      }
      parts.push_back(write_part(bin, part));
    }
    pj["parts"] = parts;
    pj["radius"] = std::sqrt(r2);
    protos.push_back(pj);
  }
  j["protos"] = protos;

  // --- instances
  {
    std::vector<float> mats;
    mats.reserve(ex.instances.size() * 12);
    nlohmann::json kinds = nlohmann::json::array();
    nlohmann::json pidx = nlohmann::json::array();
    nlohmann::json srcs = nlohmann::json::array();
    nlohmann::json ids = nlohmann::json::array();
    for (const auto& inst : ex.instances) {
      mats.insert(mats.end(), inst.m.begin(), inst.m.end());
      pidx.push_back(inst.proto);
      srcs.push_back(inst.src);
      ids.push_back(inst.add_id);
    }
    j["instances"] = {{"count", ex.instances.size()},
                      {"mat", bin.put(mats.data(), mats.size() * sizeof(float))},
                      {"proto", pidx},
                      {"src", srcs},
                      {"add_id", ids}};
  }

  auto dir = fs::path(export_dir());
  file_util::create_dir_if_needed(dir);
  file_util::write_binary_file(dir / fmt::format("{}.bin", ex.level), bin.data.data(),
                               bin.data.size());
  file_util::write_text_file(dir / fmt::format("{}.json", ex.level), j.dump());
  lg::info("PC-REMAKE editeur : export {} ({} objets, {} instances, {:.1f} Mo)", ex.level,
           ex.protos.size(), ex.instances.size(), bin.data.size() / 1048576.0);
  t_export = LevelExport{};
}

}  // namespace level_editor
}  // namespace decompiler
