#include "Shrub.h"
#include <set>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

Shrub::Shrub(const std::string& name, int my_id) : BucketRenderer(name, my_id) {
  m_color_result.resize(TIME_OF_DAY_COLOR_COUNT);
}

Shrub::~Shrub() {
  discard_tree_cache();
}

void Shrub::init_shaders(ShaderLibrary& shaders) {
  m_uniforms.decal = pc_uniform_loc(shaders[ShaderId::SHRUB].id(), "decal");
  // PC-REMAKE: grass bending around the player
  m_uniforms.grass_push = pc_uniform_loc(shaders[ShaderId::SHRUB].id(), "grass_push");
  m_uniforms.grass_clear = pc_uniform_loc(shaders[ShaderId::SHRUB].id(), "grass_clear");
  m_uniforms.grass_wind = pc_uniform_loc(shaders[ShaderId::SHRUB].id(), "grass_wind");
  m_uniforms.shrub_facet = pc_uniform_loc(shaders[ShaderId::SHRUB].id(), "shrub_facet");
}

void Shrub::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::NOP ||
         data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }
  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }

  auto pc_port_data = dma.read_and_advance();
  ASSERT(pc_port_data.size_bytes == sizeof(TfragPcPortData));
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(TfragPcPortData));
  m_pc_port_data.level_name[11] = '\0';

  if (render_state->version >= GameVersion::Jak2) {
    // jak 2 proto visibility
    auto proto_mask_data = dma.read_and_advance();
    m_proto_vis_data = proto_mask_data.data;
    m_proto_vis_data_size = proto_mask_data.size_bytes;
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }

  TfragRenderSettings settings;
  settings.camera = m_pc_port_data.camera;

  settings.tree_idx = 0;

  update_render_state_from_pc_settings(render_state, m_pc_port_data);

  m_has_level = setup_for_level(m_pc_port_data.level_name, render_state);
  render_all_trees(settings, render_state, prof);
}

