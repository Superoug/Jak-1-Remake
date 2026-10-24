#include "Tfrag3Data.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/simd_util.h"

namespace tfrag3 {

// PC-REMAKE: runtime Phong subdivision toggles (defined lower in this file,
// forward-declared here because TieTree::unpack precedes them)
extern bool g_tfrag_phong_subdiv;
extern float g_tfrag_phong_strength;
// PC-REMAKE : affinage du decor (defini plus bas)
size_t pc_refine_decor_impl(std::vector<PreloadedVertex>& verts,
                            std::vector<u32>& indices,
                            std::vector<StripDraw>& draws,
                            const std::vector<u8>& classes,
                            const std::function<u32(u32, u32)>& phong_mid,
                            const std::vector<const PcSculptHeight*>* maps);


void PackedTimeOfDay::serialize(Serializer& ser) {
  ser.from_pod_vector(&data);
  ser.from_ptr(&color_count);
}

void PackedTieVertices::serialize(Serializer& ser) {
  ser.from_pod_vector(&color_indices);
  ser.from_pod_vector(&matrices);
  ser.from_pod_vector(&matrix_groups);
  ser.from_pod_vector(&vertices);
  ser.from_pod_vector(&color_indices2);
  ser.from_pod_vector(&color_indices3);
  ser.from_pod_vector(&color_weights);
}

void PackedShrubVertices::serialize(Serializer& ser) {
  ser.from_pod_vector(&matrices);
  ser.from_pod_vector(&instance_groups);
  ser.from_pod_vector(&vertices);
  ser.from_ptr(&total_vertex_count);
}

void StripDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_pod_vector(&runs);
  ser.from_pod_vector(&plain_indices);
  ser.from_pod_vector(&vis_groups);
  ser.from_ptr(&num_triangles);
}

void ShrubDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_ptr(&num_triangles);
  ser.from_ptr(&first_index_index);
  ser.from_ptr(&num_indices);
  ser.from_ptr(&proto_idx);
}

void InstancedStripDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_pod_vector(&vertex_index_stream);
  ser.from_pod_vector(&instance_groups);
  ser.from_ptr(&num_triangles);
}

void TieWindInstance::serialize(Serializer& ser) {
  ser.from_ptr(&matrix);
  ser.from_ptr(&wind_idx);
  ser.from_ptr(&stiffness);
}

void TfragTree::serialize(Serializer& ser) {
  ser.from_ptr(&kind);

  if (ser.is_saving()) {
    ser.save<size_t>(draws.size());
  } else {
    draws.resize(ser.load<size_t>());
  }
  for (auto& draw : draws) {
    draw.serialize(ser);
  }

  ser.from_pod_vector(&packed_vertices.vertices);
  ser.from_pod_vector(&packed_vertices.cluster_origins);
  colors.serialize(ser);
  bvh.serialize(ser);
  ser.from_ptr(&use_strips);
}

math::Vector3f vopmula(math::Vector3f a, math::Vector3f b) {
  return math::Vector3f(a.y() * b.z(), a.z() * b.x(), a.x() * b.y());
}

math::Vector3f vopmsub(math::Vector3f acc, math::Vector3f a, math::Vector3f b) {
  return acc - vopmula(a, b);
}

/*!
 * Compute the normal transformation for a TIE from the TIE matrix. This will return properly scaled
 * normals.
 */
std::array<math::Vector3f, 3> tie_normal_transform_v2(const std::array<math::Vector4f, 4>& m) {
  // let:
  // vf10, vf11, vf12, vf13 be the input matrix m
  std::array<math::Vector3f, 3> result;
  auto& vf10 = m[0];
  auto& vf11 = m[1];
  // auto& vf12 = m[2];

  //  lui t6, 16256
  //  mtc1 f1, t6 ;; 1.0
  //
  //  qmfc2.i s1, vf10
  //  mtc1 f12, s1
  float f12 = vf10.x();
  //  dsra32 s2, s1, 0
  //  mtc1 f13, s2
  float f13 = vf10.y();
  //  pextuw s2, r0, s2
  //  mtc1 f14, s2
  float f14 = vf10.z();
  //  mula.s f12, f12
  //  madda.s f13, f13
  //  madd.s f15, f14, f14
  float f15 = f12 * f12 + f13 * f13 + f14 * f14;
  float scale = 1.f / sqrtf(f15);
  //  rsqrt.s f15, f1, f15
  //  mfc1 s1, f15
  //  qmtc2.i vf14, s1
  //  vmulx.xyz vf16, vf10, vf14

  // vmulx.xyz vf16, vf10, vf14
  math::Vector3f vf16 = vf10.xyz() * scale;

  // vopmula.xyz acc, vf11, vf16
  math::Vector3f acc = vopmula(vf11.xyz(), vf16);

  // vopmsub.xyz vf17, vf16, vf11
  math::Vector3f vf17 = vopmsub(acc, vf16, vf11.xyz());

  // vopmula.xyz acc, vf16, vf17
  acc = vopmula(vf16, vf17);

  // vopmsub.xyz vf17, vf17, vf16
  vf17 = vopmsub(acc, vf17, vf16);

  // vmul.xyz vf14, vf17, vf17
  math::Vector3f vf14 = vf17.elementwise_multiply(vf17);

  // vmulax.w acc, vf0, vf14
  // vmadday.w acc, vf0, vf14
  // vmaddz.w vf14, vf0, vf14
  float sum = vf14.x() + vf14.y() + vf14.z();

  // vrsqrt Q, vf0.w, vf14.w
  float Q = 1.f / std::sqrt(sum);

  // vmulax.xyzw acc, vf24, vf16
  // vmadday.xyzw acc, vf25, vf16
  // vmaddz.xyzw vf10, vf26, vf16
  // vf10 = vf16; // assume cam is identity here.
  result[0] = vf16;

  // vwaitq
  // vmulq.xyz vf17, vf17, Q
  vf17 *= Q;

  // vopmula.xyz acc, vf16, vf17
  acc = vopmula(vf16, vf17);
  // vopmsub.xyz vf18, vf17, vf16
  math::Vector3f vf18 = vopmsub(acc, vf17, vf16);

  // vmulax.xyzw acc, vf24, vf17
  // vmadday.xyzw acc, vf25, vf17
  // vmaddz.xyzw vf11, vf26, vf17
  result[1] = vf17;

  // vmulax.xyzw acc, vf24, vf18
  // vmadday.xyzw acc, vf25, vf18
  // vmaddz.xyzw vf12, vf26, vf18
  result[2] = vf18;

  return result;
  //
  // sqc2 vf10, -112(t8)
  // sqc2 vf11, -96(t8)
  // sqc2 vf12, -80(t8)
}

s16 saturate_for_s10(s16 s10) {
  // our error should be 1 or less as an s8, or 4 as a s10.
  ASSERT(s10 >= -520 && s10 < 520);
  if (s10 < -512) {
    return -512;
  }
  if (s10 > 511) {
    return 511;
  }
  return s10;
}

u32 pack_to_gl_normal(s16 nx, s16 ny, s16 nz) {
  return (saturate_for_s10(nx) & 0x3ff) | ((saturate_for_s10(ny) & 0x3ff) << 10) |
         ((saturate_for_s10(nz) & 0x3ff) << 20);
}

/*!
 * Unpack tie normal by transforming and converting to s16 for OpenGL.
 */
u32 unpack_tie_normal(const std::array<math::Vector3f, 3>& mat, s8 nx, s8 ny, s8 nz) {
  // rotate the normal
  math::Vector3f nrm = math::Vector3f::zero();
  nrm += mat[0] * nx;
  nrm += mat[1] * ny;
  nrm += mat[2] * nz;

  // game used signed 8-bit normals, but OpenGL uses signed 10-bit
  // multiply by 2^2 = 4
  nrm *= 4;

  auto as_int = nrm.cast<s16>();

  return pack_to_gl_normal(as_int.x(), as_int.y(), as_int.z());
}

/*
void tie_normal_v3(__m128* out, const std::array<math::Vector4f, 4>& in) {
  math::Vector3f x_row = in[0].xyz();
  math::Vector3f y_row = in[1].xyz();
  math::Vector3f z_row = in[2].xyz();

  x_row.normalize();
  y_row = x_row.cross(y_row.cross(x_row)).normalized();
  z_row = x_row.cross(y_row);

  out[0] = _mm_setr_ps(x_row[0], x_row[1], x_row[2], 0);
  out[1] = _mm_setr_ps(y_row[0], y_row[1], y_row[2], 0);
  out[2] = _mm_setr_ps(z_row[0], z_row[1], z_row[2], 0);
}
 */

// PC-REMAKE : couleurs a 3 indices des prototypes remodeles (voir PackedTieVertices)
static inline void pc_tie_color_blend(const PackedTieVertices& pv, size_t i, PreloadedVertex& vtx) {
  if (i < pv.color_indices2.size()) {
    const u16 w = pv.color_weights[i];
    if (w != 0) {
      vtx.color_index2 = pv.color_indices2[i];
      vtx.color_index3 = pv.color_indices3[i];
      vtx.cw0 = (u8)(w & 0xff);
      vtx.cw1 = (u8)(w >> 8);
      return;
    }
  }
  vtx.color_index2 = vtx.color_index;
}

void TieTree::unpack() {
  unpacked.vertices.resize(packed_vertices.color_indices.size());
  size_t i = 0;
  for (const auto& grp : packed_vertices.matrix_groups) {
    if (grp.matrix_idx == -1) {
      for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
        auto& vtx = unpacked.vertices[i];
        vtx.color_index = packed_vertices.color_indices[i];
        pc_tie_color_blend(packed_vertices, i, vtx);
        const auto& proto_vtx = packed_vertices.vertices[src_idx];
        vtx.x = proto_vtx.x;
        vtx.y = proto_vtx.y;
        vtx.z = proto_vtx.z;
        vtx.s = proto_vtx.s;
        vtx.t = proto_vtx.t;
        vtx.nor = pack_to_gl_normal(proto_vtx.nx << 1, proto_vtx.ny << 1, proto_vtx.nz << 1);
        vtx.r = proto_vtx.r;
        vtx.g = proto_vtx.g;
        vtx.b = proto_vtx.b;
        vtx.a = proto_vtx.a;
        i++;
      }
    } else {
      const auto& mat = packed_vertices.matrices[grp.matrix_idx];

      __m128 mat0 = _mm_loadu_ps(mat[0].data());
      __m128 mat1 = _mm_loadu_ps(mat[1].data());
      __m128 mat2 = _mm_loadu_ps(mat[2].data());
      __m128 mat3 = _mm_loadu_ps(mat[3].data());

      if (grp.has_normals) {
        auto nmat = tie_normal_transform_v2(mat);
        for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
          auto& vtx = unpacked.vertices[i];
          vtx.color_index = packed_vertices.color_indices[i];
        pc_tie_color_blend(packed_vertices, i, vtx);
          const auto& proto_vtx = packed_vertices.vertices[src_idx];
          // auto temp = mat[0] * proto_vtx.x + mat[1] * proto_vtx.y + mat[2] * proto_vtx.z +
          // mat[3];
          __m128 transformed = mat3;
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.x), mat0));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.y), mat1));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.z), mat2));
          _mm_storeu_ps(&vtx.x, transformed);
          vtx.s = proto_vtx.s;
          vtx.t = proto_vtx.t;
          vtx.nor = unpack_tie_normal(nmat, proto_vtx.nx, proto_vtx.ny, proto_vtx.nz);
          vtx.r = proto_vtx.r;
          vtx.g = proto_vtx.g;
          vtx.b = proto_vtx.b;
          vtx.a = proto_vtx.a;
          i++;
        }
      } else {
        for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
          auto& vtx = unpacked.vertices[i];
          vtx.color_index = packed_vertices.color_indices[i];
        pc_tie_color_blend(packed_vertices, i, vtx);
          const auto& proto_vtx = packed_vertices.vertices[src_idx];
          __m128 transformed = mat3;
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.x), mat0));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.y), mat1));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.z), mat2));
          _mm_storeu_ps(&vtx.x, transformed);
          vtx.s = proto_vtx.s;
          vtx.t = proto_vtx.t;
          vtx.nor = 0;
          vtx.r = proto_vtx.r;
          vtx.g = proto_vtx.g;
          vtx.b = proto_vtx.b;
          vtx.a = proto_vtx.a;
          i++;
        }
      }
    }
  }

  for (auto& draw : static_draws) {
    draw.unpacked.idx_of_first_idx_in_full_buffer = unpacked.indices.size();
    // indices can come from either runs or already in plain indices.
    for (auto& run : draw.runs) {
      for (u32 ri = 0; ri < run.length; ri++) {
        unpacked.indices.push_back(run.vertex0 + ri);
      }
      unpacked.indices.push_back(UINT32_MAX);
    }
    unpacked.indices.insert(unpacked.indices.end(), draw.plain_indices.begin(),
                            draw.plain_indices.end());
  }

  // PC-REMAKE: TIE subdivision is invoked by the Loader after unpack — it
  // needs the level's texture names to protect foliage draws.
}

void ShrubTree::unpack() {
  unpacked.vertices.resize(packed_vertices.total_vertex_count);
  size_t i = 0;

  for (const auto& grp : packed_vertices.instance_groups) {
    const auto& mat = packed_vertices.matrices[grp.matrix_idx];
    for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
      auto& vtx = unpacked.vertices[i];
      vtx.color_index = grp.color_index;
      const auto& proto_vtx = packed_vertices.vertices[src_idx];
      auto temp = mat[0] * proto_vtx.x + mat[1] * proto_vtx.y + mat[2] * proto_vtx.z + mat[3];
      vtx.x = temp.x();
      vtx.y = temp.y();
      vtx.z = temp.z();
      vtx.s = proto_vtx.s;
      vtx.t = proto_vtx.t;
      memcpy(vtx.rgba_base, proto_vtx.rgba, 3);
      i++;
    }
  }
  ASSERT(i == unpacked.vertices.size());
}

// PC-REMAKE: runtime Phong subdivision of tfrag geometry. Every triangle is
// split into four; the new edge-midpoints are curved along smoothed vertex
// normals, so faceted cliffs and hills become genuinely rounder. Runs at level
// load (nothing serialized), preserves per-vis-group culling, and converts the
// tree from strips to plain triangles.
// 22/09 : QA_NO_SUBDIV=1 desactive la subdivision Phong (A/B contre le rendu d'origine)
bool g_tfrag_phong_subdiv = [] {
  const char* e = getenv("QA_NO_SUBDIV");
  return !(e && e[0] == '1');
}();
float g_tfrag_phong_strength = 0.35f;

namespace {
struct QuantPos {
  s32 x, y, z;
  bool operator==(const QuantPos& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct QuantPosHash {
  size_t operator()(const QuantPos& p) const {
    return ((size_t)(u32)p.x * 73856093u) ^ ((size_t)(u32)p.y * 19349663u) ^
           ((size_t)(u32)p.z * 83492791u);
  }
};
inline QuantPos quant(const PreloadedVertex& v) {
  // weld tolerance: 4 world units (~1mm) — packed verts on shared edges match
  return {(s32)std::lround(v.x * 0.25f), (s32)std::lround(v.y * 0.25f),
          (s32)std::lround(v.z * 0.25f)};
}
}  // namespace

void TfragTree::unpack() {
  unpacked.vertices.resize(packed_vertices.vertices.size());
  for (size_t i = 0; i < unpacked.vertices.size(); i++) {
    auto& o = unpacked.vertices[i];
    auto& in = packed_vertices.vertices[i];
    auto& cluster = packed_vertices.cluster_origins.at(in.cluster_idx);
    constexpr float kClusterSize = 4096 * 40;  // 100 in-game meters
    constexpr float kMasterOffset = 12000 * 4096;
    constexpr float rescale = kClusterSize / UINT16_MAX;
    float cx = -kMasterOffset + kClusterSize * cluster.x();
    float cy = -kMasterOffset + kClusterSize * cluster.y();
    float cz = -kMasterOffset + kClusterSize * cluster.z();
    o.x = cx + in.xoff * rescale;
    o.y = cy + in.yoff * rescale;
    o.z = cz + in.zoff * rescale;
    o.s = in.s / (1024.f);
    o.t = in.t / (1024.f);
    o.color_index = in.color_index;
    o.color_index2 = in.color_index;
  }

  for (auto& draw : draws) {
    draw.unpacked.idx_of_first_idx_in_full_buffer = unpacked.indices.size();
    for (auto& run : draw.runs) {
      for (u32 ri = 0; ri < run.length; ri++) {
        unpacked.indices.push_back(run.vertex0 + ri);
      }
      if (use_strips) {
        unpacked.indices.push_back(UINT32_MAX);
      }
    }
    unpacked.indices.insert(unpacked.indices.end(), draw.plain_indices.begin(),
                            draw.plain_indices.end());
  }

  // PC-REMAKE: tfrag subdivision is invoked by the Loader after unpack (kept
  // symmetric with TIE).
}

namespace {
// shared implementation for tfrag and tie trees (same vertex/draw formats).
// skip_draw: caller-provided predicate — draws it accepts keep their authored
// geometry (de-stripped only). Used to protect foliage lamellas, which Phong
// displacement tears apart (bugged palms report).
void phong_subdivide_impl(std::vector<PreloadedVertex>& verts,
                          std::vector<u32>& indices_ref,
                          std::vector<StripDraw>& draws,
                          const std::function<bool(const StripDraw&)>& skip_draw,
                          PackedTimeOfDay* colors,
                          float strength,
                          const std::vector<u8>* decor_classes = nullptr,
                          const std::vector<const PcSculptHeight*>* sculpt_maps = nullptr);

// PC-REMAKE : prototypes du decor deja remodeles (nom suffixe " [remake]" par l'extracteur) : ni
// subdivision ni affinage au chargement (pose par TieTree::subdivide_phong, par fil)
thread_local const std::vector<char>* t_proto_remplace = nullptr;
inline bool proto_remplace(const StripDraw::VisGroup& g) {
  return t_proto_remplace && g.tie_proto_idx < t_proto_remplace->size() &&
         (*t_proto_remplace)[g.tie_proto_idx];
}

bool tex_name_is_foliage(const std::string& n) {
  static const char* kWords[] = {"palm",  "leaf", "leaves", "frond", "branch", "treetop",
                                 "plant", "fern", "bush",   "vine",  "flower", "grass"};
  for (auto* w : kWords) {
    if (n.find(w) != std::string::npos) {
      return true;
    }
  }
  return false;
}
}  // namespace

void TfragTree::subdivide_phong(const std::vector<Texture>* textures, bool sculpture) {
  // PC-REMAKE 22/09 : TERRAIN = subdivision SANS deplacement (force 0). Le sol
  // est fait de COUCHES COPLANAIRES (sable + decals d'herbe/chemin en alpha, arbre
  // dirt) : deplacer les milieux le long des normales faisait se CROISER ces
  // couches -> pans plats a bords droits, clairs ou sombres, partout au sol (bugs
  // de textures et d'ombres au sol presents depuis le 16/08).
  // La subdivision reste pour les vraies normales lisses du G-buffer.
  // (original : g_tfrag_phong_strength)
  // PC-REMAKE : affinage du decor (bois, paille) sur le terrain normal
  std::vector<u8> classes;
  if (textures && kind == TFragmentTreeKind::NORMAL && !std::getenv("QA_NO_DECOR")) {
    classes.assign(draws.size(), 0);
    for (size_t i = 0; i < draws.size(); i++) {
      const auto& d = draws[i];
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)textures->size() && !d.mode.get_ab_enable()) {
        const int k = pc_decor_class_for_texture((*textures)[d.tree_tex_id].debug_name);
        classes[i] = (k == 1 || k == 3) ? (u8)k : 0;
      }
    }
  }
  // PC-REMAKE : SCULPTURE — dessins opaques dont la texture a une carte de hauteur : volume de la
  // geometrie (classe 4 si le dessin n'etait pas deja affine)
  std::vector<const PcSculptHeight*> maps;
  // PC-REMAKE : volume geometrique de la roche — ESSAYE puis coupe par defaut : a l'echelle d'un niveau
  // entier, le budget impose un pas de deux metres et rien ne se voit (le relief au pixel fait
  // l'essentiel) pour 0,7 s de chargement de plus au village. PC_SCULPT_GEO=1 le remet.
  if (sculpture && !classes.empty() && std::getenv("PC_SCULPT_GEO")) {
    maps.assign(draws.size(), nullptr);
    for (size_t i = 0; i < draws.size(); i++) {
      const auto& d = draws[i];
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)textures->size() && !d.mode.get_ab_enable() &&
          !(d.mode.get_at_enable() && d.mode.get_aref() > 8)) {
        maps[i] = pc_sculpt_height((*textures)[d.tree_tex_id].debug_name);
        // volume geometrique : la roche (falaises, rochers, murs de pierre) ; le reste a son relief
        // au pixel
        if (maps[i] && maps[i]->famille != 2) {
          maps[i] = nullptr;
        }
        if (maps[i] && classes[i] == 0) {
          classes[i] = 4;
        }
      }
    }
  }
  phong_subdivide_impl(unpacked.vertices, unpacked.indices, draws, nullptr, &colors, 0.f,
                       classes.empty() ? nullptr : &classes, maps.empty() ? nullptr : &maps);
  use_strips = false;
}

