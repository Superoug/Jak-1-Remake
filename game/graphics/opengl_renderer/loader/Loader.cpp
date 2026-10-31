#include "Loader.h"

#include <map>
#include <ranges>
#include <set>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/LoaderStages.h"

#include "third-party/imgui/imgui.h"

// Windows (MSVC, clang-cl) ne definit M_PI qu'avec _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// decide whether a surface is metal, so the engine's reflection pass can be switched on for it
bool texture_is_metal(const tfrag3::Texture& tex) {
  // "never" is checked FIRST: a mixed texture like crate-woodandrim is mostly wood
  static const char* kNeverWords[] = {"wood", "cloth", "skin",  "fur",  "leaf",
                                      "grass", "sand", "dirt",  "hair", "eye",
                                      "straw", "thatch", "rope", "sky",  "cloud"};
  for (auto* w : kNeverWords) {
    if (tex.debug_name.find(w) != std::string::npos) {
      return false;
    }
  }
  static const char* kMetalWords[] = {
      "metal", "iron",  "steel", "bolt",  "rim",   "edging", "chrome", "blade",
      "gear",  "valve", "pipe",  "grate", "plate", "hinge",  "buckle", "silver",
      "machine", "cannon", "screw", "nail", "lens", "shiny", "armor", "weld"};
  for (auto* w : kMetalWords) {
    if (tex.debug_name.find(w) != std::string::npos) {
      return true;
    }
  }

  // pixel analysis fallback
  if (tex.data.empty()) {
    return false;
  }
  double sat_sum = 0, lum_sum = 0, lum_sq = 0;
  int n = 0;
  const int step = std::max<int>(1, (int)tex.data.size() / 1024);
  for (size_t i = 0; i < tex.data.size(); i += step) {
    u32 t = tex.data[i];
    double r = (t & 0xff) / 255.0;
    double g = ((t >> 8) & 0xff) / 255.0;
    double b = ((t >> 16) & 0xff) / 255.0;
    double a = ((t >> 24) & 0xff) / 255.0;
    if (a < 0.3) {
      continue;  // ignore cut-out pixels
    }
    double mx = std::max(r, std::max(g, b));
    double mn = std::min(r, std::min(g, b));
    double sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    sat_sum += sat;
    lum_sum += lum;
    lum_sq += lum * lum;
    n++;
  }
  if (n < 16) {
    return false;
  }
  double sat = sat_sum / n;
  double lum = lum_sum / n;
  // Thresholds measured on the game's own sheets: real metal sits around sat 0.10-0.18 / lum 0.44-0.78
  return sat < 0.30 && lum > 0.30;
}

}  // namespace

// the little path lamps dotted along the village
void pc_remake_add_static_lamp(long long id, float x, float y, float z, float radius, float r,
                               float g, float b, float intensity, int priority);

void register_scenery_lamps(tfrag3::Level& lev) {
  auto is_lamp_texture = [&](int tex_id) {
    if (tex_id < 0 || tex_id >= (int)lev.textures.size()) {
      return false;
    }
    const auto& n = lev.textures[tex_id].debug_name;
    // "-wood" is the post the lamp sits on, not the lit shade
    return n.find("lamp") != std::string::npos && n.find("wood") == std::string::npos;
  };

  struct Cluster {
    double x = 0, y = 0, z = 0;
    int n = 0;
  };
  std::vector<Cluster> clusters;
  constexpr float kMergeDist = 4096.f * 3.f;  // 3 m: one lamp fixture
  auto add_point = [&](float x, float y, float z) {
    for (auto& c : clusters) {
      float dx = (float)(c.x / c.n) - x, dy = (float)(c.y / c.n) - y, dz = (float)(c.z / c.n) - z;
      if (dx * dx + dy * dy + dz * dz < kMergeDist * kMergeDist) {
        c.x += x;
        c.y += y;
        c.z += z;
        c.n++;
        return;
      }
    }
    clusters.push_back({x, y, z, 1});
  };

  for (auto& tree_cat : lev.tie_trees) {
    for (auto& tree : tree_cat) {
      for (const auto& draw : tree.static_draws) {
        if (!is_lamp_texture(draw.tree_tex_id)) {
          continue;
        }
        auto take = [&](u32 idx) {
          if (idx < tree.unpacked.vertices.size()) {
            const auto& v = tree.unpacked.vertices[idx];
            add_point(v.x, v.y, v.z);
          }
        };
        // this draw's own vertices only
        for (const auto& run : draw.runs) {
          for (u32 k = 0; k < run.length; k++) {
            take(run.vertex0 + k);
          }
        }
        for (u32 idx : draw.plain_indices) {
          take(idx);
        }
      }
    }
  }

  int lit = 0;
  for (const auto& c : clusters) {
    if (c.n < 6) {
      continue;  // a stray triangle, not a lamp
    }
    float x = (float)(c.x / c.n), y = (float)(c.y / c.n), z = (float)(c.z / c.n);
    // id from the position: stable across reloads, unique per fixture
    long long id = ((long long)(x * 0.25f) * 73856093ll) ^ ((long long)(y * 0.25f) * 19349663ll) ^
                   ((long long)(z * 0.25f) * 83492791ll);
    pc_remake_add_static_lamp(id, x, y, z, 4096.f * 11.f, 1.00f, 0.52f, 0.16f, 0.85f, 2);
    lit++;
  }
  if (lit) {
    lg::info("[PC-REMAKE] {} lampes de décor allumées", lit);
  }
}