void Shrub::update_load(const LevelData* loader_data) {
  const tfrag3::Level* lev_data = loader_data->level.get();
  // We changed level!
  discard_tree_cache();
  m_trees.resize(lev_data->shrub_trees.size());

  size_t max_draws = 0;
  u32 time_of_day_count = 0;
  size_t max_num_grps = 0;
  size_t max_inds = 0;

  for (u32 l_tree = 0; l_tree < lev_data->shrub_trees.size(); l_tree++) {
    size_t num_grps = 0;

    const auto& tree = lev_data->shrub_trees[l_tree];
    max_draws = std::max(tree.static_draws.size(), max_draws);
    for (auto& draw : tree.static_draws) {
      (void)draw;
      // num_grps += draw.vis_groups.size(); TODO
      max_num_grps += 1;
    }
    max_num_grps = std::max(max_num_grps, num_grps);

    time_of_day_count = std::max(tree.time_of_day_colors.color_count, time_of_day_count);
    max_inds = std::max(tree.indices.size(), max_inds);
    u32 verts = tree.unpacked.vertices.size();
    glGenVertexArrays(1, &m_trees[l_tree].vao);
    glBindVertexArray(m_trees[l_tree].vao);
    m_trees[l_tree].vertex_buffer = loader_data->shrub_vertex_data[l_tree];
    m_trees[l_tree].vert_count = verts;
    m_trees[l_tree].draws = &tree.static_draws;
    // PC-REMAKE: only vegetation sways in the wind — ropes, pebbles and other
    // small props are shrubs too and must stay still (bug report).
    m_trees[l_tree].veg_flags.clear();
    m_trees[l_tree].facet_flags.clear();
    m_trees[l_tree].veg_mark.clear();
    for (auto& draw : tree.static_draws) {
      // classes : 0 = immobile (cordes, cailloux, TIGES), 1 = herbe (flexion
      // normale), 2 = FEUILLES de palmier/bananier (amplitude réglable en
      // direct via pc-set-palm-wind! w — tige figée, feuilles vivantes)
      u8 vclass = 0;
      if (draw.tree_tex_id < loader_data->level->textures.size()) {
        const auto& name = loader_data->level->textures[draw.tree_tex_id].debug_name;
        static const char* kStill[] = {"pstr", "trunk", "stem", "bark"};
        static const char* kPalmLeaf[] = {"palmplant", "palmbush", "palm-leaf", "banana",
                                          "frond"};
        static const char* kVeg[] = {"grass", "plant", "leaf",  "leaves", "palm",   "fern",
                                     "bush",  "vine",  "reed",  "weed",   "flower",
                                     "hay",   "wheat", "moss",  "bamboo", "shrub",  "riceplant",
                                     // 20/10 : riz des rizieres et quenouilles du village (dans l'eau,
                                     // pres du fermier) : ils plient sous les pas comme l'herbe
                                     "rice",  "cattail"};
        bool still = false;
        for (auto* w : kStill) {
          if (name.find(w) != std::string::npos) {
            still = true;
            break;
          }
        }
        if (!still) {
          for (auto* w : kPalmLeaf) {
            if (name.find(w) != std::string::npos) {
              vclass = 2;
              break;
            }
          }
          if (vclass == 0) {
            for (auto* w : kVeg) {
              if (name.find(w) != std::string::npos) {
                vclass = 1;
                break;
              }
            }
          }
        }
      }
      m_trees[l_tree].veg_flags.push_back(vclass);
      // 13/10 : normale du G-buffer. Vegetation (meme immobile : riz, quenouilles,
      // mousse, corail) et galets 3D = verticale, comme le sol ; le reste (bois,
      // toits, bancs, cordes, rochers) = vraie facette.
      u8 facet = 0;
      if (vclass == 0 && draw.tree_tex_id < loader_data->level->textures.size()) {
        const auto& name = loader_data->level->textures[draw.tree_tex_id].debug_name;
        static const char* kSoft[] = {"pebble", "cattail", "rice",   "kelp",  "coral",
                                      "tuft",   "moss",    "grass",  "plant", "leaf",
                                      "flower", "hay",     "reed",   "weed",  "vine",
                                      "fern",   "bush",    "shrub-grass"};
        facet = 1;
        for (auto* w : kSoft) {
          if (name.find(w) != std::string::npos) {
            facet = 0;
            break;
          }
        }
      }
      m_trees[l_tree].facet_flags.push_back(facet);
      // vegetation = tout ce qui n'est ni objet rigide ni galet
      u8 veg = 0;
      if (draw.tree_tex_id < loader_data->level->textures.size()) {
        const auto& name = loader_data->level->textures[draw.tree_tex_id].debug_name;
        veg = (facet == 0 && name.find("pebble") == std::string::npos) ? 1 : 0;
      }
      m_trees[l_tree].veg_mark.push_back(veg);
    }
    // projecteur violet : reclassification à chaud dans le rendu
    m_trees[l_tree].tex_names = &lev_data->textures;
    m_trees[l_tree].palm_debug_seq_built = 0;
    m_trees[l_tree].proto_vis_mask.clear();
    m_trees[l_tree].proto_vis_mask.resize(tree.proto_names.size(), true);
    m_trees[l_tree].proto_name_to_idx.clear();
    size_t i = 0;
    for (auto& name : tree.proto_names) {
      m_trees[l_tree].proto_name_to_idx[name].push_back(i++);
    }
    m_trees[l_tree].colors = &tree.time_of_day_colors;
    // 14/10 : index buffer reordonne par cases (voir build_cells)
    build_cells(m_trees[l_tree], tree);
    m_trees[l_tree].index_data = m_trees[l_tree].reordered_indices.data();
    glBindBuffer(GL_ARRAY_BUFFER, m_trees[l_tree].vertex_buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);

    glVertexAttribPointer(0,                                          // location 0 in the shader
                          3,                                          // 3 values per vert
                          GL_FLOAT,                                   // floats
                          GL_FALSE,                                   // normalized
                          sizeof(tfrag3::ShrubGpuVertex),             // stride
                          (void*)offsetof(tfrag3::ShrubGpuVertex, x)  // offset (0)
    );

    glVertexAttribPointer(1,                                          // location 1 in the shader
                          2,                                          // 3 values per vert
                          GL_FLOAT,                                   // floats
                          GL_FALSE,                                   // normalized
                          sizeof(tfrag3::ShrubGpuVertex),             // stride
                          (void*)offsetof(tfrag3::ShrubGpuVertex, s)  // offset (0)
    );

    glVertexAttribPointer(2,                               // location 1 in the shader
                          3,                               // 4 color components
                          GL_UNSIGNED_BYTE,                // u8
                          GL_TRUE,                         // normalized (255 becomes 1)
                          sizeof(tfrag3::ShrubGpuVertex),  //
                          (void*)offsetof(tfrag3::ShrubGpuVertex, rgba_base)  //
    );

    glVertexAttribIPointer(3,                               // location 2 in the shader
                           1,                               // 1 values per vert
                           GL_UNSIGNED_SHORT,               // u16
                           sizeof(tfrag3::ShrubGpuVertex),  // stride
                           (void*)offsetof(tfrag3::ShrubGpuVertex, color_index)  // offset (0)
    );

    glGenBuffers(1, &m_trees[l_tree].single_draw_index_buffer);
    glGenBuffers(1, &m_trees[l_tree].index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_trees[l_tree].index_buffer);
    // (original : tree.indices tel quel)
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 m_trees[l_tree].reordered_indices.size() * sizeof(u32),
                 m_trees[l_tree].reordered_indices.data(), GL_STATIC_DRAW);
    max_inds = std::max(m_trees[l_tree].reordered_indices.size(), max_inds);
    {
      size_t segs = 0;
      for (auto& v : m_trees[l_tree].draw_segments) {
        segs += v.size();
      }
      max_num_grps = std::max(max_num_grps, segs + tree.static_draws.size());
    }

    glActiveTexture(GL_TEXTURE10);
    glGenTextures(1, &m_trees[l_tree].time_of_day_texture);
    glBindTexture(GL_TEXTURE_1D, m_trees[l_tree].time_of_day_texture);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, TIME_OF_DAY_COLOR_COUNT, 0, GL_RGBA,
                 GL_UNSIGNED_INT_8_8_8_8, nullptr);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindVertexArray(0);
  }

  m_cache.multidraw_offset_per_stripdraw.resize(max_draws);
  m_cache.multidraw_count_buffer.resize(max_num_grps);
  m_cache.multidraw_index_offset_buffer.resize(max_num_grps);
  m_cache.draw_idx_temp.resize(max_draws);
  m_cache.index_temp.resize(max_inds);
  ASSERT(time_of_day_count <= TIME_OF_DAY_COLOR_COUNT);
}

