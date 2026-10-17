#include "extract_shrub.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "decompiler/ObjectFile/LinkedObjectFile.h"
#include "decompiler/level_extractor/extract_common.h"
#include "decompiler/level_extractor/level_editor.h"

// Windows (MSVC, clang-cl) ne definit M_PI qu'avec _USE_MATH_DEFINES.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace decompiler {
using namespace level_tools;

std::array<math::Vector4f, 4> extract_shrub_matrix(const u16* data) {
  std::array<math::Vector4f, 4> result;
  for (int i = 0; i < 4; i++) {
    s32 x = data[12 + i];
    x <<= 16;
    x >>= 10;
    result[3][i] = x;
  }

  for (int vec = 0; vec < 3; vec++) {
    for (int i = 0; i < 4; i++) {
      s32 x = data[vec * 4 + i];
      x <<= 16;
      x >>= 16;
      result[vec][i] = (float)x / 4096.f;
    }
  }

  return result;
}

struct ShrubVertex {
  math::Vector<float, 3> xyz;
  math::Vector<float, 2> st;
  math::Vector<u8, 3> rgba_generic;
  bool adc = false;
};
struct DrawSettings {
  DrawMode mode;
  u32 combo_tex;
};
struct ShrubDraw {
  u32 start_vtx_idx = -1;
  AdGifData adgif;
  DrawSettings settings;
  std::vector<ShrubVertex> vertices;
};

struct ShrubFrag {
  std::vector<ShrubDraw> draws;
};

struct ShrubInstanceInfo {
  u32 proto_idx;
  u32 color_idx;
  std::array<math::Vector4f, 4> mat;
  math::Vector4f bsphere;
  // PC-REMAKE 13/10 (editeur de niveaux) : origine de l'instance
  // 0 = artistes, 1 = touffe clonee, 2 = tapis d'herbe, 3 = ajout de l'editeur
  u8 src = 0;
  int add_id = 0;
};

struct ShrubProtoInfo {
  std::vector<ShrubFrag> frags;
  std::vector<ShrubInstanceInfo> instances;
  std::string name;  // PC-REMAKE : le semeur et les remakes s'y réfèrent
};

std::string debug_dump_proto_to_obj(const ShrubProtoInfo& proto) {
  std::vector<math::Vector<float, 3>> verts;
  std::vector<math::Vector<float, 2>> tcs;
  std::vector<math::Vector<int, 3>> faces;

  for (auto& frag : proto.frags) {
    for (auto& strip : frag.draws) {
      // add verts...
      ASSERT(strip.vertices.size() >= 3);

      int vert_idx = 0;

      int vtx_idx_queue[3];

      int q_idx = 0;
      int startup = 0;
      while (vert_idx < (int)strip.vertices.size()) {
        verts.push_back(strip.vertices.at(vert_idx).xyz / 65536);  // no idea
        tcs.push_back(math::Vector<float, 2>{strip.vertices.at(vert_idx).st.x(),
                                             strip.vertices.at(vert_idx).st.y()});

        vtx_idx_queue[q_idx++] = verts.size();

        // wrap the index
        if (q_idx == 3) {
          q_idx = 0;
        }

        // bump the startup
        if (startup < 3) {
          startup++;
        }

        if (startup >= 3 && strip.vertices.at(vert_idx).adc) {
          faces.push_back(
              math::Vector<int, 3>{vtx_idx_queue[0], vtx_idx_queue[1], vtx_idx_queue[2]});
        }
        vert_idx++;
      }
    }
  }

  std::string result;
  for (auto& vert : verts) {
    result += fmt::format("v {} {} {}\n", vert.x(), vert.y(), vert.z());
  }
  for (auto& tc : tcs) {
    result += fmt::format("vt {} {}\n", tc.x(), tc.y());
  }
  for (auto& face : faces) {
    result += fmt::format("f {}/{} {}/{} {}/{}\n", face.x(), face.x(), face.y(), face.y(), face.z(),
                          face.z());
  }

  return result;
}

namespace {
/*!
 * adgif shader texture id's can be "remapped". I think it allows textures to be shared.
 * So far we haven't seen this feature used, but we do have the texture map and we check it here.
 */
u32 remap_texture(u32 original, const std::vector<level_tools::TextureRemap>& map) {
  auto masked = original & 0xffffff00;
  for (auto& t : map) {
    if (t.original_texid == masked) {
      ASSERT_MSG(false, "OKAY! remapped!");
      return t.new_texid | 20;
    }
  }
  return original;
}
}  // namespace

DrawSettings adgif_to_draw_mode(const AdGifData& ad,
                                const TextureDB& tdb,
                                const std::vector<level_tools::TextureRemap>& map,
                                int count,
                                bool alpha_tpage_flag) {
  // initialize draw mode
  DrawMode current_mode;
  current_mode.set_at(true);
  current_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  current_mode.set_aref(0x26);
  current_mode.set_alpha_fail(GsTest::AlphaFail::KEEP);
  current_mode.set_zt(true);
  current_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  current_mode.set_depth_write_enable(true);  // todo, is this actual true
  current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
  current_mode.enable_fog();
  current_mode.set_ab(false);

  if (alpha_tpage_flag) {
    current_mode.set_alpha_test(DrawMode::AlphaTest::NEVER);
    current_mode.set_aref(0);
    current_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
    current_mode.set_ab(true);
  }

  // ADGIF 0
  bool weird = (u8)ad.tex0_addr != (u32)GsRegisterAddress::TEX0_1;
  if (weird) {
    lg::info("----------------  WEIRD: 0x{:x}", ad.tex0_addr);
    lg::info("i have {} verts", count);
  } else {
    if (ad.tex0_data == 0) {
      current_mode.set_decal(false);
    } else if (ad.tex0_data == 0x8'0000'0000) {
      current_mode.set_decal(true);
    } else {
      ASSERT(false);
    }
  }

  // tw/th

  // ADGIF 1
  ASSERT((u8)ad.tex1_addr == (u32)GsRegisterAddress::TEX1_1);
  u32 original_tex = ad.tex1_addr;
  u32 new_tex = remap_texture(original_tex, map);
  // try remapping it
  if (original_tex != new_tex) {
    lg::info("map from 0x{:x} to 0x{:x}", original_tex, new_tex);
  }
  // texture the texture page/texture index, and convert to a PC port texture ID
  u32 tpage = new_tex >> 20;
  u32 tidx = (new_tex >> 8) & 0b1111'1111'1111;
  u32 tex_combo = (((u32)tpage) << 16) | tidx;
  // look up the texture to make sure it's valid
  auto tex = tdb.textures.find(tex_combo);
  ASSERT(tex != tdb.textures.end());
  if (weird) {
    lg::info("tex: {}", tex->second.name);
  }

  // ADGIF 2
  ASSERT((u8)ad.mip_addr == (u32)GsRegisterAddress::MIPTBP1_1);

  // ADGIF 3
  ASSERT((u8)ad.clamp_addr == (u32)GsRegisterAddress::CLAMP_1);
  {
    bool clamp_s = ad.clamp_data & 0b001;
    bool clamp_t = ad.clamp_data & 0b100;
    current_mode.set_clamp_s_enable(clamp_s);
    current_mode.set_clamp_t_enable(clamp_t);
  }

  u64 final_alpha;

  // ADGIF 4
  if ((u8)ad.alpha_addr == (u32)GsRegisterAddress::ALPHA_1) {
    final_alpha = ad.alpha_data;
  } else {
    ASSERT(false);
    // ASSERT((u8)ad.alpha_addr == (u32)GsRegisterAddress::MIPTBP2_1);
  }

  GsAlpha reg(final_alpha);
  auto a = reg.a_mode();
  auto b = reg.b_mode();
  auto c = reg.c_mode();
  auto d = reg.d_mode();
  if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
      c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
  } else if (a == GsAlpha::BlendMode::ZERO_OR_FIXED && b == GsAlpha::BlendMode::SOURCE &&
             c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::DEST) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
  } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::DEST &&
             c == GsAlpha::BlendMode::ZERO_OR_FIXED && d == GsAlpha::BlendMode::DEST) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
  } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::SOURCE &&
             c == GsAlpha::BlendMode::SOURCE && d == GsAlpha::BlendMode::SOURCE) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
  } else if (a == GsAlpha::BlendMode::SOURCE && b == GsAlpha::BlendMode::ZERO_OR_FIXED &&
             c == GsAlpha::BlendMode::DEST && d == GsAlpha::BlendMode::DEST) {
    current_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
  } else {
    // unsupported blend: a 0 b 2 c 2 d 1
    // lg::error("unsupported blend: a {} b {} c {} d {}", (int)a, (int)b, (int)c, (int)d);
    //      ASSERT(false);
  }

  return {current_mode, tex_combo};
}