// Wind-animated TIE draws (palm crowns, banners) are rendered with the TREE's
// draw mode. De-stripping the static geometry flips that mode to GL_TRIANGLES,
// so their strip streams must be converted too — otherwise the crowns shatter
// into flying triangles (the "broken palms" bug).
static void convert_wind_strips_to_tris(TieTree& tree) {
  for (auto& d : tree.instanced_wind_draws) {
    std::vector<u32> out;
    out.reserve(d.vertex_index_stream.size() * 3);
    size_t pos = 0;
    u32 tris = 0;
    for (auto& g : d.instance_groups) {
      size_t begin = pos;
      size_t end = std::min(pos + g.num, d.vertex_index_stream.size());
      pos += g.num;
      size_t before = out.size();
      u32 a = UINT32_MAX, b = UINT32_MAX;
      bool parity = false;
      for (size_t i = begin; i < end; i++) {
        u32 c = d.vertex_index_stream[i];
        if (c == UINT32_MAX) {
          a = b = UINT32_MAX;
          parity = false;
          continue;
        }
        if (a != UINT32_MAX && b != UINT32_MAX && a != b && b != c && a != c) {
          if (parity) {
            out.push_back(b);
            out.push_back(a);
            out.push_back(c);
          } else {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
          }
          tris++;
        }
        if (a != UINT32_MAX || b != UINT32_MAX) {
          parity = !parity;
        }
        a = b;
        b = c;
      }
      g.num = (u32)(out.size() - before);
    }
    d.vertex_index_stream = std::move(out);
    d.num_triangles = tris;
  }
}

void TieTree::subdivide_phong(const std::vector<Texture>& textures, bool sculpture) {
  auto skip = [&](const StripDraw& d) {
    // cutout draws (alpha tested) OR any draw textured like foliage
    if (d.mode.get_at_enable() && d.mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
        d.mode.get_aref() > 8) {
      return true;
    }
    if (d.tree_tex_id >= 0 && d.tree_tex_id < (s32)textures.size() &&
        tex_name_is_foliage(textures[d.tree_tex_id].debug_name)) {
      return true;
    }
    return false;
  };
  // PC-REMAKE : affinage du decor (bois, paille) — prioritaire sur le classement feuillage par le
  // nom (un tronc de palmier est du bois), jamais sur une decoupe
  std::vector<u8> classes;
  if (!std::getenv("QA_NO_DECOR")) {
    classes.assign(static_draws.size(), 0);
    for (size_t i = 0; i < static_draws.size(); i++) {
      const auto& d = static_draws[i];
      const bool cutout = d.mode.get_at_enable() &&
                          d.mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
                          d.mode.get_aref() > 8;
      if (!cutout && d.tree_tex_id >= 0 && d.tree_tex_id < (s32)textures.size() &&
          !d.mode.get_ab_enable()) {
        const int k = pc_decor_class_for_texture(textures[d.tree_tex_id].debug_name);
        classes[i] = (k == 1 || k == 3) ? (u8)k : 0;
      }
    }
  }
  std::vector<const PcSculptHeight*> maps;
  // PC-REMAKE : volume geometrique de la roche — ESSAYE puis coupe par defaut : a l'echelle d'un niveau
  // entier, le budget impose un pas de deux metres et rien ne se voit (le relief au pixel fait
  // l'essentiel) pour 0,7 s de chargement de plus au village. PC_SCULPT_GEO=1 le remet.
  if (sculpture && !classes.empty() && std::getenv("PC_SCULPT_GEO")) {
    maps.assign(static_draws.size(), nullptr);
    for (size_t i = 0; i < static_draws.size(); i++) {
      const auto& d = static_draws[i];
      const bool cutout = d.mode.get_at_enable() &&
                          d.mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
                          d.mode.get_aref() > 8;
      if (!cutout && d.tree_tex_id >= 0 && d.tree_tex_id < (s32)textures.size() &&
          !d.mode.get_ab_enable()) {
        maps[i] = pc_sculpt_height(textures[d.tree_tex_id].debug_name);
        if (maps[i] && maps[i]->famille != 2) {
          maps[i] = nullptr;
        }
        if (maps[i] && classes[i] == 0) {
          classes[i] = 4;
        }
      }
    }
  }
  std::vector<char> remplace(proto_names.size(), 0);
  for (size_t i = 0; i < proto_names.size(); i++) {
    const auto& n = proto_names[i];
    remplace[i] = n.size() > 7 && n.compare(n.size() - 7, 7, " [remake]") == 0;
  }
  t_proto_remplace = &remplace;
  phong_subdivide_impl(unpacked.vertices, unpacked.indices, static_draws, skip, &colors,
                       g_tfrag_phong_strength, classes.empty() ? nullptr : &classes,
                       maps.empty() ? nullptr : &maps);
  t_proto_remplace = nullptr;
  convert_wind_strips_to_tris(*this);
  use_strips = false;
}

namespace {
void phong_subdivide_impl(std::vector<PreloadedVertex>& verts,
                          std::vector<u32>& indices_ref,
                          std::vector<StripDraw>& draws,
                          const std::function<bool(const StripDraw&)>& skip_draw,
                          PackedTimeOfDay* colors,
                          float strength,
                          const std::vector<u8>* decor_classes,
                          const std::vector<const PcSculptHeight*>* sculpt_maps) {
  const auto& old_indices = indices_ref;
  // PC-REMAKE 22/09 : COULEUR DES MILIEUX. Les sommets du decor portent un INDEX
  // dans la palette heure du jour ; un milieu d'arete prenait l'index de
  // l'extremite A -> a chaque triangle subdivise, une moitie prenait la couleur
  // de A, l'autre celle de B : les ombres/lumieres PEINTES par les artistes
  // (degrades par sommet) devenaient des pans plats a bords droits (bugs de
  // textures et d'ombres au sol presents depuis la subdivision).
  // Remede : le milieu porte les DEUX indices (color_index, color_index2) et le
  // shader moyenne les deux couleurs de palette (tfrag3/etie/tie_wind .vert).
  (void)colors;
  auto is_skipped = [&](const StripDraw& d) { return skip_draw && skip_draw(d); };

  // helper: iterate the triangles of an index range (strips + restarts),
  // preserving original winding via strip parity
  // 22/09 : parite corrigee — l'ancienne version emettait le PREMIER triangle
  // deja retourne (parite basculee des le 2e sommet), donc TOUS les triangles a
  // l'envers de la convention GL (k pair = (a,b,c), k impair = (b,a,c)). Sans
  // culling ca ne se voyait pas a l'affichage, mais les normales de la passe 1
  // (produits vectoriels) pointaient vers le BAS ; le post les retournait par
  // son garde-fou face a la camera sauf en incidence rasante -> plaques de
  // normales fausses au sol (vue QA : polygones bleu nuit).
  auto for_each_tri = [&](size_t begin, size_t count, auto&& fn) {
    size_t end = begin + count;
    u32 a = UINT32_MAX, b = UINT32_MAX;
    size_t n = 0;  // sommets vus depuis le dernier restart (degeneres compris)
    for (size_t i = begin; i < end; i++) {
      u32 c = old_indices[i];
      if (c == UINT32_MAX) {
        a = b = UINT32_MAX;
        n = 0;
        continue;
      }
      n++;
      if (n >= 3 && a != UINT32_MAX && b != UINT32_MAX && a != b && b != c && a != c) {
        if ((n - 3) & 1) {
          fn(b, a, c);
        } else {
          fn(a, b, c);
        }
      }
      a = b;
      b = c;
    }
  };

  // pass 1 (22/09, reecrite) : FACES par position soudee, puis normale lissee
  // PAR COIN avec ANGLE DE LISSAGE. L'ancienne somme aveugle de toutes les
  // faces d'une position melangeait falaise et sable, dessus et dessous : de
  // grands triangles du sol heritaient d'une normale penchee/inversee (pans
  // sombres de la plage, a toute heure). Ici un coin ne lisse qu'avec les faces
  // a moins de ~60 degres de la sienne (cos > 0.5) ; aux plis, le sommet est
  // DUPLIQUE (positions identiques -> pas de fissure, juste une arete vive,
  // comme dans tout outil de modelisation).
  std::unordered_map<QuantPos, std::vector<math::Vector3f>, QuantPosHash> faces_at;
  faces_at.reserve(verts.size());
  for (auto& draw : draws) {
    size_t base = draw.unpacked.idx_of_first_idx_in_full_buffer;
    size_t count = 0;
    for (auto& g : draw.vis_groups) {
      count += g.num_inds;
    }
    for_each_tri(base, count, [&](u32 i0, u32 i1, u32 i2) {
      const auto& p0 = verts[i0];
      const auto& p1 = verts[i1];
      const auto& p2 = verts[i2];
      math::Vector3f e1(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
      math::Vector3f e2(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
      math::Vector3f n = e1.cross(e2);  // area-weighted
      if (n.length() < 1e-6f) {
        return;
      }
      faces_at[quant(p0)].push_back(n);
      faces_at[quant(p1)].push_back(n);
      faces_at[quant(p2)].push_back(n);
    });
  }
  // normale d'un coin : faces de la position dont l'orientation est proche de
  // celle de la face courante (angle de lissage 60 deg)
  auto corner_normal = [&](const PreloadedVertex& v, const math::Vector3f& face_n) -> math::Vector3f {
    auto it = faces_at.find(quant(v));
    if (it == faces_at.end()) {
      return math::Vector3f(0.f, 0.f, 0.f);
    }
    float fl = face_n.length();
    if (fl < 1e-6f) {
      return math::Vector3f(0.f, 0.f, 0.f);
    }
    math::Vector3f fn = face_n / fl;
    math::Vector3f acc(0.f, 0.f, 0.f);
    for (const auto& f : it->second) {
      float l = f.length();
      if (l < 1e-6f) {
        continue;
      }
      if ((f / l).dot(fn) > 0.5f) {
        acc += f;
      }
    }
    float l = acc.length();
    return l > 1e-6f ? acc / l : fn;
  };
  // normale (moyenne de toutes les faces) d'une position : repli pour les
  // sommets qui ne passent par aucun triangle (jamais en pratique)
  auto normal_of = [&](const PreloadedVertex& v) -> math::Vector3f {
    auto it = faces_at.find(quant(v));
    if (it == faces_at.end()) {
      return math::Vector3f(0.f, 0.f, 0.f);
    }
    math::Vector3f acc(0.f, 0.f, 0.f);
    for (const auto& f : it->second) {
      acc += f;
    }
    float l = acc.length();
    return l > 1e-6f ? acc / l : math::Vector3f(0.f, 0.f, 0.f);
  };
  auto unpack_unit = [](u32 packed) -> math::Vector3f {
    auto sx = [](u32 v) -> float {
      s32 i = (s32)(v & 0x3ff);
      if (i & 0x200) {
        i -= 0x400;
      }
      return (float)i / 511.f;
    };
    math::Vector3f n(sx(packed), sx(packed >> 10), sx(packed >> 20));
    float l = n.length();
    return l > 1e-6f ? n / l : math::Vector3f(0.f, 0.f, 0.f);
  };
  // PC-REMAKE 21/09 : les normales lissees sont STOCKEES dans les sommets
  // (`nor`, 10 bits signes par axe, attribut 3 des shaders du decor). Avant,
  // elles ne servaient qu'a courber les milieux d'aretes : le terrain arrivait
  // au GPU avec nor = 0 et tout l'eclairage moderne devait reconstruire une
  // normale depuis la profondeur (facettes, lignes de niveau au loin). Les
  // sommets des draws feuillage (skip) recoivent une normale VERTICALE : des
  // lamelles fines n'ont pas de normale de surface qui ait un sens, et le
  // vers le haut eclaire un feuillage comme une canopee. On ne touche pas
  // aux normales deja presentes (TIE envmap : les vraies normales des artistes).
  auto pack_unit = [](const math::Vector3f& n) -> u32 {
    return pack_to_gl_normal((s16)std::lround(n.x() * 511.f), (s16)std::lround(n.y() * 511.f),
                             (s16)std::lround(n.z() * 511.f));
  };
  std::vector<u8> nor_fixed;
  {
    std::vector<u8> foliage_vert(verts.size(), 0);
    for (auto& draw : draws) {
      if (!is_skipped(draw)) {
        continue;
      }
      size_t base = draw.unpacked.idx_of_first_idx_in_full_buffer;
      size_t count = 0;
      for (auto& g : draw.vis_groups) {
        count += g.num_inds;
      }
      for (size_t i = base; i < base + count && i < old_indices.size(); i++) {
        u32 vi = old_indices[i];
        if (vi != UINT32_MAX && vi < foliage_vert.size()) {
          foliage_vert[vi] = 1;
        }
      }
    }
    const u32 up = pack_unit(math::Vector3f(0.f, 1.f, 0.f));
    // normales deja presentes (TIE envmap : celles des artistes) : figees
    nor_fixed.assign(verts.size(), 0);
    for (size_t i = 0; i < verts.size(); i++) {
      auto& v = verts[i];
      if (v.nor != 0) {
        nor_fixed[i] = 1;
        continue;
      }
      if (foliage_vert[i]) {
        v.nor = up;
        nor_fixed[i] = 1;
        continue;
      }
      // repli (sommets hors triangles) : moyenne de toutes les faces ; les
      // triangles reassignent leurs coins ci-dessous
      math::Vector3f n = normal_of(v);
      if (n.length() > 0.5f) {
        v.nor = pack_unit(n);
      }
    }
  }
  // coin d'un triangle : le sommet `ic` avec la normale lissee `nc` — le sommet
  // lui-meme si sa normale est libre ou identique, sinon un DUPLIQUE (par
  // (sommet, normale)) ; les duplicats partagent position/uv/couleurs
  std::vector<u8> corner_assigned(verts.size(), 0);
  std::unordered_map<u64, u32> corner_dup;
  auto corner_vertex = [&](u32 ic, const math::Vector3f& nc) -> u32 {
    if (ic < nor_fixed.size() && nor_fixed[ic]) {
      return ic;
    }
    if (nc.length() < 0.5f) {
      return ic;
    }
    u32 packed = pack_unit(nc);
    if (ic < corner_assigned.size() && !corner_assigned[ic]) {
      corner_assigned[ic] = 1;
      verts[ic].nor = packed;
      return ic;
    }
    if (ic < verts.size() && verts[ic].nor == packed) {
      return ic;
    }
    u64 key = ((u64)ic << 32) | packed;
    auto it = corner_dup.find(key);
    if (it != corner_dup.end()) {
      return it->second;
    }
    PreloadedVertex d = verts[ic];
    d.nor = packed;
    u32 ni = (u32)verts.size();
    verts.push_back(d);
    corner_dup.emplace(key, ni);
    return ni;
  };

  // pass 2: rebuild indices, splitting each triangle into four with Phong
  // midpoints (deduped per welded edge so neighbors stay crack-free)
  std::unordered_map<u64, u32> edge_mid;
  edge_mid.reserve(old_indices.size());
  auto midpoint = [&](u32 ia, u32 ib) -> u32 {
    u64 key = ia < ib ? ((u64)ia << 32) | ib : ((u64)ib << 32) | ia;
    auto it = edge_mid.find(key);
    if (it != edge_mid.end()) {
      return it->second;
    }
    const auto va = verts[ia];
    const auto vb = verts[ib];
    PreloadedVertex m;
    m.x = 0.5f * (va.x + vb.x);
    m.y = 0.5f * (va.y + vb.y);
    m.z = 0.5f * (va.z + vb.z);
    m.s = 0.5f * (va.s + vb.s);
    m.t = 0.5f * (va.t + vb.t);
    m.color_index = va.color_index;
    m.color_index2 = vb.color_index;
    // 13/10 (POINTS sur les objets precurseurs) : la teinte de
    // REFLET (r g b a = force de l'environnement, objets precurseurs) restait a 0
    // sur les milieux -> le reflet ne brillait qu'aux sommets d'origine : une
    // grille de points et de losanges. Moyenne des deux extremites.
    m.r = (u8)(((u32)va.r + vb.r + 1) / 2);
    m.g = (u8)(((u32)va.g + vb.g + 1) / 2);
    m.b = (u8)(((u32)va.b + vb.b + 1) / 2);
    m.a = (u8)(((u32)va.a + vb.a + 1) / 2);
    // Phong: project the flat midpoint onto both endpoint tangent planes and
    // pull toward their average — rounds silhouettes without shrinking flats
    // (22/09 : normales des COINS, deja lissees avec angle)
    math::Vector3f na = unpack_unit(va.nor);
    math::Vector3f nb = unpack_unit(vb.nor);
    // 17/10 (FISSURES dans les rochers) : le deplacement se calcule avec la normale
    // SOUDEE de chaque position (moyenne de toutes ses faces), pas avec celle du coin.
    // Aux aretes vives, les deux cotes d'une arete portent des sommets DUPLIQUES
    // (meme position, normales differentes) : chacun bombait son milieu dans sa
    // propre direction -> deux milieux a deux endroits, un jour entre les faces ou
    // l'on voyait le fond. Avec la normale soudee, les deux milieux coincident ; la
    // normale d'eclairage du milieu reste celle du coin (arete vive a l'ombrage).
    // (original : deplacement avec na / nb ci-dessus)
    // QA_FISSURES_AVANT=1 : ancien calcul (A/B de controle)
    static const bool qa_old_crease = getenv("QA_FISSURES_AVANT") != nullptr;
    const math::Vector3f wa = qa_old_crease ? na : normal_of(va);
    const math::Vector3f wb = qa_old_crease ? nb : normal_of(vb);
    if (wa.length() > 0.5f && wb.length() > 0.5f) {
      math::Vector3f mp(m.x, m.y, m.z);
      math::Vector3f pa(va.x, va.y, va.z);
      math::Vector3f pb(vb.x, vb.y, vb.z);
      math::Vector3f da = wa * (wa.dot(pa - mp));
      math::Vector3f db = wb * (wb.dot(pb - mp));
      math::Vector3f disp = (da + db) * 0.5f * strength;
      // safety clamp: never move a midpoint more than a third of the edge
      float edge_len = (pb - pa).length();
      float dl = disp.length();
      float max_d = edge_len * 0.33f;
      if (dl > max_d && dl > 1e-6f) {
        disp = disp * (max_d / dl);
      }
      m.x += disp.x();
      m.y += disp.y();
      m.z += disp.z();
    }
    // PC-REMAKE 21/09 : normale du milieu = moyenne des deux extremites
    {
      math::Vector3f nm = na + nb;
      float l = nm.length();
      if (l > 1e-4f) {
        nm = nm / l;
        m.nor = pack_unit(nm);
      } else if (va.nor != 0) {
        m.nor = va.nor;
      } else {
        m.nor = vb.nor;
      }
    }
    u32 idx = (u32)verts.size();
    verts.push_back(m);
    edge_mid.emplace(key, idx);
    return idx;
  };

  std::vector<u32> new_indices;
  new_indices.reserve(old_indices.size() * 6);
  for (size_t draw_i = 0; draw_i < draws.size(); draw_i++) {
    auto& draw = draws[draw_i];
    size_t base = draw.unpacked.idx_of_first_idx_in_full_buffer;
    size_t group_start = base;
    u32 new_first = (u32)new_indices.size();
    // PC-REMAKE : dessin de bois / paille (decor affine) : triangle d'origine garde, coins lisses ;
    // l'affinage (pc_refine_decor_impl) le redecoupe ensuite
    const bool decor = decor_classes && draw_i < decor_classes->size() && (*decor_classes)[draw_i];
    const bool skip = !decor && is_skipped(draw);
    for (auto& g : draw.vis_groups) {
      size_t before = new_indices.size();
      u32 tris = 0;
      const bool skip_g = skip || proto_remplace(g);
      for_each_tri(group_start, g.num_inds, [&](u32 i0, u32 i1, u32 i2) {
        if (skip_g) {
          // foliage cutouts: de-strip only (whole tree becomes GL_TRIANGLES),
          // geometry untouched so lamellas keep their authored shape
          const u32 out[3] = {i0, i1, i2};
          new_indices.insert(new_indices.end(), out, out + 3);
          tris += 1;
          return;
        }
        // 22/09 : coins avec normale lissee par angle (duplicats aux plis)
        static const bool qa_no_corner = [] {
          const char* e = getenv("QA_NO_CORNER");
          return e && e[0] == '1';
        }();
        if (!qa_no_corner) {
          const auto& p0 = verts[i0];
          const auto& p1 = verts[i1];
          const auto& p2 = verts[i2];
          math::Vector3f e1(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
          math::Vector3f e2(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
          math::Vector3f fn = e1.cross(e2);
          math::Vector3f n0 = corner_normal(p0, fn), n1 = corner_normal(p1, fn), n2 = corner_normal(p2, fn);
          i0 = corner_vertex(i0, n0);
          i1 = corner_vertex(i1, n1);
          i2 = corner_vertex(i2, n2);
        }
        if (decor) {
          const u32 out[3] = {i0, i1, i2};
          new_indices.insert(new_indices.end(), out, out + 3);
          tris += 1;
          return;
        }
        u32 m01 = midpoint(i0, i1);
        u32 m12 = midpoint(i1, i2);
        u32 m20 = midpoint(i2, i0);
        const u32 out[12] = {i0, m01, m20, m01, i1, m12, m20, m12, i2, m01, m12, m20};
        new_indices.insert(new_indices.end(), out, out + 12);
        tris += 4;
      });
      group_start += g.num_inds;
      g.num_inds = (u32)(new_indices.size() - before);
      g.num_tris = tris;
    }
    draw.unpacked.idx_of_first_idx_in_full_buffer = new_first;
  }

  indices_ref = std::move(new_indices);
  // PC-REMAKE : DECOR AFFINE — normales soudees puis redecoupage des dessins de bois et de paille. Aux
  // aretes partagees avec un dessin subdivise, on reprend EXACTEMENT son milieu (midpoint) : rien
  // ne s'ouvre entre les deux.
  if (decor_classes) {
    pc_prepare_decor_weld(verts, indices_ref, draws);
    pc_refine_decor_impl(verts, indices_ref, draws, *decor_classes,
                         [&](u32 a, u32 b) { return midpoint(a, b); }, sculpt_maps);
  }
}
}  // namespace

void TieTree::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(static_draws.size());
  } else {
    static_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : static_draws) {
    draw.serialize(ser);
  }

  ser.from_ptr(&category_draw_indices);

  if (ser.is_saving()) {
    ser.save<size_t>(instanced_wind_draws.size());
  } else {
    instanced_wind_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : instanced_wind_draws) {
    draw.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(wind_instance_info.size());
  } else {
    wind_instance_info.resize(ser.load<size_t>());
  }
  for (auto& inst : wind_instance_info) {
    inst.serialize(ser);
  }

  packed_vertices.serialize(ser);
  colors.serialize(ser);
  bvh.serialize(ser);

  ser.from_ptr(&use_strips);

  ser.from_ptr(&has_per_proto_visibility_toggle);
  ser.from_string_vector(&proto_names);
}

void ShrubTree::serialize(Serializer& ser) {
  time_of_day_colors.serialize(ser);
  ser.from_pod_vector(&indices);
  packed_vertices.serialize(ser);
  if (ser.is_saving()) {
    ser.save<size_t>(static_draws.size());
  } else {
    static_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : static_draws) {
    draw.serialize(ser);
  }

  ser.from_ptr(&has_per_proto_visibility_toggle);
  ser.from_string_vector(&proto_names);
}

void HfragmentBucket::serialize(Serializer& ser) {
  ser.from_pod_vector(&corners);
  ser.from_ptr(&montage_table);
}

void Hfragment::serialize(Serializer& ser) {
  ser.from_pod_vector(&vertices);
  ser.from_pod_vector(&indices);
  ser.from_pod_vector(&corners);

  if (ser.is_saving()) {
    ser.save<size_t>(buckets.size());
  } else {
    buckets.resize(ser.load<size_t>());
  }
  for (auto& x : buckets) {
    x.serialize(ser);
  }
  time_of_day_colors.serialize(ser);
  ser.from_ptr(&wang_tree_tex_id);
  ser.from_ptr(&draw_mode);
  ser.from_ptr(&occlusion_offset);
}

void BVH::serialize(Serializer& ser) {
  ser.from_ptr(&first_leaf_node);
  ser.from_ptr(&last_leaf_node);
  ser.from_ptr(&first_root);
  ser.from_ptr(&num_roots);
  ser.from_ptr(&only_children);
  ser.from_pod_vector(&vis_nodes);
}

void Texture::serialize(Serializer& ser) {
  ser.from_ptr(&w);
  ser.from_ptr(&h);
  ser.from_ptr(&combo_id);
  ser.from_pod_vector(&data);
  ser.from_str(&debug_name);
  ser.from_str(&debug_tpage_name);
  ser.from_ptr(&load_to_pool);
}

void IndexTexture::serialize(Serializer& ser) {
  ser.from_ptr(&w);
  ser.from_ptr(&h);
  ser.from_ptr(&combo_id);
  ser.from_pod_vector(&index_data);
  ser.from_ptr(&color_table);
  ser.from_str(&name);
  ser.from_str(&tpage_name);
  ser.from_string_vector(&level_names);
}

void CollisionMesh::serialize(Serializer& ser) {
  ser.from_pod_vector(&vertices);
}

void MercDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_ptr(&eye_id);
  ser.from_ptr(&first_index);
  ser.from_ptr(&index_count);
  ser.from_ptr(&num_triangles);
  ser.from_ptr(&no_strip);
}

void Blerc::serialize(Serializer& ser) {
  ser.from_pod_vector(&float_data);
  ser.from_pod_vector(&int_data);
}

void MercModifiableDrawGroup::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(mod_draw.size());
  } else {
    mod_draw.resize(ser.load<size_t>());
  }
  for (auto& draw : mod_draw) {
    draw.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(fix_draw.size());
  } else {
    fix_draw.resize(ser.load<size_t>());
  }
  for (auto& draw : fix_draw) {
    draw.serialize(ser);
  }
  ser.from_pod_vector(&vertices);
  ser.from_pod_vector(&vertex_lump4_addr);
  ser.from_pod_vector(&fragment_mask);
  ser.from_ptr(&expect_vidx_end);

  blerc.serialize(ser);
}