bool Shrub::setup_for_level(const std::string& level, SharedRenderState* render_state) {
  // make sure we have the level data.
  Timer tfrag3_setup_timer;
  auto lev_data = render_state->loader->get_tfrag3_level(level);

  if (!lev_data) {
    // not loaded
    m_has_level = false;
    m_textures = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return false;
  }

  if (m_has_level && lev_data->load_id != m_load_id) {
    m_has_level = false;
    m_textures = nullptr;
    m_level_name = "";
    discard_tree_cache();
    return setup_for_level(level, render_state);
  }

  m_textures = &lev_data->textures;
  m_load_id = lev_data->load_id;

  if (m_level_name != level) {
    update_load(lev_data);
    m_has_level = true;
    m_level_name = level;
  } else {
    m_has_level = true;
  }

  if (tfrag3_setup_timer.getMs() > 5) {
    lg::info("Shrub setup: {:.1f}ms", tfrag3_setup_timer.getMs());
  }

  return m_has_level;
}

void Shrub::discard_tree_cache() {
  for (auto& tree : m_trees) {
    glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
    glDeleteTextures(1, &tree.time_of_day_texture);
    glDeleteBuffers(1, &tree.index_buffer);
    glDeleteBuffers(1, &tree.single_draw_index_buffer);
    glDeleteVertexArrays(1, &tree.vao);
  }

  m_trees.clear();
}