ShrubProtoInfo extract_proto(const shrub_types::PrototypeBucketShrub& proto,
                             const TextureDB& tdb,
                             const std::vector<level_tools::TextureRemap>& map,
                             GameVersion version) {
  ShrubProtoInfo result;
  for (int frag_idx = 0; frag_idx < proto.generic_geom.length; frag_idx++) {
    auto& frag_out = result.frags.emplace_back();
    auto& frag = proto.generic_geom.shrubs.at(frag_idx);

    std::vector<AdGifData> adgif_data;
    adgif_data.resize(frag.textures.size() / sizeof(AdGifData));
    memcpy(adgif_data.data(), frag.textures.data(), frag.textures.size());

    if (frag_idx == 0 && proto.name == "vil2-cattail.mb") {
      lg::info("Skipping broken village2 thing");
      continue;
    }

    for (size_t i = 0; i < adgif_data.size(); i++) {
      auto& draw = frag_out.draws.emplace_back();
      draw.adgif = adgif_data[i];

      const auto& ag = adgif_data[i];
      int count = (ag.tex1_addr >> 32) & 0xfff;  // drop the eop flag
      draw.start_vtx_idx = ((ag.tex0_addr >> 32) & 0xffff) / 3;

      if (i > 0) {
        auto& prev_draw = frag_out.draws[frag_out.draws.size() - 2];
        ASSERT(prev_draw.start_vtx_idx + prev_draw.vertices.size() + 3 == draw.start_vtx_idx);
      }

      for (int vert_idx = 0; vert_idx < count; vert_idx++) {
        auto& vert_out = draw.vertices.emplace_back();
        s16 vert_data[3];
        memcpy(vert_data, frag.vtx.data() + sizeof(u16) * 3 * (vert_idx + draw.start_vtx_idx),
               3 * sizeof(u16));
        vert_out.xyz = math::Vector3f(vert_data[0], vert_data[1], vert_data[2]);

        s16 st_data[2];
        memcpy(st_data, frag.stq.data() + sizeof(u16) * 2 * (vert_idx + draw.start_vtx_idx),
               2 * sizeof(u16));
        vert_out.st = math::Vector2f(st_data[0], st_data[1]);
        vert_out.adc = (st_data[0] & 1) == 0;  // adc in the low bit of texture coordinate

        memcpy(vert_out.rgba_generic.data(), frag.col.data() + 3 * (vert_idx + draw.start_vtx_idx),
               3);
        ASSERT(3 * (vert_idx + draw.start_vtx_idx) + 3 <= frag.col.size());
      }

      bool alpha_tpage_flag = false;
      if (version > GameVersion::Jak1) {
        alpha_tpage_flag = proto.flags & 0x4;  // tpage-alpha
      }
      draw.settings = adgif_to_draw_mode(ag, tdb, map, count, alpha_tpage_flag);
    }

    ASSERT(frag.vtx_cnt * 3 * sizeof(u16) <= frag.vtx.size());
  }

  /*
  file_util::write_text_file(
      file_util::get_file_path({fmt::format("debug_out/shrub/{}.obj", proto.name)}),
      debug_dump_proto_to_obj(result));
      */
  return result;
}