// the far-view jungle temple rings are flat billboards in the medres-jungle model
void remake_medres_rings(tfrag3::Level& lev) {
  auto& mercs = lev.merc_data;
  int done = 0;
  for (auto& model : mercs.models) {
    if (model.name.rfind("medres-", 0) != 0) {
      continue;
    }
    for (auto& eff : model.effects) {
      for (auto& draw : eff.all_draws) {
        if (draw.tree_tex_id < 0 || draw.tree_tex_id >= (int)lev.textures.size()) {
          continue;
        }
        const auto& tex_name = lev.textures.at(draw.tree_tex_id).debug_name;
        if (tex_name.find("jng-ttop-lod-ring") == std::string::npos) {
          continue;
        }
        // collect the billboard's vertices
        std::vector<u32> vidx;
        for (u32 ii = 0; ii < draw.index_count; ii++) {
          u32 vi = mercs.indices.at(draw.first_index + ii);
          if (vi == UINT32_MAX) {
            continue;
          }
          if (std::find(vidx.begin(), vidx.end(), vi) == vidx.end()) {
            vidx.push_back(vi);
          }
        }
        if (vidx.size() < 3) {
          continue;
        }
        const tfrag3::MercVertex ref = mercs.vertices.at(vidx[0]);
        math::Vector3f mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        math::Vector2f st_mn(1e30f, 1e30f), st_mx(-1e30f, -1e30f);
        for (u32 vi : vidx) {
          const auto& v = mercs.vertices.at(vi);
          for (int c = 0; c < 3; c++) {
            mn[c] = std::min(mn[c], v.pos[c]);
            mx[c] = std::max(mx[c], v.pos[c]);
          }
          for (int c = 0; c < 2; c++) {
            st_mn[c] = std::min(st_mn[c], v.st[c]);
            st_mx[c] = std::max(st_mx[c], v.st[c]);
          }
        }
        const math::Vector3f center = (mn + mx) * 0.5f;
        const math::Vector3f ext = (mx - mn) * 0.5f;
        // ring radius = biggest half-extent of the quad; the ring lies flat (XZ)
        const float R = std::max(std::max(ext.x(), ext.y()), ext.z());
        if (R <= 0.f) {
          continue;
        }
        const float r_tube = R * 0.13f;
        const math::Vector2f uc = (st_mn + st_mx) * 0.5f;
        const math::Vector2f ur = (st_mx - st_mn) * 0.5f;
        // texture annulus measured on the redrawn 256² rings
        const float rho_in = 0.55f, rho_out = 0.93f;

        // ring-01 (the small one) must lie in its billboard's own plane
        int e1 = 0, e2 = 2, en = 1;  // default: XZ plane, normal Y
        if (tex_name.find("ring-01") != std::string::npos) {
          int order[3] = {0, 1, 2};
          std::sort(order, order + 3, [&](int a, int b) { return ext[a] > ext[b]; });
          e1 = order[0];
          e2 = order[1];
          en = order[2];
        }

        const int M = 22, m = 6;
        const u32 base_vtx = (u32)mercs.vertices.size();
        for (int i = 0; i < M; i++) {
          const float th = 2.f * (float)M_PI * i / M;
          const float ct = std::cos(th), stn = std::sin(th);
          for (int j = 0; j < m; j++) {
            const float ph = 2.f * (float)M_PI * j / m;
            const float cp = std::cos(ph), sp = std::sin(ph);
            tfrag3::MercVertex v = ref;  // copies weights / mats / rgba / pads
            const float rad = R - r_tube + r_tube * cp;  // outer at ph=0
            v.pos[e1] = center[e1] + ct * rad;
            v.pos[en] = center[en] + sp * r_tube;
            v.pos[e2] = center[e2] + stn * rad;
            v.normal[e1] = ct * cp;
            v.normal[en] = sp;
            v.normal[e2] = stn * cp;
            // sample the ring texture as an annulus; tube angle -> radial band
            const float rho = rho_in + (rho_out - rho_in) * (cp * 0.5f + 0.5f);
            v.st[0] = uc.x() + ct * rho * ur.x();
            v.st[1] = uc.y() + stn * rho * ur.y();
            mercs.vertices.push_back(v);
          }
        }
        const u32 first_index = (u32)mercs.indices.size();
        for (int i = 0; i < M; i++) {
          const int i2 = (i + 1) % M;
          for (int j = 0; j < m; j++) {
            const int j2 = (j + 1) % m;
            const u32 a = base_vtx + i * m + j;
            const u32 b = base_vtx + i2 * m + j;
            const u32 c = base_vtx + i * m + j2;
            const u32 d = base_vtx + i2 * m + j2;
            mercs.indices.push_back(a);
            mercs.indices.push_back(b);
            mercs.indices.push_back(c);
            mercs.indices.push_back(c);
            mercs.indices.push_back(b);
            mercs.indices.push_back(d);
          }
        }
        draw.first_index = first_index;
        draw.index_count = (u32)M * m * 6;
        draw.num_triangles = (u32)M * m * 2;
        draw.no_strip = true;
        done++;
        lg::info("PC-REMAKE medres ring -> torus: {} '{}' R={:.1f}", model.name, tex_name, R);
      }
    }
  }
  if (done) {
    lg::info("PC-REMAKE medres rings: {} billboards replaced by tori", done);
  }
}