void Shrub::build_cells(Tree& out, const tfrag3::ShrubTree& tree) {
  constexpr float kCell = 32.f * 4096.f;
  constexpr float kMargin = 2.5f * 4096.f;  // vent + poussee des pas
  const auto& verts = tree.unpacked.vertices;
  out.cells.clear();
  out.draw_segments.clear();
  out.draw_full_range.clear();
  out.reordered_indices.clear();
  out.reordered_indices.reserve(tree.indices.size() + tree.indices.size() / 4);
  std::unordered_map<u64, u32> cell_of_key;
  std::vector<math::Vector3f> cmin, cmax;

  struct Strip {
    u32 begin, end;  // dans tree.indices, sans le code de reprise
    u32 cell;
  };
  std::vector<Strip> strips;
  for (const auto& draw : tree.static_draws) {
    strips.clear();
    const u32 d_end = draw.first_index_index + draw.num_indices;
    u32 i = draw.first_index_index;
    while (i < d_end) {
      while (i < d_end && tree.indices[i] == UINT32_MAX) {
        i++;
      }
      u32 b = i;
      math::Vector3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
      while (i < d_end && tree.indices[i] != UINT32_MAX) {
        const u32 vi = tree.indices[i];
        if (vi < verts.size()) {
          const auto& v = verts[vi];
          lo.x() = std::min(lo.x(), v.x);
          lo.y() = std::min(lo.y(), v.y);
          lo.z() = std::min(lo.z(), v.z);
          hi.x() = std::max(hi.x(), v.x);
          hi.y() = std::max(hi.y(), v.y);
          hi.z() = std::max(hi.z(), v.z);
        }
        i++;
      }
      if (i == b) {
        continue;
      }
      if (lo.x() > hi.x()) {
        lo = hi = math::Vector3f(0, 0, 0);
      }
      const float cx = 0.5f * (lo.x() + hi.x());
      const float cz = 0.5f * (lo.z() + hi.z());
      const s32 kx = (s32)std::floor(cx / kCell);
      const s32 kz = (s32)std::floor(cz / kCell);
      const u64 key = ((u64)(u32)kx << 32) | (u32)kz;
      auto it = cell_of_key.find(key);
      u32 cell;
      if (it == cell_of_key.end()) {
        cell = out.cells.size();
        cell_of_key[key] = cell;
        out.cells.emplace_back();
        cmin.push_back(lo);
        cmax.push_back(hi);
      } else {
        cell = it->second;
        for (int k = 0; k < 3; k++) {
          cmin[cell][k] = std::min(cmin[cell][k], lo[k]);
          cmax[cell][k] = std::max(cmax[cell][k], hi[k]);
        }
      }
      strips.push_back({b, i, cell});
    }
    // bloc du draw : bandes groupees par case (ordre de case stable)
    std::stable_sort(strips.begin(), strips.end(),
                     [](const Strip& a, const Strip& b) { return a.cell < b.cell; });
    auto& segs = out.draw_segments.emplace_back();
    const u32 block_start = out.reordered_indices.size();
    for (const auto& st : strips) {
      if (segs.empty() || segs.back().cell != st.cell) {
        Tree::Segment sg;
        sg.first_index = out.reordered_indices.size();
        sg.cell = st.cell;
        segs.push_back(sg);
      }
      for (u32 k = st.begin; k < st.end; k++) {
        out.reordered_indices.push_back(tree.indices[k]);
      }
      out.reordered_indices.push_back(UINT32_MAX);
      segs.back().count = out.reordered_indices.size() - segs.back().first_index;
    }
    out.draw_full_range.push_back({block_start, (u32)out.reordered_indices.size() - block_start});
  }
  for (size_t c = 0; c < out.cells.size(); c++) {
    math::Vector3f ctr = (cmin[c] + cmax[c]) * 0.5f;
    math::Vector3f half = (cmax[c] - cmin[c]) * 0.5f;
    out.cells[c].bsphere =
        math::Vector4f(ctr.x(), ctr.y(), ctr.z(), half.length() + kMargin);
  }
  out.cell_visible.assign(out.cells.size(), 1);
  lg::info("[QA-SHRUB] {} bandes reparties en {} cases ({} draws, {} -> {} indices)",
           [&] {
             size_t n = 0;
             for (auto& v : out.draw_segments) {
               n += v.size();
             }
             return n;
           }(),
           out.cells.size(), tree.static_draws.size(), tree.indices.size(),
           out.reordered_indices.size());
}

void Shrub::render_all_trees(const TfragRenderSettings& settings,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  for (u32 i = 0; i < m_trees.size(); i++) {
    render_tree(i, settings, render_state, prof);
  }
}