// PC-REMAKE: the beach pebbles are flat sprite quads (proto names containing
// "pebble"). Rebuild each sprite quad as a small squashed 3D pebble with the
// SAME footprint and height, keeping the original texture (UVs pinned inside
// the stone of that very quad in the atlas) and the designer tint (mean quad
// color, shaded by the fake normal so the pebble reads as round).
namespace {
u32 pebble_hash(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

void remake_pebble_proto(ShrubProtoInfo& proto, const std::string& name) {
  struct Quad {
    math::Vector3f mn, mx;
    math::Vector2f st_mn, st_mx;
    float col[3];
  };
  std::vector<Quad> quads;
  const ShrubDraw* template_draw = nullptr;
  for (auto& frag : proto.frags) {
    for (auto& draw : frag.draws) {
      if (draw.vertices.size() < 4) {
        continue;
      }
      if (!template_draw) {
        template_draw = &draw;
      }
      const auto& vs = draw.vertices;
      size_t i = 0;
      while (i + 3 < vs.size()) {
        // a sprite quad shows up as a fresh strip start: [adc F, F, T, T]
        if (!vs[i].adc && !vs[i + 1].adc && vs[i + 2].adc && vs[i + 3].adc) {
          Quad q;
          q.mn = q.mx = vs[i].xyz;
          q.st_mn = q.st_mx = vs[i].st;
          q.col[0] = q.col[1] = q.col[2] = 0.f;
          for (size_t k = i; k < i + 4; k++) {
            for (int c = 0; c < 3; c++) {
              q.mn[c] = std::min(q.mn[c], vs[k].xyz[c]);
              q.mx[c] = std::max(q.mx[c], vs[k].xyz[c]);
              q.col[c] += vs[k].rgba_generic[c] / 4.f;
            }
            for (int c = 0; c < 2; c++) {
              q.st_mn[c] = std::min(q.st_mn[c], vs[k].st[c]);
              q.st_mx[c] = std::max(q.st_mx[c], vs[k].st[c]);
            }
          }
          quads.push_back(q);
          i += 4;
        } else {
          i++;
        }
      }
    }
  }
  if (quads.empty() || !template_draw) {
    return;
  }

  ShrubDraw out;
  out.adgif = template_draw->adgif;
  out.settings = template_draw->settings;
  out.start_vtx_idx = 0;
  auto& vs_out = out.vertices;
  const int R = 8;   // latitude bands
  const int S = 16;  // segments around — a 16-gon silhouette reads as round
  for (size_t qi = 0; qi < quads.size(); qi++) {
    const auto& q = quads[qi];
    const float cx = (q.mn.x() + q.mx.x()) * 0.5f;
    const float cz = (q.mn.z() + q.mx.z()) * 0.5f;
    const float rx = (q.mx.x() - q.mn.x()) * 0.5f;
    const float rz = (q.mx.z() - q.mn.z()) * 0.5f;
    const float r_xz = std::max(std::max(rx, rz), 1.f) * 0.88f;
    // ground end = the vertical extreme closest to the proto origin
    const float ybot = (std::abs(q.mn.y()) <= std::abs(q.mx.y())) ? q.mn.y() : q.mx.y();
    const float ytop = (ybot == q.mn.y()) ? q.mx.y() : q.mn.y();
    const float up = (ytop >= ybot) ? 1.f : -1.f;
    // vrai galet : ellipsoïde asymétrique (dessous plat enterré, dessus bombé),
    // émis en SOUPE DE TRIANGLES explicites (adc F,F,T par face) — les formes
    // « triangle » persistaient à travers 3 maillages en rubans, on n'accorde
    // plus aucune confiance à l'interprétation des strips en aval.
    const u32 hsh = pebble_hash((u32)qi * 977u + (u32)name.size() * 131u);
    const float ang0 = (hsh & 255) * (2.f * (float)M_PI / 256.f);
    const float elong = 1.1f + ((hsh >> 8) & 127) / 127.f * 0.4f;
    const float h_top = std::max(std::abs(ytop - ybot) * 0.45f, r_xz * 0.35f);
    const float h_bot = h_top * 0.35f;
    const float sxe = r_xz * elong;
    const float sze = r_xz / elong;
    const math::Vector2f uc = (q.st_mn + q.st_mx) * 0.5f;
    const math::Vector2f ur = (q.st_mx - q.st_mn) * 0.5f * 0.55f;
    auto emit_vert = [&](float th, float ph, bool adc) {
      const float ny = std::cos(ph);
      const float sph = std::sin(ph);
      const float nx = std::cos(th) * sph;
      const float nz = std::sin(th) * sph;
      auto& v = vs_out.emplace_back();
      const float hy = (ny >= 0.f) ? h_top : h_bot;
      v.xyz = math::Vector3f(cx + nx * sxe, ybot + up * (h_bot + ny * hy) * 0.9f, cz + nz * sze);
      const float duv = std::pow(std::max(sph, 0.f), 0.75f);
      v.st = math::Vector2f(uc.x() + std::cos(th) * duv * ur.x(),
                            uc.y() + std::sin(th) * duv * ur.y());
      v.adc = adc;
      const float shade = 0.85f + 0.15f * std::max(0.f, ny * up);
      for (int c = 0; c < 3; c++) {
        v.rgba_generic[c] = (u8)std::min(255.f, std::max(0.f, q.col[c] * shade));
      }
    };
    for (int ri = 0; ri < R; ri++) {
      const float p0 = (float)M_PI * ri / R;
      const float p1 = (float)M_PI * (ri + 1) / R;
      for (int si = 0; si < S; si++) {
        const float t0 = 2.f * (float)M_PI * si / S + ang0;
        const float t1 = 2.f * (float)M_PI * (si + 1) / S + ang0;
        emit_vert(t0, p0, false);
        emit_vert(t0, p1, false);
        emit_vert(t1, p0, true);
        emit_vert(t1, p0, false);
        emit_vert(t0, p1, false);
        emit_vert(t1, p1, true);
      }
    }
  }
  const size_t n_quads = quads.size();
  proto.frags.clear();
  auto& frag = proto.frags.emplace_back();
  frag.draws.push_back(std::move(out));
  lg::info("PC-REMAKE pebbles: '{}' {} sprite quads -> 3D pebbles, {} verts", name, n_quads,
           frag.draws.front().vertices.size());
}

// PC-REMAKE (17/08, herbe du village à la place des
// touffes noires) : les dômes « palmbush » de la PLAGE deviennent des
// touffes d'herbe façon village — deux quads en croix par dôme source, UV sur
// le rect st du quad (le créneau de texture est ré-habillé en herbe du village
// côté texture_replacements). Soupe de triangles (adc F,F,T), comme les galets.
void remake_bush_to_grass_proto(ShrubProtoInfo& proto,
                                const std::string& name,
                                const ShrubDraw* adgif_from = nullptr) {
  struct Quad {
    math::Vector3f mn, mx;
    math::Vector2f st_mn, st_mx;
    float col[3];
  };
  std::vector<Quad> quads;
  const ShrubDraw* template_draw = nullptr;
  for (auto& frag : proto.frags) {
    for (auto& draw : frag.draws) {
      if (draw.vertices.size() < 4) {
        continue;
      }
      if (!template_draw) {
        template_draw = &draw;
      }
      const auto& vs = draw.vertices;
      size_t i = 0;
      while (i + 3 < vs.size()) {
        if (!vs[i].adc && !vs[i + 1].adc && vs[i + 2].adc && vs[i + 3].adc) {
          Quad q;
          q.mn = q.mx = vs[i].xyz;
          q.st_mn = q.st_mx = vs[i].st;
          q.col[0] = q.col[1] = q.col[2] = 0.f;
          for (size_t k = i; k < i + 4; k++) {
            for (int c = 0; c < 3; c++) {
              q.mn[c] = std::min(q.mn[c], vs[k].xyz[c]);
              q.mx[c] = std::max(q.mx[c], vs[k].xyz[c]);
              q.col[c] += vs[k].rgba_generic[c] / 4.f;
            }
            for (int c = 0; c < 2; c++) {
              q.st_mn[c] = std::min(q.st_mn[c], vs[k].st[c]);
              q.st_mx[c] = std::max(q.st_mx[c], vs[k].st[c]);
            }
          }
          quads.push_back(q);
          i += 4;
        } else {
          i++;
        }
      }
    }
  }
  if (quads.empty() || !template_draw) {
    return;
  }
  ShrubDraw out;
  out.adgif = adgif_from ? adgif_from->adgif : template_draw->adgif;
  out.settings = adgif_from ? adgif_from->settings : template_draw->settings;
  out.start_vtx_idx = 0;
  // avec adgif_from, les UV visent le créneau ré-habillé PLEIN CADRE
  const bool full_uv = (adgif_from != nullptr);
  auto& vs_out = out.vertices;
  for (size_t qi = 0; qi < quads.size(); qi++) {
    const auto& q = quads[qi];
    const float cx = (q.mn.x() + q.mx.x()) * 0.5f;
    const float cz = (q.mn.z() + q.mx.z()) * 0.5f;
    const float rx = std::max((q.mx.x() - q.mn.x()) * 0.5f, 1.f);
    const float rz = std::max((q.mx.z() - q.mn.z()) * 0.5f, 1.f);
    const float ybot = (std::abs(q.mn.y()) <= std::abs(q.mx.y())) ? q.mn.y() : q.mx.y();
    const float ytop = (ybot == q.mn.y()) ? q.mx.y() : q.mn.y();
    const u32 hsh = pebble_hash((u32)qi * 613u + 41u);
    const float ang = (hsh & 255) * ((float)M_PI / 256.f);
    // GROSSES touffes :
    // pleine hauteur du dôme et empreinte large
    const float h = std::max(std::abs(ytop - ybot) * 1.0f, 1536.f);
    const float w = std::max(rx, rz) * 1.05f;
    const float y0 = ybot;
    const float y1 = ybot + ((ytop >= ybot) ? h : -h);
    auto push = [&](float sx, float sy, float sz, float u, float v, bool adc) {
      auto& vv = vs_out.emplace_back();
      vv.xyz = math::Vector3f(sx, sy, sz);
      vv.st = math::Vector2f(u, v);
      vv.adc = adc;
      for (int c = 0; c < 3; c++) {
        vv.rgba_generic[c] = (u8)std::min(255.f, std::max(0.f, q.col[c]));
      }
    };
    auto emit_cross = [&](float a) {
      const float dx = std::cos(a) * w;
      const float dz = std::sin(a) * w;
      // plein cadre (0..4096 en unités st shrub) quand la texture est le
      // créneau ré-habillé ; sinon le rect st du quad source
      const math::Vector2f s0 = full_uv ? math::Vector2f(0.f, 0.f) : q.st_mn;
      const math::Vector2f s1 = full_uv ? math::Vector2f(4096.f, 4096.f) : q.st_mx;
      // deux triangles en soupe (adc F,F,T) — haut de touffe sur st_mn.y
      push(cx - dx, y0, cz - dz, s0.x(), s1.y(), false);
      push(cx - dx, y1, cz - dz, s0.x(), s0.y(), false);
      push(cx + dx, y0, cz + dz, s1.x(), s1.y(), true);
      push(cx + dx, y0, cz + dz, s1.x(), s1.y(), false);
      push(cx - dx, y1, cz - dz, s0.x(), s0.y(), false);
      push(cx + dx, y1, cz + dz, s1.x(), s0.y(), true);
    };
    emit_cross(ang);
    emit_cross(ang + (float)M_PI * 0.5f);
  }
  const size_t n = quads.size();
  proto.frags.clear();
  auto& frag2 = proto.frags.emplace_back();
  frag2.draws.push_back(std::move(out));
  lg::info("PC-REMAKE plage: '{}' {} domes -> touffes d'herbe ({} verts)", name, n,
           frag2.draws.front().vertices.size());
}
}  // namespace

// PC-REMAKE: grass densification. Prototypes whose name matches this list get
// extra jittered clones baked into the level at extraction time, so fields read
// as lush instead of sparse. Deterministic (hashed from instance position) so
// every extraction produces identical levels.
namespace {
bool is_densify_shrub_proto(const std::string& name) {
  static const char* kKeywords[] = {"grass", "weed",  "fern",  "flower", "riceplant",
                                    "plant", "tuft",  "reed",  "hay",    "wheat",
                                    "grs",   "bush"};
  for (auto* kw : kKeywords) {
    if (name.find(kw) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// PC-REMAKE (17/08) : TRANSPLANTATION du proto d'herbe du village (l'herbe
// de chez Samos) vers d'autres niveaux. Le village sérialise son proto
// dominant dans un fichier texte ; la plage le charge et l'utilise tel quel
// (géométrie en éventail + texture village via le combo global). Le fichier
// est GÉNÉRÉ chez le joueur, dans decompiler_out (rien du jeu n'est distribué) ;
// extract_all_levels extrait le village avant les autres niveaux pour qu'il
// existe quand la plage le lit.
std::string village_grass_file() {
  return (file_util::get_jak_project_dir() / "decompiler_out" / "jak1" /
          "village_grass_proto.txt")
      .string();
}

void dump_grass_proto(const ShrubProtoInfo& proto) {
  const auto path = village_grass_file();
  file_util::create_dir_if_needed_for_file(path);
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    return;
  }
  for (auto& frag : proto.frags) {
    for (auto& d : frag.draws) {
      fprintf(f, "draw %u\n", d.settings.combo_tex);
      // le mode de dessin est re-dérivé du proto hôte à la charge ; seuls le
      // combo de texture et les sommets comptent
      fprintf(f, "verts %zu\n", d.vertices.size());
      for (auto& v : d.vertices) {
        fprintf(f, "%f %f %f %f %f %u %u %u %d\n", v.xyz.x(), v.xyz.y(), v.xyz.z(), v.st.x(),
                v.st.y(), v.rgba_generic[0], v.rgba_generic[1], v.rgba_generic[2], v.adc ? 1 : 0);
      }
    }
  }
  fclose(f);
  lg::info("PC-REMAKE herbe de Samos sérialisée ({})", path);
}

bool load_grass_proto_into(ShrubProtoInfo& proto, const std::string& name) {
  FILE* f = fopen(village_grass_file().c_str(), "r");
  if (!f) {
    return false;
  }
  // gabarit de réglages : le premier draw existant du proto hôte
  const ShrubDraw* tpl = nullptr;
  for (auto& fr : proto.frags) {
    for (auto& d : fr.draws) {
      tpl = &d;
      break;
    }
    if (tpl) {
      break;
    }
  }
  if (!tpl) {
    fclose(f);
    return false;
  }
  ShrubProtoInfo fresh;
  fresh.name = proto.name;
  fresh.instances = proto.instances;
  auto& frag = fresh.frags.emplace_back();
  char kind[16];
  u32 combo = 0;
  while (fscanf(f, "%15s", kind) == 1) {
    if (std::string(kind) == "draw") {
      if (fscanf(f, "%u", &combo) != 1) {
        break;
      }
      auto& d = frag.draws.emplace_back();
      d.adgif = tpl->adgif;
      d.settings = tpl->settings;
      d.settings.combo_tex = combo;  // la texture du VILLAGE, via le combo global
      d.start_vtx_idx = 0;
    } else if (std::string(kind) == "verts" && !frag.draws.empty()) {
      size_t n = 0;
      if (fscanf(f, "%zu", &n) != 1) {
        break;
      }
      for (size_t i = 0; i < n; i++) {
        float x, y, z, s, t;
        unsigned r, g, b;
        int adc;
        if (fscanf(f, "%f %f %f %f %f %u %u %u %d", &x, &y, &z, &s, &t, &r, &g, &b, &adc) != 9) {
          break;
        }
        auto& v = frag.draws.back().vertices.emplace_back();
        v.xyz = math::Vector3f(x, y, z);
        v.st = math::Vector2f(s, t);
        v.rgba_generic[0] = (u8)r;
        v.rgba_generic[1] = (u8)g;
        v.rgba_generic[2] = (u8)b;
        v.adc = adc != 0;
      }
    }
  }
  fclose(f);
  if (frag.draws.empty()) {
    return false;
  }
  proto = std::move(fresh);
  lg::info("PC-REMAKE herbe de Samos transplantée sur '{}'", name);
  return true;
}

// cheap deterministic hash -> float in [0, 1)
float hash_to_unit_float(u32 seed) {
  seed ^= seed >> 16;
  seed *= 0x7feb352du;
  seed ^= seed >> 15;
  seed *= 0x846ca68bu;
  seed ^= seed >> 16;
  return (seed & 0xffffff) / 16777216.f;
}
}  // namespace

void extract_instance(const shrub_types::InstanceShrubbery& inst,
                      std::vector<ShrubProtoInfo>& protos,
                      const std::vector<char>& densify_flags) {
  ShrubInstanceInfo result;
  result.proto_idx = inst.bucket_index;
  for (int i = 0; i < 4; i++) {
    result.bsphere[i] = inst.bsphere.data[i];
  }

  // from ee asm
  result.mat = extract_shrub_matrix(inst.origin.data);
  result.mat[3][0] += result.bsphere[0];
  result.mat[3][1] += result.bsphere[1];
  result.mat[3][2] += result.bsphere[2];
  // result.wind_index = instance.wind_index;

  result.mat[0][3] = 0.f;
  result.color_idx = inst.color_indices / 4;

  protos.at(result.proto_idx).instances.push_back(result);

  // PC-REMAKE: bake extra grass clones around the original tuft.
  if (result.proto_idx < densify_flags.size() && densify_flags[result.proto_idx]) {
    constexpr int kClones = 2;
    constexpr float kMeter = 4096.f;
    u32 seed = (u32)(s32)result.mat[3][0] ^ ((u32)(s32)result.mat[3][2] << 11) ^
               ((u32)(s32)result.mat[3][1] << 22);
    for (int c = 0; c < kClones; c++) {
      ShrubInstanceInfo clone = result;
      float angle = hash_to_unit_float(seed + c * 7919u) * 6.2831853f;
      float radius = (0.45f + 0.5f * hash_to_unit_float(seed + c * 104729u + 13u)) * kMeter;
      float scale = 0.78f + 0.22f * hash_to_unit_float(seed + c * 15485863u + 29u);
      clone.mat[3][0] += std::cos(angle) * radius;
      clone.mat[3][2] += std::sin(angle) * radius;
      for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
          clone.mat[col][row] *= scale;
        }
      }
      clone.bsphere[0] = clone.mat[3][0];
      clone.bsphere[1] = clone.mat[3][1];
      clone.bsphere[2] = clone.mat[3][2];
      clone.src = 1;
      protos.at(clone.proto_idx).instances.push_back(clone);
    }
  }
}

/*!
 * Transform a point in a prototype to the actual point location in the game world.
 */
math::Vector<float, 3> transform_shrub(const std::array<math::Vector4f, 4> mat,
                                       const math::Vector3f& pt) {
  auto temp = mat[0] * pt.x() + mat[1] * pt.y() + mat[2] * pt.z() + mat[3];
  math::Vector3f result;
  result.x() = temp.x();
  result.y() = temp.y();
  result.z() = temp.z();
  return result;
}

/*!
 * Dump the entire tie tree to an obj. Used to debug the transform_tie function. If we get this
 * right, it should fit in with .obj's produced from the tfrag debug.
 */
std::string dump_full_to_obj(const std::vector<ShrubProtoInfo>& protos) {
  std::vector<math::Vector<float, 3>> verts;
  std::vector<math::Vector<int, 3>> faces;

  for (auto& proto : protos) {
    for (auto& inst : proto.instances) {
      auto& mat = inst.mat;
      for (auto& frag : proto.frags) {
        for (auto& strip : frag.draws) {
          // add verts...
          ASSERT(strip.vertices.size() >= 3);

          int vert_idx = 0;

          int vtx_idx_queue[3];

          int q_idx = 0;
          int startup = 0;
          while (vert_idx < (int)strip.vertices.size()) {
            verts.push_back(transform_shrub(mat, strip.vertices.at(vert_idx).xyz) /
                            65536);  // no idea

            vtx_idx_queue[q_idx++] = verts.size();

            // wrap the index
            if (q_idx == 3) {
              q_idx = 0;
            }

            // bump the startup
            if (startup < 3) {
              startup++;
            }

            if (startup >= 3 && strip.vertices.at(vert_idx).adc) {
              faces.push_back(
                  math::Vector<int, 3>{vtx_idx_queue[0], vtx_idx_queue[1], vtx_idx_queue[2]});
            }
            vert_idx++;
          }
        }
      }
    }
  }

  std::string result;
  for (auto& vert : verts) {
    result += fmt::format("v {} {} {}\n", vert.x(), vert.y(), vert.z());
  }

  for (auto& face : faces) {
    result += fmt::format("f {}/{} {}/{} {}/{}\n", face.x(), face.x(), face.y(), face.y(), face.z(),
                          face.z());
  }

  return result;
}

void make_draws(tfrag3::Level& lev,
                tfrag3::ShrubTree& tree_out,
                const std::vector<ShrubProtoInfo>& protos,
                const TextureDB& tdb) {
  std::vector<std::vector<u32>> indices_regrouped_by_draw;
  std::unordered_map<u32, std::vector<u32>> static_draws_by_tex;
  size_t global_vert_counter = 0;
  for (u32 proto_idx = 0; proto_idx < protos.size(); proto_idx++) {
    auto& proto = protos[proto_idx];
    // packed_vert_indices[frag][draw] = {start, end}
    std::vector<std::vector<std::pair<int, int>>> packed_vert_indices;

    for (size_t frag_idx = 0; frag_idx < proto.frags.size(); frag_idx++) {
      auto& frag_inds = packed_vert_indices.emplace_back();
      auto& frag = proto.frags[frag_idx];

      for (auto& draw : frag.draws) {
        int start = tree_out.packed_vertices.vertices.size();
        for (auto& vert : draw.vertices) {
          tree_out.packed_vertices.vertices.push_back(
              {vert.xyz.x(),
               vert.xyz.y(),
               vert.xyz.z(),
               vert.st.x(),
               vert.st.y(),
               {vert.rgba_generic[0], vert.rgba_generic[1], vert.rgba_generic[2]}});
        }
        int end = tree_out.packed_vertices.vertices.size();
        frag_inds.emplace_back(start, end);
      }
    }

    for (auto& inst : proto.instances) {
      u32 matrix_idx = tree_out.packed_vertices.matrices.size();
      tree_out.packed_vertices.matrices.push_back(inst.mat);

      for (size_t frag_idx = 0; frag_idx < proto.frags.size(); frag_idx++) {
        auto& frag = proto.frags[frag_idx];  // shared info for all instances of this frag

        for (size_t draw_idx = 0; draw_idx < frag.draws.size(); draw_idx++) {
          auto& draw = frag.draws[draw_idx];

          // what texture are we using?
          u32 combo_tex = draw.settings.combo_tex;

          // try looking it up in the existing textures that we have in the C++ renderer data.
          // (this is shared with tfrag)
          u32 idx_in_lev_data = UINT32_MAX;
          for (u32 i = 0; i < lev.textures.size(); i++) {
            if (lev.textures[i].combo_id == combo_tex) {
              idx_in_lev_data = i;
              break;
            }
          }

          if (idx_in_lev_data == UINT32_MAX) {
            // didn't find it, have to add a new one texture.
            auto tex_it = tdb.textures.find(combo_tex);
            if (tex_it == tdb.textures.end()) {
              bool ok_to_miss = false;  // for TIE, there's no missing textures.
              if (ok_to_miss) {
                // we're missing a texture, just use the first one.
                tex_it = tdb.textures.begin();
              } else {
                ASSERT_MSG(
                    false,
                    fmt::format(
                        "texture {} wasn't found. make sure it is loaded somehow. You may need to "
                        "include ART.DGO or GAME.DGO in addition to the level DGOs for shared "
                        "textures. tpage is {} id is {} (0x{:x})",
                        combo_tex, combo_tex >> 16, combo_tex & 0xffff, combo_tex & 0xffff));
              }
            }
            // PC-REMAKE 14/10 : pas de HD calculee pour l'herbe et les buissons (rendu
            // valide = textures d'origine) sauf pc_faithful_hd_models
            auto resolved = tdb.resolve_texture(combo_tex, tdb.faithful_hd_models);
            // add a new texture to the level data
            idx_in_lev_data = lev.textures.size();
            lev.textures.emplace_back();
            auto& new_tex = lev.textures.back();
            new_tex.combo_id = combo_tex;
            new_tex.w = resolved.w;
            new_tex.h = resolved.h;
            new_tex.debug_name = tex_it->second.name;
            new_tex.debug_tpage_name = tdb.tpage_names.at(tex_it->second.page);
            new_tex.data = std::move(resolved.rgba);
          }

          DrawMode mode = draw.settings.mode;

          // okay, we now have a texture and draw mode, let's see if we can add to an existing...
          auto existing_draws_in_tex = static_draws_by_tex.find(idx_in_lev_data);
          tfrag3::ShrubDraw* draw_to_add_to = nullptr;
          std::vector<u32>* verts_to_add_to = nullptr;
          if (existing_draws_in_tex != static_draws_by_tex.end()) {
            for (auto idx : existing_draws_in_tex->second) {
              auto& candidate_draw_out = tree_out.static_draws.at(idx);
              if (candidate_draw_out.mode == mode && candidate_draw_out.proto_idx == proto_idx) {
                draw_to_add_to = &tree_out.static_draws[idx];
                verts_to_add_to = &indices_regrouped_by_draw[idx];
              }
            }
          }

          if (!draw_to_add_to) {
            // nope, need to create a new draw
            tree_out.static_draws.emplace_back();
            static_draws_by_tex[idx_in_lev_data].push_back(tree_out.static_draws.size() - 1);
            draw_to_add_to = &tree_out.static_draws.back();
            draw_to_add_to->mode = mode;
            draw_to_add_to->tree_tex_id = idx_in_lev_data;
            // jak 1 has no per-proto visibility toggle, but the gltf exporter still uses this to
            // split the tree back up into one mesh per prototype
            draw_to_add_to->proto_idx = proto_idx;
            verts_to_add_to = &indices_regrouped_by_draw.emplace_back();
          }

          // now we have a draw, time to add vertices
          // draw_to_add_to->num_triangles += draw.vertices.size() - 2;
          tfrag3::PackedShrubVertices::InstanceGroup grp;
          grp.matrix_idx = matrix_idx;
          grp.color_index = inst.color_idx;
          grp.start_vert = packed_vert_indices.at(frag_idx).at(draw_idx).first;
          grp.end_vert = packed_vert_indices.at(frag_idx).at(draw_idx).second;
          tree_out.packed_vertices.instance_groups.push_back(grp);

          for (size_t vidx = 0; vidx < draw.vertices.size(); vidx++) {
            if (draw.vertices[vidx].adc) {
              verts_to_add_to->push_back(vidx + global_vert_counter);
              draw_to_add_to->num_triangles++;
            } else {
              verts_to_add_to->push_back(UINT32_MAX);
              verts_to_add_to->push_back(vidx + global_vert_counter - 1);
              verts_to_add_to->push_back(vidx + global_vert_counter);
            }
          }
          verts_to_add_to->push_back(UINT32_MAX);
          global_vert_counter += draw.vertices.size();
        }
      }
    }
  }

  for (size_t didx = 0; didx < tree_out.static_draws.size(); didx++) {
    auto& draw = tree_out.static_draws[didx];
    auto& inds = indices_regrouped_by_draw[didx];
    draw.num_triangles = clean_up_vertex_indices(inds);
    draw.num_indices = inds.size();
    draw.first_index_index = tree_out.indices.size();
    tree_out.indices.insert(tree_out.indices.end(), inds.begin(), inds.end());
  }

  tree_out.packed_vertices.total_vertex_count = global_vert_counter;
}

// PC-REMAKE: seed a continuous grass carpet on grass-material ground triangles
// (FF15-style fields). Uses the level's own most-common grass prototype so the
// look stays coherent, inherits time-of-day color from the nearest designer
// instance, and skips steep triangles. Deterministic. Paths/sand/dirt are
// naturally excluded because their collision material is not grass.
namespace {

// PC-REMAKE: samples the visual ground color so the carpet respects painted
// wear paths (yellowed trails) that stay "grass" in collision. Built from the
// unpacked tfrag vertices: nearest ground vertex in a 2D grid -> its draw's
// texture texel. A tuft is only planted where the ground texel is green.
struct GroundSampler {
  static constexpr float kCell = 2.f * 4096.f;  // 2 m grid
  struct Sample {
    float x, z;
    s32 tex;
    float s, t;
    // 13/10 : couleur d'eclairage peinte du sol a ce sommet (palettes par heure)
    const tfrag3::TfragTree* tree = nullptr;
    u32 cidx = 0;
  };
  std::unordered_map<u64, std::vector<Sample>> grid;
  const tfrag3::Level* level = nullptr;