void MercEffect::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(all_draws.size());
  } else {
    all_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : all_draws) {
    draw.serialize(ser);
  }

  mod.serialize(ser);

  ser.from_ptr(&envmap_mode);
  ser.from_ptr(&envmap_texture);
  ser.from_ptr(&has_envmap);
  ser.from_ptr(&has_mod_draw);
}

void MercModel::serialize(Serializer& ser) {
  ser.from_str(&name);
  if (ser.is_saving()) {
    ser.save<size_t>(effects.size());
  } else {
    effects.resize(ser.load<size_t>());
  }
  for (auto& effect : effects) {
    effect.serialize(ser);
  }
  ser.from_ptr(&max_draws);
  ser.from_ptr(&max_bones);
  ser.from_ptr(&st_vif_add);
  ser.from_ptr(&xyz_scale);
  ser.from_ptr(&st_magic);
}

void MercModelGroup::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(models.size());
  } else {
    models.resize(ser.load<size_t>());
  }
  for (auto& model : models) {
    model.serialize(ser);
  }

  ser.from_pod_vector(&indices);
  ser.from_pod_vector(&vertices);

  // PC-REMAKE : objets remodeles = remplaces par un glTF (tous les dessins hors yeux en liste des
  // l'extraction). Releve AVANT le decoupage de chargement, qui met aussi les autres en liste.
  if (!ser.is_saving()) {
    for (auto& model : models) {
      bool liste = false, strip = false;
      for (const auto& effect : model.effects) {
        for (const auto& draw : effect.all_draws) {
          if (draw.eye_id != 0xff) {
            continue;
          }
          (draw.no_strip ? liste : strip) = true;
        }
      }
      model.pc_remodele = liste && !strip;
    }
  }

  // PC-REMAKE: round actor/prop models right after the level file is read
  // (game side only — the extractor never deserializes fr3s).
  if (!ser.is_saving() && g_tfrag_phong_subdiv) {
    subdivide_phong();
    replace_demo_models();
  }
  if (!ser.is_saving()) {
    pc_prepare_characters();
  }
}

// ---------------------------------------------------------------------------
// PC-REMAKE 21/10 : PERSONNAGES REMODELES
// Groupes d'animation des etres vivants du jeu : Jak et ses variantes, Daxter, les PNJ, les
// animaux et tous les ennemis et boss. Le nom d'un modele est <groupe>-lod0, <groupe>-lod1,
// <groupe>-<variante>-lod0... : un groupe correspond s'il est suivi d'un tiret.
namespace {
const char* const kPcCharacterGroups[] = {
    // Jak et Daxter
    "eichar", "sidekick", "jak-white", "billy-sidekick",
    // PNJ
    "assistant", "bird-lady", "bluesage", "redsage", "yellowsage", "sage", "evilbro", "evilsis",
    "explorer", "farmer", "fisher", "gambler", "geologist", "mayor", "minershort", "minertall",
    "sculptor", "warrior", "billy", "oracle", "muse", "farthy-snack",
    // animaux
    "yakow", "flutflut", "pelican", "seagull", "sharkey", "junglefish", "villa-starfish",
    // ennemis et boss
    "aphid-lurker", "babak", "baby-spider", "mother-spider", "balloonlurker", "bonelurker",
    "bully", "double-lurker", "driller-lurker", "flying-lurker", "gnawer", "green-eco-lurker",
    "hopper", "junglesnake", "kermit", "lightning-mole", "lurkercrab", "lurkerpuppy",
    "lurkerworm", "plunger-lurker", "puffer", "quicksandlurker", "ram", "ram-boss", "snow-bunny",
    "swamp-bat", "swamp-rat", "yeti", "ice-cube", "plant-boss", "ogreboss", "robotboss", "robber",
    "citb-bunny", "evilplant", "happy-plant", "dark-plant",
};
}  // namespace

// PC-REMAKE : objets ranges sous le nom d'un personnage (plates-formes du flutflut, rochers de l'ogre,
// nids de rats, cage brisee) : ils sont remodeles et rendus comme les autres objets
const char* const kPcObjetsDePersonnages[] = {
    "flutflut-plat-", "ogreboss-bounce-boulder-", "ogreboss-column-", "ogreboss-shoot-boulder-",
    "ogreboss-super-boulder-", "swamp-rat-nest-", "bully-broken-cage-",
};

bool pc_is_character_model(const std::string& name) {
  for (const char* o : kPcObjetsDePersonnages) {
    if (name.compare(0, strlen(o), o) == 0) {
      return false;
    }
  }
  for (const char* g : kPcCharacterGroups) {
    const size_t n = strlen(g);
    if (name.size() > n && name.compare(0, n, g) == 0 && name[n] == '-') {
      return true;
    }
  }
  return false;
}

u32 pc_pack_weld_normal(float x, float y, float z, bool sharp) {
  const float l = std::abs(x) + std::abs(y) + std::abs(z);
  if (l < 1e-6f) {
    return 0;
  }
  float px = x / l, py = y / l;
  if (z < 0.f) {
    const float ox = (1.f - std::abs(py)) * (px >= 0.f ? 1.f : -1.f);
    const float oy = (1.f - std::abs(px)) * (py >= 0.f ? 1.f : -1.f);
    px = ox;
    py = oy;
  }
  auto q = [](float v) -> u32 {
    return (u32)std::clamp((int)std::lround((v * 0.5f + 0.5f) * 32767.f), 0, 32767);
  };
  return 1u | (sharp ? 2u : 0u) | (q(px) << 2) | (q(py) << 17);
}

// ---- PC-REMAKE : NORMALES SOUDEES DU DECOR (decoupage tfrag3_tess) ----------------------------
// Pour que la surface courbe (triangles PN) ne s'ouvre jamais, tous les sommets a une meme
// position doivent utiliser la MEME normale de forme. Regles :
//  - une seule famille de normales a cette position (surface lisse) : leur moyenne ;
//  - plusieurs familles (arete vive : rebord d'un rondin, coin d'une caisse) : on garde la
//    famille qui se COURBE le long de la surface (la face laterale d'un rondin, dont la normale
//    tourne d'un sommet voisin a l'autre) — le bord du rondin s'arrondit et son disque reste
//    plat. Si aucune famille ne se courbe (coin d'une boite), l'arete reste droite (bit 1).
//  - bord (bit 2) : position partagee par plusieurs dessins (deux textures) : l'arete y reste
//    droite et reprend le milieu du dessin voisin (pc_refine_decor_impl), sans fissure.
namespace {
struct DecorKey {
  int32_t x, y, z;
  bool operator==(const DecorKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct DecorKeyHash {
  size_t operator()(const DecorKey& k) const {
    return (size_t)k.x * 73856093u ^ (size_t)k.y * 19349663u ^ (size_t)k.z * 83492791u;
  }
};
math::Vector3f decor_unpack_nor(u32 packed) {
  auto sx = [](u32 v) -> float {
    int i = (int)(v & 0x3ff);
    if (i & 0x200) {
      i -= 0x400;
    }
    return (float)i / 511.f;
  };
  math::Vector3f n(sx(packed), sx(packed >> 10), sx(packed >> 20));
  const float l = n.length();
  return l > 1e-6f ? n / l : math::Vector3f(0.f, 0.f, 0.f);
}
u32 decor_pack(const math::Vector3f& n, bool vif, bool border) {
  const float l = std::abs(n.x()) + std::abs(n.y()) + std::abs(n.z());
  if (l < 1e-6f) {
    return 0;
  }
  float px = n.x() / l, py = n.y() / l;
  if (n.z() < 0.f) {
    const float ox = (1.f - std::abs(py)) * (px >= 0.f ? 1.f : -1.f);
    const float oy = (1.f - std::abs(px)) * (py >= 0.f ? 1.f : -1.f);
    px = ox;
    py = oy;
  }
  auto q = [](float v) -> u32 {
    return (u32)std::clamp((int)std::lround((v * 0.5f + 0.5f) * 16383.f), 0, 16383);
  };
  return 1u | (vif ? 2u : 0u) | (border ? 4u : 0u) | (q(px) << 3) | (q(py) << 17);
}
}  // namespace

void pc_prepare_decor_weld(std::vector<PreloadedVertex>& verts,
                           const std::vector<u32>& indices,
                           const std::vector<StripDraw>& draws) {
  const size_t nv = verts.size();
  if (nv == 0) {
    return;
  }
  auto key_of = [](const PreloadedVertex& v) {
    return DecorKey{(int32_t)std::lround(v.x * 0.25f), (int32_t)std::lround(v.y * 0.25f),
                    (int32_t)std::lround(v.z * 0.25f)};
  };
  std::vector<u32> pos_id(nv);
  std::unordered_map<DecorKey, u32, DecorKeyHash> ids;
  ids.reserve(nv);
  for (size_t i = 0; i < nv; i++) {
    auto it = ids.emplace(key_of(verts[i]), (u32)ids.size()).first;
    pos_id[i] = it->second;
  }
  const size_t np = ids.size();
  std::vector<math::Vector3f> nrm(nv);
  for (size_t i = 0; i < nv; i++) {
    nrm[i] = decor_unpack_nor(verts[i].nor);
  }
  // courbure de chaque sommet dans sa famille (angle max avec ses voisins de triangle)
  std::vector<float> curv(nv, 0.f);
  std::vector<int32_t> pos_draw(np, -1);
  std::vector<u8> pos_border(np, 0);
  for (size_t di = 0; di < draws.size(); di++) {
    const auto& d = draws[di];
    size_t base = d.unpacked.idx_of_first_idx_in_full_buffer;
    size_t count = 0;
    for (auto& g : d.vis_groups) {
      count += g.num_inds;
    }
    for (size_t i = base; i + 2 < base + count && i + 2 < indices.size(); i += 3) {
      const u32 t[3] = {indices[i], indices[i + 1], indices[i + 2]};
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) {
        continue;
      }
      for (int k = 0; k < 3; k++) {
        const u32 a = t[k], b = t[(k + 1) % 3];
        const float c = nrm[a].dot(nrm[b]);
        const float ang = std::acos(std::clamp(c, -1.f, 1.f));
        curv[a] = std::max(curv[a], ang);
        curv[b] = std::max(curv[b], ang);
        const u32 pa = pos_id[a];
        if (pos_draw[pa] == -1) {
          pos_draw[pa] = (int32_t)di;
        } else if (pos_draw[pa] != (int32_t)di) {
          pos_border[pa] = 1;
        }
      }
    }
  }
  // par position : somme des normales, meilleure famille courbe, ecart des familles
  std::vector<math::Vector3f> sum(np, math::Vector3f(0.f, 0.f, 0.f));
  std::vector<int64_t> best(np, -1);
  std::vector<float> best_curv(np, -1.f);
  std::vector<math::Vector3f> first(np, math::Vector3f(0.f, 0.f, 0.f));
  std::vector<u8> spread(np, 0);
  std::vector<float> mindot(np, 1.f);  // plus grand ecart entre familles (cosinus)
  const float cos_same = std::cos(20.f * 3.14159265f / 180.f);
  // pans d'un objet rond dessine en huit faces (45 degres) : surface lisse, pas un coin de boite
  const float cos_pans = std::cos(50.f * 3.14159265f / 180.f);
  for (size_t i = 0; i < nv; i++) {
    const u32 p = pos_id[i];
    if (nrm[i].length() < 0.5f) {
      continue;
    }
    sum[p] += nrm[i];
    if (first[p].length() < 0.5f) {
      first[p] = nrm[i];
    } else if (first[p].dot(nrm[i]) < cos_same) {
      spread[p] = 1;
    }
    if (first[p].length() > 0.5f) {
      mindot[p] = std::min(mindot[p], first[p].dot(nrm[i]));
    }
    if (curv[i] > best_curv[p]) {
      best_curv[p] = curv[i];
      best[p] = (int64_t)i;
    }
  }
  const float flat = 4.f * 3.14159265f / 180.f;
  size_t n_vif = 0, n_border = 0;
  for (size_t i = 0; i < nv; i++) {
    const u32 p = pos_id[i];
    math::Vector3f g = sum[p];
    bool vif = false;
    if (spread[p]) {
      if (mindot[p] > cos_pans) {
        // faces planes d'un objet a pans (cerclage octogonal) : moyenne, l'arete s'arrondit
      } else if (best_curv[p] < flat || best[p] < 0) {
        vif = true;
      } else {
        g = nrm[(size_t)best[p]];
      }
    }
    const float l = g.length();
    if (l < 1e-5f) {
      verts[i].weld = 0;
      continue;
    }
    g /= l;
    verts[i].weld = decor_pack(g, vif, pos_border[p] != 0);
    n_vif += vif;
    n_border += pos_border[p] != 0;
  }
  lg::debug("PC-REMAKE decor decoupe : {} sommets ({} aretes vives, {} bords)", nv, n_vif,
            n_border);
}

int pc_decor_class_for_texture(const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), ::tolower);
  // cerclages et anneaux (pied des poteaux de cabane, tonneaux) : des cercles dessines en huit
  // pans ; le decoupage par courbure les arrondit, une plaque plate n'en est pas changee
  if (n.find("precursor") == std::string::npos) {
    static const char* kCercle[] = {"band", "hoop", "ring", "collar", "brass"};
    for (auto* k : kCercle) {
      if (n.find(k) != std::string::npos) {
        return 1;
      }
    }
  }
  static const char* kNon[] = {"metal", "precursor", "glass", "lrkr", "crystal", "grate",
                               "sign",  "paint",     "glow",  "light", "lamp",   "flag",
                               "cloth", "canvas",    "leaf",  "grass", "fringe", "hang",
                               "water", "lava",      "eco",   "sky",   "cloud",  "black"};
  for (auto* k : kNon) {
    if (n.find(k) != std::string::npos) {
      return 0;
    }
  }
  static const char* kPaille[] = {"straw", "thatch", "hay"};
  for (auto* k : kPaille) {
    if (n.find(k) != std::string::npos) {
      return 3;
    }
  }
  static const char* kRoche[] = {"rock", "stone", "cliff", "boulder", "sculpt", "cavewall",
                                 "cave-wall", "rubble", "crag", "pillar-rock"};
  for (auto* k : kRoche) {
    if (n.find(k) != std::string::npos) {
      return 2;
    }
  }
  static const char* kBois[] = {"wood", "plank", "log", "trunk", "bark", "post", "pole",
                                "beam", "branch", "root", "bamboo", "stick", "rope", "timber",
                                "bench", "fence", "palmbase", "bedpost", "pier", "dock",
                                "stump", "twig", "vine"};
  for (auto* k : kBois) {
    if (n.find(k) != std::string::npos) {
      return 1;
    }
  }
  return 0;
}