// les palmiers penchés (et feuillages suspendus) sont FONDUS dans le terrain TFRAG
void tag_tfrag_foliage(tfrag3::Level& lev) {
  // palm-leaf ET leafyground-hang sont désormais TRANSPLANTÉES dans le circuit vent TIE
  static const char* kLeafTex[] = {"jamais-rien-ne-matche-ceci"};
  int tagged = 0;
  for (auto& geom_trees : lev.tfrag_trees) {
    for (auto& tree : geom_trees) {
      std::vector<char> done(tree.unpacked.vertices.size(), 0);
      for (auto& draw : tree.draws) {
        if (draw.tree_tex_id < 0 || draw.tree_tex_id >= (int)lev.textures.size()) {
          continue;
        }
        const auto& tex_name = lev.textures.at(draw.tree_tex_id).debug_name;
        bool is_leaf = false;
        for (auto* kw : kLeafTex) {
          if (tex_name.find(kw) != std::string::npos) {
            is_leaf = true;
            break;
          }
        }
        if (!is_leaf) {
          continue;
        }
        auto tag_vertex = [&](u32 vi) {
          if (vi == UINT32_MAX || vi >= tree.unpacked.vertices.size() || done[vi]) {
            return;
          }
          done[vi] = 1;
          tree.unpacked.vertices[vi].s += 100.f;
          tagged++;
        };
        for (auto& run : draw.runs) {
          for (u32 j = 0; j < run.length; j++) {
            tag_vertex(run.vertex0 + j);
          }
        }
        for (u32 vi : draw.plain_indices) {
          tag_vertex(vi);
        }
      }

      // reconstruire le squelette de chaque couronne (la même information que possèdent les palmiers TIE)
      {
        struct Crown {
          double x = 0, y = 0, z = 0;
          int n = 0;
          float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        };
        std::vector<Crown> crowns;
        std::vector<int> vert_crown(tree.unpacked.vertices.size(), -1);
        constexpr float kMerge = 4096.f * 7.f;  // 7 m : une couronne
        for (u32 vi = 0; vi < tree.unpacked.vertices.size(); vi++) {
          if (!done[vi]) {
            continue;
          }
          const auto& v = tree.unpacked.vertices[vi];
          int best = -1;
          for (size_t c = 0; c < crowns.size(); c++) {
            float dx = (float)(crowns[c].x / crowns[c].n) - v.x;
            float dz = (float)(crowns[c].z / crowns[c].n) - v.z;
            if (dx * dx + dz * dz < kMerge * kMerge) {
              best = (int)c;
              break;
            }
          }
          if (best < 0) {
            crowns.push_back({});
            best = (int)crowns.size() - 1;
          }
          auto& c = crowns[best];
          c.x += v.x;
          c.y += v.y;
          c.z += v.z;
          c.n++;
          c.x0 = std::min(c.x0, v.x);
          c.x1 = std::max(c.x1, v.x);
          c.y0 = std::min(c.y0, v.y);
          c.y1 = std::max(c.y1, v.y);
          c.z0 = std::min(c.z0, v.z);
          c.z1 = std::max(c.z1, v.z);
          vert_crown[vi] = best;
        }
        for (u32 vi = 0; vi < tree.unpacked.vertices.size(); vi++) {
          int ci = vert_crown[vi];
          if (ci < 0) {
            continue;
          }
          auto& c = crowns[ci];
          auto& v = tree.unpacked.vertices[vi];
          const float cx = (float)(c.x / c.n);
          const float cz = (float)(c.z / c.n);
          const float r = std::max(std::max(c.x1 - c.x0, c.z1 - c.z0) * 0.5f, 1.f);
          const float dx = v.x - cx;
          const float dz = v.z - cz;
          const float fr = std::min(std::sqrt(dx * dx + dz * dz) / r, 1.f);
          const float az = (std::atan2(dz, dx) + (float)M_PI) / (2.f * (float)M_PI);
          const float hn = std::min(std::max((v.y - c.y0) / std::max(c.y1 - c.y0, 1.f), 0.f), 1.f);
          v.r = (u8)(fr * 255.f);
          v.g = (u8)(az * 255.f);
          v.b = (u8)(hn * 255.f);
          v.a = (u8)((ci * 37) & 0xff);  // phase de houle propre à la couronne
        }
      }
    }
  }
  if (tagged) {
    lg::info("PC-REMAKE tfrag foliage: {} sommets de feuilles marqués pour le vent", tagged);
  } else {
    // diagnostic
    std::set<std::string> seen;
    for (auto& geom_trees : lev.tfrag_trees) {
      for (auto& tree : geom_trees) {
        for (auto& draw : tree.draws) {
          if (draw.tree_tex_id >= 0 && draw.tree_tex_id < (int)lev.textures.size()) {
            seen.insert(lev.textures.at(draw.tree_tex_id).debug_name);
          }
        }
      }
    }
    std::string all;
    int n = 0;
    for (auto& s : seen) {
      all += s + " ";
      if (++n >= 60) {
        break;
      }
    }
    lg::info("PC-REMAKE tfrag foliage: 0 marqué. Textures tfrag: {}", all);
  }
}