  static u64 cell_key(float x, float z) {
    s32 cx = (s32)std::floor(x / kCell);
    s32 cz = (s32)std::floor(z / kCell);
    return ((u64)(u32)cx << 32) | (u64)(u32)cz;
  }

  void build(tfrag3::Level& lev) {
    level = &lev;
    // geom 0 = highest detail geometry
    for (auto& tree : lev.tfrag_trees[0]) {
      using K = tfrag3::TFragmentTreeKind;
      if (tree.kind != K::NORMAL && tree.kind != K::TRANS && tree.kind != K::DIRT) {
        continue;
      }
      tree.unpack();  // unpacked data is not serialized: safe to build here
      auto add_vert = [&](u32 vidx, s32 tex) {
        if (vidx == UINT32_MAX || vidx >= tree.unpacked.vertices.size()) {
          return;
        }
        auto& v = tree.unpacked.vertices[vidx];
        grid[cell_key(v.x, v.z)].push_back(Sample{v.x, v.z, tex, v.s, v.t, &tree, v.color_index});
      };
      for (auto& draw : tree.draws) {
        if (draw.tree_tex_id < 0) {
          continue;
        }
        for (auto& run : draw.runs) {
          for (u32 j = 0; j < run.length; j++) {
            add_vert(run.vertex0 + j, draw.tree_tex_id);
          }
        }
        for (u32 vidx : draw.plain_indices) {
          add_vert(vidx, draw.tree_tex_id);
        }
      }
    }
  }