// ---- PC-REMAKE : AFFINAGE DU DECOR AU CHARGEMENT -------------------------------------------------
// Le decor est fixe : on le redecoupe une fois (le decoupage par la carte graphique, emule sur Mac,
// coutait 34 images par seconde a lui seul). Dessins de bois, de cordes et de paille seulement (la
// roche garde ses facettes d'artiste ; son relief reste dans l'eclairage, tfrag3.frag). Appele
// par phong_subdivide_impl sur les triangles D'ORIGINE de ces dessins (couleurs peintes exactes).
//  - chaque arete est coupee selon sa COURBURE (angle entre les normales soudees de ses deux
//    bouts, un morceau tous les ~12 degres) : une longue arete droite (le long d'un rondin) ne se
//    decoupe pas, le tour d'un rondin a huit pans s'arrondit. Ne depend que de l'arete : les deux
//    triangles qui la partagent la coupent pareil ;
//  - surface courbe PN tiree de la normale SOUDEE (pc_prepare_decor_weld) ; aretes vives droites ;
//  - arete partagee avec un autre dessin : droite, avec EXACTEMENT le milieu que ce dessin calcule
//    (phong_mid) : aucune fente entre une piece affinee et ses voisines ;
//  - les points d'une arete moins decoupee que l'interieur sont poses sur sa ligne brisee ;
//  - couleur peinte : melange des trois coins (PreloadedVertex::color_index3, cw0, cw1).
// (v1 : apres la subdivision, decoupage selon la longueur + relief geometrique — 172 millions de
//  triangles au village, plaques de couleur sur les bouts de rondins)
namespace {
struct DecorCorner {
  math::Vector3f p, n, g;  // position, normale d'eclairage, normale soudee
  bool vif, border;
  float s, t;
  u16 col;
};
math::Vector3f decor_unpack_weld(u32 w, bool* vif, bool* border) {
  *vif = (w & 2u) != 0;
  *border = (w & 4u) != 0;
  if ((w & 1u) == 0) {
    *vif = true;
    return math::Vector3f(0.f, 0.f, 0.f);
  }
  float fx = (float)((w >> 3) & 0x3fff) / 16383.f * 2.f - 1.f;
  float fy = (float)((w >> 17) & 0x3fff) / 16383.f * 2.f - 1.f;
  math::Vector3f n(fx, fy, 1.f - std::abs(fx) - std::abs(fy));
  if (n.z() < 0.f) {
    const float ox = (1.f - std::abs(n.y())) * (n.x() >= 0.f ? 1.f : -1.f);
    const float oy = (1.f - std::abs(n.x())) * (n.y() >= 0.f ? 1.f : -1.f);
    n.x() = ox;
    n.y() = oy;
  }
  return n.normalized();
}
u32 decor_pack_nor(const math::Vector3f& n) {
  auto q = [](float v) -> u32 {
    int i = (int)std::lround(std::clamp(v, -1.f, 1.f) * 511.f);
    return (u32)i & 0x3ff;
  };
  return q(n.x()) | (q(n.y()) << 10) | (q(n.z()) << 20);
}
math::Vector3f decor_ctrl(const DecorCorner& i, const DecorCorner& j, bool straight) {
  math::Vector3f r = i.p * 2.f + j.p;
  if (!straight) {
    r -= i.g * (j.p - i.p).dot(i.g);
  }
  return r / 3.f;
}

// PC-REMAKE : SCULPTURE — decoupage d'un triangle d'origine avec un niveau PAR ARETE (ne[e]) et un
// niveau interieur N, raccordes par une couronne en fermeture eclair : aucune jointure en T, donc
// aucune fente entre deux triangles qui partagent une arete. Points en barycentriques (u, v, w)
// des coins c0, c1, c2 ; aretes 0 = c0->c1, 1 = c1->c2, 2 = c2->c0.
struct PatchPoint {
  float u, v, w;
  int arete;  // -1 interieur ou coin ; 0..2 point d'arete
  int k;      // rang sur l'arete (0..ne)
};
void sculpt_patch_topology(int N,
                           const int ne[3],
                           std::vector<PatchPoint>& pts,
                           std::vector<std::array<int, 3>>& tris) {
  pts.clear();
  tris.clear();
  // points d'arete (coins compris, une seule fois chacun)
  int edge_first[3];
  int corner[3];
  for (int e = 0; e < 3; e++) {
    edge_first[e] = (int)pts.size();
    for (int k = 0; k < ne[e]; k++) {
      const float t = (float)k / ne[e];
      PatchPoint p;
      if (e == 0) {
        p = {1.f - t, t, 0.f, k == 0 ? -1 : 0, k};
      } else if (e == 1) {
        p = {0.f, 1.f - t, t, k == 0 ? -1 : 1, k};
      } else {
        p = {t, 0.f, 1.f - t, k == 0 ? -1 : 2, k};
      }
      pts.push_back(p);
    }
    corner[e] = edge_first[e];  // coin de depart de l'arete e : c0, c1, c2
  }
  auto outer = [&](int e, int k) {
    if (k == ne[e]) {
      return corner[(e + 1) % 3];
    }
    return edge_first[e] + k;
  };
  if (N < 3) {
    // pas d'interieur : eventail depuis le premier point (ne vaut 1 partout ici)
    tris.push_back({corner[0], corner[1], corner[2]});
    return;
  }
  // points interieurs (a = indice v, b = indice w ; a, b >= 1, a + b <= N - 1)
  std::vector<int> inner((size_t)(N + 1) * (N + 1), -1);
  auto in_at = [&](int a, int b) -> int& { return inner[(size_t)a * (N + 1) + b]; };
  for (int a = 1; a <= N - 2; a++) {
    for (int b = 1; a + b <= N - 1; b++) {
      in_at(a, b) = (int)pts.size();
      pts.push_back({(float)(N - a - b) / N, (float)a / N, (float)b / N, -1, 0});
    }
  }
  for (int a = 1; a <= N - 2; a++) {
    for (int b = 1; a + b <= N - 2; b++) {
      tris.push_back({in_at(a, b), in_at(a + 1, b), in_at(a, b + 1)});
      if (a + b <= N - 3) {
        tris.push_back({in_at(a + 1, b), in_at(a + 1, b + 1), in_at(a, b + 1)});
      }
    }
  }
  // couronne : chaque arete cousue a la rangee interieure qui la longe
  const int m = N - 2;
  for (int e = 0; e < 3; e++) {
    std::vector<int> row(m);
    for (int j = 0; j < m; j++) {
      if (e == 0) {
        row[j] = in_at(j + 1, 1);
      } else if (e == 1) {
        row[j] = in_at(N - 2 - j, j + 1);
      } else {
        row[j] = in_at(1, N - 2 - j);
      }
    }
    int o = 0, j = 0;
    while (o < ne[e] || j < m - 1) {
      const bool can_o = o < ne[e];
      const bool can_j = j < m - 1;
      const float to = can_o ? (float)(o + 1) / ne[e] : 2.f;
      const float tj = can_j ? (float)(j + 2) / (N - 1) : 2.f;
      if (can_o && (!can_j || to <= tj)) {
        tris.push_back({outer(e, o), outer(e, o + 1), row[j]});
        o++;
      } else {
        tris.push_back({outer(e, o), row[j + 1], row[j]});
        j++;
      }
    }
  }
}
}  // namespace

size_t g_pc_decor_tris_added = 0;
size_t g_pc_sculpt_tris_added = 0;
double g_pc_sculpt_budget = 1.2e6;

float PcSculptHeight::doux_at(float s, float t) const {
  const float x = (s - std::floor(s)) * w - 0.5f;
  const float y = (t - std::floor(t)) * h - 0.5f;
  const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  const float fx = x - x0, fy = y - y0;
  auto at = [&](int i, int j) { return doux[(size_t)((j % h + h) % h) * w + ((i % w + w) % w)]; };
  return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
         (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

const PcSculptHeight* pc_sculpt_height(const std::string& debug_name) {
  static const bool off = std::getenv("QA_NO_SCULPT") != nullptr;
  if (off) {
    return nullptr;
  }
  static std::mutex mtx;
  static std::unordered_map<std::string, std::unique_ptr<PcSculptHeight>> cache;
  std::lock_guard<std::mutex> lk(mtx);
  auto it = cache.find(debug_name);
  if (it != cache.end()) {
    return it->second.get();
  }
  std::unique_ptr<PcSculptHeight> m;
  auto path = file_util::get_jak_project_dir() / "custom_assets" / "jak1" / "sculpture" /
              (debug_name + ".hgt");
  if (file_util::file_exists(path.string())) {
    auto b = file_util::read_binary_file(path);
    if (b.size() >= 16 && memcmp(b.data(), "HGT1", 4) == 0) {
      u16 w, h;
      float amp;
      memcpy(&w, b.data() + 4, 2);
      memcpy(&h, b.data() + 6, 2);
      memcpy(&amp, b.data() + 8, 4);
      if (w > 0 && h > 0 && b.size() >= 16 + (size_t)w * h && amp > 0.f) {
        m = std::make_unique<PcSculptHeight>();
        m->w = w;
        m->h = h;
        m->amp = amp;
        m->famille = b[12];
        m->data.assign(b.begin() + 16, b.begin() + 16 + (size_t)w * h);
        // version adoucie (flou en boite repete, rayon ~ 1/20 de la texture) : le volume de la
        // geometrie suit les pieces et les pierres, pas le grain (le grain reste au pixel)
        std::vector<float> f(m->data.size());
        for (size_t i = 0; i < f.size(); i++) {
          f[i] = m->data[i] / 127.5f - 1.f;
        }
        const int r = std::max(1, (int)std::max(w, h) / 20);
        std::vector<float> tmp(f.size());
        for (int pass = 0; pass < 2; pass++) {
          for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
              float acc = 0;
              for (int d = -r; d <= r; d++) {
                acc += f[(size_t)y * w + ((x + d) % w + w) % w];
              }
              tmp[(size_t)y * w + x] = acc / (2 * r + 1);
            }
          }
          for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
              float acc = 0;
              for (int d = -r; d <= r; d++) {
                acc += tmp[(size_t)(((y + d) % h + h) % h) * w + x];
              }
              f[(size_t)y * w + x] = acc / (2 * r + 1);
            }
          }
        }
        m->doux = std::move(f);
      }
    }
  }
  auto* res = m.get();
  cache[debug_name] = std::move(m);
  return res;
}

namespace {
struct SculptEdgeKey {
  s32 ax, ay, az, bx, by, bz;
  int k, ne;
  bool operator==(const SculptEdgeKey& o) const {
    return ax == o.ax && ay == o.ay && az == o.az && bx == o.bx && by == o.by && bz == o.bz &&
           k == o.k && ne == o.ne;
  }
};
struct SculptEdgeKeyHash {
  size_t operator()(const SculptEdgeKey& e) const {
    size_t h = (size_t)(u32)e.ax * 73856093u ^ (size_t)(u32)e.ay * 19349663u ^
               (size_t)(u32)e.az * 83492791u;
    h = h * 1000003u ^ ((size_t)(u32)e.bx * 2654435761u ^ (size_t)(u32)e.by * 40503u ^
                        (size_t)(u32)e.bz * 2246822519u);
    return h * 31u + (size_t)e.k * 131u + (size_t)e.ne;
  }
};
inline std::array<s32, 3> qkey(const math::Vector3f& p) {
  return {(s32)std::lround(p.x() * 0.25f), (s32)std::lround(p.y() * 0.25f),
          (s32)std::lround(p.z() * 0.25f)};
}
}  // namespace

// PC-REMAKE : SCULPTURE d'un triangle d'origine du decor. Les coins restent a leur place ; les points
// nouveaux suivent la surface (plate, ou courbe PN pour le bois et la paille) puis la carte de
// hauteur adoucie, le long de la normale, a l'echelle reelle de la texture. Les aretes vives,
// les coutures de texture et les aretes partagees avec un autre dessin restent droites et sans
// relief ; les points d'une arete sont calcules une fois (cache) : les deux triangles qui la
// partagent ont exactement les memes.
u32 pc_sculpt_triangle(std::vector<PreloadedVertex>& verts,
                       std::vector<u32>& out,
                       const u32 t3[3],
                       const DecorCorner c[3],
                       bool courbe,
                       const PcSculptHeight& carte,
                       float r0,
                       float r1,
                       float r2,
                       float pas,
                       float K,
                       float CAP,
                       const std::unordered_set<SculptEdgeKey, SculptEdgeKeyHash>& coutures,
                       std::unordered_map<SculptEdgeKey, math::Vector3f, SculptEdgeKeyHash>& cache,
                       const std::function<u32(u32, u32)>& phong_mid,
                       size_t* added) {
  bool shared[3], straight[3];
  int ne[3];
  float lmax = 0.f;
  for (int e = 0; e < 3; e++) {
    const auto& a = c[e];
    const auto& b = c[(e + 1) % 3];
    shared[e] = a.border && b.border;
    auto ka = qkey(a.p), kb = qkey(b.p);
    if (kb < ka) {
      std::swap(ka, kb);
    }
    const bool couture = coutures.count({ka[0], ka[1], ka[2], kb[0], kb[1], kb[2], 0, 0}) > 0;
    straight[e] = a.vif || b.vif || shared[e] || couture;
    const float len = (b.p - a.p).length();
    lmax = std::max(lmax, len);
    if (shared[e]) {
      ne[e] = 2;
    } else if (straight[e]) {
      ne[e] = 1;
    } else {
      int n = std::clamp((int)std::ceil(len / pas), 1, 16);
      if (courbe) {
        const float ang = std::acos(std::clamp(a.g.dot(b.g), -1.f, 1.f)) * 57.29578f;
        n = std::max(n, std::clamp((int)std::ceil(ang / 12.f - 0.15f), 1, 6));
      }
      ne[e] = n;
    }
  }
  int N = std::clamp((int)std::ceil(lmax / pas), 1, 16);
  N = std::max(N, std::max(ne[0], std::max(ne[1], ne[2])));
  if (N > 1) {
    N = std::max(N, 3);
  }
  // milieux exacts des aretes partagees (le dessin voisin les a deja), avant tout ajout de sommet
  math::Vector3f shared_mid[3];
  for (int e = 0; e < 3; e++) {
    if (shared[e]) {
      const u32 m = phong_mid(t3[e], t3[(e + 1) % 3]);
      shared_mid[e] = math::Vector3f(verts[m].x, verts[m].y, verts[m].z);
    }
  }
  // surface : plate, ou courbe (triangle PN) pour le bois et la paille
  const math::Vector3f b01 = decor_ctrl(c[0], c[1], straight[0]),
                       b10 = decor_ctrl(c[1], c[0], straight[0]),
                       b12 = decor_ctrl(c[1], c[2], straight[1]),
                       b21 = decor_ctrl(c[2], c[1], straight[1]),
                       b20 = decor_ctrl(c[2], c[0], straight[2]),
                       b02 = decor_ctrl(c[0], c[2], straight[2]);
  const math::Vector3f E = (b01 + b10 + b12 + b21 + b20 + b02) / 6.f;
  const math::Vector3f Vc = (c[0].p + c[1].p + c[2].p) / 3.f;
  const math::Vector3f b111 = E + (E - Vc) * 0.5f;
  auto surface = [&](float u, float v, float w) {
    if (!courbe) {
      return c[0].p * u + c[1].p * v + c[2].p * w;
    }
    return c[0].p * (u * u * u) + c[1].p * (v * v * v) + c[2].p * (w * w * w) +
           b01 * (3.f * u * u * v) + b10 * (3.f * u * v * v) + b02 * (3.f * u * u * w) +
           b20 * (3.f * u * w * w) + b12 * (3.f * v * v * w) + b21 * (3.f * v * w * w) +
           b111 * (6.f * u * v * w);
  };
  math::Vector3f fn = (c[1].p - c[0].p).cross(c[2].p - c[0].p);
  const float fl = fn.length();
  fn = fl > 1e-9f ? fn / fl : c[0].n;
  auto relief = [&](float u, float v, float w) {
    const float r = r0 * u + r1 * v + r2 * w;
    if (r <= 0.f) {
      return math::Vector3f(0.f, 0.f, 0.f);
    }
    math::Vector3f dir = fn;
    if (courbe) {
      math::Vector3f g = c[0].g * u + c[1].g * v + c[2].g * w;
      const float gl = g.length();
      if (gl > 1e-6f) {
        dir = g / gl;
      }
    }
    const float A = std::min(carte.amp * r * K, CAP);
    const float h = carte.doux_at(c[0].s * u + c[1].s * v + c[2].s * w,
                                  c[0].t * u + c[1].t * v + c[2].t * w);
    return dir * (h * A);
  };
  std::vector<PatchPoint> pts;
  std::vector<std::array<int, 3>> tris;
  sculpt_patch_topology(N, ne, pts, tris);

  auto mid_n = [&](const DecorCorner& a, const DecorCorner& b, bool st) {
    math::Vector3f s2 = a.n + b.n;
    if (!st) {
      math::Vector3f dd = b.p - a.p;
      const float l2 = std::max(dd.dot(dd), 1e-8f);
      s2 -= dd * (2.f * dd.dot(a.n + b.n) / l2);
    }
    const float l = s2.length();
    return l > 1e-6f ? s2 / l : a.n;
  };
  const math::Vector3f m01 = mid_n(c[0], c[1], straight[0]), m12 = mid_n(c[1], c[2], straight[1]),
                       m20 = mid_n(c[2], c[0], straight[2]);
  const u32 base = (u32)verts.size();
  const PreloadedVertex modele = verts[t3[0]];
  for (const auto& pt : pts) {
    const float u = pt.u, v = pt.v, w = pt.w;
    math::Vector3f p;
    if (pt.arete < 0 && ((u == 1.f) || (v == 1.f) || (w == 1.f))) {
      p = u == 1.f ? c[0].p : (v == 1.f ? c[1].p : c[2].p);  // coin : ne bouge pas
    } else if (pt.arete >= 0) {
      const int e = pt.arete;
      if (shared[e]) {
        p = shared_mid[e];
      } else {
        auto ka = qkey(c[e].p), kb = qkey(c[(e + 1) % 3].p);
        const bool flip = kb < ka;
        SculptEdgeKey key = flip ? SculptEdgeKey{kb[0], kb[1], kb[2], ka[0], ka[1], ka[2],
                                                 ne[e] - pt.k, ne[e]}
                                 : SculptEdgeKey{ka[0], ka[1], ka[2], kb[0], kb[1], kb[2], pt.k,
                                                 ne[e]};
        auto it = cache.find(key);
        if (it != cache.end()) {
          p = it->second;
        } else {
          p = surface(u, v, w);
          if (!straight[e]) {
            p += relief(u, v, w);
          }
          cache.emplace(key, p);
        }
      }
    } else {
      p = surface(u, v, w) + relief(u, v, w);
    }
    math::Vector3f nl = c[0].n * (u * u) + c[1].n * (v * v) + c[2].n * (w * w) + m01 * (u * v) +
                        m12 * (v * w) + m20 * (w * u);
    const float nll = nl.length();
    nl = nll > 1e-6f ? nl / nll : c[0].n;
    PreloadedVertex nvx = modele;
    nvx.x = p.x();
    nvx.y = p.y();
    nvx.z = p.z();
    nvx.s = c[0].s * u + c[1].s * v + c[2].s * w;
    nvx.t = c[0].t * u + c[1].t * v + c[2].t * w;
    nvx.nor = decor_pack_nor(nl);
    nvx.weld = 0;
    nvx.color_index = c[0].col;
    nvx.color_index2 = c[1].col;
    nvx.color_index3 = c[2].col;
    nvx.cw0 = (u8)std::clamp((int)std::lround(u * 255.f), 0, 255);
    nvx.cw1 = (u8)std::clamp((int)std::lround(v * 255.f), 0, 255 - nvx.cw0);
    if (nvx.cw0 == 0 && nvx.cw1 == 0) {
      nvx.color_index = c[2].col;
      nvx.color_index2 = c[2].col;
    }
    verts.push_back(nvx);
  }
  for (const auto& t : tris) {
    out.push_back(base + t[0]);
    out.push_back(base + t[1]);
    out.push_back(base + t[2]);
  }
  *added += tris.size() > 0 ? tris.size() - 1 : 0;
  g_pc_sculpt_tris_added += tris.size() > 0 ? tris.size() - 1 : 0;
  return (u32)tris.size();
}