// LA CHIRURGIE DES PALMIERS PENCHÉS
void transplant_leaning_palms(tfrag3::Level& lev) {
  if (lev.tie_trees[0].empty()) {
    return;  // pas d'arbre TIE où greffer
  }
  int crowns_made = 0;
  for (auto& geom_trees : lev.tfrag_trees) {
    for (auto& tree : geom_trees) {
      // 1) collecter et VIDER les draws de feuilles de palmier
      struct LeafVert {
        float x, y, z, s, t;
        u16 color_index;
      };
      struct SrcDraw {
        DrawMode mode;
        s32 tex;
        std::vector<u32> vidx;
      };
      std::vector<SrcDraw> excised;
      for (auto& draw : tree.draws) {
        if (draw.tree_tex_id < 0 || draw.tree_tex_id >= (int)lev.textures.size()) {
          continue;
        }
        const auto& tn = lev.textures.at(draw.tree_tex_id).debug_name;
        // les couronnes des grands penchés sont peintes avec leafyground-hang, PAS palm-leaf
        if (tn.find("palm-leaf") == std::string::npos &&
            tn.find("leafyground-hang") == std::string::npos) {
          continue;
        }
        SrcDraw sd;
        sd.mode = draw.mode;
        sd.tex = draw.tree_tex_id;
        for (auto& run : draw.runs) {
          for (u32 j = 0; j < run.length; j++) {
            sd.vidx.push_back(run.vertex0 + j);
          }
          sd.vidx.push_back(UINT32_MAX);  // fin de ruban
        }
        for (u32 vi : draw.plain_indices) {
          sd.vidx.push_back(vi);
        }
        excised.push_back(std::move(sd));
        // excision : le draw entier disparaît du terrain
        draw.runs.clear();
        draw.plain_indices.clear();
        draw.num_triangles = 0;
      }
      if (excised.empty()) {
        continue;
      }

      // 2) clusteriser les sommets par couronne (xz, fusion 7 m)
      auto& tie = lev.tie_trees[0][0];
      struct Crown {
        double sx = 0, sy = 0, sz = 0;
        int n = 0;
      };
      std::vector<Crown> crowns;
      auto crown_of = [&](float x, float z) -> int {
        constexpr float kMerge = 4096.f * 7.f;
        for (size_t c = 0; c < crowns.size(); c++) {
          float dx = (float)(crowns[c].sx / crowns[c].n) - x;
          float dz = (float)(crowns[c].sz / crowns[c].n) - z;
          if (dx * dx + dz * dz < kMerge * kMerge) {
            return (int)c;
          }
        }
        crowns.push_back({});
        return (int)crowns.size() - 1;
      };
      // premier passage
      std::map<std::pair<int, u32>, int> vert_crown;  // (draw, vert) -> couronne
      for (size_t di = 0; di < excised.size(); di++) {
        for (u32 vi : excised[di].vidx) {
          if (vi == UINT32_MAX || vi >= tree.unpacked.vertices.size()) {
            continue;
          }
          const auto& v = tree.unpacked.vertices[vi];
          int ci = crown_of(v.x, v.z);
          auto& c = crowns[ci];
          c.sx += v.x;
          c.sy += v.y;
          c.sz += v.z;
          c.n++;
          vert_crown[{(int)di, vi}] = ci;
        }
      }
      if (crowns.empty()) {
        continue;
      }

      // 3) palette
      std::map<u32, u32> quad_remap;  // quad tfrag -> quad tie
      auto remap_color = [&](u16 old_color) -> u16 {
        u32 oq = old_color / 4;
        auto it = quad_remap.find(oq);
        if (it == quad_remap.end()) {
          const u32 nq = tie.colors.color_count / 4;
          if ((nq + 1) * 4 > 8190) {
            return 0;  // palette pleine : teinte 0 plutôt qu'un débordement
          }
          const size_t src_off = (size_t)oq * 4 * 4 * 8;
          if (src_off + 128 > tree.colors.data.size()) {
            return 0;
          }
          tie.colors.data.insert(tie.colors.data.end(), tree.colors.data.begin() + src_off,
                                 tree.colors.data.begin() + src_off + 128);
          tie.colors.color_count += 4;
          it = quad_remap.emplace(oq, nq).first;
        }
        return (u16)(it->second * 4 + (old_color % 4));
      };

      // 4) réinjection
      const u32 first_new_instance = (u32)tie.wind_instance_info.size();
      std::vector<u32> crown_vert_base(crowns.size());
      // les sommets de chaque couronne, en espace LOCAL (centre soustrait)
      std::map<std::pair<int, u32>, u32> new_index;  // (draw, vert) -> index tie
      for (size_t ci = 0; ci < crowns.size(); ci++) {
        auto& inst = tie.wind_instance_info.emplace_back();
        inst.matrix[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
        inst.matrix[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
        inst.matrix[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
        inst.matrix[3] = math::Vector4f((float)(crowns[ci].sx / crowns[ci].n),
                                        (float)(crowns[ci].sy / crowns[ci].n),
                                        (float)(crowns[ci].sz / crowns[ci].n), 1.f);
        inst.wind_idx = 0;
        inst.stiffness = 0.3f;
      }
      for (size_t di = 0; di < excised.size(); di++) {
        for (u32 vi : excised[di].vidx) {
          if (vi == UINT32_MAX) {
            continue;
          }
          auto key = std::make_pair((int)di, vi);
          if (new_index.count(key)) {
            continue;
          }
          int ci = vert_crown[key];
          const auto& c = crowns[ci];
          const auto& src = tree.unpacked.vertices[vi];
          tfrag3::PreloadedVertex nv = src;
          nv.x = src.x - (float)(c.sx / c.n);
          nv.y = src.y - (float)(c.sy / c.n);
          nv.z = src.z - (float)(c.sz / c.n);
          nv.color_index = remap_color(src.color_index);
          nv.color_index2 = nv.color_index;
          new_index[key] = (u32)tie.unpacked.vertices.size();
          tie.unpacked.vertices.push_back(nv);
        }
      }
      // 5) un draw de vent par (draw source × couronne)
      for (size_t di = 0; di < excised.size(); di++) {
        std::map<int, std::vector<u32>> per_crown_stream;
        int cur_crown = -1;
        for (u32 vi : excised[di].vidx) {
          if (vi == UINT32_MAX) {
            if (cur_crown >= 0) {
              per_crown_stream[cur_crown].push_back(UINT32_MAX);
            }
            cur_crown = -1;
            continue;
          }
          auto key = std::make_pair((int)di, vi);
          int ci = vert_crown[key];
          if (cur_crown >= 0 && ci != cur_crown) {
            per_crown_stream[cur_crown].push_back(UINT32_MAX);
          }
          cur_crown = ci;
          per_crown_stream[ci].push_back(new_index[key]);
        }
        for (auto& [ci, stream] : per_crown_stream) {
          if (stream.size() < 3) {
            continue;
          }
          auto& wd = tie.instanced_wind_draws.emplace_back();
          wd.mode = excised[di].mode;
          wd.tree_tex_id = excised[di].tex;
          wd.vertex_index_stream = stream;
          tfrag3::InstancedStripDraw::InstanceGroup grp;
          grp.num = (u32)stream.size();
          grp.instance_idx = first_new_instance + (u32)ci;
          grp.vis_idx = 0;
          wd.instance_groups.push_back(grp);
        }
      }
      for (size_t ci = 0; ci < crowns.size() && ci < 3; ci++) {
        lg::info("PC-REMAKE couronne greffée #{} à ({:.1f}, {:.1f}, {:.1f}) m", ci,
                 crowns[ci].sx / crowns[ci].n / 4096.0, crowns[ci].sy / crowns[ci].n / 4096.0,
                 crowns[ci].sz / crowns[ci].n / 4096.0);
      }
      crowns_made += (int)crowns.size();
    }
  }
  if (crowns_made) {
    lg::info("PC-REMAKE transplantation : {} couronnes de palmiers penchés greffées au vent",
             crowns_made);
  }
}

void apply_material_reflections(tfrag3::Level& lev) {
  // rien dans le niveau TITRE
  if (lev.level_name == "title") {
    return;
  }
  auto& models = lev.merc_data.models;

  // an envmap texture the game already uses for reflective materials
  u32 donor_tex = 0;
  DrawMode donor_mode;
  bool have_donor = false;
  for (auto& model : models) {
    for (auto& eff : model.effects) {
      if (eff.has_envmap) {
        donor_tex = eff.envmap_texture;
        donor_mode = eff.envmap_mode;
        have_donor = true;
        break;
      }
    }
    if (have_donor) {
      break;
    }
  }
  if (!have_donor) {
    return;  // this level ships no reflective material to borrow from
  }

  int lit = 0;
  for (auto& model : models) {
    for (auto& eff : model.effects) {
      if (eff.has_envmap || eff.has_mod_draw) {
        continue;
      }
      // an effect is one material: look at the texture of its largest draw
      u32 best = 0;
      s32 tex_id = -1;
      for (auto& d : eff.all_draws) {
        if (d.eye_id == 0xff && d.index_count > best && d.tree_tex_id >= 0) {
          best = d.index_count;
          tex_id = d.tree_tex_id;
        }
      }
      if (tex_id < 0 || tex_id >= (s32)lev.textures.size()) {
        continue;
      }
      if (!texture_is_metal(lev.textures[tex_id])) {
        continue;
      }
      eff.has_envmap = true;
      eff.envmap_texture = donor_tex;
      eff.envmap_mode = donor_mode;
      lit++;
    }
  }
  if (lit) {
    lg::info("PC-REMAKE reflections: {} metal materials made reflective", lit);
  }
}

Loader::Loader(const fs::path& base_path, int max_levels)
    : m_base_path(base_path), m_max_levels(max_levels) {
  m_loader_thread = std::thread(&Loader::loader_thread, this);
  m_loader_stages = make_loader_stages();
}

Loader::~Loader() {
  {
    std::lock_guard<std::mutex> lk(m_loader_mutex);
    m_want_shutdown = true;
    m_loader_cv.notify_all();
  }
  m_loader_thread.join();
}

/*!
 * Try to get a loaded level by name. It may fail and return nullptr.
 * Getting a level will reset the counter for the level and prevent it from being kicked out
 * for a little while.
 *
 * This is safe to call from the graphics thread
 */
const LevelData* Loader::get_tfrag3_level(const std::string& level_name) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  const auto& existing = m_loaded_tfrag3_levels.find(level_name);
  if (existing == m_loaded_tfrag3_levels.end()) {
    return nullptr;
  } else {
    existing->second->frames_since_last_used = 0;
    return existing->second.get();
  }
}

void Loader::debug_print_loaded_levels() {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (const auto& [name, _] : m_loaded_tfrag3_levels) {
    fmt::print("{}\n", name);
  }
}

/*!
 * The game calls this to give the loader a hint on which levels we want.
 * If the loader is not busy, it will begin loading the level.
 * This should be called on every frame.
 */
void Loader::set_want_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_desired_levels = levels;
  if (!m_level_to_load.empty()) {
    // can't do anything, we're loading a level right now
    return;
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    // can't do anything, we're initializing a level right now
    return;
  }

  // loader isn't busy, try to load one of the requested levels.
  for (auto& lev : levels) {
    auto it = m_loaded_tfrag3_levels.find(lev);
    if (it == m_loaded_tfrag3_levels.end()) {
      // we haven't loaded it yet. Request this level to load and wake up the thread.
      m_level_to_load = lev;
      lk.unlock();
      m_loader_cv.notify_all();
      return;
    }
  }
}

/*!
 * The game calls this to tell the loader that we absolutely want these levels active.
 * This will NOT trigger a load!
 */
void Loader::set_active_levels(const std::vector<std::string>& levels) {
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  m_active_levels = levels;
}

/*!
 * Get all levels that are in memory and used very recently.
 */
std::vector<LevelData*> Loader::get_in_use_levels() {
  std::vector<LevelData*> result;
  std::unique_lock<std::mutex> lk(m_loader_mutex);

  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used < 5) {
      result.push_back(lev.get());
    }
  }
  return result;
}