  // texel test for one sample: green-dominant ground = grass; yellow/beige
  // wear paths have r >= g
  bool sample_is_green(const Sample& smp) const {
    if (smp.tex < 0 || smp.tex >= (s32)level->textures.size()) {
      return true;
    }
    const auto& tex = level->textures[smp.tex];
    if (tex.w <= 0 || tex.h <= 0 || tex.data.empty()) {
      return true;
    }
    float fs = smp.s - std::floor(smp.s);
    float ft = smp.t - std::floor(smp.t);
    int px = std::min((int)(fs * tex.w), tex.w - 1);
    int py = std::min((int)(ft * tex.h), tex.h - 1);
    // 3x3 box average: robust to painterly detail (golden blade tips etc.)
    float r = 0.f, g = 0.f, b = 0.f;
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        int sx = (px + dx + tex.w) % tex.w;
        int sy = (py + dy + tex.h) % tex.h;
        u32 texel = tex.data[sy * tex.w + sx];
        r += (float)(texel & 0xff);
        g += (float)((texel >> 8) & 0xff);
        b += (float)((texel >> 16) & 0xff);
        n++;
      }
    }
    r /= n;
    g /= n;
    b /= n;
    return g > r * 1.05f && g > b * 1.05f;
  }

  // 13/10 : luminance de l'eclairage PEINT du sol en (x, z), pour chacune des 8
  // palettes horaires (moyenne ponderee des 4 sommets les plus proches, 3 m)
  bool ground_light(float x, float z, float out[8]) const {
    float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float wsum = 0.f;
    const float kMaxD = 3.f * 4096.f * 3.f * 4096.f;
    for (s32 dx = -1; dx <= 1; dx++) {
      for (s32 dz = -1; dz <= 1; dz++) {
        auto it = grid.find(cell_key(x + dx * kCell, z + dz * kCell));
        if (it == grid.end()) {
          continue;
        }
        for (auto& smp : it->second) {
          if (!smp.tree || smp.cidx >= smp.tree->colors.color_count) {
            continue;
          }
          float ddx = smp.x - x;
          float ddz = smp.z - z;
          float d = ddx * ddx + ddz * ddz;
          if (d >= kMaxD) {
            continue;
          }
          float w = 1.f / (d + 0.25f * 4096.f * 4096.f);
          for (int p = 0; p < 8; p++) {
            float r = smp.tree->colors.read(smp.cidx, p, 0);
            float g = smp.tree->colors.read(smp.cidx, p, 1);
            float b = smp.tree->colors.read(smp.cidx, p, 2);
            acc[p] += w * (0.299f * r + 0.587f * g + 0.114f * b);
          }
          wsum += w;
        }
      }
    }
    if (wsum <= 0.f) {
      return false;
    }
    for (int p = 0; p < 8; p++) {
      out[p] = acc[p] / wsum;
    }
    return true;
  }