size_t pc_refine_decor_impl(std::vector<PreloadedVertex>& verts,
                            std::vector<u32>& indices,
                            std::vector<StripDraw>& draws,
                            const std::vector<u8>& classes,
                            const std::function<u32(u32, u32)>& phong_mid,
                            const std::vector<const PcSculptHeight*>* maps) {
  const float M = 4096.f;
  std::vector<u32> out;
  out.reserve(indices.size() * 2);
  size_t added = 0;
  const size_t nv0 = verts.size();

  // ---- PC-REMAKE : SCULPTURE, preparation (si des dessins ont une carte de hauteur) ----
  // echelle reelle par sommet (unites du jeu par unite de texture), coutures de texture (memes
  // positions, coordonnees differentes : l'arete y reste droite, sans relief) et pas du
  // decoupage, regle pour tenir un budget de triangles
  std::vector<float> rep_v;
  std::unordered_set<SculptEdgeKey, SculptEdgeKeyHash> coutures;
  std::unordered_map<SculptEdgeKey, math::Vector3f, SculptEdgeKeyHash> edge_cache;
  float pas = 0.35f * M;  // 35 cm
  const float K_GEO = 0.8f, CAP_GEO = 0.035f * M;  // volume : 80 % de la carte, 3,5 cm au plus
  auto sculpt_map = [&](size_t di) -> const PcSculptHeight* {
    return (maps && di < maps->size()) ? (*maps)[di] : nullptr;
  };
  bool any_sculpt = false;
  for (size_t di = 0; maps && di < draws.size(); di++) {
    any_sculpt |= sculpt_map(di) != nullptr;
  }
  if (any_sculpt) {
    std::vector<float> rep_n(nv0, 0.f);
    rep_v.assign(nv0, 0.f);
    std::unordered_map<SculptEdgeKey, std::array<float, 4>, SculptEdgeKeyHash> uv_edge;
    double tris_estimes = 0;
    std::vector<float> longueurs;
    for (size_t di = 0; di < draws.size(); di++) {
      if (!sculpt_map(di)) {
        continue;
      }
      size_t src = draws[di].unpacked.idx_of_first_idx_in_full_buffer;
      size_t count = 0;
      for (auto& g : draws[di].vis_groups) {
        count += g.num_inds;
      }
      for (size_t i = src; i + 2 < src + count && i + 2 < indices.size(); i += 3) {
        const u32 t3[3] = {indices[i], indices[i + 1], indices[i + 2]};
        if (t3[0] >= nv0 || t3[1] >= nv0 || t3[2] >= nv0) {
          continue;
        }
        const auto &a = verts[t3[0]], &b = verts[t3[1]], &c = verts[t3[2]];
        math::Vector3f e1(b.x - a.x, b.y - a.y, b.z - a.z), e2(c.x - a.x, c.y - a.y, c.z - a.z);
        const float aire = e1.cross(e2).length() * 0.5f;
        const float auv = std::abs((b.s - a.s) * (c.t - a.t) - (c.s - a.s) * (b.t - a.t)) * 0.5f;
        if (aire > 1e-6f && auv > 1e-12f) {
          const float r = std::sqrt(aire / auv);
          for (u32 k : t3) {
            rep_v[k] += r;
            rep_n[k] += 1.f;
          }
        }
        float lmax = 0.f;
        for (int e = 0; e < 3; e++) {
          const auto& p = verts[t3[e]];
          const auto& q = verts[t3[(e + 1) % 3]];
          math::Vector3f pp(p.x, p.y, p.z), qq(q.x, q.y, q.z);
          lmax = std::max(lmax, (qq - pp).length());
          auto ka = qkey(pp), kb = qkey(qq);
          std::array<float, 4> uv = {p.s, p.t, q.s, q.t};
          if (kb < ka) {
            std::swap(ka, kb);
            uv = {q.s, q.t, p.s, p.t};
          }
          SculptEdgeKey key{ka[0], ka[1], ka[2], kb[0], kb[1], kb[2], 0, 0};
          auto it = uv_edge.find(key);
          if (it == uv_edge.end()) {
            uv_edge.emplace(key, uv);
          } else {
            float d = 0.f;
            for (int j = 0; j < 4; j++) {
              d = std::max(d, std::abs(it->second[j] - uv[j]));
            }
            if (d > 1e-4f) {
              coutures.insert(key);
            }
          }
        }
        longueurs.push_back(lmax);
      }
    }
    for (size_t k = 0; k < nv0; k++) {
      if (rep_n[k] > 0.f) {
        rep_v[k] /= rep_n[k];
      }
    }
    // budget : environ N^2 - 1 triangles AJOUTES par triangle d'origine, N = longueur / pas (16 au
    // plus) ; part du budget du niveau qui reste
    const double budget = std::max(0.0, g_pc_sculpt_budget);
    for (int iter = 0; iter < 8; iter++) {
      tris_estimes = 0;
      for (float l : longueurs) {
        const float n = std::clamp(std::ceil(l / pas), 1.f, 16.f);
        tris_estimes += n > 1.f ? std::max(9.f, n * n) - 1.f : 0.f;
      }
      if (tris_estimes <= budget) {
        break;
      }
      pas *= (float)std::sqrt(tris_estimes / budget);
    }
    g_pc_sculpt_budget -= tris_estimes;
    lg::info("PC-REMAKE sculpture du decor : {} triangles d'origine, pas {:.0f} cm, ~{:.0f} k triangles",
             longueurs.size(), pas / M * 100.f, tris_estimes / 1000.0);
  }

  for (size_t di = 0; di < draws.size(); di++) {
    auto& d = draws[di];
    const int classe = di < classes.size() ? classes[di] : 0;
    const PcSculptHeight* carte = sculpt_map(di);
    size_t src = d.unpacked.idx_of_first_idx_in_full_buffer;
    d.unpacked.idx_of_first_idx_in_full_buffer = out.size();
    for (auto& g : d.vis_groups) {
      const size_t before = out.size();
      u32 tris = 0;
      const bool remplace = proto_remplace(g);
      for (size_t i = src; i + 2 < src + g.num_inds && i + 2 < indices.size(); i += 3) {
        const u32 t3[3] = {indices[i], indices[i + 1], indices[i + 2]};
        if (classe == 0 || remplace || t3[0] >= nv0 || t3[1] >= nv0 || t3[2] >= nv0) {
          out.insert(out.end(), t3, t3 + 3);
          tris++;
          continue;
        }
        DecorCorner c[3];
        for (int k = 0; k < 3; k++) {
          const auto& v = verts[t3[k]];
          c[k].p = math::Vector3f(v.x, v.y, v.z);
          c[k].n = decor_unpack_nor(v.nor);
          c[k].g = decor_unpack_weld(v.weld, &c[k].vif, &c[k].border);
          if (c[k].g.length() < 0.5f) {
            c[k].g = c[k].n;
          }
          c[k].s = v.s;
          c[k].t = v.t;
          c[k].col = v.color_index;
        }
        if (carte && !(c[0].border && c[1].border && c[2].border)) {
          tris += pc_sculpt_triangle(verts, out, t3, c, classe == 1 || classe == 3, *carte,
                                     rep_v.empty() ? 0.f : rep_v[t3[0]],
                                     rep_v.empty() ? 0.f : rep_v[t3[1]],
                                     rep_v.empty() ? 0.f : rep_v[t3[2]], pas, K_GEO, CAP_GEO,
                                     coutures, edge_cache, phong_mid, &added);
          continue;
        }
        bool straight[3], shared[3];
        int ne[3];
        for (int e = 0; e < 3; e++) {
          const auto& a = c[e];
          const auto& b = c[(e + 1) % 3];
          shared[e] = a.border && b.border;
          straight[e] = a.vif || b.vif || shared[e];
          const float len = (b.p - a.p).length() / M;
          if (shared[e]) {
            ne[e] = 2;  // milieu du dessin voisin
          } else if (straight[e] || len < 0.03f) {
            ne[e] = 1;
          } else {
            const float ang = std::acos(std::clamp(a.g.dot(b.g), -1.f, 1.f)) * 57.29578f;
            ne[e] = std::clamp((int)std::ceil(ang / 12.f - 0.15f), 1, 6);
          }
        }
        int N = std::max(ne[0], std::max(ne[1], ne[2]));
        bool any_curve = false;
        for (int e = 0; e < 3; e++) {
          any_curve |= !shared[e] && ne[e] > 1;
        }
        if (!any_curve) {
          // rien a arrondir : decoupage d'origine (milieux du voisinage aux aretes partagees)
          N = 2;
          ne[0] = ne[1] = ne[2] = 2;
        }
        const math::Vector3f b01 = decor_ctrl(c[0], c[1], straight[0]),
                             b10 = decor_ctrl(c[1], c[0], straight[0]),
                             b12 = decor_ctrl(c[1], c[2], straight[1]),
                             b21 = decor_ctrl(c[2], c[1], straight[1]),
                             b20 = decor_ctrl(c[2], c[0], straight[2]),
                             b02 = decor_ctrl(c[0], c[2], straight[2]);
        const math::Vector3f E = (b01 + b10 + b12 + b21 + b20 + b02) / 6.f;
        const math::Vector3f Vc = (c[0].p + c[1].p + c[2].p) / 3.f;
        const math::Vector3f b111 = E + (E - Vc) * 0.5f;
        auto pn_at = [&](float u, float v, float w) {
          return c[0].p * (u * u * u) + c[1].p * (v * v * v) + c[2].p * (w * w * w) +
                 b01 * (3.f * u * u * v) + b10 * (3.f * u * v * v) + b02 * (3.f * u * u * w) +
                 b20 * (3.f * u * w * w) + b12 * (3.f * v * v * w) + b21 * (3.f * v * w * w) +
                 b111 * (6.f * u * v * w);
        };
        // milieux du voisinage (aretes partagees), calcules une fois
        math::Vector3f shared_mid[3];
        for (int e = 0; e < 3; e++) {
          if (shared[e]) {
            const u32 m = phong_mid(t3[e], t3[(e + 1) % 3]);
            shared_mid[e] = math::Vector3f(verts[m].x, verts[m].y, verts[m].z);
          }
        }
        // point d'une arete a son parametre te (0 = debut de l'arete, 1 = fin)
        auto edge_curve = [&](int e, float te) {
          float bu = 0, bv = 0, bw = 0;
          if (e == 0) {
            bu = 1 - te;
            bv = te;
          } else if (e == 1) {
            bv = 1 - te;
            bw = te;
          } else {
            bw = 1 - te;
            bu = te;
          }
          return pn_at(bu, bv, bw);
        };
        auto edge_pt = [&](int e, float te) {
          const int n = ne[e];
          if (shared[e]) {
            const math::Vector3f a = c[e].p, b = c[(e + 1) % 3].p;
            return te < 0.5f ? a + (shared_mid[e] - a) * (te * 2.f)
                             : shared_mid[e] + (b - shared_mid[e]) * (te * 2.f - 1.f);
          }
          const float x = te * n;
          const int k0 = std::min((int)std::floor(x), n - 1);
          const float f = x - k0;
          return edge_curve(e, (float)k0 / n) * (1.f - f) + edge_curve(e, (float)(k0 + 1) / n) * f;
        };
        auto mid_n = [&](const DecorCorner& a, const DecorCorner& b, bool st) {
          math::Vector3f s2 = a.n + b.n;
          if (!st) {
            math::Vector3f dd = b.p - a.p;
            const float l2 = std::max(dd.dot(dd), 1e-8f);
            s2 -= dd * (2.f * dd.dot(a.n + b.n) / l2);
          }
          const float l = s2.length();
          return l > 1e-6f ? s2 / l : a.n;
        };
        const math::Vector3f m01 = mid_n(c[0], c[1], straight[0]),
                             m12 = mid_n(c[1], c[2], straight[1]),
                             m20 = mid_n(c[2], c[0], straight[2]);
        const u32 base = (u32)verts.size();
        auto vid = [&](int i, int j) { return base + (u32)(i * (2 * N + 3 - i) / 2 + j); };
        for (int i = 0; i <= N; i++) {
          for (int j = 0; j <= N - i; j++) {
            const float v = (float)i / N, w = (float)j / N, u = 1.f - v - w;
            math::Vector3f p;
            if (j == 0) {
              p = edge_pt(0, v);
            } else if (i + j == N) {
              p = edge_pt(1, w);
            } else if (i == 0) {
              p = edge_pt(2, 1.f - w);
            } else {
              p = pn_at(u, v, w);
            }
            math::Vector3f nl = c[0].n * (u * u) + c[1].n * (v * v) + c[2].n * (w * w) +
                                m01 * (u * v) + m12 * (v * w) + m20 * (w * u);
            const float nll = nl.length();
            if (nll > 1e-6f) {
              nl /= nll;
            } else {
              nl = c[0].n;
            }
            PreloadedVertex nvx = verts[t3[0]];
            nvx.x = p.x();
            nvx.y = p.y();
            nvx.z = p.z();
            nvx.s = c[0].s * u + c[1].s * v + c[2].s * w;
            nvx.t = c[0].t * u + c[1].t * v + c[2].t * w;
            nvx.nor = decor_pack_nor(nl);
            nvx.weld = 0;
            nvx.color_index = c[0].col;
            nvx.color_index2 = c[1].col;
            nvx.color_index3 = c[2].col;
            nvx.cw0 = (u8)std::clamp((int)std::lround(u * 255.f), 0, 255);
            nvx.cw1 = (u8)std::clamp((int)std::lround(v * 255.f), 0, 255 - nvx.cw0);
            if (nvx.cw0 == 0 && nvx.cw1 == 0) {
              // tout le poids sur le 3e coin : (0, 0) est le code du melange d'origine
              nvx.color_index = c[2].col;
              nvx.color_index2 = c[2].col;
            }
            verts.push_back(nvx);
          }
        }
        for (int i = 0; i < N; i++) {
          for (int j = 0; j < N - i; j++) {
            const u32 a = vid(i, j), b = vid(i + 1, j), cc = vid(i, j + 1);
            out.push_back(a);
            out.push_back(b);
            out.push_back(cc);
            tris++;
            if (j < N - i - 1) {
              const u32 d2 = vid(i + 1, j + 1);
              out.push_back(b);
              out.push_back(d2);
              out.push_back(cc);
              tris++;
            }
          }
        }
        added += (size_t)N * N - 1;
      }
      src += g.num_inds;
      g.num_inds = (u32)(out.size() - before);
      g.num_tris = tris;
    }
  }
  indices = std::move(out);
  g_pc_decor_tris_added += added;
  return added;
}