void Loader::draw_debug_window() {
  ImGui::Begin("Loader");

  if (!m_selected_level_for_reload.empty() &&
      m_loaded_tfrag3_levels.find(m_selected_level_for_reload) == m_loaded_tfrag3_levels.end()) {
    m_selected_level_for_reload.clear();
  }

  const char* preview = m_selected_level_for_reload.empty() ? "(select a level)"
                                                            : m_selected_level_for_reload.c_str();
  if (ImGui::BeginCombo("Level Select", preview)) {
    for (const auto& [name, data] : m_loaded_tfrag3_levels) {
      bool selected = (name == m_selected_level_for_reload);
      if (ImGui::Selectable(name.c_str(), selected)) {
        m_selected_level_for_reload = name;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  bool single_reload_pending = !m_single_level_to_reload.empty();
  bool can_reload_selected =
      !m_selected_level_for_reload.empty() && !single_reload_pending && !m_want_reload;
  if (!can_reload_selected) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Force Reload Selected")) {
    m_single_level_to_reload = m_selected_level_for_reload;
  }
  if (!can_reload_selected) {
    ImGui::EndDisabled();
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload Common")) {
    m_want_reload_common = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Force Reload All")) {
    m_want_reload = true;
  }
  ImGui::SameLine();
  if (m_want_reload) {
    ImGui::TextUnformatted("(waiting for full reload...)");
  } else if (single_reload_pending) {
    ImGui::Text("(waiting to reload %s...)", m_single_level_to_reload.c_str());
  }
  ImGui::Separator();

  std::unique_lock<std::mutex> lk(m_loader_mutex);
  ImVec4 blue(0.3, 0.3, 0.8, 1.0);
  ImVec4 red(0.8, 0.3, 0.3, 1.0);
  ImVec4 green(0.3, 0.8, 0.3, 1.0);

  if (!m_desired_levels.empty()) {
    ImGui::Text("desired levels");
    for (auto& lev : m_desired_levels) {
      auto lev_color = red;
      if (m_initializing_tfrag3_levels.find(lev) != m_initializing_tfrag3_levels.end()) {
        lev_color = blue;
      }
      if (m_loaded_tfrag3_levels.find(lev) != m_loaded_tfrag3_levels.end()) {
        lev_color = green;
      }
      ImGui::TextColored(lev_color, "%s", lev.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_initializing_tfrag3_levels.empty()) {
    ImGui::Text("init levels");
    for (auto& lev : m_initializing_tfrag3_levels) {
      ImGui::TextColored(blue, "%s", lev.first.c_str());
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  if (!m_loaded_tfrag3_levels.empty()) {
    ImGui::Text("loaded levels");
    for (auto& lev : m_loaded_tfrag3_levels) {
      auto lev_color = green;
      if (lev.second->frames_since_last_used > 0) {
        lev_color = blue;
      }
      if (lev.second->frames_since_last_used > 180) {
        lev_color = red;
      }
      ImGui::TextColored(lev_color, "%20s : %3d", lev.first.c_str(),
                         lev.second->frames_since_last_used);
      ImGui::Text("  %d textures", (int)lev.second->textures.size());
      ImGui::Text("  %d merc", (int)lev.second->merc_model_lookup.size());
    }
    ImGui::NewLine();
    ImGui::Separator();
  }

  ImGui::End();
}

/*!
 * Loader function that runs in a completely separate thread.
 * This is used for file I/O and unpacking.
 */
void Loader::loader_thread() {
  try {
    while (!m_want_shutdown) {
      prof().root_event();
      std::unique_lock<std::mutex> lk(m_loader_mutex);

      // this will keep us asleep until we've got a level to load.
      m_loader_cv.wait(lk, [&] { return !m_level_to_load.empty() || m_want_shutdown; });
      if (m_want_shutdown) {
        return;
      }
      std::string lev = m_level_to_load;
      // don't hold the lock while reading the file.
      lk.unlock();

      // simulate slower hard drive (so that the loader thread can lose to the game loads)
      // std::this_thread::sleep_for(std::chrono::milliseconds(1500));

      // load the fr3 file
      prof().begin_event("read-file");
      Timer disk_timer;
      auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", lev));
      double disk_load_time = disk_timer.getSeconds();
      prof().end_event();

      // the FR3 files are compressed
      prof().begin_event("decompress-file");
      Timer decomp_timer;
      auto decomp_data = compression::decompress_zstd(data.data(), data.size());
      double decomp_time = decomp_timer.getSeconds();
      prof().end_event();

      // Read back into the tfrag3::Level structure
      prof().begin_event("deserialize");
      Timer import_timer;
      auto result = std::make_unique<tfrag3::Level>();
      Serializer ser(decomp_data.data(), decomp_data.size());
      result->serialize(ser);
      double import_time = import_timer.getSeconds();
      prof().end_event();

      // and finally "unpack", which creates the vertex data we'll upload to the GPU

      Timer unpack_timer;
      const size_t n_decor0 = tfrag3::g_pc_decor_tris_added;  // affinage du decor
      const size_t n_sculpt0 = tfrag3::g_pc_sculpt_tris_added;
      // budget de la sculpture du decor pour ce niveau
      tfrag3::g_pc_sculpt_budget = [] {
        const char* e = std::getenv("QA_SCULPT_BUDGET");
        return e ? std::atof(e) : 1.2e6;
      }();
      {
        auto p = scoped_prof("tie-unpack");
        for (size_t geo = 0; geo < result->tie_trees.size(); geo++) {
          auto& tie_tree = result->tie_trees[geo];
          for (auto& tree : tie_tree) {
            tree.unpack();
            // round static TIE geometry (huts, barrels, bridges, fences)
            if (tfrag3::g_tfrag_phong_subdiv && tree.use_strips) {
              tree.subdivide_phong(result->textures, geo == 0);
            }

          }
        }
      }

      {
        auto p = scoped_prof("tfrag-unpack");
        for (size_t geo = 0; geo < result->tfrag_trees.size(); geo++) {
          auto& t_tree = result->tfrag_trees[geo];
          for (auto& tree : t_tree) {
            tree.unpack();
            // round terrain geometry
            if (tfrag3::g_tfrag_phong_subdiv && tree.use_strips) {
              // avec les textures = affinage du decor (bois, paille)
              tree.subdivide_phong(&result->textures, geo == 0);
            }
          }
        }
        // balaye la géométrie du niveau pour les surfaces qui brillent (lave, feux, lueurs précurseurs…)
        pc_emissive_scan_level(*result);
      }
      lg::info("PC-REMAKE : decor affine, {} triangles ajoutes dont {} sculptes ({:.0f} ms)",
               tfrag3::g_pc_decor_tris_added - n_decor0, tfrag3::g_pc_sculpt_tris_added - n_sculpt0,
               unpack_timer.getMs());

      {
        auto p = scoped_prof("shrub-unpack");
        for (auto& shrub_tree : result->shrub_trees) {
          shrub_tree.unpack();
        }
      }

      // material-driven reflections
      apply_material_reflections(*result);
      register_scenery_lamps(*result);
      // far-view temple rings become real tori
      remake_medres_rings(*result);
      // leaning palms baked in the terrain get wind too
      tag_tfrag_foliage(*result);
      // chirurgie RÉVOQUÉE le

      fmt::print(
          "------------> Load from file: {:.3f}s, import {:.3f}s, decomp {:.3f}s unpack {:.3f}s\n",
          disk_load_time, import_time, decomp_time, unpack_timer.getSeconds());

      // grab the lock again
      lk.lock();
      // move this level to "initializing" state.
      m_initializing_tfrag3_levels[lev] = std::make_unique<LevelData>();  // reset load state
      m_initializing_tfrag3_levels[lev]->level = std::move(result);
      m_level_to_load = "";
      m_file_load_done_cv.notify_all();
    }
  } catch (std::exception& e) {
    ASSERT_MSG(false, fmt::format("Exception {} encountered in loader_thread", e.what()));
  }
}

/*!
 * Load a "common" FR3 file that has non-level textures.
 * This should be called during initialization, before any threaded loading goes on.
 */
const tfrag3::Level& Loader::load_common(TexturePool& tex_pool, const std::string& name) {
  auto data = file_util::read_binary_file(m_base_path / fmt::format("{}.fr3", name));

  auto decomp_data = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp_data.data(), decomp_data.size());
  m_common_level.level = std::make_unique<tfrag3::Level>();
  m_common_level.level->serialize(ser);
  for (auto& tex : m_common_level.level->textures) {
    m_common_level.textures.push_back(add_texture(tex_pool, tex, true));
  }

  Timer tim;
  MercLoaderStage mls;
  LoaderInput input;
  input.tex_pool = &tex_pool;
  input.mercs = &m_all_merc_models;
  input.lev_data = &m_common_level;
  bool done = false;
  while (!done) {
    done = mls.run(tim, input);
  }
  return *m_common_level.level;
}

bool Loader::upload_textures(Timer& timer, LevelData& data, TexturePool& texture_pool) {
  // try to move level from initializing to initialized:

  auto evt = scoped_prof("upload-textures");
  constexpr int MAX_TEX_BYTES_PER_FRAME = 1024 * 128;

  int bytes_this_run = 0;
  int tex_this_run = 0;
  if (data.textures.size() < data.level->textures.size()) {
    std::unique_lock<std::mutex> tpool_lock(texture_pool.mutex());
    while (data.textures.size() < data.level->textures.size()) {
      auto& tex = data.level->textures[data.textures.size()];
      data.textures.push_back(add_texture(texture_pool, tex, false));
      bytes_this_run += tex.w * tex.h * 4;
      tex_this_run++;
      if (tex_this_run > 20) {
        break;
      }
      if (bytes_this_run > MAX_TEX_BYTES_PER_FRAME || timer.getMs() > SHARED_TEXTURE_LOAD_BUDGET) {
        break;
      }
    }
  }
  return data.textures.size() == data.level->textures.size();
}

void Loader::update_blocking(TexturePool& tex_pool) {
  fmt::print("NOTE: coming out of blackout on next frame, doing all loads now...\n");

  bool missing_levels = true;
  while (missing_levels) {
    bool needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_level_to_load.empty()) {
          m_file_load_done_cv.wait(lk, [&]() { return m_level_to_load.empty(); });
        }
      }
    }

    needs_run = true;

    while (needs_run) {
      needs_run = false;
      {
        std::unique_lock<std::mutex> lk(m_loader_mutex);
        if (!m_initializing_tfrag3_levels.empty()) {
          needs_run = true;
        }
      }

      if (needs_run) {
        update(tex_pool);
      }
    }

    {
      std::unique_lock<std::mutex> lk(m_loader_mutex);
      missing_levels = false;
      for (auto& des : m_desired_levels) {
        if (m_loaded_tfrag3_levels.find(des) == m_loaded_tfrag3_levels.end()) {
          fmt::print("blackout loader doing additional level {}...\n", des);
          missing_levels = true;
        }
      }
    }

    if (missing_levels) {
      set_want_levels(m_desired_levels);
    }
  }

  fmt::print("Blackout loads done. Current status:");
  std::unique_lock<std::mutex> lk(m_loader_mutex);
  for (auto& ld : m_loaded_tfrag3_levels) {
    fmt::print("  {} is loaded.\n", ld.first);
  }
}

const std::string* Loader::get_most_unloadable_level() {
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180 &&
        std::find(m_desired_levels.begin(), m_desired_levels.end(), name) ==
            m_desired_levels.end()) {
      return &name;
    }
  }

  for (const auto& [name, lev] : m_loaded_tfrag3_levels) {
    if (lev->frames_since_last_used > 180) {
      return &name;
    }
  }
  return nullptr;
}