  // true when the visual ground at (x, z) looks like green grass. The nearest
  // vertex is not enough: at a path edge it often belongs to the neighbouring
  // grass polygon, which is exactly how tufts ended up ON the brown trails.
  // Every close-by sample must be green.
  bool is_green_at(float x, float z) const {
    const Sample* cand[4] = {nullptr, nullptr, nullptr, nullptr};
    float cand_d[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    const float kMaxD = 3.f * 4096.f * 3.f * 4096.f;  // search radius 3 m
    for (s32 dx = -1; dx <= 1; dx++) {
      for (s32 dz = -1; dz <= 1; dz++) {
        auto it = grid.find(cell_key(x + dx * kCell, z + dz * kCell));
        if (it == grid.end()) {
          continue;
        }
        for (auto& smp : it->second) {
          float ddx = smp.x - x;
          float ddz = smp.z - z;
          float d = ddx * ddx + ddz * ddz;
          if (d >= kMaxD) {
            continue;
          }
          // insertion dans le top-4 des plus proches
          for (int k = 0; k < 4; k++) {
            if (d < cand_d[k]) {
              for (int m = 3; m > k; m--) {
                cand_d[m] = cand_d[m - 1];
                cand[m] = cand[m - 1];
              }
              cand_d[k] = d;
              cand[k] = &smp;
              break;
            }
          }
        }
      }
    }
    if (!cand[0]) {
      return true;  // no visual ground data: fall back to collision-only
    }
    // le plus proche doit etre vert, et AUCUN voisin a moins de 1,2 m ne doit
    // etre un texel de sentier (2 m rasait trop large : plus d'herbe devant la
    // hutte de Samos)
    const float kNear = 1.2f * 4096.f * 1.2f * 4096.f;
    for (int k = 0; k < 4; k++) {
      if (!cand[k]) {
        break;
      }
      if (k > 0 && cand_d[k] > kNear) {
        break;
      }
      if (!sample_is_green(*cand[k])) {
        return false;
      }
    }
    return true;
  }
};

void seed_grass_carpet(std::vector<ShrubProtoInfo>& protos,
                       const std::vector<char>& densify_flags,
                       const std::vector<GrassSeedTri>& tris,
                       tfrag3::Level& level_for_ground,
                       const level_tools::TimeOfDayPalette* shrub_tod) {
  if (tris.empty()) {
    return;
  }
  constexpr float kMeter = 4096.f;
  // 12/10 (tapis d'herbe des images de reference) : plafond releve pour les zones de depart (voir plus bas)
  int kMaxSeeded = 15000;  // garde-fou perf par niveau

  // Densité et taille des touffes, par niveau. La plage est une grande étendue
  // plate et verte : la même règle que Sandover y plante beaucoup trop de
  // touffes, et trop grandes. Les autres niveaux gardent
  // le réglage validé.
  float area_per_tuft = 2.5f * kMeter * kMeter;  // ~1 touffe / 2.5 m^2
  float scale_lo = 0.65f, scale_span = 0.35f;    // taille relative au modèle
  if (level_for_ground.level_name == "beach") {
    // 17/08 : la plage porte désormais la VRAIE herbe de Samos (transplantée) —
    // mêmes densité et tailles que le village (le tapis de sa photo)
    area_per_tuft = 2.5f * kMeter * kMeter;
    scale_lo = 0.65f;
    scale_span = 0.35f;
  }
  // 12/10 RENDU REMAKE : un VRAI tapis dans les zones de depart (village, plage,
  // ile de l'entrainement) — densite x2.8 (1 touffe / 0,9 m2) et tailles plus
  // variees (0,50 a 1,10) pour un gazon naturel, plafond 26 000 par niveau.
  // (original : 2,5 m2, 0,65 + 0,35, plafond 15 000)
  if (level_for_ground.level_name == "village1" || level_for_ground.level_name == "beach" ||
      level_for_ground.level_name == "training") {
    area_per_tuft = 0.9f * kMeter * kMeter;
    scale_lo = 0.50f;
    scale_span = 0.60f;
    kMaxSeeded = 26000;
  }
  const float kAreaPerTuft = area_per_tuft;

  // pick the grass proto with the most designer instances
  int best_proto = -1;
  size_t best_count = 0;
  for (size_t i = 0; i < protos.size(); i++) {
    if (i < densify_flags.size() && densify_flags[i] && protos[i].instances.size() > best_count) {
      best_count = protos[i].instances.size();
      best_proto = (int)i;
    }
  }
  if (best_proto < 0 || best_count == 0) {
    return;  // level has no grass-like prototype
  }
  auto& proto = protos[best_proto];

  // visual-ground sampler: keeps the carpet off painted wear paths and
  // non-green ground (snow, scorched dirt), even where collision says "grass"
  GroundSampler ground;
  ground.build(level_for_ground);

  // subsample existing instances for nearest-color lookup
  std::vector<ShrubInstanceInfo> color_refs;
  {
    size_t step = std::max<size_t>(1, proto.instances.size() / 256);
    for (size_t i = 0; i < proto.instances.size(); i += step) {
      color_refs.push_back(proto.instances[i]);
    }
  }

  // 13/10 : calibration couleur des touffes <-> eclairage peint du sol. Pour
  // chaque touffe d'artiste : luminance de sa couleur / luminance du sol sous
  // elle, par palette horaire -> rapport median. Candidats = couleurs distinctes
  // des touffes d'artistes.
  bool calibrated = false;
  float kratio[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  float kweight[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  float gmed[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<float> gls[8];
  std::vector<std::pair<u32, std::array<float, 8>>> cand_lum;
  if (shrub_tod && shrub_tod->height > 0) {
    auto lum_of = [&](u32 cidx, int p) {
      u32 c = shrub_tod->colors.at(cidx * 8 + p);
      return 0.299f * (float)(c & 0xff) + 0.587f * (float)((c >> 8) & 0xff) +
             0.114f * (float)((c >> 16) & 0xff);
    };
    std::vector<float> ratios[8];
    std::set<u32> seen;
    for (const auto& inst : proto.instances) {
      if (inst.src != 0 || inst.color_idx >= shrub_tod->height) {
        continue;
      }
      if (seen.insert(inst.color_idx).second) {
        std::array<float, 8> l;
        for (int p = 0; p < 8; p++) {
          l[p] = lum_of(inst.color_idx, p);
        }
        cand_lum.emplace_back(inst.color_idx, l);
      }
      float gl[8];
      if (ground.ground_light(inst.mat[3][0], inst.mat[3][2], gl)) {
        for (int p = 0; p < 8; p++) {
          if (gl[p] > 4.f) {
            ratios[p].push_back(lum_of(inst.color_idx, p) / gl[p]);
          }
          gls[p].push_back(gl[p]);
        }
        static const char* qd = getenv("QA_GRASS_DUMP");
        float qx = 0, qz = 0, qr = 0;
        if (qd && sscanf(qd, "%f,%f,%f", &qx, &qz, &qr) == 3) {
          float ddx = inst.mat[3][0] / kMeter - qx, ddz = inst.mat[3][2] / kMeter - qz;
          if (ddx * ddx + ddz * ddz < qr * qr) {
            lg::info("QA-ARTISTE {:.1f} {:.1f} sol {:.0f} -> c{} {:.0f}", inst.mat[3][0] / kMeter,
                     inst.mat[3][2] / kMeter, gl[2], inst.color_idx, lum_of(inst.color_idx, 2));
          }
        }
      }
    }
    // eclairage typique du sol (mediane sous les touffes d'artistes), par palette
    for (int p = 0; p < 8; p++) {
      if (!gls[p].empty()) {
        std::nth_element(gls[p].begin(), gls[p].begin() + gls[p].size() / 2, gls[p].end());
        gmed[p] = gls[p][gls[p].size() / 2];
      }
    }
    // une palette sans assez de mesures (nuit tres sombre, sol noir) ne compte pas
    int usable = 0;
    for (int p = 0; p < 8; p++) {
      if (ratios[p].size() < 8) {
        kweight[p] = 0.f;
        continue;
      }
      std::nth_element(ratios[p].begin(), ratios[p].begin() + ratios[p].size() / 2,
                       ratios[p].end());
      kratio[p] = ratios[p][ratios[p].size() / 2];
      usable++;
    }
    calibrated = !cand_lum.empty() && usable >= 2;
    lg::info("PC-REMAKE grass carpet [{}]: couleur par eclairage du sol {} ({} couleurs, k {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f})",
             level_for_ground.level_name, calibrated ? "oui" : "NON", cand_lum.size(),
             kweight[0] * kratio[0], kweight[1] * kratio[1], kweight[2] * kratio[2],
             kweight[3] * kratio[3], kweight[4] * kratio[4], kweight[5] * kratio[5],
             kweight[6] * kratio[6], kweight[7] * kratio[7]);
  }

  // couleur d'une touffe automatique en (x, z). L'eclairage peint du sol est
  // suivi a 50 % autour de sa valeur typique : l'ombre peinte sous un palmier
  // reste lisible, mais l'herbe (texture sombre x couleur sombre, touffes
  // empilees) ne vire plus au noir — mesure a l'ecran : herbe 3x plus sombre
  // que le sol nu au meme endroit
  auto pick_color = [&](float x, float z, u32 fallback) -> u32 {
    if (!calibrated) {
      return fallback;
    }
    float gl[8];
    if (!ground.ground_light(x, z, gl)) {
      return fallback;
    }
    u32 best = fallback;
    float best_e = 1e30f;
    for (const auto& [cidx, lum] : cand_lum) {
      float e = 0.f;
      for (int p = 0; p < 8; p++) {
        // v2 : 0.5 -> 0.25 (le diagnostic « couleur unique » a prouve que les
        // plaques venaient de l'ombre PEINTE sous les palmiers ; l'ombre en temps
        // reel du soleil la remplace et suit l'heure)
        // v3 : 0.25 -> 0.10 — l'ombre des palmiers et des falaises est desormais
        // portee EN TEMPS REEL par le soleil ; l'ombre peinte, doublee, faisait des
        // plaques sombres (Rocher du Geyser, ronds noirs)
        float target = kratio[p] * (gmed[p] + 0.10f * (gl[p] - gmed[p]));
        float dlt = lum[p] - target;
        e += kweight[p] * dlt * dlt;
      }
      if (e < best_e) {
        best_e = e;
        best = cidx;
      }
    }
    return best;
  };
  // les touffes CLONEES autour des touffes d'artistes suivent la meme regle
  if (calibrated) {
    size_t recol = 0;
    for (auto& pr : protos) {
      for (auto& inst : pr.instances) {
        if (inst.src == 1) {
          inst.color_idx = pick_color(inst.mat[3][0], inst.mat[3][2], inst.color_idx);
          recol++;
        }
      }
    }
    // 13/10 (Rocher du Geyser : grandes plaques d'herbe sombre, ronds noirs)
    // les touffes d'ARTISTES de l'herbe gardent leur nuance, mais a moitie de
    // l'ecart a la valeur typique : semees clairsemees sur un sol en 2001, leurs
    // couleurs sombres etaient des touffes ; en tapis dense, ce sont des taches.
    // (original : couleur d'artiste telle quelle)
    {
      float lmed[8];
      for (int p = 0; p < 8; p++) {
        std::vector<float> v;
        for (const auto& inst : proto.instances) {
          if (inst.src == 0 && inst.color_idx < shrub_tod->height) {
            u32 c = shrub_tod->colors.at(inst.color_idx * 8 + p);
            v.push_back(0.299f * (float)(c & 0xff) + 0.587f * (float)((c >> 8) & 0xff) +
                        0.114f * (float)((c >> 16) & 0xff));
          }
        }
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        lmed[p] = v.empty() ? 0.f : v[v.size() / 2];
      }
      size_t soft = 0;
      for (size_t pi = 0; pi < protos.size(); pi++) {
        if (pi >= densify_flags.size() || !densify_flags[pi]) {
          continue;
        }
        for (auto& inst : protos[pi].instances) {
          if (inst.src != 0 || inst.color_idx >= shrub_tod->height) {
            continue;
          }
          float own[8];
          for (int p = 0; p < 8; p++) {
            u32 c = shrub_tod->colors.at(inst.color_idx * 8 + p);
            own[p] = 0.299f * (float)(c & 0xff) + 0.587f * (float)((c >> 8) & 0xff) +
                     0.114f * (float)((c >> 16) & 0xff);
          }
          u32 best = inst.color_idx;
          float best_e = 1e30f;
          for (const auto& [cidx, lum] : cand_lum) {
            float e = 0.f;
            for (int p = 0; p < 8; p++) {
              float dlt = lum[p] - (lmed[p] + 0.25f * (own[p] - lmed[p]));
              e += kweight[p] * dlt * dlt;
            }
            if (e < best_e) {
              best_e = e;
              best = cidx;
            }
          }
          if (best != inst.color_idx) {
            inst.color_idx = best;
            soft++;
          }
        }
      }
      lg::info("PC-REMAKE grass carpet [{}]: {} touffes d'artistes adoucies", level_for_ground.level_name,
               soft);
    }
    lg::info("PC-REMAKE grass carpet [{}]: {} touffes clonees recolorees", level_for_ground.level_name,
             recol);
  }

  int seeded = 0;
  for (auto& tri : tris) {
    if (seeded >= kMaxSeeded) {
      break;
    }
    math::Vector3f e1 = tri.b - tri.a;
    math::Vector3f e2 = tri.c - tri.a;
    math::Vector3f n = e1.cross(e2);
    float area2 = n.length();
    if (area2 <= 0.f) {
      continue;
    }
    // skip steep ground (normal must be mostly vertical)
    if (std::abs(n.y()) / area2 < 0.75f) {
      continue;
    }
    float area = 0.5f * area2;
    u32 tri_seed = (u32)(s32)tri.a.x() ^ ((u32)(s32)tri.a.z() << 9) ^ ((u32)(s32)tri.b.x() << 18);
    float want = area / kAreaPerTuft;
    int count = (int)want;
    if (hash_to_unit_float(tri_seed + 77u) < (want - (float)count)) {
      count++;
    }
    for (int k = 0; k < count && seeded < kMaxSeeded; k++) {
      float r1 = hash_to_unit_float(tri_seed + (u32)k * 2654435761u + 1u);
      float r2 = hash_to_unit_float(tri_seed + (u32)k * 2246822519u + 2u);
      if (r1 + r2 > 1.f) {  // uniform on the triangle
        r1 = 1.f - r1;
        r2 = 1.f - r2;
      }
      math::Vector3f pos = tri.a + e1 * r1 + e2 * r2;

      // painted wear path / non-green ground: no tuft here
      if (!ground.is_green_at(pos.x(), pos.z())) {
        continue;
      }

      ShrubInstanceInfo inst;
      inst.proto_idx = (u32)best_proto;
      float angle = hash_to_unit_float(tri_seed + (u32)k * 3266489917u + 3u) * 6.2831853f;
      float scale =
          scale_lo + scale_span * hash_to_unit_float(tri_seed + (u32)k * 668265263u + 4u);
      float ca = std::cos(angle) * scale;
      float sa = std::sin(angle) * scale;
      inst.mat[0] = math::Vector4f(ca, 0.f, -sa, 0.f);
      inst.mat[1] = math::Vector4f(0.f, scale, 0.f, 0.f);
      inst.mat[2] = math::Vector4f(sa, 0.f, ca, 0.f);
      inst.mat[3] = math::Vector4f(pos.x(), pos.y(), pos.z(), 1.f);
      inst.bsphere = math::Vector4f(pos.x(), pos.y(), pos.z(), 2.f * kMeter);

      // inherit lighting color from the nearest designer instance
      inst.color_idx = color_refs.front().color_idx;
      float best_d = 1e30f;
      for (auto& ref : color_refs) {
        float dx = ref.mat[3][0] - pos.x();
        float dz = ref.mat[3][2] - pos.z();
        float d = dx * dx + dz * dz;
        if (d < best_d) {
          best_d = d;
          inst.color_idx = ref.color_idx;
        }
      }
      // 13/10 (ronds noirs d'ombre un peu partout, Rocher du Geyser) : la
      // touffe d'artiste la plus proche pouvait etre a l'ombre d'un palmier ou
      // d'une falaise — toute la zone autour heritait de sa couleur sombre, en
      // plaques rondes, sur un sol pourtant clair. La couleur suit maintenant
      // l'eclairage PEINT DU SOL sous la touffe (8 palettes horaires), parmi les
      // couleurs des touffes d'artistes (rien n'est invente).
      inst.color_idx = pick_color(pos.x(), pos.z(), inst.color_idx);
      // [QA] 13/10 : QA_CARPET_FIXED=<index> force la couleur du tapis (diagnostic)
      if (const char* qf = getenv("QA_CARPET_FIXED")) {
        inst.color_idx = (u32)atoi(qf);
      }

      inst.src = 2;
      proto.instances.push_back(inst);
      seeded++;
    }
  }
  lg::info("PC-REMAKE grass carpet [{}]: seeded {} tufts (proto had {} designer instances, {} tris)",
           level_for_ground.level_name,
           seeded, best_count, tris.size());
}
}  // namespace

namespace {
thread_local std::vector<math::Vector4f> g_grass_keep_clear;

void clear_auto_grass_near_actors(std::vector<ShrubProtoInfo>& protos, const std::string& level) {
  if (g_grass_keep_clear.empty()) {
    return;
  }
  constexpr float kMeter = 4096.f;
  int removed = 0;
  for (auto& proto : protos) {
    auto& v = proto.instances;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const ShrubInstanceInfo& inst) {
                             if (inst.src != 1 && inst.src != 2) {
                               return false;  // touffes des artistes : jamais retirees
                             }
                             for (const auto& z : g_grass_keep_clear) {
                               const float dx = inst.mat[3][0] - z.x();
                               const float dz = inst.mat[3][2] - z.z();
                               const float dy = inst.mat[3][1] - z.y();
                               if (dx * dx + dz * dz < z.w() * z.w() && std::abs(dy) < 6.f * kMeter) {
                                 return true;
                               }
                             }
                             return false;
                           }),
            v.end());
    removed += (int)(before - v.size());
  }
  if (removed) {
    lg::info("PC-REMAKE herbe [{}] : {} touffes automatiques retirees autour des objets", level,
             removed);
  }
}
}  // namespace

