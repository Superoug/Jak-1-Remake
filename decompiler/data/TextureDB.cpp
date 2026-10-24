#include "TextureDB.h"

#include <cstdlib>
#include <deque>
#include <mutex>

#include "decompiler/data/faithful_upscale.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "fmt/format.h"
#define STBI_WINDOWS_UTF8
#include "third-party/stb_image/stb_image.h"
#include "third-party/stb_image/stb_image_write.h"

namespace decompiler {

namespace {
// PC-REMAKE 21/10 : BORDS TRANSPARENTS FIDELES. L'agrandissement voit la texture comme un motif
// qui se repete : il reporte sur la derniere rangee un peu de la premiere. Sur une frange d'herbe
// (haut opaque, bas transparent), cette rangee devenait a demi opaque ; etiree par le mode clamp
// sous la frange, elle tracait un fil vert le long de nombreux bords d'herbe. Partout ou le bord
// de l'original est transparent, la bande correspondante de la version HD le redevient.
// Le reste de la texture n'est pas touche.
void pc_restore_transparent_border(const std::vector<u32>& orig,
                                   int w,
                                   int h,
                                   ResolvedTextureData& hd) {
  const int W = hd.w, H = hd.h;
  if (w <= 0 || h <= 0 || (int)orig.size() != w * h || (int)hd.rgba.size() != W * H) {
    return;
  }
  if (W < w || H < h || W % w || H % h || (W == w && H == h)) {
    return;
  }
  const int sx = W / w, sy = H / h;
  for (int y = 0; y < H; y++) {
    const bool rangee_bord = y < sy || y >= H - sy;
    for (int x = 0; x < W; x++) {
      if (!rangee_bord && x >= sx && x < W - sx) {
        continue;
      }
      if ((orig[(size_t)(y / sy) * w + (x / sx)] >> 24) == 0) {
        hd.rgba[(size_t)y * W + x] &= 0x00ffffffu;
      }
    }
  }
}
}  // namespace

TextureDB::TextureDB() {
  std::vector<u32> data(16 * 16, 0xffffffff);
  add_texture(kPlaceholderWhiteTexturePage, kPlaceholderWhiteTextureId, data, 16, 16,
              "placeholder-white", "placeholder", {}, 1, 0);
}

void TextureDB::add_texture(u32 tpage,
                            u32 texid,
                            const std::vector<u32>& data,
                            u16 w,
                            u16 h,
                            const std::string& tex_name,
                            const std::string& tpage_name,
                            const std::vector<std::string>& level_names,
                            u32 num_mips,
                            u32 dest) {
  auto existing_tpage_name = tpage_names.find(tpage);
  if (existing_tpage_name == tpage_names.end()) {
    tpage_names[tpage] = tpage_name;
  } else {
    ASSERT(existing_tpage_name->second == tpage_name);
  }

  u32 combo_id = (tpage << 16) | texid;
  auto existing_tex = textures.find(combo_id);
  if (existing_tex != textures.end()) {
    ASSERT(existing_tex->second.name == tex_name);
    ASSERT(existing_tex->second.w == w);
    ASSERT(existing_tex->second.h == h);
    ASSERT(existing_tex->second.rgba_bytes == data);
    ASSERT(existing_tex->second.page == tpage);
    ASSERT(existing_tex->second.num_mips == num_mips);
    ASSERT(existing_tex->second.dest == dest);
  } else {
    auto& new_tex = textures[combo_id];
    new_tex.rgba_bytes = data;
    new_tex.name = tex_name;
    new_tex.w = w;
    new_tex.h = h;
    new_tex.page = tpage;
    new_tex.num_mips = num_mips;
    new_tex.dest = dest;
  }
  for (const auto& level_name : level_names) {
    texture_ids_per_level[level_name].insert(combo_id);
  }
}

void TextureDB::add_index_texture(u32 tpage,
                                  u32 texid,
                                  const std::vector<u8>& index_data,
                                  const std::array<math::Vector4<u8>, 256>& clut,
                                  u16 w,
                                  u16 h,
                                  const std::string& tex_name,
                                  const std::string& tpage_name,
                                  const std::vector<std::string>& level_names) {
  auto existing_tpage_name = tpage_names.find(tpage);
  if (existing_tpage_name == tpage_names.end()) {
    tpage_names[tpage] = tpage_name;
  } else {
    ASSERT(existing_tpage_name->second == tpage_name);
  }

  u32 combo_id = (tpage << 16) | texid;
  auto existing_tex = index_textures_by_combo_id.find(combo_id);
  if (existing_tex != index_textures_by_combo_id.end()) {
    ASSERT(existing_tex->second.name == tex_name);
    ASSERT(existing_tex->second.w == w);
    ASSERT(existing_tex->second.h == h);
    ASSERT(existing_tex->second.index_data == index_data);
    ASSERT(existing_tex->second.combo_id == combo_id);
    ASSERT(existing_tex->second.color_table == clut);
    ASSERT(existing_tex->second.tpage_name == tpage_name);
    for (auto& ln : level_names) {
      existing_tex->second.level_names.push_back(ln);
    }
  } else {
    auto& new_tex = index_textures_by_combo_id[combo_id];
    new_tex.index_data = index_data;
    new_tex.color_table = clut;
    new_tex.name = tex_name;
    new_tex.w = w;
    new_tex.h = h;
    new_tex.tpage_name = tpage_name;
    new_tex.combo_id = combo_id;
    new_tex.level_names = level_names;
  }
}

void TextureDB::merge_textures(const fs::path& base_path) {
  merge_texture_dir = base_path;
}

void TextureDB::replace_textures(const fs::path& path) {
  replace_texture_dir = path;
}

void TextureDB::merge_texture(u32 id, std::vector<u32>& rgba) const {
  if (!merge_texture_dir) {
    return;
  }

  const auto& tex = textures.at(id);
  fs::path full_path = *merge_texture_dir / tpage_names.at(tex.page) / (tex.name + ".png");

  if (!fs::exists(full_path)) {
    return;
  }

  lg::info("Merging {}", full_path.string().c_str());

  int w, h;
  auto merge_data = stbi_load(full_path.string().c_str(), &w, &h, 0, 4);

  if (!merge_data) {
    lg::warn("failed to load PNG file: {}", full_path.string().c_str());
    return;
  }

  if (w != tex.w || h != tex.h) {
    lg::warn("merge texture does not match the same dimensions: {}, {} != {} || {} != {}",
             full_path.string().c_str(), w, tex.w, h, tex.h);
    stbi_image_free(merge_data);
    return;
  }
  // Merge any non-transparent pixels into the existing texture
  for (int i = 0; i < w * h * 4; i += 4) {
    const auto merge_pixel_a = merge_data[i + 3];
    if (merge_pixel_a != 0) {
      u32 merge_pixel;
      memcpy(&merge_pixel, &merge_data[i], sizeof(u32));
      rgba.at(i / 4) = merge_pixel;
    }
  }

  stbi_image_free(merge_data);
}

std::optional<ResolvedTextureData> TextureDB::replace_texture(u32 id) const {
  if (!replace_texture_dir) {
    return std::nullopt;
  }

  const auto& tex = textures.at(id);
  const auto& tpage_name = tpage_names.at(tex.page);

  fs::path full_path = *replace_texture_dir / tpage_name / (tex.name + ".png");

  if (!fs::exists(full_path)) {
    full_path = *replace_texture_dir / "_all" / (tex.name + ".png");

    if (!fs::exists(full_path)) {
      return std::nullopt;
    }
  }

  lg::info("Replacing {}", tpage_name + "/" + tex.name);

  int w, h;
  auto data = stbi_load(full_path.string().c_str(), &w, &h, 0, 4);

  if (!data) {
    lg::warn("failed to load PNG file: {}", full_path.string().c_str());
    return std::nullopt;
  }

  ResolvedTextureData result;
  result.w = static_cast<u16>(w);
  result.h = static_cast<u16>(h);
  result.rgba.resize(w * h);

  memcpy(result.rgba.data(), data, w * h * 4);

  stbi_image_free(data);

  return result;
}

ResolvedTextureData TextureDB::resolve_texture(u32 id, bool allow_faithful) const {
  const auto& tex = textures.at(id);

  ResolvedTextureData result{
      .w = tex.w,
      .h = tex.h,
      .rgba = tex.rgba_bytes,
  };

  merge_texture(id, result.rgba);

  if (auto replacement = replace_texture(id)) {
    result = std::move(*replacement);
    pc_restore_transparent_border(tex.rgba_bytes, tex.w, tex.h, result);
  } else if (faithful_hd && allow_faithful && !faithful_upscale_frozen(tex.name)) {
    // PC-REMAKE 14/10 : version HD FIDELE calculee ici depuis l'original du joueur
    // (cache borne : une texture partagee par plusieurs niveaux n'est calculee
    // qu'une fois tant qu'elle reste dans les 256 derniers Mo)
    static std::mutex cache_mutex;
    static std::unordered_map<u32, ResolvedTextureData> cache;
    static std::deque<u32> order;
    static size_t cache_bytes = 0;
    {
      std::lock_guard<std::mutex> lk(cache_mutex);
      auto it = cache.find(id);
      if (it != cache.end()) {
        return it->second;
      }
    }
    auto up = faithful_iris_texture(tex.name)
                  ? faithful_iris_upscale(result.rgba.data(), result.w, result.h)
                  : faithful_upscale(result.rgba.data(), result.w, result.h);
    if (up) {
      const auto& tpage_name = tpage_names.at(tex.page);
      if (tpage_name == "title-vis-pris" && tex.name == "trademark") {
        // TM du logo : son RGB d'origine est un rebut violet/noir (seul l'alpha a un
        // sens) -> dessin d'origine en creme (13/10)
        for (auto& px : up->rgba) {
          px = (px & 0xff000000u) | (222u << 16) | (232u << 8) | 235u;
        }
      }
      result.w = (u16)up->w;
      result.h = (u16)up->h;
      result.rgba = std::move(up->rgba);
      pc_restore_transparent_border(tex.rgba_bytes, tex.w, tex.h, result);
      if (const char* dump = std::getenv("PC_FAITHFUL_DUMP")) {
        auto path = fs::path(dump) / tpage_name / (tex.name + ".png");
        file_util::create_dir_if_needed_for_file(path);
        stbi_write_png(path.string().c_str(), result.w, result.h, 4, result.rgba.data(),
                       result.w * 4);
      }
      std::lock_guard<std::mutex> lk(cache_mutex);
      cache[id] = result;
      order.push_back(id);
      cache_bytes += result.rgba.size() * 4;
      while (cache_bytes > (256u << 20) && !order.empty()) {
        auto old = cache.find(order.front());
        if (old != cache.end()) {
          cache_bytes -= old->second.rgba.size() * 4;
          cache.erase(old);
        }
        order.pop_front();
      }
    }
  }

  return result;
}

/*!
 * Generate a table of offsets
 */
std::string TextureDB::generate_texture_dest_adjustment_table() const {
  // group textures by page
  std::map<u32, std::vector<u32>> textures_by_page;
  for (const auto& [texture_id, texture] : textures) {
    textures_by_page[texture.page].push_back(texture_id);
  }

  std::string result = "{\n";
  // loop over pages (this overlap trick only applies within a page)
  for (const auto& [tpage, texture_ids_in_page] : textures_by_page) {
    // organize by tbp offset
    std::map<u32, std::vector<u32>> textures_by_tbp_offset;
    std::set<u32> all_used_tbp_offsets;
    u32 max_tbp_offset = 0;
    for (auto tid : texture_ids_in_page) {
      u32 tbp = textures.at(tid).dest;
      textures_by_tbp_offset[tbp].push_back(tid);
      all_used_tbp_offsets.insert(tbp);
      max_tbp_offset = std::max(max_tbp_offset, tbp);
    }

    // find tbp's with overlaps:
    bool needs_remap = false;
    for (const auto& [tbp, tex_ids] : textures_by_tbp_offset) {
      if (tex_ids.size() > 1) {
        needs_remap = true;
        break;
      }
    }

    if (needs_remap) {
      result += fmt::format("{{{},{{\n", tpage);
      for (const auto& [tbp, tex_ids] : textures_by_tbp_offset) {
        if (tex_ids.size() > 1) {
          int offset = 1;
          for (size_t id_id = 1; id_id < tex_ids.size(); id_id++) {
            auto id = tex_ids[id_id];

            bool ok = false;
            int tries = 50;
            // make sure we don't overlap again.
            while (!ok && tries > 0) {
              tries--;
              if (!all_used_tbp_offsets.count(tbp + offset)) {
                ok = true;
                break;
              }
              offset++;
            }

            ASSERT(ok);
            all_used_tbp_offsets.insert(tbp + offset);
            result += fmt::format("{{{}, {}}},", id & 0xffff, offset);
            offset++;
          }
        }
      }
      result.pop_back();
      result += "}},\n";
    }
  }
  result += "}\n";
  return result;
}
}  // namespace decompiler