void Loader::update(TexturePool& texture_pool) {
  Timer loader_timer;

  if (m_want_reload) {
    std::unique_lock lk(m_loader_mutex);
    if (m_level_to_load.empty() && m_initializing_tfrag3_levels.empty()) {
      m_want_reload = false;
      lk.unlock();
      do_reload(texture_pool);
      return;
    }
  }

  if (!m_single_level_to_reload.empty()) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      std::string name = std::move(m_single_level_to_reload);
      m_single_level_to_reload.clear();
      lk.unlock();
      do_reload_level(name, texture_pool);
      return;
    }
  }

  if (m_want_reload_common) {
    std::unique_lock lk(m_loader_mutex);
    bool in_init = m_initializing_tfrag3_levels.count(m_single_level_to_reload) > 0;
    if (m_level_to_load.empty() && !in_init) {
      m_want_reload_common = false;
      lk.unlock();
      do_reload_common(texture_pool);
      return;
    }
  }

  {
    // lock because we're accessing m_active_levels
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // only main thread can touch this.
    for (auto& [name, lev] : m_loaded_tfrag3_levels) {
      if (std::find(m_active_levels.begin(), m_active_levels.end(), name) ==
          m_active_levels.end()) {
        lev->frames_since_last_used++;
      } else {
        lev->frames_since_last_used = 0;
      }
    }
  }

  bool did_gpu_stuff = false;

  // work on moving initializing to initialized.
  {
    // accessing initializing, should lock
    std::unique_lock<std::mutex> lk(m_loader_mutex);
    // grab the first initializing level:
    const auto& it = m_initializing_tfrag3_levels.begin();
    if (it != m_initializing_tfrag3_levels.end()) {
      did_gpu_stuff = true;
      std::string name = it->first;
      auto& lev = it->second;
      if (it->second->load_id == UINT64_MAX) {
        it->second->load_id = m_id++;
      }

      // we're the only place that erases, so it's okay to unlock and hold a reference
      lk.unlock();
      bool done = true;
      LoaderInput loader_input;
      loader_input.lev_data = lev.get();
      loader_input.mercs = &m_all_merc_models;
      loader_input.tex_pool = &texture_pool;

      for (auto& stage : m_loader_stages) {
        auto evt = scoped_prof(fmt::format("stage-{}", stage->name()).c_str());
        Timer stage_timer;
        done = stage->run(loader_timer, loader_input);
        if (stage_timer.getMs() > 5.f) {
          fmt::print("stage {} took {:.2f} ms\n", stage->name(), stage_timer.getMs());
        }
        if (!done) {
          break;
        }
      }

      if (done) {
        auto evt = scoped_prof("finish-stages");
        lk.lock();
        m_loaded_tfrag3_levels[name] = std::move(lev);
        m_initializing_tfrag3_levels.erase(it);

        for (auto& stage : m_loader_stages) {
          stage->reset();
        }
      }
    }
  }

  if (!did_gpu_stuff) {
    auto evt = scoped_prof("gpu-unload");
    // try to remove levels.
    Timer unload_timer;
    if ((int)m_loaded_tfrag3_levels.size() >= m_max_levels) {
      auto to_unload = get_most_unloadable_level();
      if (to_unload) {
        auto& lev = m_loaded_tfrag3_levels.at(*to_unload);
        // retire les lumières émissives scannées de ce niveau
        pc_emissive_drop_level(*to_unload);
        std::unique_lock<std::mutex> lk(texture_pool.mutex());
        fmt::print("------------------------- PC unloading {}\n", *to_unload);
        for (size_t i = 0; i < lev->level->textures.size(); i++) {
          auto& tex = lev->level->textures[i];
          if (tex.load_to_pool) {
            texture_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id),
                                        lev->textures.at(i));
          }
        }
        lk.unlock();
        for (auto tex : lev->textures) {
          if (EXTRA_TEX_DEBUG) {
            for (auto& slot : texture_pool.all_textures()) {
              if (slot.source) {
                ASSERT(slot.gpu_texture != tex);
              } else {
                ASSERT(slot.gpu_texture != tex);
              }
            }
          }
          m_garbage_textures.push_back(tex);
          pc_sculpt_forget_texture(tex);
          pc_normal_forget_texture(tex);
          pc_det_forget_texture(tex);
        }

        for (auto& tie_geo : lev->tie_data) {
          for (auto& tie_tree : tie_geo) {
            m_garbage_buffers.push_back(tie_tree.vertex_buffer);
            if (tie_tree.has_wind) {
              m_garbage_buffers.push_back(tie_tree.wind_indices);
            }
            m_garbage_buffers.push_back(tie_tree.index_buffer);
          }
        }

        for (auto& tfrag_geo : lev->tfrag_vertex_data) {
          for (auto& tfrag_buff : tfrag_geo) {
            m_garbage_buffers.push_back(tfrag_buff);
          }
        }

        m_garbage_buffers.push_back(lev->hfrag_indices);
        m_garbage_buffers.push_back(lev->hfrag_indices);

        m_garbage_buffers.push_back(lev->collide_vertices);
        m_garbage_buffers.push_back(lev->merc_vertices);
        m_garbage_buffers.push_back(lev->merc_indices);

        for (auto& model : lev->level->merc_data.models) {
          auto& mercs = m_all_merc_models.at(model.name);
          MercRef ref{&model, lev->load_id};
          auto it = std::find(mercs.begin(), mercs.end(), ref);
          ASSERT_MSG(it != mercs.end(), fmt::format("missing merc: {}\n", model.name));
          mercs.erase(it);
        }

        m_loaded_tfrag3_levels.erase(*to_unload);
      }
    }

    if (unload_timer.getMs() > 5.f) {
      fmt::print("Unload took {:.2f}ms\n", unload_timer.getMs());
    }

    if (!m_garbage_buffers.empty()) {
      did_gpu_stuff = true;
      for (int i = 0; i < 5 && !m_garbage_buffers.empty(); i++) {
        glDeleteBuffers(1, &m_garbage_buffers.back());
        m_garbage_buffers.pop_back();
      }
    }

    if (!did_gpu_stuff && !m_garbage_textures.empty()) {
      for (int i = 0; i < 20 && !m_garbage_textures.empty(); i++) {
        glDeleteTextures(1, &m_garbage_textures.back());
        m_garbage_textures.pop_back();
      }
    }
  }

  if (loader_timer.getMs() > 5) {
    fmt::print("Loader::update slow setup: {:.1f}ms\n", loader_timer.getMs());
  }
}