void set_grass_keep_clear(const std::vector<math::Vector4f>& zones) {
  g_grass_keep_clear = zones;
}

// ============================================================================
// PC-REMAKE 13/10 : EDITEUR DE NIVEAUX (voir level_editor.h)
// ============================================================================
namespace {
constexpr u32 kShrubLibMagic = 0x5053474a;  // "JGSP"

std::string sanitize_file_name(const std::string& n) {
  std::string r;
  for (char c : n) {
    r.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_');
  }
  return r;
}

fs::path shrub_library_path(const std::string& level, const std::string& proto) {
  return fs::path(level_editor::library_dir()) / level / (sanitize_file_name(proto) + ".shrub");
}

// bibliotheque : chaque niveau depose ses arbustes (geometrie finale) pour que
// l'editeur puisse les planter dans N'IMPORTE QUEL niveau
void write_shrub_library(const std::string& level, const ShrubProtoInfo& proto) {
  std::vector<u8> out;
  auto put = [&](const void* p, size_t n) {
    const u8* b = (const u8*)p;
    out.insert(out.end(), b, b + n);
  };
  auto put32 = [&](u32 v) { put(&v, 4); };
  put32(kShrubLibMagic);
  put32((u32)proto.frags.size());
  for (const auto& frag : proto.frags) {
    put32((u32)frag.draws.size());
    for (const auto& d : frag.draws) {
      put(&d.adgif, sizeof(AdGifData));
      put32(d.settings.mode.as_int());
      put32(d.settings.combo_tex);
      put32((u32)d.vertices.size());
      for (const auto& v : d.vertices) {
        float f[5] = {v.xyz.x(), v.xyz.y(), v.xyz.z(), v.st.x(), v.st.y()};
        put(f, sizeof(f));
        u8 c[4] = {v.rgba_generic[0], v.rgba_generic[1], v.rgba_generic[2], (u8)(v.adc ? 1 : 0)};
        put(c, 4);
      }
    }
  }
  auto path = shrub_library_path(level, proto.name);
  file_util::create_dir_if_needed_for_file(path);
  file_util::write_binary_file(path, out.data(), out.size());
}

bool read_shrub_library(const std::string& level, const std::string& name, ShrubProtoInfo& out) {
  auto path = shrub_library_path(level, name);
  if (!fs::exists(path)) {
    return false;
  }
  auto data = file_util::read_binary_file(path);
  size_t off = 0;
  bool ok = true;
  auto get = [&](void* p, size_t n) {
    if (off + n > data.size()) {
      ok = false;
      return;
    }
    memcpy(p, data.data() + off, n);
    off += n;
  };
  auto get32 = [&]() {
    u32 v = 0;
    get(&v, 4);
    return v;
  };
  if (get32() != kShrubLibMagic) {
    return false;
  }
  out.frags.clear();
  u32 nf = get32();
  for (u32 f = 0; f < nf && ok; f++) {
    auto& frag = out.frags.emplace_back();
    u32 nd = get32();
    for (u32 di = 0; di < nd && ok; di++) {
      auto& d = frag.draws.emplace_back();
      get(&d.adgif, sizeof(AdGifData));
      d.settings.mode.as_int() = get32();
      d.settings.combo_tex = get32();
      d.start_vtx_idx = 0;
      u32 nv = get32();
      for (u32 vi = 0; vi < nv && ok; vi++) {
        float fl[5];
        get(fl, sizeof(fl));
        u8 c[4];
        get(c, 4);
        auto& v = d.vertices.emplace_back();
        v.xyz = math::Vector3f(fl[0], fl[1], fl[2]);
        v.st = math::Vector2f(fl[3], fl[4]);
        v.rgba_generic[0] = c[0];
        v.rgba_generic[1] = c[1];
        v.rgba_generic[2] = c[2];
        v.adc = c[3] != 0;
      }
    }
  }
  return ok;
}

// geometrie locale d'un arbuste -> parts de l'export (triangles, metres)
std::vector<level_editor::ExportPart> shrub_export_parts(const ShrubProtoInfo& proto,
                                                         const TextureDB& tdb) {
  std::map<u32, level_editor::ExportPart> by_tex;
  for (const auto& frag : proto.frags) {
    for (const auto& d : frag.draws) {
      auto& part = by_tex[d.settings.combo_tex];
      if (part.tex_name.empty()) {
        auto it = tdb.textures.find(d.settings.combo_tex);
        if (it != tdb.textures.end()) {
          part.tex_name = it->second.name;
          auto pit = tdb.tpage_names.find(it->second.page);
          part.tex_page = pit != tdb.tpage_names.end() ? pit->second : "";
        }
      }
      // memes regles que make_draws : adc = le ruban continue, sinon il repart
      std::vector<int> strip;
      auto flush = [&]() {
        for (size_t i = 2; i < strip.size(); i++) {
          for (int k : {strip[i - 2], strip[i - 1], strip[i]}) {
            const auto& v = d.vertices.at(k);
            part.pos.push_back(v.xyz.x() / level_editor::kMeter);
            part.pos.push_back(v.xyz.y() / level_editor::kMeter);
            part.pos.push_back(v.xyz.z() / level_editor::kMeter);
            part.uv.push_back(v.st.x());
            part.uv.push_back(v.st.y());
            part.rgb.push_back(v.rgba_generic[0]);
            part.rgb.push_back(v.rgba_generic[1]);
            part.rgb.push_back(v.rgba_generic[2]);
          }
        }
        strip.clear();
      };
      for (int vi = 0; vi < (int)d.vertices.size(); vi++) {
        if (d.vertices[vi].adc || vi == 0) {
          strip.push_back(vi);
        } else {
          flush();
          strip.push_back(vi - 1);
          strip.push_back(vi);
        }
      }
      flush();
    }
  }
  std::vector<level_editor::ExportPart> parts;
  for (auto& [combo, part] : by_tex) {
    parts.push_back(std::move(part));
  }
  return parts;
}

// applique les modifications validees dans l'app, puis exporte le niveau
void editor_apply_and_export_shrubs(std::vector<ShrubProtoInfo>& protos,
                                    const std::string& level,
                                    const TextureDB& tdb) {
  // 1. bibliotheque (protos de CE niveau, geometrie finale)
  for (const auto& p : protos) {
    write_shrub_library(level, p);
  }
  const auto edits = level_editor::load_edits(level);

  // 2. ajouts (avant les suppressions : la couleur vient des voisins d'origine)
  std::vector<std::pair<math::Vector3f, u32>> color_refs;
  for (const auto& p : protos) {
    for (const auto& inst : p.instances) {
      color_refs.emplace_back(math::Vector3f(inst.mat[3][0], inst.mat[3][1], inst.mat[3][2]),
                              inst.color_idx);
    }
  }
  for (const auto& a : edits.adds) {
    if (a.kind != "shrub") {
      continue;
    }
    const std::string key = a.from_level.empty() ? a.proto : a.from_level + ":" + a.proto;
    int pidx = -1;
    for (int i = 0; i < (int)protos.size(); i++) {
      if (protos[i].name == key) {
        pidx = i;
        break;
      }
    }
    if (pidx < 0 && !a.from_level.empty()) {
      ShrubProtoInfo imported;
      imported.name = key;
      if (read_shrub_library(a.from_level, a.proto, imported)) {
        protos.push_back(std::move(imported));
        pidx = (int)protos.size() - 1;
      }
    }
    if (pidx < 0) {
      lg::warn("PC-REMAKE editeur [{}] : arbuste '{}' introuvable — ajout ignore", level, key);
      continue;
    }
    ShrubInstanceInfo inst;
    inst.proto_idx = (u32)pidx;
    inst.mat = level_editor::add_matrix(a);
    inst.bsphere = math::Vector4f(inst.mat[3][0], inst.mat[3][1], inst.mat[3][2],
                                  4.f * level_editor::kMeter * std::max(a.scale, 0.1f));
    inst.color_idx = 0;
    float best = 1e30f;
    for (const auto& [pos, col] : color_refs) {
      float dx = pos.x() - inst.mat[3][0], dy = pos.y() - inst.mat[3][1],
            dz = pos.z() - inst.mat[3][2];
      float d = dx * dx + dy * dy + dz * dz;
      if (d < best) {
        best = d;
        inst.color_idx = col;
      }
    }
    inst.src = 3;
    inst.add_id = a.id;
    protos[pidx].instances.push_back(inst);
  }

  // 3. suppressions (jamais les ajouts : on les retire de la liste des ajouts)
  size_t removed = 0;
  for (auto& p : protos) {
    const size_t before = p.instances.size();
    p.instances.erase(std::remove_if(p.instances.begin(), p.instances.end(),
                                     [&](const ShrubInstanceInfo& inst) {
                                       return inst.src != 3 &&
                                              level_editor::is_removed(edits, "shrub", p.name,
                                                                       inst.mat[3]);
                                     }),
                      p.instances.end());
    removed += before - p.instances.size();
  }
  if (!edits.empty()) {
    lg::info("PC-REMAKE editeur [{}] : {} arbuste(s) retire(s)", level, removed);
  }

  // 4. export pour l'app
  static const char* kSrcNames[] = {"jeu", "touffe-auto", "tapis-auto", "ajout"};
  for (const auto& p : protos) {
    const auto colon = p.name.find(':');
    const std::string from = colon == std::string::npos ? "" : p.name.substr(0, colon);
    const std::string name = colon == std::string::npos ? p.name : p.name.substr(colon + 1);
    int eidx = level_editor::add_proto("shrub", name, from, shrub_export_parts(p, tdb));
    for (const auto& inst : p.instances) {
      level_editor::add_instance("shrub", eidx, inst.mat, kSrcNames[std::min<int>(inst.src, 3)],
                                 inst.add_id);
    }
  }
}
}  // namespace