void MercModelGroup::pc_prepare_characters() {
  int n_models = 0;
  size_t n_converted = 0;
  // une bande (strip, coupee par UINT32_MAX) -> liste de triangles ajoutee en fin de tampon
  auto to_list = [&](MercDraw& d) {
    if (d.no_strip) {
      return;
    }
    const u32 first = (u32)indices.size();
    u32 a = UINT32_MAX, b = UINT32_MAX, tris = 0;
    bool parity = false;
    for (u32 i = 0; i < d.index_count; i++) {
      const u32 c = indices[d.first_index + i];
      if (c == UINT32_MAX) {
        a = b = UINT32_MAX;
        parity = false;
        continue;
      }
      if (a != UINT32_MAX && b != UINT32_MAX && a != b && b != c && a != c) {
        if (parity) {
          indices.insert(indices.end(), {b, a, c});
        } else {
          indices.insert(indices.end(), {a, b, c});
        }
        tris++;
      }
      if (a != UINT32_MAX || b != UINT32_MAX) {
        parity = !parity;
      }
      a = b;
      b = c;
    }
    d.first_index = first;
    d.index_count = (u32)indices.size() - first;
    d.num_triangles = tris;
    d.no_strip = true;
    n_converted++;
  };

  auto qpos = [](float x, float y, float z) -> u64 {
    auto q = [](float f) -> u64 { return (u64)(u32)(s32)std::lround(f * 64.f) & 0x1fffff; };
    return (q(x) << 42) | (q(y) << 21) | q(z);
  };

  for (auto& model : models) {
    model.pc_character = pc_is_character_model(model.name);
    if (!model.pc_character) {
      continue;
    }
    n_models++;
    for (auto& effect : model.effects) {
      for (auto& d : effect.all_draws) {
        to_list(d);
      }
      for (auto& d : effect.mod.fix_draw) {
        to_list(d);
      }
      for (auto& d : effect.mod.mod_draw) {
        to_list(d);
      }
    }

    // NORMALE SOUDEE : moyenne, par position, des normales de toutes les pieces du modele
    // (sommets fixes et sommets des visages animes). Le decoupage courbe les aretes avec elle :
    // deux pieces qui se touchent suivent la meme courbe, sans jour. Une position dont les
    // normales divergent de plus de 50 degres est une arete vive : elle reste droite.
    std::vector<u32> used;
    for (auto& effect : model.effects) {
      for (auto* list : {&effect.all_draws, &effect.mod.fix_draw}) {
        for (auto& d : *list) {
          for (u32 i = 0; i < d.index_count; i++) {
            const u32 v = indices[d.first_index + i];
            if (v != UINT32_MAX && v < vertices.size()) {
              used.push_back(v);
            }
          }
        }
      }
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    struct Acc {
      math::Vector3f n{0.f, 0.f, 0.f};
      std::vector<math::Vector3f> each;
    };
    std::unordered_map<u64, Acc> acc;
    auto add = [&](const MercVertex& v) {
      math::Vector3f n(v.nx, v.ny, v.nz);
      const float l = n.length();
      if (l < 1e-5f) {
        return;
      }
      n /= l;
      auto& a = acc[qpos(v.x, v.y, v.z)];
      a.n += n;
      a.each.push_back(n);
    };
    for (u32 v : used) {
      add(vertices[v]);
    }
    for (auto& effect : model.effects) {
      for (auto& v : effect.mod.vertices) {
        add(v);
      }
    }
    const float cos_vif = std::cos(50.f * 3.14159265f / 180.f);
    auto pack = [&](MercVertex& v) {
      u32 bits = 0;
      auto it = acc.find(qpos(v.x, v.y, v.z));
      if (it != acc.end()) {
        math::Vector3f n = it->second.n;
        const float l = n.length();
        if (l > 1e-5f) {
          n /= l;
          bool sharp = false;
          for (auto& e : it->second.each) {
            if (e.dot(n) < cos_vif) {
              sharp = true;
              break;
            }
          }
          bits = pc_pack_weld_normal(n.x(), n.y(), n.z(), sharp);
        }
      }
      memcpy(&v.pad0, &bits, 4);
    };
    for (u32 v : used) {
      pack(vertices[v]);
    }
    for (auto& effect : model.effects) {
      for (auto& v : effect.mod.vertices) {
        pack(v);
      }
    }
  }
  if (n_models) {
    lg::info("PC-REMAKE personnages : {} modeles prepares au decoupage ({} dessins convertis)",
             n_models, n_converted);
  }
}

// ---------------------------------------------------------------------------
// PC-REMAKE: parametric prop remodeling (Sandover Village pass).
//
// Every listed prop is rebuilt from scratch with modern geometry, auto-fitted
// to the original model's bounding box. UVs are transferred from the original
// mesh by nearest-neighbour lookup, so each new surface inherits the texture
// atlas region the artists authored for that part of the object — the prop
// keeps its identity while gaining real volume, bevels and roundness.
//
// Materials: metal props get the merc envmap pass (they reflect the sky).
// ---------------------------------------------------------------------------
namespace {

struct MeshBuild {
  std::vector<MercVertex> verts;
  std::vector<u32> idx;
  const MercVertex* ref = nullptr;  // skinning + colour template

  u32 add(float x, float y, float z, float nx, float ny, float nz) {
    MercVertex m = *ref;
    m.x = x;
    m.y = y;
    m.z = z;
    float l = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (l > 1e-6f) {
      nx /= l;
      ny /= l;
      nz /= l;
    }
    m.nx = nx;
    m.ny = ny;
    m.nz = nz;
    verts.push_back(m);
    return (u32)verts.size() - 1;
  }
  void tri(u32 a, u32 b, u32 c) {
    idx.push_back(a);
    idx.push_back(b);
    idx.push_back(c);
  }
  void quad(u32 a, u32 b, u32 c, u32 d) {
    tri(a, b, c);
    tri(a, c, d);
  }
  // revolve a profile (radius, height) around Y; profile[i] = {r, y}
  void revolve(const std::vector<std::array<float, 2>>& profile,
               float cx,
               float cz,
               int segments,
               bool cap_bottom,
               bool cap_top) {
    std::vector<std::vector<u32>> rings;
    for (size_t p = 0; p < profile.size(); p++) {
      std::vector<u32> ring;
      float r = profile[p][0], y = profile[p][1];
      // normal from the profile slope
      float dr = 0.f, dy = 1.f;
      if (p + 1 < profile.size()) {
        dr = profile[p + 1][0] - r;
        dy = profile[p + 1][1] - y;
      } else if (p > 0) {
        dr = r - profile[p - 1][0];
        dy = y - profile[p - 1][1];
      }
      for (int s = 0; s <= segments; s++) {
        float a = 6.2831853f * (float)s / segments;
        float ca = std::cos(a), sa = std::sin(a);
        ring.push_back(add(cx + ca * r, y, cz + sa * r, ca * dy, -dr, sa * dy));
      }
      rings.push_back(ring);
    }
    for (size_t p = 0; p + 1 < rings.size(); p++) {
      for (int s = 0; s < segments; s++) {
        quad(rings[p][s], rings[p][s + 1], rings[p + 1][s + 1], rings[p + 1][s]);
      }
    }
    if (cap_bottom && !profile.empty()) {
      float r = profile.front()[0], y = profile.front()[1];
      u32 c = add(cx, y, cz, 0.f, -1.f, 0.f);
      std::vector<u32> rim;
      for (int s = 0; s <= segments; s++) {
        float a = 6.2831853f * (float)s / segments;
        rim.push_back(add(cx + std::cos(a) * r, y, cz + std::sin(a) * r, 0.f, -1.f, 0.f));
      }
      for (int s = 0; s < segments; s++) {
        tri(c, rim[s + 1], rim[s]);
      }
    }
    if (cap_top && !profile.empty()) {
      float r = profile.back()[0], y = profile.back()[1];
      u32 c = add(cx, y, cz, 0.f, 1.f, 0.f);
      std::vector<u32> rim;
      for (int s = 0; s <= segments; s++) {
        float a = 6.2831853f * (float)s / segments;
        rim.push_back(add(cx + std::cos(a) * r, y, cz + std::sin(a) * r, 0.f, 1.f, 0.f));
      }
      for (int s = 0; s < segments; s++) {
        tri(c, rim[s], rim[s + 1]);
      }
    }
  }
  // axis-aligned rounded box: beveled edges, real corner geometry
  void rounded_box(float cx, float cy, float cz, float hx, float hy, float hz, float bevel) {
    float b = bevel;
    // 8 corner points expanded into 3 bevel points each = 24 verts, then faces
    const int sx[2] = {-1, 1}, sy[2] = {-1, 1}, sz[2] = {-1, 1};
    // build the 6 main faces (inset by bevel) and connect with bevel strips
    struct Face {
      int axis;
      int sign;
    };
    const Face faces[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};
    u32 fverts[6][4];
    for (int f = 0; f < 6; f++) {
      int ax = faces[f].axis;
      int sg = faces[f].sign;
      int u = (ax + 1) % 3, v = (ax + 2) % 3;
      float ext[3] = {hx, hy, hz};
      float c[3] = {cx, cy, cz};
      float n[3] = {0, 0, 0};
      n[ax] = (float)sg;
      const int uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
      for (int k = 0; k < 4; k++) {
        float p[3] = {c[0], c[1], c[2]};
        p[ax] += sg * ext[ax];
        p[u] += uv[k][0] * (ext[u] - b);
        p[v] += uv[k][1] * (ext[v] - b);
        fverts[f][k] = add(p[0], p[1], p[2], n[0], n[1], n[2]);
      }
      if (sg > 0) {
        quad(fverts[f][0], fverts[f][1], fverts[f][2], fverts[f][3]);
      } else {
        quad(fverts[f][3], fverts[f][2], fverts[f][1], fverts[f][0]);
      }
    }
    // 12 bevel strips along the edges: connect adjacent face borders
    auto edge_strip = [&](int f0, int k0a, int k0b, int f1, int k1a, int k1b) {
      quad(fverts[f0][k0a], fverts[f0][k0b], fverts[f1][k1a], fverts[f1][k1b]);
    };
    // +X face (0) edges to +Y(2), -Y(3), +Z(4), -Z(5)
    edge_strip(0, 3, 2, 2, 1, 0);
    edge_strip(0, 1, 0, 3, 0, 1);
    edge_strip(0, 2, 1, 4, 1, 2);
    edge_strip(0, 0, 3, 5, 3, 0);
    // -X face (1)
    edge_strip(1, 2, 3, 2, 3, 2);
    edge_strip(1, 0, 1, 3, 2, 3);
    edge_strip(1, 1, 2, 4, 0, 3);
    edge_strip(1, 3, 0, 5, 2, 1);
    // Y/Z edges
    edge_strip(2, 2, 3, 4, 2, 3);
    edge_strip(2, 0, 1, 5, 0, 1);
    edge_strip(3, 3, 2, 4, 1, 0);
    edge_strip(3, 1, 0, 5, 3, 2);
    (void)sx;
    (void)sy;
    (void)sz;
  }
  // a raised band around a box face set (straps / reinforcement)
  void box_band(float cx, float cy, float cz, float hx, float hy, float hz, float t) {
    rounded_box(cx, cy, cz, hx, hy, hz, t * 0.4f);
  }
  // torus ring around Y (rims, hoops, gear bodies)
  void ring(float cx, float cy, float cz, float radius, float thick, int seg, int sides) {
    std::vector<std::vector<u32>> rings;
    for (int s = 0; s <= seg; s++) {
      float a = 6.2831853f * (float)s / seg;
      float ca = std::cos(a), sa = std::sin(a);
      std::vector<u32> col;
      for (int t = 0; t <= sides; t++) {
        float b = 6.2831853f * (float)t / sides;
        float cb = std::cos(b), sb = std::sin(b);
        float rr = radius + thick * cb;
        col.push_back(add(cx + ca * rr, cy + thick * sb, cz + sa * rr, ca * cb, sb, sa * cb));
      }
      rings.push_back(col);
    }
    for (int s = 0; s < seg; s++) {
      for (int t = 0; t < sides; t++) {
        quad(rings[s][t], rings[s + 1][t], rings[s + 1][t + 1], rings[s][t + 1]);
      }
    }
  }
};

// ---- individual prop generators -------------------------------------------

// wooden barrel: bulged staves, three iron hoops, recessed lids
void gen_barrel(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile;
  const int rows = 12;
  for (int r = 0; r <= rows; r++) {
    float t = (float)r / rows;
    float bulge = 1.f - 0.26f * (2.f * t - 1.f) * (2.f * t - 1.f);
    profile.push_back({radius * bulge, y0 + t * height});
  }
  if (!detail) {
    mb.revolve(profile, cx, cz, 30, true, true);
  }
  for (float rt : {0.13f, 0.5f, 0.87f}) {
    float bulge = 1.f - 0.26f * (2.f * rt - 1.f) * (2.f * rt - 1.f);
    mb.ring(cx, y0 + rt * height, cz, radius * bulge * 1.035f, height * 0.052f, 30, 8);
  }
}

// metal crate: the original box silhouette (so the painted texture still maps
// 1:1) with chamfered edges, corner armour and rivetted straps added as relief
void gen_metal_crate(MeshBuild& mb, float cx, float y0, float cz, float half, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  float hh = height * 0.5f;
  if (!detail) {
    mb.rounded_box(cx, cy, cz, half, hh, half, half * 0.055f);
  }
  // narrow straps standing proud of the faces
  float lift = half * 0.055f;
  mb.box_band(cx, cy, cz, half + lift, hh * 0.14f, half + lift, half * 0.04f);
  mb.box_band(cx, cy, cz, half + lift, hh + lift, half * 0.14f, half * 0.04f);
  mb.box_band(cx, cy, cz, half * 0.14f, hh + lift, half + lift, half * 0.04f);
  // corner armour: small chamfered blocks hugging the eight corners
  for (int i = 0; i < 8; i++) {
    float sx = (i & 1) ? 1.f : -1.f;
    float sy = (i & 2) ? 1.f : -1.f;
    float sz = (i & 4) ? 1.f : -1.f;
    float k = half * 0.19f;
    mb.rounded_box(cx + sx * (half - k * 0.62f), cy + sy * (hh - k * 0.62f),
                   cz + sz * (half - k * 0.62f), k, k, k, k * 0.3f);
  }
}

// scout-fly cage: an open welded frame — thin mesh panels on the faces plus a
// real bar skeleton, so the creature inside stays visible (the original is a
// see-through cage; a solid box reads as a block of glass).
// IMPORTANT: this prop renders alpha-blended, so NOTHING may overlap — every
// stacked layer would add up and wash the cage out to solid white. All pieces
// below tile the volume exactly once.
void gen_cage(MeshBuild& mb, float cx, float y0, float cz, float half, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  float hh = height * 0.5f;
  float bar = half * 0.09f;      // frame bar half-thickness
  float panel = half * 0.010f;   // mesh panel half-thickness
  float inner = half - 2.f * bar;      // free span between the uprights
  float inner_y = hh - 2.f * bar;

  // 4 uprights at the corners
  for (int i = 0; i < 4; i++) {
    float sx = (i & 1) ? 1.f : -1.f;
    float sz = (i & 2) ? 1.f : -1.f;
    mb.rounded_box(cx + sx * (half - bar), cy, cz + sz * (half - bar), bar, hh, bar, bar * 0.3f);
  }
  // 8 horizontal rails spanning BETWEEN the uprights (no corner overlap)
  for (int lv = 0; lv < 2; lv++) {
    float y = cy + (lv ? 1.f : -1.f) * (hh - bar);
    for (int s = 0; s < 2; s++) {
      float sg = s ? 1.f : -1.f;
      mb.rounded_box(cx, y, cz + sg * (half - bar), inner, bar, bar, bar * 0.3f);
      mb.rounded_box(cx + sg * (half - bar), y, cz, bar, bar, inner, bar * 0.3f);
    }
  }
  if (detail) {
    return;  // additive mode: the original panels are still there
  }
  // 6 mesh panels, inset so they never intersect the frame
  mb.rounded_box(cx, cy + hh - bar * 2.f - panel, cz, inner, panel, inner, panel * 0.5f);
  mb.rounded_box(cx, cy - hh + bar * 2.f + panel, cz, inner, panel, inner, panel * 0.5f);
  mb.rounded_box(cx + half - bar * 2.f - panel, cy, cz, panel, inner_y, inner, panel * 0.5f);
  mb.rounded_box(cx - half + bar * 2.f + panel, cy, cz, panel, inner_y, inner, panel * 0.5f);
  mb.rounded_box(cx, cy, cz + half - bar * 2.f - panel, inner, inner_y, panel, panel * 0.5f);
  mb.rounded_box(cx, cy, cz - half + bar * 2.f + panel, inner, inner_y, panel, panel * 0.5f);
}

// wooden crate: original silhouette, planks in relief and corner posts
void gen_wood_crate(MeshBuild& mb, float cx, float y0, float cz, float half, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  float hh = height * 0.5f;
  if (!detail) {
    mb.rounded_box(cx, cy, cz, half, hh, half, half * 0.04f);
  }
  // three horizontal planks standing proud on each side
  float lift = half * 0.05f;
  for (int p = 0; p < 3; p++) {
    float t = (p + 0.5f) / 3.f;
    float y = y0 + t * height;
    float ph = height / 3.f * 0.36f;
    mb.box_band(cx, y, cz, half + lift, ph, half + lift, half * 0.03f);
  }
  // corner posts
  for (int i = 0; i < 4; i++) {
    float sx = (i & 1) ? 1.f : -1.f;
    float sz = (i & 2) ? 1.f : -1.f;
    float k = half * 0.10f;
    mb.rounded_box(cx + sx * (half - k * 0.6f), cy, cz + sz * (half - k * 0.6f), k, hh * 0.99f, k,
                   k * 0.35f);
  }
}

// bucket: tapered staved tub with a top rim
void gen_bucket(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile;
  const int rows = 8;
  for (int r = 0; r <= rows; r++) {
    float t = (float)r / rows;
    profile.push_back({radius * (0.72f + 0.28f * t), y0 + t * height});
  }
  if (!detail) {
    mb.revolve(profile, cx, cz, 26, true, false);
  }
  mb.ring(cx, y0 + height * 0.97f, cz, radius, height * 0.05f, 26, 8);
  mb.ring(cx, y0 + height * 0.35f, cz, radius * 0.83f, height * 0.035f, 26, 8);
}

// power / fuel cell: faceted crystal core inside a metal cage
void gen_cell(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  // crystal core: bipyramid-ish revolve
  std::vector<std::array<float, 2>> profile = {
      {0.02f * radius, y0},
      {0.55f * radius, y0 + height * 0.18f},
      {0.80f * radius, y0 + height * 0.42f},
      {0.72f * radius, y0 + height * 0.62f},
      {0.38f * radius, y0 + height * 0.85f},
      {0.02f * radius, y0 + height},
  };
  if (!detail) {
    mb.revolve(profile, cx, cz, 20, false, false);
  }
  // cage: three vertical straps + two hoops
  for (int i = 0; i < 3; i++) {
    float a = 6.2831853f * i / 3.f;
    float px = cx + std::cos(a) * radius * 0.78f;
    float pz = cz + std::sin(a) * radius * 0.78f;
    mb.rounded_box(px, cy, pz, radius * 0.09f, height * 0.46f, radius * 0.09f, radius * 0.03f);
  }
  mb.ring(cx, y0 + height * 0.22f, cz, radius * 0.80f, radius * 0.07f, 22, 8);
  mb.ring(cx, y0 + height * 0.78f, cz, radius * 0.80f, radius * 0.07f, 22, 8);
}

// precursor orb: smooth faceted sphere
void gen_orb(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  float r = std::min(radius, height * 0.5f);
  const int rows = 14;
  std::vector<std::array<float, 2>> profile;
  for (int i = 0; i <= rows; i++) {
    float t = (float)i / rows;
    float ang = 3.14159265f * t;
    profile.push_back({r * std::sin(ang) * 1.0f, cy - r * std::cos(ang)});
  }
  mb.revolve(profile, cx, cz, 24, false, false);
}

// lamp: post + housing + shade
void gen_lamp(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> post = {
      {radius * 0.16f, y0},
      {radius * 0.13f, y0 + height * 0.55f},
      {radius * 0.15f, y0 + height * 0.62f},
  };
  if (!detail) {
    mb.revolve(post, cx, cz, 14, true, false);
  }
  std::vector<std::array<float, 2>> shade = {
      {radius * 0.20f, y0 + height * 0.62f},
      {radius * 0.85f, y0 + height * 0.80f},
      {radius * 0.80f, y0 + height * 0.86f},
      {radius * 0.30f, y0 + height * 1.00f},
  };
  if (!detail) {
    mb.revolve(shade, cx, cz, 18, false, true);
  }
  mb.ring(cx, y0 + height * 0.72f, cz, radius * 0.55f, height * 0.03f, 18, 8);
}

// pole / post: tapered cylinder with collar rings
void gen_pole(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile = {
      {radius * 1.00f, y0},
      {radius * 0.86f, y0 + height * 0.35f},
      {radius * 0.80f, y0 + height * 0.75f},
      {radius * 0.88f, y0 + height * 0.96f},
      {radius * 0.70f, y0 + height},
  };
  if (!detail) {
    mb.revolve(profile, cx, cz, 18, true, true);
  }
  mb.ring(cx, y0 + height * 0.30f, cz, radius * 0.90f, radius * 0.10f, 18, 8);
  mb.ring(cx, y0 + height * 0.80f, cz, radius * 0.86f, radius * 0.10f, 18, 8);
}

// gear: hub, spokes, toothed rim
void gen_gear(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  float cy = y0 + height * 0.5f;
  float th = height * 0.5f;
  std::vector<std::array<float, 2>> hub = {
      {radius * 0.22f, cy - th},
      {radius * 0.26f, cy - th * 0.6f},
      {radius * 0.26f, cy + th * 0.6f},
      {radius * 0.22f, cy + th},
  };
  if (!detail) {
    mb.revolve(hub, cx, cz, 16, true, true);
  }
  std::vector<std::array<float, 2>> body = {
      {radius * 0.80f, cy - th * 0.55f},
      {radius * 0.86f, cy - th * 0.3f},
      {radius * 0.86f, cy + th * 0.3f},
      {radius * 0.80f, cy + th * 0.55f},
  };
  if (!detail) {
    mb.revolve(body, cx, cz, 28, true, true);
  }
  const int teeth = 12;
  for (int i = 0; i < teeth; i++) {
    float a = 6.2831853f * i / teeth;
    float px = cx + std::cos(a) * radius * 0.93f;
    float pz = cz + std::sin(a) * radius * 0.93f;
    mb.rounded_box(px, cy, pz, radius * 0.12f, th * 0.5f, radius * 0.12f, radius * 0.04f);
  }
  for (int i = 0; i < 5; i++) {
    float a = 6.2831853f * i / 5.f;
    float px = cx + std::cos(a) * radius * 0.52f;
    float pz = cz + std::sin(a) * radius * 0.52f;
    mb.rounded_box(px, cy, pz, radius * 0.30f * std::abs(std::cos(a)) + radius * 0.06f,
                   th * 0.35f, radius * 0.30f * std::abs(std::sin(a)) + radius * 0.06f,
                   radius * 0.03f);
  }
}

// cloth sack: slumped bag, gathered neck, soft folds
void gen_sack(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile = {
      {radius * 0.55f, y0},
      {radius * 0.92f, y0 + height * 0.16f},
      {radius * 1.00f, y0 + height * 0.38f},
      {radius * 0.88f, y0 + height * 0.60f},
      {radius * 0.52f, y0 + height * 0.78f},
      {radius * 0.30f, y0 + height * 0.88f},
      {radius * 0.36f, y0 + height * 0.96f},
      {radius * 0.16f, y0 + height},
  };
  if (!detail) {
    mb.revolve(profile, cx, cz, 24, true, true);
  }
  // tie cord at the neck
  mb.ring(cx, y0 + height * 0.86f, cz, radius * 0.31f, radius * 0.06f, 20, 8);
}

// speaker horn: flared cone on a short body
void gen_speaker(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile = {
      {radius * 0.26f, y0},
      {radius * 0.30f, y0 + height * 0.30f},
      {radius * 0.42f, y0 + height * 0.48f},
      {radius * 0.78f, y0 + height * 0.74f},
      {radius * 1.00f, y0 + height * 0.94f},
      {radius * 0.96f, y0 + height},
  };
  if (!detail) {
    mb.revolve(profile, cx, cz, 22, true, false);
  }
  mb.ring(cx, y0 + height * 0.96f, cz, radius * 0.98f, height * 0.035f, 22, 8);
  mb.ring(cx, y0 + height * 0.44f, cz, radius * 0.40f, height * 0.03f, 18, 8);
}

// pedestal dome (orb cache): stepped base topped by a dome
void gen_dome(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> base = {
      {radius * 0.98f, y0},
      {radius * 0.94f, y0 + height * 0.16f},
      {radius * 0.80f, y0 + height * 0.22f},
      {radius * 0.76f, y0 + height * 0.36f},
  };
  if (!detail) {
    mb.revolve(base, cx, cz, 24, true, false);
  }
  const int rows = 10;
  std::vector<std::array<float, 2>> dome;
  for (int i = 0; i <= rows; i++) {
    float t = (float)i / rows;
    float ang = 1.5707963f * t;
    dome.push_back({radius * 0.76f * std::cos(ang), y0 + height * (0.36f + 0.64f * std::sin(ang))});
  }
  if (!detail) {
    mb.revolve(dome, cx, cz, 24, false, false);
  }
  mb.ring(cx, y0 + height * 0.36f, cz, radius * 0.78f, height * 0.035f, 24, 8);
}

// warp gate: standing torus arch on a plinth
void gen_warpgate(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  // plinth
  std::vector<std::array<float, 2>> plinth = {
      {radius * 0.62f, y0},
      {radius * 0.56f, y0 + height * 0.10f},
      {radius * 0.40f, y0 + height * 0.16f},
  };
  if (!detail) {
    mb.revolve(plinth, cx, cz, 20, true, true);
  }
  // upright ring built from segment blocks (a vertical torus)
  const int seg = 26;
  float ring_r = std::min(radius * 0.86f, height * 0.40f);
  float cy = y0 + height * 0.55f;
  for (int i = 0; i < seg; i++) {
    float a = 3.14159265f * ((float)i / (seg - 1));  // half-arch: 0..pi
    float px = cx + std::cos(a) * ring_r;
    float py = cy + std::sin(a) * ring_r;
    float k = radius * 0.09f;
    mb.rounded_box(px, py, cz, k, k, k * 1.6f, k * 0.35f);
  }
}

// cannon: tapered tube with reinforcement bands and a muzzle ring
void gen_cannon(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  std::vector<std::array<float, 2>> profile = {
      {radius * 0.90f, y0},
      {radius * 0.86f, y0 + height * 0.18f},
      {radius * 0.68f, y0 + height * 0.46f},
      {radius * 0.60f, y0 + height * 0.78f},
      {radius * 0.66f, y0 + height * 0.95f},
      {radius * 0.58f, y0 + height},
  };
  if (!detail) {
    mb.revolve(profile, cx, cz, 22, true, false);
  }
  mb.ring(cx, y0 + height * 0.20f, cz, radius * 0.88f, height * 0.03f, 22, 8);
  mb.ring(cx, y0 + height * 0.55f, cz, radius * 0.66f, height * 0.028f, 22, 8);
  mb.ring(cx, y0 + height * 0.96f, cz, radius * 0.66f, height * 0.03f, 22, 8);
}

// natural rock: irregular deformed sphere (deterministic hash noise)
void gen_rock(MeshBuild& mb, float cx, float y0, float cz, float radius, float height, bool detail) {
  const int rows = 10, cols = 18;
  float cy = y0 + height * 0.5f;
  auto noise = [](int a, int b) {
    u32 h = (u32)(a * 73856093) ^ (u32)(b * 19349663);
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return (float)(h & 0xffff) / 65535.f;
  };
  std::vector<std::vector<u32>> grid;
  for (int r = 0; r <= rows; r++) {
    float t = (float)r / rows;
    float ang = 3.14159265f * t;
    std::vector<u32> row;
    for (int s = 0; s <= cols; s++) {
      float a = 6.2831853f * (float)s / cols;
      int sw = s % cols;
      float bump = 0.78f + 0.34f * noise(r, sw) + 0.12f * noise(r / 2, sw / 2);
      float rr = radius * std::sin(ang) * bump;
      float yy = cy - height * 0.5f * std::cos(ang) * bump;
      float nx = std::cos(a) * std::sin(ang), ny = -std::cos(ang), nz = std::sin(a) * std::sin(ang);
      row.push_back(mb.add(cx + std::cos(a) * rr, yy, cz + std::sin(a) * rr, nx, ny, nz));
    }
    grid.push_back(row);
  }
  for (int r = 0; r < rows; r++) {
    for (int s = 0; s < cols; s++) {
      mb.quad(grid[r][s], grid[r][s + 1], grid[r + 1][s + 1], grid[r + 1][s]);
    }
  }
}

// ---- prop registry ---------------------------------------------------------

enum class PropShape {
  Barrel,
  MetalCrate,
  WoodCrate,
  Bucket,
  Cell,
  Orb,
  Lamp,
  Pole,
  Gear,
  Rock,
  Sack,
  Speaker,
  Dome,
  WarpGate,
  Cannon,
  Cage,
};

// Replace  : the original is a crude primitive — the rebuilt shape is strictly
//            better (spheres instead of faceted blobs, staved barrels…).
// Augment  : the original already carries modelled detail (rivets, trims). We
//            KEEP all of it and add the new geometry on top, so the prop can
//            never come out poorer than it started.
enum class PropMode { Replace, Augment };

struct PropRule {
  const char* prefix;
  PropShape shape;
  bool metallic;
  PropMode mode;
};

// Sandover Village (village1) + its shared props. Order matters: first match.
const PropRule kPropRules[] = {
    // crude primitives → rebuilt outright
    {"money", PropShape::Orb, true, PropMode::Replace},
    {"kickrock", PropShape::Rock, false, PropMode::Replace},
    {"lrocklrg", PropShape::Rock, false, PropMode::Replace},
    {"ecoventrock", PropShape::Rock, false, PropMode::Replace},
    // detailed props → original kept, modern relief added on top
    {"crate-barrel", PropShape::Barrel, false, PropMode::Augment},
    {"barrel-", PropShape::Barrel, false, PropMode::Augment},
    {"crate-bucket", PropShape::Bucket, false, PropMode::Augment},
    {"crate-wood", PropShape::WoodCrate, false, PropMode::Augment},
    {"crate-iron", PropShape::Cage, true, PropMode::Augment},
    {"crate-steel", PropShape::MetalCrate, true, PropMode::Augment},
    {"crate-darkeco", PropShape::MetalCrate, true, PropMode::Augment},
    {"fuel-cell", PropShape::Cell, true, PropMode::Augment},
    {"fuelcell", PropShape::Cell, true, PropMode::Augment},
    {"hutlamp", PropShape::Lamp, false, PropMode::Augment},
    {"grottopole", PropShape::Pole, false, PropMode::Augment},
    {"mayorgears", PropShape::Gear, true, PropMode::Augment},
    {"ecovalve", PropShape::Gear, true, PropMode::Augment},
    {"sack", PropShape::Sack, false, PropMode::Augment},
    {"speaker", PropShape::Speaker, true, PropMode::Augment},
    {"orb-cache-top", PropShape::Dome, false, PropMode::Augment},
    {"warpgate", PropShape::WarpGate, true, PropMode::Augment},
    {"mistycannon", PropShape::Cannon, true, PropMode::Augment},
};

const PropRule* find_prop_rule(const std::string& name) {
  for (const auto& r : kPropRules) {
    if (name.rfind(r.prefix, 0) == 0) {
      return &r;
    }
  }
  return nullptr;
}

// closest point on a triangle to p, returned with its barycentric weights.
// (Ericson, Real-Time Collision Detection §5.1.5)
struct ClosestHit {
  float dist2;
  float w0, w1, w2;
};
ClosestHit closest_on_tri(const math::Vector3f& p,
                          const math::Vector3f& a,
                          const math::Vector3f& b,
                          const math::Vector3f& c) {
  math::Vector3f ab = b - a, ac = c - a, ap = p - a;
  float d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0.f && d2 <= 0.f) {
    return {(p - a).squared_length(), 1.f, 0.f, 0.f};
  }
  math::Vector3f bp = p - b;
  float d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0.f && d4 <= d3) {
    return {(p - b).squared_length(), 0.f, 1.f, 0.f};
  }
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
    float v = d1 / (d1 - d3);
    math::Vector3f q = a + ab * v;
    return {(p - q).squared_length(), 1.f - v, v, 0.f};
  }
  math::Vector3f cp = p - c;
  float d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0.f && d5 <= d6) {
    return {(p - c).squared_length(), 0.f, 0.f, 1.f};
  }
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
    float w = d2 / (d2 - d6);
    math::Vector3f q = a + ac * w;
    return {(p - q).squared_length(), 1.f - w, 0.f, w};
  }
  float va = d3 * d6 - d5 * d4;
  if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    math::Vector3f q = b + (c - b) * w;
    return {(p - q).squared_length(), 0.f, 1.f - w, w};
  }
  float denom = 1.f / (va + vb + vc);
  float v = vb * denom, w = vc * denom;
  math::Vector3f q = a + ab * v + ac * w;
  return {(p - q).squared_length(), 1.f - v - w, v, w};
}

}  // namespace