namespace {
void update_vis_mask(std::vector<bool>& vis_mask,
                     const u8* data,
                     u32 data_size,
                     const std::unordered_map<std::string, std::vector<u32>>& name_to_idx) {
  char name_buffer[256];  // ??

  for (u32 i = 0; i < vis_mask.size(); i++) {
    vis_mask[i] = true;
  }

  const u8* end = data + data_size;
  while (true) {
    int name_idx = 0;
    while (*data) {
      name_buffer[name_idx++] = *data;
      data++;
    }
    if (name_idx) {
      ASSERT(name_idx < 254);
      name_buffer[name_idx] = '\0';
      const auto& it = name_to_idx.find(name_buffer);
      if (it != name_to_idx.end()) {
        for (auto x : name_to_idx.at(name_buffer)) {
          vis_mask[x] = 0;
        }
      }
    }

    while (*data == 0) {
      if (data >= end) {
        return;
      }
      data++;
    }
  }
}
}  // namespace

void Shrub::render_tree(int idx,
                        const TfragRenderSettings& settings,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof) {
  Timer tree_timer;
  auto& tree = m_trees.at(idx);
  tree.perf.draws = 0;
  tree.perf.wind_draws = 0;
  if (!m_has_level) {
    return;
  }

  if (m_color_result.size() < tree.colors->color_count) {
    m_color_result.resize(tree.colors->color_count);
  }

  Timer interp_timer;
  interp_time_of_day(settings.camera.itimes, *tree.colors, m_color_result.data());
  tree.perf.tod_time.add(interp_timer.getSeconds());

  Timer setup_timer;
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
  glTexSubImage1D(GL_TEXTURE_1D, 0, 0, tree.colors->color_count, GL_RGBA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, m_color_result.data());

  // [QA 22/09] modes des draws shrub (feuillages) — la grande feuille du village
  {
    static std::set<const void*> logged;
    if (tree.tex_names && tree.draws && logged.insert((const void*)tree.draws).second) {
      for (const auto& d : *tree.draws) {
        std::string tn = (d.tree_tex_id >= 0 && d.tree_tex_id < (int)tree.tex_names->size())
                             ? tree.tex_names->at(d.tree_tex_id).debug_name
                             : std::string("?");
        const auto& m = d.mode;
        lg::info("[QA-FEUILLE] shrub tex='{}' tris={} zwrite={} ztest={} ablend={}({}) atest={}({}) "
                 "aref={} afail={} decal={}",
                 tn, d.num_triangles, (int)m.get_depth_write_enable(), (int)m.get_zt_enable(),
                 (int)m.get_ab_enable(), (int)m.get_alpha_blend(), (int)m.get_at_enable(),
                 (int)m.get_alpha_test(), (int)m.get_aref(), (int)m.get_alpha_fail(),
                 (int)m.get_decal());
      }
    }
  }
  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB);

  // PC-REMAKE: feed the player position so the vertex shader can bend grass.
  glUniform4fv(m_uniforms.grass_push, 1, g_grass_push);
  // PC-REMAKE: ambient wind sway — time from the frame counter (60 fps game).
  // Amplitude is set PER DRAW below: vegetation sways, ropes/pebbles stay still.
  const float wind_time = (float)(render_state->frame_idx % 100000) / 60.f;
  // PC-REMAKE: grass receives sun shadows (ground and tufts darken together).
  if (g_sun_shadow.enabled()) {
    g_sun_shadow.bind_for_receive(render_state->shaders[ShaderId::SHRUB].id());
  } else {
    // 21/09 : sampler toujours pose sur l'unite 14 (voir bind_disabled)
    g_sun_shadow.bind_disabled(render_state->shaders[ShaderId::SHRUB].id());
  }

  // 13/10 : marquage stencil de la vegetation (voir veg_mark) — efface par
  // ShadowRenderer avant son propre usage du stencil
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0x80);
  glStencilFunc(GL_ALWAYS, 0x80, 0x80);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  render_state->stencil_dirty = true;

  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  if (m_proto_vis_data) {
    update_vis_mask(tree.proto_vis_mask, m_proto_vis_data, m_proto_vis_data_size,
                    tree.proto_name_to_idx);
  }
  tree.perf.tod_time.add(setup_timer.getSeconds());

  int last_texture = -1;

  // 14/10 : tri de visibilite par cases (voir build_cells) — original :
  //   no_multidraw : make_all_visible_index_list(...) ; sinon
  //   make_all_visible_multidraws(...) = tout le niveau a chaque image
  Timer cull_timer;
  if ((m_cull_poll++ % 64) == 0) {
    m_cull_enabled = !fs::exists("/tmp/qa_no_shrub_cull");
  }
  for (size_t c = 0; c < tree.cells.size(); c++) {
    tree.cell_visible[c] =
        !m_cull_enabled || sphere_in_view_ref(tree.cells[c].bsphere, settings.camera.planes);
  }
  tree.perf.cull_time.add(cull_timer.getSeconds());
  Timer index_timer;
  {
    u32 md = 0;
    u32 out_idx = 0;
    for (size_t d = 0; d < tree.draw_segments.size(); d++) {
      const auto& segs = tree.draw_segments[d];
      m_cull_stats.segs_total += segs.size();
      if (render_state->no_multidraw) {
        const u32 start = out_idx;
        for (const auto& sg : segs) {
          if (!tree.cell_visible[sg.cell]) {
            continue;
          }
          m_cull_stats.segs_drawn++;
          memcpy(&m_cache.index_temp[out_idx], &tree.reordered_indices[sg.first_index],
                 sg.count * sizeof(u32));
          out_idx += sg.count;
        }
        m_cache.draw_idx_temp[d] = {(int)start, (int)(out_idx - start)};
      } else {
        const u32 first_md = md;
        for (const auto& sg : segs) {
          if (!tree.cell_visible[sg.cell]) {
            continue;
          }
          m_cull_stats.segs_drawn++;
          // segments contigus dans le buffer : on les fusionne en un seul appel
          if (md > first_md &&
              (size_t)m_cache.multidraw_index_offset_buffer[md - 1] / sizeof(u32) +
                      m_cache.multidraw_count_buffer[md - 1] ==
                  sg.first_index) {
            m_cache.multidraw_count_buffer[md - 1] += sg.count;
          } else {
            m_cache.multidraw_count_buffer[md] = sg.count;
            m_cache.multidraw_index_offset_buffer[md] = (void*)((size_t)sg.first_index * sizeof(u32));
            md++;
          }
        }
        m_cache.multidraw_offset_per_stripdraw[d] = {(int)first_md, (int)(md - first_md)};
      }
    }
    if (render_state->no_multidraw) {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, out_idx * sizeof(u32), m_cache.index_temp.data(),
                   GL_STREAM_DRAW);
    }
    if (++m_cull_stats.frames % 1200 == 0) {
      lg::info("[QA-SHRUB] cases dessinees {:.1f} % ({} / {} segments par image)",
               100.0 * m_cull_stats.segs_drawn / std::max<u64>(1, m_cull_stats.segs_total),
               m_cull_stats.segs_drawn / 1200, m_cull_stats.segs_total / 1200);
      m_cull_stats.segs_total = m_cull_stats.segs_drawn = 0;
    }
  }

  tree.perf.index_time.add(index_timer.getSeconds());

  Timer draw_timer;

  for (size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
    if (!tree.proto_vis_mask.at(draw.proto_idx)) {
      continue;
    }
    const auto& multidraw_indices = m_cache.multidraw_offset_per_stripdraw[draw_idx];
    const auto& singledraw_indices = m_cache.draw_idx_temp[draw_idx];

    if (render_state->no_multidraw) {
      if (singledraw_indices.second == 0) {
        continue;
      }
    } else {
      if (multidraw_indices.second == 0) {
        continue;
      }
    }

    if ((int)draw.tree_tex_id != last_texture) {
      glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
      last_texture = draw.tree_tex_id;
    }

    glUniform1i(m_uniforms.decal, draw.mode.get_decal() ? 1 : 0);
    // PC-REMAKE: wind per vegetation class (0 immobile, 1 herbe,
    // 2 feuilles de palmier — amplitude en direct via pc-set-palm-wind! w)
    {
      // projecteur violet : reclassification à chaud + uniform par draw
      if (tree.palm_debug_seq_built != g_palm_debug_seq && tree.tex_names && tree.draws) {
        tree.palm_debug_seq_built = g_palm_debug_seq;
        tree.draw_palm_debug.assign(tree.draws->size(), 0);
        for (size_t i = 0; i < tree.draws->size(); i++) {
          s32 ti = (s32)(*tree.draws)[i].tree_tex_id;
          if (ti >= 0 && ti < (int)tree.tex_names->size() &&
              palm_debug_matches(ti, (*tree.tex_names)[ti].debug_name)) {
            tree.draw_palm_debug[i] = 1;
          }
        }
      }
      glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::SHRUB].id(), "palm_debug"),
                  (draw_idx < tree.draw_palm_debug.size() && tree.draw_palm_debug[draw_idx])
                      ? 1.f
                      : 0.f);
      u8 vclass = draw_idx < tree.veg_flags.size() ? tree.veg_flags[draw_idx] : 0;
      float amp = 0.f;
      if (vclass == 1) {
        amp = 380.f;
      } else if (vclass == 2) {
        amp = 380.f * g_palm_wind[3];
      }
      float wind[4] = {wind_time, amp, 0.f, 0.f};
      glUniform4fv(m_uniforms.grass_wind, 1, wind);
      glUniform1f(m_uniforms.shrub_facet,
                  (draw_idx < tree.facet_flags.size() && tree.facet_flags[draw_idx]) ? 1.f : 0.f);
      glStencilOp(GL_KEEP, GL_KEEP,
                  (draw_idx < tree.veg_mark.size() && tree.veg_mark[draw_idx]) ? GL_REPLACE
                                                                                : GL_KEEP);
      // la POUSSÉE des pas suit la même classe : la classe 0 (galets, tiges,
      // cordes) est rigide — sinon les cailloux réagissaient aux pas
      if (vclass == 0) {
        float rigid[4] = {g_grass_push[0], g_grass_push[1], g_grass_push[2], 0.f};
        glUniform4fv(m_uniforms.grass_push, 1, rigid);
        // 20/10 : galets, cordes, rochers : jamais couches
        float rigid_clear[4] = {g_grass_clear[0], g_grass_clear[1], g_grass_clear[2], 0.f};
        glUniform4fv(m_uniforms.grass_clear, 1, rigid_clear);
      } else {
        glUniform4fv(m_uniforms.grass_push, 1, g_grass_push);
        glUniform4fv(m_uniforms.grass_clear, 1, g_grass_clear);
      }
    }

    auto double_draw = setup_tfrag_shader(render_state, draw.mode, ShaderId::SHRUB);

    prof.add_draw_call();
    prof.add_tri(draw.num_triangles);

    tree.perf.draws++;

    if (render_state->no_multidraw) {
      glDrawElements(GL_TRIANGLE_STRIP, singledraw_indices.second, GL_UNSIGNED_INT,
                     (void*)(singledraw_indices.first * sizeof(u32)));
    } else {
      glMultiDrawElements(GL_TRIANGLE_STRIP,
                          &m_cache.multidraw_count_buffer[multidraw_indices.first], GL_UNSIGNED_INT,
                          &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
                          multidraw_indices.second);
    }

    switch (double_draw.kind) {
      case DoubleDrawKind::NONE:
        break;
      case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
        tree.perf.draws++;
        prof.add_draw_call();
        glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::SHRUB].id(), "alpha_min"),
                    -10.f);
        glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::SHRUB].id(), "alpha_max"),
                    double_draw.aref_second);
        glDepthMask(GL_FALSE);
        if (render_state->no_multidraw) {
          glDrawElements(GL_TRIANGLE_STRIP, singledraw_indices.second, GL_UNSIGNED_INT,
                         (void*)(singledraw_indices.first * sizeof(u32)));
        } else {
          glMultiDrawElements(
              GL_TRIANGLE_STRIP, &m_cache.multidraw_count_buffer[multidraw_indices.first],
              GL_UNSIGNED_INT, &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
              multidraw_indices.second);
        }
        break;
      default:
        ASSERT(false);
    }
  }

  glBindVertexArray(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glStencilMask(0xFF);
  glDisable(GL_STENCIL_TEST);
  tree.perf.draw_time.add(draw_timer.getSeconds());
  tree.perf.tree_time.add(tree_timer.getSeconds());
}

void Shrub::draw_debug_window() {}