std::optional<MercRef> Loader::get_merc_model(const char* model_name) {
  // don't think we need to lock here...
  const auto& it = m_all_merc_models.find(model_name);
  if (it != m_all_merc_models.end() && !it->second.empty()) {
    // it->second.front().parent_level->frames_since_last_used = 0;
    return it->second.front();
  } else {
    return std::nullopt;
  }
}

void Loader::unload_level_data(const std::string& name, LevelData& lev, TexturePool& tex_pool) {
  fmt::print("force reload: unloading {}\n", name);
  {
    std::unique_lock lk(tex_pool.mutex());
    for (size_t i = 0; i < lev.level->textures.size(); i++) {
      auto& tex = lev.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id), lev.textures.at(i));
      }
    }
  }
  for (auto tex : lev.textures) {
    glDeleteTextures(1, &tex);
  }
  for (auto& tie_geo : lev.tie_data) {
    for (auto& tie_tree : tie_geo) {
      glDeleteBuffers(1, &tie_tree.vertex_buffer);
      if (tie_tree.has_wind) {
        glDeleteBuffers(1, &tie_tree.wind_indices);
      }
      glDeleteBuffers(1, &tie_tree.index_buffer);
    }
  }
  for (auto& tfrag_geo : lev.tfrag_vertex_data) {
    for (auto& buf : tfrag_geo) {
      glDeleteBuffers(1, &buf);
    }
  }
  for (auto& buf : lev.shrub_vertex_data) {
    glDeleteBuffers(1, &buf);
  }
  glDeleteBuffers(1, &lev.hfrag_indices);
  glDeleteBuffers(1, &lev.hfrag_vertices);
  glDeleteBuffers(1, &lev.collide_vertices);
  glDeleteBuffers(1, &lev.merc_vertices);
  glDeleteBuffers(1, &lev.merc_indices);
  for (auto& model : lev.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end())
      continue;
    MercRef ref{&model, lev.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end())
      it->second.erase(ref_it);
  }
}