void MercModelGroup::replace_demo_models() {
  // OFF BY DEFAULT: the rebuilt crates did not match the original art.
  // The original props ship as-is; set PC_REMAKE_PROPS=1 to
  // switch the parametric versions back on for experimentation.
  {
    const char* env = getenv("PC_REMAKE_PROPS");
    if (!env || env[0] != '1') {
      return;
    }
  }

  // donor envmap (an effect that already reflects) for metal props
  u32 donor_envmap_tex = 0;
  DrawMode donor_envmap_mode;
  bool have_donor = false;
  for (auto& model : models) {
    for (auto& eff : model.effects) {
      if (eff.has_envmap) {
        donor_envmap_tex = eff.envmap_texture;
        donor_envmap_mode = eff.envmap_mode;
        have_donor = true;
        break;
      }
    }
    if (have_donor) {
      break;
    }
  }

  for (auto& model : models) {
    const PropRule* rule = find_prop_rule(model.name);
    if (!rule) {
      continue;
    }

    // Gather EVERY eligible sub-mesh. Props are multi-material (metal frame +
    // glass panel + painted trim); rebuilding into a single draw would paint
    // the whole object with one texture, so we keep them all and re-assign the
    // new geometry to whichever original material it sits closest to.
    struct SrcVert {
      float x, y, z, u, v;
    };
    struct SrcTri {
      u32 a, b, c;
      u32 slot;  // index into `slots`
    };
    struct Slot {
      MercEffect* eff;
      MercDraw* draw;
    };
    std::vector<Slot> slots;
    std::vector<SrcVert> src;
    std::vector<u32> src_vi;  // original vertex index behind each src entry
    std::vector<SrcTri> src_tris;
    const bool augment = (rule->mode == PropMode::Augment);
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    const MercVertex* ref = nullptr;

    for (auto& eff : model.effects) {
      if (eff.has_mod_draw) {
        continue;  // blendshape-streamed geometry: never touch
      }
      for (auto& d : eff.all_draws) {
        if (d.eye_id != 0xff || d.index_count < 3) {
          continue;
        }
        u32 slot = (u32)slots.size();
        slots.push_back({&eff, &d});
        u32 base = (u32)src.size();
        for (u32 i = 0; i < d.index_count; i++) {
          u32 vi = indices[d.first_index + i];
          if (vi == UINT32_MAX || vi >= vertices.size()) {
            // keep positions aligned with the strip: push a degenerate marker
            src.push_back({1e30f, 1e30f, 1e30f, 0.f, 0.f});
            src_vi.push_back(UINT32_MAX);
            continue;
          }
          src_vi.push_back(vi);
          const auto& v = vertices[vi];
          if (!ref) {
            ref = &v;
          }
          mn[0] = std::min(mn[0], v.x);
          mn[1] = std::min(mn[1], v.y);
          mn[2] = std::min(mn[2], v.z);
          mx[0] = std::max(mx[0], v.x);
          mx[1] = std::max(mx[1], v.y);
          mx[2] = std::max(mx[2], v.z);
          src.push_back({v.x, v.y, v.z, v.st[0], v.st[1]});
        }
        u32 count = (u32)src.size() - base;
        if (d.no_strip) {
          for (u32 i = 0; i + 2 < count; i += 3) {
            src_tris.push_back({base + i, base + i + 1, base + i + 2, slot});
          }
        } else {
          for (u32 i = 2; i < count; i++) {
            src_tris.push_back({base + i - 2, base + i - 1, base + i, slot});
          }
        }
      }
    }
    if (!ref || slots.empty() || src_tris.empty()) {
      continue;
    }
    // drop triangles touching a degenerate marker
    src_tris.erase(std::remove_if(src_tris.begin(), src_tris.end(),
                                  [&](const SrcTri& t) {
                                    return src[t.a].x > 1e29f || src[t.b].x > 1e29f ||
                                           src[t.c].x > 1e29f;
                                  }),
                   src_tris.end());
    if (src_tris.empty()) {
      continue;
    }
    MercVertex ref_copy = *ref;

    float cx = (mn[0] + mx[0]) * 0.5f;
    float cz = (mn[2] + mx[2]) * 0.5f;
    float height = mx[1] - mn[1];
    float radius = std::max(mx[0] - mn[0], mx[2] - mn[2]) * 0.5f;
    if (height < 1e-3f || radius < 1e-3f) {
      continue;
    }

    MeshBuild mb;
    mb.ref = &ref_copy;
    switch (rule->shape) {
      case PropShape::Barrel:
        gen_barrel(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::MetalCrate:
        gen_metal_crate(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::WoodCrate:
        gen_wood_crate(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Bucket:
        gen_bucket(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Cell:
        gen_cell(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Orb:
        gen_orb(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Lamp:
        gen_lamp(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Pole:
        gen_pole(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Gear:
        gen_gear(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Rock:
        gen_rock(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Sack:
        gen_sack(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Speaker:
        gen_speaker(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Dome:
        gen_dome(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::WarpGate:
        gen_warpgate(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Cannon:
        gen_cannon(mb, cx, mn[1], cz, radius, height, augment);
        break;
      case PropShape::Cage:
        gen_cage(mb, cx, mn[1], cz, radius, height, augment);
        break;
    }
    if (mb.idx.size() < 3) {
      continue;
    }

    // MATERIAL + UV TRANSFER: project each new vertex onto the closest point of
    // the ORIGINAL mesh; take its interpolated texture coordinates AND remember
    // which sub-mesh (material) it landed on.
    std::vector<u32> vert_slot(mb.verts.size(), 0);
    for (size_t vi = 0; vi < mb.verts.size(); vi++) {
      auto& nv = mb.verts[vi];
      math::Vector3f p(nv.x, nv.y, nv.z);
      float best = 1e30f;
      float bu = src[src_tris[0].a].u, bv = src[src_tris[0].a].v;
      u32 bslot = src_tris[0].slot;
      for (const auto& t : src_tris) {
        const auto& A = src[t.a];
        const auto& B = src[t.b];
        const auto& C = src[t.c];
        auto hit = closest_on_tri(p, math::Vector3f(A.x, A.y, A.z), math::Vector3f(B.x, B.y, B.z),
                                  math::Vector3f(C.x, C.y, C.z));
        if (hit.dist2 < best) {
          best = hit.dist2;
          bu = A.u * hit.w0 + B.u * hit.w1 + C.u * hit.w2;
          bv = A.v * hit.w0 + B.v * hit.w1 + C.v * hit.w2;
          bslot = t.slot;
        }
      }
      nv.st[0] = bu;
      nv.st[1] = bv;
      vert_slot[vi] = bslot;
    }

    // bucket the new triangles per material (majority vote of its vertices)
    std::vector<std::vector<u32>> per_slot(slots.size());
    for (size_t t = 0; t + 2 < mb.idx.size(); t += 3) {
      u32 a = mb.idx[t], b = mb.idx[t + 1], c = mb.idx[t + 2];
      u32 s = vert_slot[a];
      if (vert_slot[b] == vert_slot[c]) {
        s = vert_slot[b];
      }
      per_slot[s].push_back(a);
      per_slot[s].push_back(b);
      per_slot[s].push_back(c);
    }

    // In Augment mode the original triangles are re-emitted first (converted
    // from strips to a plain list) and the new relief is appended after them,
    // so nothing the artists modelled is ever lost.
    std::vector<std::vector<u32>> orig_per_slot(slots.size());
    if (augment) {
      for (const auto& t : src_tris) {
        u32 a = src_vi[t.a], b = src_vi[t.b], c = src_vi[t.c];
        if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX || a == b || b == c || a == c) {
          continue;
        }
        auto& dst = orig_per_slot[t.slot];
        dst.push_back(a);
        dst.push_back(b);
        dst.push_back(c);
      }
    }

    // emit one index range per material and repoint the original draws
    u32 vbase = (u32)vertices.size();
    vertices.insert(vertices.end(), mb.verts.begin(), mb.verts.end());
    u32 total_tris = 0;
    for (size_t s = 0; s < slots.size(); s++) {
      auto* d = slots[s].draw;
      if (per_slot[s].empty() && orig_per_slot[s].empty()) {
        d->index_count = 0;
        d->num_triangles = 0;
        continue;
      }
      u32 ibase = (u32)indices.size();
      for (u32 vi : orig_per_slot[s]) {
        indices.push_back(vi);
      }
      for (u32 ii : per_slot[s]) {
        indices.push_back(vbase + ii);
      }
      d->first_index = ibase;
      d->index_count = (u32)(orig_per_slot[s].size() + per_slot[s].size());
      d->num_triangles = d->index_count / 3;
      d->no_strip = true;
      total_tris += d->num_triangles;
    }

    // material: metal reflects the sky (merc envmap pass)
    if (rule->metallic && have_donor) {
      for (auto& eff : model.effects) {
        if (!eff.has_mod_draw && !eff.has_envmap) {
          eff.has_envmap = true;
          eff.envmap_texture = donor_envmap_tex;
          eff.envmap_mode = donor_envmap_mode;
          break;
        }
      }
    }

    lg::info("PC-REMAKE {} '{}': {} tris across {} materials{}", augment ? "augmented" : "rebuilt",
             model.name, total_tris, slots.size(),
             rule->metallic ? " (metal, reflective)" : "");
  }
}

// PC-REMAKE: split every merc triangle in four, curving midpoints along the
// authored vertex normals. Skips eye draws (dedicated texture system) and any
// effect with modifiable draws (blendshape faces stream vertices at runtime).
// Rounds jars, baskets, machines, NPCs — every prop in the level.
// 17/10 : les logos de l'ecran titre (Naughty Dog, Jak and Daxter : modeles
// logo-* et ndi-* du niveau title) gardent leur forme d'origine. Ce sont des
// lettres extrudees a aretes vives : l'arrondi bombait les parois creuses du
// logo Naughty Dog, qui passaient devant les lettres (trainees bleues).
// 21/10 : les plateformes a eco bleu (plat-eco, citb-plat-eco, plat-eco-finalboss...) aussi : leur
// symbole bleu est une piece posee juste au-dessus du plateau ; arrondi, le plateau bombait et
// passait par-dessus le symbole (symbole a moitie masque)
static bool merc_model_keeps_original_shape(const std::string& name) {
  return name.rfind("logo-", 0) == 0 || name.rfind("ndi-", 0) == 0 ||
         name.find("plat-eco") != std::string::npos;
}

void MercModelGroup::subdivide_phong() {
  std::vector<u32> new_indices;
  new_indices.reserve(indices.size() * 5);

  std::unordered_map<u64, u32> edge_mid;

  // 17/10 (FISSURES) : normale SOUDEE par position (somme des normales des sommets qui
  // partagent la position). Aux aretes vives, les sommets sont dupliques avec des normales
  // differentes : chaque cote bombait son milieu dans sa direction -> jour entre les faces.
  // Le deplacement utilise la normale soudee, identique des deux cotes.
  static const bool qa_old_crease = getenv("QA_FISSURES_AVANT") != nullptr;
  auto qpos = [](const MercVertex& v) -> u64 {
    auto q = [](float f) -> u64 { return (u64)(u32)(s32)std::lround(f * 64.f) & 0x1fffff; };
    return (q(v.x) << 42) | (q(v.y) << 21) | q(v.z);
  };
  std::unordered_map<u64, math::Vector3f> welded_n;
  if (!qa_old_crease) {
    welded_n.reserve(vertices.size());
    for (const auto& v : vertices) {
      math::Vector3f n(v.nx, v.ny, v.nz);
      float l = n.length();
      if (l > 1e-5f) {
        auto& acc = welded_n.try_emplace(qpos(v), 0.f, 0.f, 0.f).first->second;
        acc += n / l;
      }
    }
  }
  auto welded_normal = [&](const MercVertex& v) -> math::Vector3f {
    if (!qa_old_crease) {
      auto it = welded_n.find(qpos(v));
      if (it != welded_n.end()) {
        return it->second;
      }
    }
    return math::Vector3f(v.nx, v.ny, v.nz);
  };

  auto midpoint = [&](u32 ia, u32 ib) -> u32 {
    u64 key = ia < ib ? ((u64)ia << 32) | ib : ((u64)ib << 32) | ia;
    auto it = edge_mid.find(key);
    if (it != edge_mid.end()) {
      return it->second;
    }
    const MercVertex va = vertices[ia];
    const MercVertex vb = vertices[ib];
    MercVertex m{};
    m.x = 0.5f * (va.x + vb.x);
    m.y = 0.5f * (va.y + vb.y);
    m.z = 0.5f * (va.z + vb.z);
    // Phong displacement using the authored normals
    // (17/10 : soudees par position ; original : na(va.nx, ...), nb(vb.nx, ...))
    {
      math::Vector3f na = welded_normal(va);
      math::Vector3f nb = welded_normal(vb);
      float la = na.length(), lb = nb.length();
      if (la > 1e-5f && lb > 1e-5f) {
        na = na / la;
        nb = nb / lb;
        math::Vector3f mp(m.x, m.y, m.z);
        math::Vector3f pa(va.x, va.y, va.z);
        math::Vector3f pb(vb.x, vb.y, vb.z);
        math::Vector3f disp =
            (na * (na.dot(pa - mp)) + nb * (nb.dot(pb - mp))) * 0.5f * g_tfrag_phong_strength;
        float edge_len = (pb - pa).length();
        float dl = disp.length();
        float max_d = edge_len * 0.3f;
        if (dl > max_d && dl > 1e-6f) {
          disp = disp * (max_d / dl);
        }
        m.x += disp.x();
        m.y += disp.y();
        m.z += disp.z();
      }
    }
    m.nx = 0.5f * (va.nx + vb.nx);
    m.ny = 0.5f * (va.ny + vb.ny);
    m.nz = 0.5f * (va.nz + vb.nz);
    m.st[0] = 0.5f * (va.st[0] + vb.st[0]);
    m.st[1] = 0.5f * (va.st[1] + vb.st[1]);
    for (int i = 0; i < 4; i++) {
      m.rgba[i] = (u8)(((u32)va.rgba[i] + (u32)vb.rgba[i]) / 2);
    }
    // skinning: same bone set -> blend weights; otherwise inherit vertex A
    if (va.mats[0] == vb.mats[0] && va.mats[1] == vb.mats[1] && va.mats[2] == vb.mats[2]) {
      for (int i = 0; i < 3; i++) {
        m.mats[i] = va.mats[i];
        m.weights[i] = 0.5f * (va.weights[i] + vb.weights[i]);
      }
    } else {
      for (int i = 0; i < 3; i++) {
        m.mats[i] = va.mats[i];
        m.weights[i] = va.weights[i];
      }
    }
    u32 idx = (u32)vertices.size();
    vertices.push_back(m);
    edge_mid.emplace(key, idx);
    return idx;
  };

  for (auto& model : models) {
    if (merc_model_keeps_original_shape(model.name)) {
      continue;
    }
    // PC-REMAKE : un modele remplace par un glTF remodele (tous ses dessins en liste) est deja au
    // detail voulu ; le redecouper quadruplerait ses triangles pour rien. Ses dessins sont
    // recales a la fin avec les autres dessins non touches.
    if (model.pc_remodele) {
      continue;
    }
    for (auto& effect : model.effects) {
      if (effect.has_mod_draw) {
        continue;  // blendshape-streamed vertices: leave untouched
      }
      for (auto& draw : effect.all_draws) {
        if (draw.eye_id != 0xff) {
          continue;  // eye system draws
        }
        u32 new_first = (u32)new_indices.size();
        u32 tris = 0;
        // decompose this draw's strip (or triangles) and subdivide
        u32 a = UINT32_MAX, b = UINT32_MAX;
        bool parity = false;
        auto emit_tri = [&](u32 i0, u32 i1, u32 i2) {
          u32 m01 = midpoint(i0, i1);
          u32 m12 = midpoint(i1, i2);
          u32 m20 = midpoint(i2, i0);
          const u32 out[12] = {i0, m01, m20, m01, i1, m12, m20, m12, i2, m01, m12, m20};
          new_indices.insert(new_indices.end(), out, out + 12);
          tris += 4;
        };
        if (draw.no_strip) {
          for (u32 i = 0; i + 2 < draw.index_count; i += 3) {
            u32 i0 = indices[draw.first_index + i];
            u32 i1 = indices[draw.first_index + i + 1];
            u32 i2 = indices[draw.first_index + i + 2];
            if (i0 != UINT32_MAX && i1 != UINT32_MAX && i2 != UINT32_MAX) {
              emit_tri(i0, i1, i2);
            }
          }
        } else {
          for (u32 i = 0; i < draw.index_count; i++) {
            u32 c = indices[draw.first_index + i];
            if (c == UINT32_MAX) {
              a = b = UINT32_MAX;
              parity = false;
              continue;
            }
            if (a != UINT32_MAX && b != UINT32_MAX && a != b && b != c && a != c) {
              if (parity) {
                emit_tri(b, a, c);
              } else {
                emit_tri(a, b, c);
              }
            }
            if (a != UINT32_MAX || b != UINT32_MAX) {
              parity = !parity;
            }
            a = b;
            b = c;
          }
        }
        draw.first_index = new_first;
        draw.index_count = (u32)new_indices.size() - new_first;
        draw.num_triangles = tris;
        draw.no_strip = true;
      }
    }
  }

  // proof-of-work log: triangle growth per level + the exact model names
  {
    size_t old_tris = indices.size() / 3;
    size_t new_tris = new_indices.size() / 3;
    lg::info("PC-REMAKE merc subdivision: {} models, ~{}k -> ~{}k tris", models.size(),
             old_tris / 1000, new_tris / 1000);
    std::string names;
    for (auto& model : models) {
      if (!names.empty()) {
        names += ", ";
      }
      names += model.name;
    }
    lg::info("PC-REMAKE merc models: {}", names);
  }

  // untouched draws (eyes, mod effects) still reference the OLD index buffer:
  // append it wholesale and shift those draws' offsets.
  u32 old_base = (u32)new_indices.size();
  new_indices.insert(new_indices.end(), indices.begin(), indices.end());
  for (auto& model : models) {
    for (auto& effect : model.effects) {
      for (auto& draw : effect.all_draws) {
        bool untouched = (draw.eye_id != 0xff) || effect.has_mod_draw ||
                         merc_model_keeps_original_shape(model.name) || model.pc_remodele;
        if (untouched) {
          draw.first_index += old_base;
        }
      }
      // mod-draw groups also index the shared buffer
      if (effect.has_mod_draw) {
        for (auto& draw : effect.mod.fix_draw) {
          draw.first_index += old_base;
        }
        for (auto& draw : effect.mod.mod_draw) {
          draw.first_index += old_base;
        }
      }
    }
  }

  indices = std::move(new_indices);
}

void Level::serialize(Serializer& ser) {
  ser.from_ptr(&version);
  if (ser.is_loading() && version != TFRAG3_VERSION) {
    ASSERT_MSG(false, fmt::format("version mismatch when loading tfrag3 data. Got {}, expected {}, "
                                  "did you forget to re-decompile?",
                                  version, TFRAG3_VERSION));
  }

  ser.from_str(&level_name);

  if (ser.is_saving()) {
    ser.save<size_t>(textures.size());
  } else {
    textures.resize(ser.load<size_t>());
  }
  for (auto& tex : textures) {
    tex.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(index_textures.size());
  } else {
    index_textures.resize(ser.load<size_t>());
  }
  for (auto& tex : index_textures) {
    tex.serialize(ser);
  }

  for (int geom = 0; geom < 3; ++geom) {
    if (ser.is_saving()) {
      ser.save<size_t>(tfrag_trees[geom].size());
    } else {
      tfrag_trees[geom].resize(ser.load<size_t>());
    }
    for (auto& tree : tfrag_trees[geom]) {
      tree.serialize(ser);
    }
  }

  for (int geom = 0; geom < 4; ++geom) {
    if (ser.is_saving()) {
      ser.save<size_t>(tie_trees[geom].size());
    } else {
      tie_trees[geom].resize(ser.load<size_t>());
    }
    for (auto& tree : tie_trees[geom]) {
      tree.serialize(ser);
    }
  }

  if (ser.is_saving()) {
    ser.save<size_t>(shrub_trees.size());
  } else {
    shrub_trees.resize(ser.load<size_t>());
  }
  for (auto& tree : shrub_trees) {
    tree.serialize(ser);
  }

  hfrag.serialize(ser);

  collision.serialize(ser);
  merc_data.serialize(ser);

  ser.from_ptr(&version2);
  if (ser.is_loading() && version2 != TFRAG3_VERSION) {
    ASSERT_MSG(false, fmt::format(
                          "version mismatch when loading tfrag3 data (at end). Got {}, expected {}",
                          version2, TFRAG3_VERSION));
  }
}

void MercModifiableDrawGroup::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_MOD_VERT, sizeof(MercVertex) * vertices.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_DRAW_1, sizeof(MercDraw) * fix_draw.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_DRAW_2, sizeof(MercDraw) * mod_draw.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_TABLE, sizeof(u16) * vertex_lump4_addr.size());
  tracker->add(MemoryUsageCategory::BLERC, sizeof(BlercFloatData) * blerc.float_data.size());
  tracker->add(MemoryUsageCategory::BLERC, sizeof(u32) * blerc.int_data.size());
}

void MercEffect::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_DRAW, sizeof(MercDraw) * all_draws.size());
  mod.memory_usage(tracker);
}

void MercModel::memory_usage(MemoryUsageTracker* tracker) const {
  for (auto& effect : effects) {
    effect.memory_usage(tracker);
  }
}

void MercModelGroup::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_VERT, sizeof(MercVertex) * vertices.size());
  tracker->add(MemoryUsageCategory::MERC_INDEX, sizeof(u32) * indices.size());
  for (auto& model : models) {
    model.memory_usage(tracker);
  }
}

void CollisionMesh::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::COLLISION, sizeof(Vertex) * vertices.size());
}

void PackedShrubVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SHRUB_VERT, 64 * matrices.size());
  tracker->add(MemoryUsageCategory::SHRUB_VERT, sizeof(InstanceGroup) * instance_groups.size());
  tracker->add(MemoryUsageCategory::SHRUB_VERT, sizeof(Vertex) * vertices.size());
}

void ShrubTree::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SHRUB_TIME_OF_DAY, sizeof(u8) * time_of_day_colors.data.size());
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::SHRUB_DRAW, sizeof(ShrubDraw) * static_draws.size());
  tracker->add(MemoryUsageCategory::SHRUB_IND, sizeof(u32) * indices.size());
}