void extract_shrub(const shrub_types::DrawableTreeInstanceShrub* tree,
                   const std::string& debug_name,
                   const std::vector<level_tools::TextureRemap>& map,
                   const TextureDB& tex_db,
                   const std::vector<std::pair<int, int>>& /*expected_missing_textures*/,
                   tfrag3::Level& out,
                   bool dump_level,
                   GameVersion version,
                   const std::vector<GrassSeedTri>& grass_seed_tris) {
  auto& tree_out = out.shrub_trees.emplace_back();

  if (version > GameVersion::Jak1) {
    tree_out.has_per_proto_visibility_toggle = true;
  }

  auto& protos = tree->info.prototype_inline_array_shrub;
  std::vector<ShrubProtoInfo> proto_info;
  // PC-REMAKE: flag grass-like prototypes for densification by name.
  std::vector<char> densify_flags;
  for (auto& proto : protos.data) {
    // inventaire (chasse aux dômes de la plage, 17/08)
    lg::info("PC-REMAKE shrub proto '{}' niveau '{}'", proto.name, out.level_name);
    proto_info.push_back(extract_proto(proto, tex_db, map, version));
    proto_info.back().name = proto.name;
    // inventaire TEXTURE par proto (l'identification ne devine plus jamais)
    for (auto& f : proto_info.back().frags) {
      for (auto& d : f.draws) {
        auto it = tex_db.textures.find(d.settings.combo_tex);
        lg::info("PC-REMAKE proto '{}' [{}] texture '{}'", proto.name, out.level_name,
                 it != tex_db.textures.end() ? it->second.name : "?");
      }
      break;
    }
    // PC-REMAKE: flat pebble sprites -> real 3D pebbles (same footprint)
    if (proto.name.find("pebble") != std::string::npos) {
      remake_pebble_proto(proto_info.back(), proto.name);
    }
    // l'herbe de Samos : le village sérialise son proto dominant
    if (out.level_name == "village1" && proto.name == "grass.mb") {
      dump_grass_proto(proto_info.back());
    }
    tree_out.proto_names.push_back(proto.name);
    densify_flags.push_back(is_densify_shrub_proto(proto.name) ? 1 : 0);
  }

  // PC-REMAKE (17/08, l'herbe de devant la maison de Samos, la même partout) :
  // sur la PLAGE, les dômes bch-bush ET les touffes pâles d'auteur deviennent
  // des croix d'herbe façon village. Les touffes pâles sont REDIRIGÉES vers le
  // créneau de texture ré-habillé (palmbush-outer = herbe du village) — leur
  // propre texture (bch-beachgrass) reste intouchée pour l'herbe du fort.
  if (out.level_name == "beach") {
    ShrubDraw bush_tpl_copy;
    bool have_tpl = false;
    for (auto& pi : proto_info) {
      if (pi.name.find("bch-bush") != std::string::npos) {
        for (auto& f : pi.frags) {
          if (!f.draws.empty()) {
            bush_tpl_copy.adgif = f.draws.front().adgif;
            bush_tpl_copy.settings = f.draws.front().settings;
            have_tpl = true;
            break;
          }
        }
        break;
      }
    }
    for (auto& pi : proto_info) {
      const auto& nm = pi.name;
      const bool is_bush = nm.find("bch-bush") != std::string::npos;
      const bool is_pale = (nm == "bch-grass.mb") || nm.find("shrub-grass") != std::string::npos;
      if (!is_bush && !is_pale) {
        continue;
      }
      // priorité : la VRAIE herbe de Samos transplantée (géométrie + texture du
      // village). Repli croix si le fichier n'existe pas encore (1re passe).
      if (load_grass_proto_into(pi, nm)) {
        continue;
      }
      if (is_bush) {
        remake_bush_to_grass_proto(pi, nm);
      } else if (have_tpl) {
        remake_bush_to_grass_proto(pi, nm, &bush_tpl_copy);
      }
    }
  }

  for (auto& arr : tree->discovered_arrays) {
    auto as_shrubs = dynamic_cast<shrub_types::DrawableInlineArrayInstanceShrub*>(arr.get());
    if (as_shrubs) {
      for (auto& inst : as_shrubs->instances) {
        extract_instance(inst, proto_info, densify_flags);
      }
    }
  }

  // PC-REMAKE: continuous grass carpet on grass-material ground.
  seed_grass_carpet(proto_info, densify_flags, grass_seed_tris, out, &tree->time_of_day);

  // PC-REMAKE 17/10 : pas d'herbe automatique sur les piles d'energie, orifices d'eco...
  clear_auto_grass_near_actors(proto_info, out.level_name);

  // PC-REMAKE 13/10 : modifications de l'editeur de niveaux + export pour l'app
  editor_apply_and_export_shrubs(proto_info, out.level_name, tex_db);

  // time of day colors
  tree_out.time_of_day_colors = pack_colors(tree->time_of_day);

  make_draws(out, tree_out, proto_info, tex_db);

  if (dump_level) {
    auto path = file_util::get_file_path({fmt::format("debug_out/shrub_all/{}.obj", debug_name)});
    file_util::create_dir_if_needed_for_file(path);
    file_util::write_text_file(path, dump_full_to_obj(proto_info));
  }
}
}  // namespace decompiler