void Loader::do_reload_level(const std::string& name, TexturePool& texture_pool) {
  auto it = m_loaded_tfrag3_levels.find(name);
  if (it == m_loaded_tfrag3_levels.end()) {
    return;
  }
  unload_level_data(name, *it->second, texture_pool);
  m_loaded_tfrag3_levels.erase(it);

  std::unique_lock lk(m_loader_mutex);
  if (m_level_to_load.empty()) {
    m_level_to_load = name;
    lk.unlock();
    m_loader_cv.notify_all();
  }
}

void Loader::do_reload_common(TexturePool& tex_pool) {
  fmt::print("loader: force reloading common level\n");
  {
    std::unique_lock lk(tex_pool.mutex());
    for (size_t i = 0;
         i < m_common_level.textures.size() && i < m_common_level.level->textures.size(); i++) {
      auto& tex = m_common_level.level->textures[i];
      if (tex.load_to_pool) {
        tex_pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id),
                                m_common_level.textures[i]);
      }
    }
  }
  for (auto tex : m_common_level.textures) {
    glDeleteTextures(1, &tex);
  }
  for (auto& model : m_common_level.level->merc_data.models) {
    auto it = m_all_merc_models.find(model.name);
    if (it == m_all_merc_models.end())
      continue;
    MercRef ref{&model, m_common_level.load_id};
    auto ref_it = std::ranges::find(it->second, ref);
    if (ref_it != it->second.end())
      it->second.erase(ref_it);
  }
  m_common_level = LevelData{};
  load_common(tex_pool, "GAME");
}

void Loader::do_reload(TexturePool& texture_pool) {
  fmt::print("loader: force reloading all levels\n");
  for (auto& [name, lev] : m_loaded_tfrag3_levels) {
    unload_level_data(name, *lev, texture_pool);
  }
  m_loaded_tfrag3_levels.clear();

  for (auto buf : m_garbage_buffers)
    glDeleteBuffers(1, &buf);
  m_garbage_buffers.clear();
  for (auto tex : m_garbage_textures)
    glDeleteTextures(1, &tex);
  m_garbage_textures.clear();

  set_want_levels(m_desired_levels);
}