void InstancedStripDraw::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_INST_INDEX, sizeof(u32) * vertex_index_stream.size());
  tracker->add(MemoryUsageCategory::TIE_INST_VIS, sizeof(InstanceGroup) * instance_groups.size());
}

void PackedTieVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_CIDX, sizeof(u16) * color_indices.size());
  tracker->add(MemoryUsageCategory::TIE_MATRICES, 64 * matrices.size());
  tracker->add(MemoryUsageCategory::TIE_GRPS, sizeof(MatrixGroup) * matrix_groups.size());
  tracker->add(MemoryUsageCategory::TIE_VERTS, sizeof(Vertex) * vertices.size());
}

void TieTree::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_BVH, sizeof(VisNode) * bvh.vis_nodes.size());
  for (auto& draw : static_draws) {
    tracker->add(MemoryUsageCategory::TIE_DEINST_INDEX,
                 draw.runs.size() * sizeof(StripDraw::VertexRun));
    tracker->add(MemoryUsageCategory::TIE_DEINST_INDEX, draw.plain_indices.size() * sizeof(u32));
    tracker->add(MemoryUsageCategory::TIE_DEINST_VIS,
                 draw.vis_groups.size() * sizeof(StripDraw::VisGroup));
  }
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::TIE_TIME_OF_DAY, sizeof(u8) * colors.data.size());

  for (auto& draw : instanced_wind_draws) {
    draw.memory_usage(tracker);
  }
  tracker->add(MemoryUsageCategory::TIE_WIND_INSTANCE_INFO,
               sizeof(TieWindInstance) * wind_instance_info.size());
}

void PackedTfragVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TFRAG_VERTS,
               sizeof(PackedTfragVertices::Vertex) * vertices.size());
  tracker->add(MemoryUsageCategory::TFRAG_CLUSTER,
               sizeof(math::Vector<u16, 3>) * cluster_origins.size());
}

void TfragTree::memory_usage(MemoryUsageTracker* tracker) const {
  for (auto& draw : draws) {
    tracker->add(MemoryUsageCategory::TFRAG_INDEX, draw.runs.size() * sizeof(StripDraw::VertexRun));
    tracker->add(MemoryUsageCategory::TFRAG_INDEX, draw.plain_indices.size() * sizeof(u32));
    tracker->add(MemoryUsageCategory::TFRAG_VIS,
                 draw.vis_groups.size() * sizeof(StripDraw::VisGroup));
  }
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::TFRAG_TIME_OF_DAY, sizeof(u8) * colors.data.size());
  tracker->add(MemoryUsageCategory::TFRAG_BVH, sizeof(VisNode) * bvh.vis_nodes.size());
}

void Texture::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TEXTURE, data.size() * sizeof(u32));
}

void IndexTexture::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SPECIAL_TEXTURE, index_data.size());
  tracker->add(MemoryUsageCategory::SPECIAL_TEXTURE, 256 * 4);  // clut
}

void Hfragment::memory_usage(tfrag3::MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::HFRAG_VERTS, vertices.size() * sizeof(HfragmentVertex));
  tracker->add(MemoryUsageCategory::HFRAG_INDEX, indices.size() * sizeof(u32));
  tracker->add(MemoryUsageCategory::HFRAG_TIME_OF_DAY, time_of_day_colors.data.size() * sizeof(u8));
  tracker->add(MemoryUsageCategory::HFRAG_CORNERS, corners.size() * sizeof(HfragmentCorner));
}

void Level::memory_usage(MemoryUsageTracker* tracker) const {
  for (const auto& texture : textures) {
    texture.memory_usage(tracker);
  }
  for (const auto& texture : index_textures) {
    texture.memory_usage(tracker);
  }
  for (const auto& tftk : tfrag_trees) {
    for (const auto& tree : tftk) {
      tree.memory_usage(tracker);
    }
  }
  for (const auto& ttk : tie_trees) {
    for (const auto& tree : ttk) {
      tree.memory_usage(tracker);
    }
  }
  for (const auto& tree : shrub_trees) {
    tree.memory_usage(tracker);
  }
  hfrag.memory_usage(tracker);
  collision.memory_usage(tracker);
  merc_data.memory_usage(tracker);
}

void print_memory_usage(const tfrag3::Level& lev, int uncompressed_data_size) {
  int total_accounted = 0;
  MemoryUsageTracker mem_use;
  lev.memory_usage(&mem_use);

  std::vector<std::pair<std::string, int>> known_categories = {
      {"texture", mem_use.data[tfrag3::MemoryUsageCategory::TEXTURE]},
      {"special-texture", mem_use.data[tfrag3::MemoryUsageCategory::SPECIAL_TEXTURE]},
      {"tie-deinst-vis", mem_use.data[tfrag3::MemoryUsageCategory::TIE_DEINST_VIS]},
      {"tie-deinst-idx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_DEINST_INDEX]},
      {"tie-inst-vis", mem_use.data[tfrag3::MemoryUsageCategory::TIE_INST_VIS]},
      {"tie-inst-idx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_INST_INDEX]},
      {"tie-bvh", mem_use.data[tfrag3::MemoryUsageCategory::TIE_BVH]},
      {"tie-verts", mem_use.data[tfrag3::MemoryUsageCategory::TIE_VERTS]},
      {"tie-colors", mem_use.data[tfrag3::MemoryUsageCategory::TIE_TIME_OF_DAY]},
      {"tie-wind-inst-info", mem_use.data[tfrag3::MemoryUsageCategory::TIE_WIND_INSTANCE_INFO]},
      {"tie-cidx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_CIDX]},
      {"tie-mats", mem_use.data[tfrag3::MemoryUsageCategory::TIE_MATRICES]},
      {"tie-grps", mem_use.data[tfrag3::MemoryUsageCategory::TIE_GRPS]},
      {"tfrag-vis", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_VIS]},
      {"tfrag-idx", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_INDEX]},
      {"tfrag-vert", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_VERTS]},
      {"tfrag-colors", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_TIME_OF_DAY]},
      {"tfrag-cluster", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_CLUSTER]},
      {"tfrag-bvh", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_BVH]},
      {"shrub-colors", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_TIME_OF_DAY]},
      {"shrub-vert", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_VERT]},
      {"shrub-ind", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_IND]},
      {"shrub-draw", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_DRAW]},
      {"collision", mem_use.data[tfrag3::MemoryUsageCategory::COLLISION]},
      {"merc-vert", mem_use.data[tfrag3::MemoryUsageCategory::MERC_VERT]},
      {"merc-idx", mem_use.data[tfrag3::MemoryUsageCategory::MERC_INDEX]},
      {"merc-draw", mem_use.data[tfrag3::MemoryUsageCategory::MERC_DRAW]},
      {"merc-mod-vert", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_VERT]},
      {"merc-mod-ind", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_IND]},
      {"merc-mod-table", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_TABLE]},
      {"merc-mod-draw-1", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_DRAW_1]},
      {"merc-mod-draw-2", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_DRAW_2]},
      {"blerc", mem_use.data[tfrag3::MemoryUsageCategory::BLERC]},
      {"hfrag-verts", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_VERTS]},
      {"hfrag-index", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_INDEX]},
      {"hfrag-time-of-day", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_TIME_OF_DAY]},
      {"hfrag-corners", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_CORNERS]}

  };
  for (auto& known : known_categories) {
    total_accounted += known.second;
  }

  known_categories.push_back({"unknown", uncompressed_data_size - total_accounted});

  std::sort(known_categories.begin(), known_categories.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  for (const auto& x : known_categories) {
    if (x.second) {
      lg::print("{:30s} : {:6d} kB {:3.1f}%\n", x.first, x.second / 1024,
                100.f * (float)x.second / uncompressed_data_size);
    }
  }
}

std::size_t PreloadedVertex::hash::operator()(const PreloadedVertex& v) const {
  return std::hash<float>()(v.x) ^ std::hash<float>()(v.y) ^ std::hash<float>()(v.z) ^
         std::hash<float>()(v.s) ^ std::hash<float>()(v.t) ^ std::hash<u16>()(v.color_index);
}

std::size_t PackedTieVertices::Vertex::hash::operator()(const Vertex& v) const {
  return std::hash<float>()(v.x) ^ std::hash<float>()(v.y) ^ std::hash<float>()(v.z) ^
         std::hash<float>()(v.r) ^ std::hash<float>()(v.g) ^ std::hash<float>()(v.b) ^
         std::hash<float>()(v.a) ^ std::hash<float>()(v.s) ^ std::hash<float>()(v.t) ^
         std::hash<float>()(v.nx) ^ std::hash<float>()(v.ny) ^ std::hash<float>()(v.nz);
}

}  // namespace tfrag3
