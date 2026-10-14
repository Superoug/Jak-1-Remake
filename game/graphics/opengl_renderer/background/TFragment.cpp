#include <map>

#include "TFragment.h"

#include <set>

#include "common/util/FileUtil.h"
#include "game/graphics/opengl_renderer/dma_helpers.h"

#include "third-party/imgui/imgui.h"

namespace {
bool looks_like_tfragment_dma(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::STCYCL;
}

bool looks_like_tfrag_init(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         follow.current_tag_vifcode1().kind == VifCode::Kind::DIRECT &&
         follow.current_tag_vifcode1().immediate == 2;
}
}  // namespace

TFragment::TFragment(const std::string& name,
                     int my_id,
                     const std::vector<tfrag3::TFragmentTreeKind>& trees,
                     bool child_mode,
                     int level_id,
                     const std::vector<GLuint>* anim_slot_array)
    : BucketRenderer(name, my_id),
      m_child_mode(child_mode),
      m_tree_kinds(trees),
      m_level_id(level_id),
      m_anim_slot_array(anim_slot_array) {
  for (auto& buf : m_buffered_data) {
    for (auto& x : buf.pad) {
      x = 0xff;
    }
  }

  glGenVertexArrays(1, &m_debug_vao);
  glBindVertexArray(m_debug_vao);
  glGenBuffers(1, &m_debug_verts);
  glBindBuffer(GL_ARRAY_BUFFER, m_debug_verts);
  glBufferData(GL_ARRAY_BUFFER, DEBUG_TRI_COUNT * 3 * sizeof(DebugVertex), nullptr,
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0,                                      // location 0 in the shader
                        3,                                      // 3 values per vert
                        GL_FLOAT,                               // floats
                        GL_FALSE,                               // normalized
                        sizeof(DebugVertex),                    // stride
                        (void*)offsetof(DebugVertex, position)  // offset (0)
  );

  glVertexAttribPointer(1,                                  // location 1 in the shader
                        4,                                  // 4 values per vert
                        GL_FLOAT,                           // floats
                        GL_FALSE,                           // normalized
                        sizeof(DebugVertex),                // stride
                        (void*)offsetof(DebugVertex, rgba)  // offset (0)
  );
  glBindVertexArray(0);
  // regardless of how many we use some fixed max
  // we won't actually interp or upload to gpu the unused ones, but we need a fixed maximum so
  // indexing works properly.
  m_color_result.resize(TIME_OF_DAY_COLOR_COUNT);
}

TFragment::~TFragment() {
  discard_tree_cache();
  glDeleteVertexArrays(1, &m_debug_vao);
}

void TFragment::render(DmaFollower& dma,
                       SharedRenderState* render_state,
                       ScopedProfilerNode& prof) {
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // First thing should be a NEXT with two nops.
  // unless we are a child, in which case our parent took this already.
  if (!m_child_mode) {
    auto data0 = dma.read_and_advance();
    ASSERT(data0.vifcode1().kind == VifCode::Kind::NOP);
    ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::MARK);
    ASSERT(data0.size_bytes == 0);
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  if (m_my_id == render_state->bucket_for_vis_copy &&
      dma.current_tag_vifcode1().kind == VifCode::Kind::PC_PORT) {
    DmaTransfer transfers[20];
    for (int i = 0; i < render_state->num_vis_to_copy; i++) {
      transfers[i] = dma.read_and_advance();
      auto next0 = dma.read_and_advance();
      ASSERT(next0.size_bytes == 0);
    }

    for (int i = 0; i < render_state->num_vis_to_copy; i++) {
      if (transfers[i].size_bytes == 128 * 16) {
        if (render_state->use_occlusion_culling) {
          render_state->occlusion_vis[i].valid = true;
          memcpy(render_state->occlusion_vis[i].data, transfers[i].data, 128 * 16);
        }
      } else {
        ASSERT(transfers[i].size_bytes == 16);
        // see VisDataHandler: a stale visibility string tears up a level that
        // is displayed while the player stands somewhere else
        render_state->occlusion_vis[i].valid = false;
      }
    }
  }

  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  std::string level_name;
  while (looks_like_tfrag_init(dma)) {
    handle_initialization(dma);
    if (level_name.empty()) {
      level_name = m_pc_port_data.level_name;
    } else if (level_name != m_pc_port_data.level_name) {
      ASSERT(false);
    }

    while (looks_like_tfragment_dma(dma)) {
      dma.read_and_advance();
    }
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }

  if (level_name.empty()) {
    return;
  }
  {
    setup_for_level(m_tree_kinds, level_name, render_state);
    TfragRenderSettings settings;

    settings.camera = m_pc_port_data.camera;
    settings.tree_idx = 0;
    if (render_state->occlusion_vis[m_level_id].valid) {
      settings.occlusion_culling = render_state->occlusion_vis[m_level_id].data;
    }

    update_render_state_from_pc_settings(render_state, m_pc_port_data);

    auto t3prof = prof.make_scoped_child("t3");
    render_matching_trees(lod(), m_tree_kinds, settings, render_state, t3prof);
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    auto tag = dma.current_tag().print();
    dma.read_and_advance();
  }
}

void TFragment::draw_debug_window() {
  for (int i = 0; i < (int)m_cached_trees.at(lod()).size(); i++) {
    auto& tree = m_cached_trees.at(lod()).at(i);
    if (tree.kind == tfrag3::TFragmentTreeKind::INVALID) {
      continue;
    }
    ImGui::PushID(i);
    ImGui::Text("[%d] %10s", i, tfrag3::tfrag_tree_names[(int)m_cached_trees[lod()][i].kind]);
    ImGui::SameLine();
    ImGui::Checkbox("Allow?", &tree.allowed);
    ImGui::SameLine();
    ImGui::Checkbox("Force?", &tree.forced);
    ImGui::SameLine();
    ImGui::Checkbox("cull debug (slow)", &tree.cull_debug);
    ImGui::PopID();
    if (tree.rendered_this_frame) {
      ImGui::Checkbox("freeze itimes", &tree.freeze_itimes);
      ImGui::Text("  tris: %d draws: %d", tree.tris_this_frame, tree.draws_this_frame);
      for (int j = 0; j < 4; j++) {
        ImGui::Text(" itimes[%d] 0x%x 0x%x 0x%x 0x%x", j, tree.itimes_debug[j][0],
                    tree.itimes_debug[j][1], tree.itimes_debug[j][2], tree.itimes_debug[j][3]);
      }
    }
  }
}

void TFragment::init_shaders(ShaderLibrary& shaders) {
  m_uniforms.decal = pc_uniform_loc(shaders[ShaderId::TFRAG3].id(), "decal");
}

void TFragment::handle_initialization(DmaFollower& dma) {
  // Set up test (different between different renderers)
  auto setup_test = dma.read_and_advance();
  ASSERT(setup_test.vif0() == 0);
  ASSERT(setup_test.vifcode1().kind == VifCode::Kind::DIRECT);
  ASSERT(setup_test.vifcode1().immediate == 2);
  ASSERT(setup_test.size_bytes == 32);
  memcpy(m_test_setup, setup_test.data, 32);

  // matrix 0
  auto mat0_upload = dma.read_and_advance();
  unpack_to_stcycl(&m_buffered_data[0].pad[TFragDataMem::TFragMatrix0 * 16], mat0_upload,
                   VifCode::Kind::UNPACK_V4_32, 4, 4, 64, TFragDataMem::TFragMatrix0, false, false);

  // matrix 1
  auto mat1_upload = dma.read_and_advance();
  unpack_to_stcycl(&m_buffered_data[1].pad[TFragDataMem::TFragMatrix0 * 16], mat1_upload,
                   VifCode::Kind::UNPACK_V4_32, 4, 4, 64, TFragDataMem::TFragMatrix1, false, false);

  // data
  auto data_upload = dma.read_and_advance();
  (void)data_upload;

  // call the setup program
  auto mscal_setup = dma.read_and_advance();
  verify_mscal(mscal_setup, TFragProgMem::TFragSetup);

  auto pc_port_data = dma.read_and_advance();
  ASSERT(pc_port_data.size_bytes == sizeof(TfragPcPortData));
  memcpy(&m_pc_port_data, pc_port_data.data, sizeof(TfragPcPortData));
  m_pc_port_data.level_name[11] = '\0';

  // setup double buffering.
  auto db_setup = dma.read_and_advance();
  ASSERT(db_setup.size_bytes == 0);
  ASSERT(db_setup.vifcode0().kind == VifCode::Kind::BASE &&
         db_setup.vifcode0().immediate == Buffer0_Start);
  ASSERT(db_setup.vifcode1().kind == VifCode::Kind::OFFSET &&
         db_setup.vifcode1().immediate == (Buffer1_Start - Buffer0_Start));
}

std::string TFragData::print() const {
  std::string result;
  result += fmt::format("fog: {}\n", fog.to_string_aligned());
  result += fmt::format("val: {}\n", val.to_string_aligned());
  result += fmt::format("str-gif: {}\n", str_gif.print());
  result += fmt::format("fan-gif: {}\n", fan_gif.print());
  result += fmt::format("ad-gif: {}\n", ad_gif.print());
  result += fmt::format("hvdf_offset: {}\n", hvdf_offset.to_string_aligned());
  result += fmt::format("hmge_scale: {}\n", hmge_scale.to_string_aligned());
  result += fmt::format("invh_scale: {}\n", invh_scale.to_string_aligned());
  result += fmt::format("ambient: {}\n", ambient.to_string_aligned());
  result += fmt::format("guard: {}\n", guard.to_string_aligned());
  result += fmt::format("k0s[0]: {}\n", k0s[0].to_string_aligned());
  result += fmt::format("k0s[1]: {}\n", k0s[1].to_string_aligned());
  result += fmt::format("k1s[0]: {}\n", k1s[0].to_string_aligned());
  result += fmt::format("k1s[1]: {}\n", k1s[1].to_string_aligned());
  return result;
}

void TFragment::update_load(const std::vector<tfrag3::TFragmentTreeKind>& tree_kinds,
                            const LevelData* loader_data) {
  const auto* lev_data = loader_data->level.get();
  discard_tree_cache();
  for (int geom = 0; geom < GEOM_MAX; ++geom) {
    m_cached_trees[geom].clear();
  }

  u32 time_of_day_count = 0;
  size_t vis_temp_len = 0;
  size_t max_draws = 0;
  size_t max_num_grps = 0;
  size_t max_inds = 0;

  for (int geom = 0; geom < GEOM_MAX; ++geom) {
    for (size_t tree_idx = 0; tree_idx < lev_data->tfrag_trees[geom].size(); tree_idx++) {
      const auto& tree = lev_data->tfrag_trees[geom][tree_idx];

      if (std::find(tree_kinds.begin(), tree_kinds.end(), tree.kind) != tree_kinds.end()) {
        auto& tree_cache = m_cached_trees[geom].emplace_back();
        tree_cache.kind = tree.kind;
        max_draws = std::max(tree.draws.size(), max_draws);
        size_t num_grps = 0;
        for (auto& draw : tree.draws) {
          num_grps += draw.vis_groups.size();
        }
        max_num_grps = std::max(max_num_grps, num_grps);
        max_inds = std::max(tree.unpacked.indices.size(), max_inds);
        time_of_day_count = std::max(tree.colors.color_count, time_of_day_count);
        u32 verts = tree.packed_vertices.vertices.size();
        glGenVertexArrays(1, &tree_cache.vao);
        glBindVertexArray(tree_cache.vao);
        // glGenBuffers(1, &tree_cache.vertex_buffer);
        tree_cache.vertex_buffer = loader_data->tfrag_vertex_data[geom][tree_idx];
        tree_cache.vert_count = verts;
        tree_cache.draws = &tree.draws;  // todo - should we just copy this?
        // projecteur violet : la classification se fait à chaud dans render_tree
        // (la spec de /tmp/palm_debug change en cours de jeu)
        tree_cache.tex_names = &lev_data->textures;
        tree_cache.palm_debug_seq_built = 0;
        // inventaire des textures du niveau, loggé UNE fois (base de la chasse
        // par dichotomie : grep 'texinv' dans le log de gk)
        {
          static std::set<const void*> inv_logged;
          if (inv_logged.insert((const void*)&lev_data->textures).second) {
            for (size_t ti = 0; ti < lev_data->textures.size(); ti++) {
              lg::info("PC-REMAKE texinv idx={} '{}'", ti, lev_data->textures[ti].debug_name);
            }
          }
        }
        // [QA 22/09] modes des draws de feuillage (tfrag) — le toit se voyait a
        // travers la feuille
        for (const auto& d : tree.draws) {
          std::string tn = (d.tree_tex_id >= 0 && d.tree_tex_id < (int)lev_data->textures.size())
                               ? lev_data->textures[d.tree_tex_id].debug_name
                               : std::string("?");
          if (tn.find("palm") == std::string::npos && tn.find("leaf") == std::string::npos) {
            continue;
          }
          const auto& m = d.mode;
          lg::info("[QA-FEUILLE] tfrag kind={} tex='{}' tris={} zwrite={} ztest={} ablend={}({}) "
                   "atest={}({}) aref={} afail={} decal={}",
                   (int)tree.kind, tn, d.num_triangles, (int)m.get_depth_write_enable(),
                   (int)m.get_zt_enable(), (int)m.get_ab_enable(), (int)m.get_alpha_blend(),
                   (int)m.get_at_enable(), (int)m.get_alpha_test(), (int)m.get_aref(),
                   (int)m.get_alpha_fail(), (int)m.get_decal());
        }
        // [QA-VISTA 25/08] : qui habille la doublure lointaine ? On liste les
        // textures reellement utilisees par CHAQUE arbre, avec son type.
        {
          std::map<std::string, int> use;
          for (const auto& d : tree.draws) {
            if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)lev_data->textures.size()) {
              use[lev_data->textures[d.tree_tex_id].debug_name] += d.num_triangles;
            }
          }
          std::string top;
          int shown = 0;
          for (const auto& kv : use) {
            if (shown++ < 12) {
              top += fmt::format("{}({}) ", kv.first, kv.second);
            }
          }
          lg::info("[QA-VISTA] arbre {} type {} : {} draws, textures = {}", tree_idx,
                   (int)tree.kind, (int)tree.draws.size(), top);
          if (tree.kind == tfrag3::TFragmentTreeKind::LOWRES) {
            for (size_t di = 0; di < tree.draws.size(); di++) {
              const auto& d = tree.draws[di];
              const auto& m = d.mode;
              lg::info(
                  "[QA-VISTA-DRAW] {} tex='{}' tris={} zwrite={} ztest={} ablend={} atest={} "
                  "aref={} decal={} filt={}",
                  di,
                  d.tree_tex_id >= 0 && d.tree_tex_id < (int)lev_data->textures.size()
                      ? lev_data->textures[d.tree_tex_id].debug_name
                      : std::string("?"),
                  d.num_triangles, (int)m.get_depth_write_enable(), (int)m.get_zt_enable(),
                  (int)m.get_ab_enable(), (int)m.get_at_enable(), (int)m.get_aref(),
                  (int)m.get_decal(), (int)m.get_filt_enable());
            }
          }
        }
        tree_cache.colors = &tree.colors;
        tree_cache.vis = &tree.bvh;
        tree_cache.index_data = tree.unpacked.indices.data();
        tree_cache.index_count = (u32)tree.unpacked.indices.size();
        tree_cache.draw_mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
        vis_temp_len = std::max(vis_temp_len, tree.bvh.vis_nodes.size());
        glBindBuffer(GL_ARRAY_BUFFER, tree_cache.vertex_buffer);
        //            glBufferData(GL_ARRAY_BUFFER, verts * sizeof(tfrag3::PreloadedVertex),
        //            nullptr,
        //                         GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);

        glVertexAttribPointer(0,                                // location 0 in the shader
                              3,                                // 3 values per vert
                              GL_FLOAT,                         // floats
                              GL_FALSE,                         // normalized
                              sizeof(tfrag3::PreloadedVertex),  // stride
                              (void*)offsetof(tfrag3::PreloadedVertex, x)  // offset (0)
        );

        glVertexAttribPointer(1,                                // location 1 in the shader
                              3,                                // 3 values per vert
                              GL_FLOAT,                         // floats
                              GL_FALSE,                         // normalized
                              sizeof(tfrag3::PreloadedVertex),  // stride
                              (void*)offsetof(tfrag3::PreloadedVertex, s)  // offset (0)
        );

        glVertexAttribIPointer(2,                                // location 2 in the shader
                               2,                                // 1 values per vert
                               GL_UNSIGNED_SHORT,                // u16
                               sizeof(tfrag3::PreloadedVertex),  // stride
                               (void*)offsetof(tfrag3::PreloadedVertex, color_index)  // offset (0)
        );
        // PC-REMAKE : normales lissées (subdiv Phong) — le battement des
        // feuillages fondus dans le terrain se fait le long de la normale
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_INT_2_10_10_10_REV, GL_TRUE,
                              sizeof(tfrag3::PreloadedVertex),
                              (void*)offsetof(tfrag3::PreloadedVertex, nor));
        // PC-REMAKE : squelette de couronne (fr, az, hn, id) gravé dans rgba au
        // chargement — mêmes données que les palmiers TIE, mêmes formules
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                              sizeof(tfrag3::PreloadedVertex),
                              (void*)offsetof(tfrag3::PreloadedVertex, r));
        glGenBuffers(1, &tree_cache.single_draw_index_buffer);
        glGenBuffers(1, &tree_cache.index_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tree_cache.index_buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                     tree.unpacked.indices.data(), GL_STREAM_DRAW);

        glGenTextures(1, &tree_cache.time_of_day_texture);
        glBindTexture(GL_TEXTURE_1D, tree_cache.time_of_day_texture);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, TIME_OF_DAY_COLOR_COUNT, 0, GL_RGBA,
                     GL_UNSIGNED_INT_8_8_8_8, nullptr);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindVertexArray(0);
      }
    }
  }

  m_cache.vis_temp.resize(vis_temp_len);
  m_cache.multidraw_offset_per_stripdraw.resize(max_draws);
  m_cache.multidraw_count_buffer.resize(max_num_grps);
  m_cache.multidraw_index_offset_buffer.resize(max_num_grps);
  m_cache.draw_idx_temp.resize(max_draws);
  m_cache.index_temp.resize(max_inds);
  ASSERT(time_of_day_count <= TIME_OF_DAY_COLOR_COUNT);
}

bool TFragment::setup_for_level(const std::vector<tfrag3::TFragmentTreeKind>& tree_kinds,
                                const std::string& level,
                                SharedRenderState* render_state) {
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
    return setup_for_level(tree_kinds, level, render_state);
  }

  m_load_id = lev_data->load_id;

  if (m_level_name != level) {
    update_load(tree_kinds, lev_data);
    m_has_level = true;
    m_textures = &lev_data->textures;
    m_level_name = level;
  } else {
    m_has_level = true;
  }

  if (tfrag3_setup_timer.getMs() > 5) {
    lg::info("TFRAG setup: {:.1f}ms", tfrag3_setup_timer.getMs());
  }

  return m_has_level;
}

void TFragment::render_tree(int geom,
                            const TfragRenderSettings& settings,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& prof) {
  if (!m_has_level) {
    return;
  }
  auto& tree = m_cached_trees.at(geom).at(settings.tree_idx);
  [[maybe_unused]] const auto* itimes = settings.camera.itimes;

  if (tree.freeze_itimes) {
    itimes = tree.itimes_debug;
  } else {
    for (int i = 0; i < 4; i++) {
      tree.itimes_debug[i] = settings.camera.itimes[i];
    }
  }

  ASSERT(tree.kind != tfrag3::TFragmentTreeKind::INVALID);

  if (m_color_result.size() < tree.colors->color_count) {
    m_color_result.resize(tree.colors->color_count);
  }
  // [QA-TOD] 14/10 : temps CPU du calcul des couleurs d'heure / de l'envoi / du reste
  static double qa_tod[3] = {0, 0, 0};
  static int qa_tod_n = 0;
  Timer qa_t;
  interp_time_of_day(settings.camera.itimes, *tree.colors, m_color_result.data());
  qa_tod[0] += qa_t.getMs();
  qa_t.start();
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
  glTexSubImage1D(GL_TEXTURE_1D, 0, 0, tree.colors->color_count, GL_RGBA,
                  GL_UNSIGNED_INT_8_8_8_8_REV, m_color_result.data());
  qa_tod[1] += qa_t.getMs();
  if (++qa_tod_n == 1200) {
    lg::info("[QA-TOD] tfrag ms/appel : couleurs {:.3f} | envoi {:.3f}", qa_tod[0] / 1200,
             qa_tod[1] / 1200);
    qa_tod[0] = qa_tod[1] = 0;
    qa_tod_n = 0;
  }

  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::TFRAG3);
  // bascule à chaud du mode « peinture violette » (fichier témoin)
  update_palm_debug_flag();
  // PC-REMAKE : temps + amplitude (réglable au REPL) du vent des feuillages
  // fondus dans le terrain
  glUniform1f(
      pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "wind_time"),
      (float)(render_state->frame_idx % 216000) / 60.f);
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "tfrag_wind_amp"),
              g_palm_wind[2]);

  // PC-REMAKE sun shadows: refresh the light matrix once per frame and feed
  // the map + uniforms to the receiving shader.
  g_sun_shadow.maybe_update_for_frame(settings.camera.trans.data(), render_state->frame_idx);
  if (g_sun_shadow.enabled()) {
    g_sun_shadow.bind_for_receive(render_state->shaders[ShaderId::TFRAG3].id());
  } else {
    // 21/09 : sampler toujours pose sur l'unite 14 (voir bind_disabled)
    g_sun_shadow.bind_disabled(render_state->shaders[ShaderId::TFRAG3].id());
  }
  // PC-REMAKE "Spyro water": WATER kind trees get animated ripples, caustic
  // shimmer and a sun sparkle in the shader. x=time, y=strength.
  {
    float wfx[4] = {(float)(render_state->frame_idx % 100000) / 60.f,
                    tree.kind == tfrag3::TFragmentTreeKind::WATER ? 1.f : 0.f, 0.f, 0.f};
    glUniform4fv(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "water_fx"), 1,
                 wfx);
  }

  glBindVertexArray(tree.vao);
  glBindBuffer(GL_ARRAY_BUFFER, tree.vertex_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
               render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  cull_check_all_slow(settings.camera.planes, tree.vis->vis_nodes, settings.occlusion_culling,
                      m_cache.vis_temp.data());

  u32 total_tris;
  if (render_state->no_multidraw) {
    u32 idx_buffer_size = make_index_list_from_vis_string(
        m_cache.draw_idx_temp.data(), m_cache.index_temp.data(), *tree.draws, m_cache.vis_temp,
        tree.index_data, &total_tris);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_size * sizeof(u32), m_cache.index_temp.data(),
                 GL_STREAM_DRAW);
  } else {
    total_tris = make_multidraws_from_vis_string(
        m_cache.multidraw_offset_per_stripdraw.data(), m_cache.multidraw_count_buffer.data(),
        m_cache.multidraw_index_offset_buffer.data(), *tree.draws, m_cache.vis_temp);
  }

  prof.add_tri(total_tris);

  // projecteur violet : reclassification à chaud quand la spec change
  if (tree.palm_debug_seq_built != g_palm_debug_seq && tree.tex_names && tree.draws) {
    tree.palm_debug_seq_built = g_palm_debug_seq;
    tree.draw_palm_debug.assign(tree.draws->size(), 0);
    for (size_t i = 0; i < tree.draws->size(); i++) {
      const auto& d = (*tree.draws)[i];
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)tree.tex_names->size() &&
          palm_debug_matches(d.tree_tex_id, (*tree.tex_names)[d.tree_tex_id].debug_name)) {
        tree.draw_palm_debug[i] = 1;
      }
    }
  }

  // PC-REMAKE émissives : auto-illumination par draw, bâtie une fois par arbre
  // (la lave EST sa propre lumière — 18/08)
  if (tree.draw_emissive.size() != tree.draws->size() && tree.tex_names && tree.draws) {
    tree.draw_emissive.assign(tree.draws->size(), 0.f);
    for (size_t i = 0; i < tree.draws->size(); i++) {
      const auto& d = (*tree.draws)[i];
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)tree.tex_names->size()) {
        tree.draw_emissive[i] =
            pc_emissive_texture_boost((*tree.tex_names)[d.tree_tex_id].debug_name);
      }
    }
  }

  // PC-REMAKE 21/09 : detail intrinseque des sols, par draw (regle par nom)
  update_detail_gain();
  if (tree.draw_detail.size() != tree.draws->size() && tree.tex_names && tree.draws) {
    tree.draw_detail.assign(tree.draws->size(), 0.f);
    for (size_t i = 0; i < tree.draws->size(); i++) {
      const auto& d = (*tree.draws)[i];
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)tree.tex_names->size()) {
        tree.draw_detail[i] = pc_detail_texture_amount((*tree.tex_names)[d.tree_tex_id].debug_name);
      }
    }
  }
  const GLint detail_loc =
      pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "detail_amt");

  // PC-REMAKE 21/09 : le shader TFRAG3 ecrit la normale monde dans le G-buffer
  pc_normal_target(true);
  for (size_t draw_idx = 0; draw_idx < tree.draws->size(); draw_idx++) {
    const auto& draw = tree.draws->operator[](draw_idx);
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

    ASSERT(m_textures);
    s32 tex_idx = draw.tree_tex_id;
    if (tex_idx >= 0) {
      glBindTexture(GL_TEXTURE_2D, m_textures->at(draw.tree_tex_id));
    } else {
      glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(-(tex_idx + 1)));
    }
    auto double_draw = setup_tfrag_shader(render_state, draw.mode, ShaderId::TFRAG3);
    // [QA-TEST 25/08] vista : on desarme le test d'alpha (seuil 38) qui perce
    // le decor lointain en tamis
    glUniform1i(m_uniforms.decal, draw.mode.get_decal() ? 1 : 0);
    // test d'identité des penchés : draws ciblés peints en violet
    glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "palm_debug"),
                (g_palm_debug > 0.5f && draw_idx < tree.draw_palm_debug.size() &&
                 tree.draw_palm_debug[draw_idx])
                    ? 1.f
                    : 0.f);
    // émissives : auto-illumination de la surface (0 = normal)
    glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "emissive_boost"),
                draw_idx < tree.draw_emissive.size() ? tree.draw_emissive[draw_idx] : 0.f);
    glUniform1f(detail_loc, (draw_idx < tree.draw_detail.size() ? tree.draw_detail[draw_idx] : 0.f) *
                                g_pc_detail_gain);
    tree.tris_this_frame += draw.num_triangles;
    tree.draws_this_frame++;

    prof.add_draw_call();
    if (render_state->no_multidraw) {
      glDrawElements(tree.draw_mode, singledraw_indices.second, GL_UNSIGNED_INT,
                     (void*)(singledraw_indices.first * sizeof(u32)));
    } else {
      glMultiDrawElements(tree.draw_mode, &m_cache.multidraw_count_buffer[multidraw_indices.first],
                          GL_UNSIGNED_INT,
                          &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
                          multidraw_indices.second);
    }

    switch (double_draw.kind) {
      case DoubleDrawKind::NONE:
        break;
      case DoubleDrawKind::AFAIL_NO_DEPTH_WRITE:
        prof.add_draw_call();
        glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "alpha_min"),
                    -10.f);
        glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "alpha_max"),
                    double_draw.aref_second);
        glDepthMask(GL_FALSE);
        if (render_state->no_multidraw) {
          glDrawElements(tree.draw_mode, singledraw_indices.second, GL_UNSIGNED_INT,
                         (void*)(singledraw_indices.first * sizeof(u32)));
        } else {
          glMultiDrawElements(
              tree.draw_mode, &m_cache.multidraw_count_buffer[multidraw_indices.first],
              GL_UNSIGNED_INT, &m_cache.multidraw_index_offset_buffer[multidraw_indices.first],
              multidraw_indices.second);
        }
        break;
      default:
        ASSERT(false);
    }
  }
  // le programme TFRAG3 est partagé (replays d'ombres, etc.) : ne pas laisser
  // traîner la teinte violette du dernier draw
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3].id(), "palm_debug"),
              0.f);
  pc_normal_target(false);

  // PC-REMAKE sun shadows: render the WHOLE tree into the shadow map (not the
  // frustum-culled subset — otherwise shadows pop as the camera turns).
  // Only solid ground kinds cast.
  if (g_sun_shadow.enabled() &&
      (tree.kind == tfrag3::TFragmentTreeKind::NORMAL ||
       tree.kind == tfrag3::TFragmentTreeKind::DIRT)) {
    // 14/10 : passe d'ombres DIFFEREE (voir SunShadowMap::flush_deferred)
    if (g_sun_shadow.deferred()) {
      static bool shadow_cull_d = true;
      static int shadow_cull_poll_d = 0;
      if ((shadow_cull_poll_d++ % 256) == 0) {
        shadow_cull_d = !fs::exists("/tmp/qa_no_shadow_cull");
      }
      auto& rec = g_sun_shadow.deferred_static.emplace_back();
      rec.vao = tree.vao;
      rec.ebo = tree.index_buffer;
      rec.mode = tree.draw_mode;
      if (shadow_cull_d && tree.vis && tree.draws) {
        static std::vector<u8> svis_d;
        shadow_vis_from_light(g_sun_shadow.light_vp, tree.vis->vis_nodes, svis_d);
        append_shadow_ranges(*tree.draws, 0, tree.draws->size(), svis_d, rec.counts,
                             rec.offsets);
      } else {
        rec.counts.push_back(tree.index_count);
        rec.offsets.push_back(nullptr);
      }
      if (rec.counts.empty()) {
        g_sun_shadow.deferred_static.pop_back();
      }
    } else
    // static scenery: only replayed when the cached layer is stale
    if (g_sun_shadow.begin_static_replay(&render_state->shaders)) {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, tree.index_buffer);
      // 14/10 : seuls les groupes qui touchent la carte d'ombres (voir
      // shadow_vis_from_light) — original : glDrawElements(tree.index_count) de tout
      // l'arbre. /tmp/qa_no_shadow_cull = ancien chemin (A/B).
      static bool shadow_cull = true;
      static int shadow_cull_poll = 0;
      if ((shadow_cull_poll++ % 256) == 0) {
        shadow_cull = !fs::exists("/tmp/qa_no_shadow_cull");
      }
      u64 kept = tree.index_count;
      if (shadow_cull && tree.vis && tree.draws) {
        static std::vector<GLsizei> counts;
        static std::vector<void*> offsets;
        static std::vector<u8> svis;
        counts.clear();
        offsets.clear();
        shadow_vis_from_light(g_sun_shadow.light_vp, tree.vis->vis_nodes, svis);
        kept = append_shadow_ranges(*tree.draws, 0, tree.draws->size(), svis, counts, offsets);
        if (!counts.empty()) {
          glMultiDrawElements(tree.draw_mode, counts.data(), GL_UNSIGNED_INT, offsets.data(),
                              counts.size());
        }
      } else {
        glDrawElements(tree.draw_mode, tree.index_count, GL_UNSIGNED_INT, nullptr);
      }
      {
        static int qa_n = 0;
        if ((qa_n++ % 600) == 0) {
          lg::warn("[QA] replay TFRAG: {} / {} indices", kept, tree.index_count);
        }
      }
      // restore the per-mode element buffer for subsequent draws
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                   render_state->no_multidraw ? tree.single_draw_index_buffer : tree.index_buffer);
      g_sun_shadow.end_depth_pass();
    }
  }

  glBindVertexArray(0);
}

/*!
 * Render all trees with settings for the given tree.
 * This is intended to be used only for debugging when we can't easily get commands for all trees
 * working.
 */
void TFragment::render_all_trees(int geom,
                                 const TfragRenderSettings& settings,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof) {
  TfragRenderSettings settings_copy = settings;
  for (size_t i = 0; i < m_cached_trees[geom].size(); i++) {
    if (m_cached_trees[geom][i].kind != tfrag3::TFragmentTreeKind::INVALID) {
      settings_copy.tree_idx = i;
      render_tree(geom, settings_copy, render_state, prof);
    }
  }
}

void TFragment::render_matching_trees(int geom,
                                      const std::vector<tfrag3::TFragmentTreeKind>& trees,
                                      const TfragRenderSettings& settings,
                                      SharedRenderState* render_state,
                                      ScopedProfilerNode& prof) {
  TfragRenderSettings settings_copy = settings;
  for (size_t i = 0; i < m_cached_trees[geom].size(); i++) {
    auto& tree = m_cached_trees[geom][i];
    tree.reset_stats();
    if (!tree.allowed) {
      continue;
    }
    if (std::find(trees.begin(), trees.end(), tree.kind) != trees.end() || tree.forced) {
      // [QA-LOWRES 21/09] experience decisive du treillis lointain : NORMAL et
      // LOWRES (la doublure basse resolution du MEME niveau) sont dessines
      // ensemble -> combat de profondeur au loin ? QA_NO_LOWRES=1 saute LOWRES
      // et LOWRES_TRANS. Sans la variable, aucun effet.
      {
        static const bool no_lowres = [] {
          const char* e = getenv("QA_NO_LOWRES");
          return e && e[0] == '1';
        }();
        if (no_lowres && (tree.kind == tfrag3::TFragmentTreeKind::LOWRES ||
                          tree.kind == tfrag3::TFragmentTreeKind::LOWRES_TRANS)) {
          continue;
        }
      }
      tree.rendered_this_frame = true;
      settings_copy.tree_idx = i;
      // 13/10 (herbes au sol trop transparentes) :
      // les couches de SALISSURE (chemins de sable, tfrag DIRT, semi-transparentes)
      // sont dessinees APRES les arbustes et recouvraient le bas des touffes —
      // le sable du chemin traversait l'herbe. Les arbustes de vegetation marquent
      // leurs pixels (stencil 0x80, Shrub.cpp) : la salissure ne les touche plus.
      const bool dirt = tree.kind == tfrag3::TFragmentTreeKind::DIRT;
      if (dirt) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x00);
        glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      }
      render_tree(geom, settings_copy, render_state, prof);
      if (dirt) {
        glStencilMask(0xFF);
        glDisable(GL_STENCIL_TEST);
      }
      if (tree.cull_debug) {
        render_tree_cull_debug(settings_copy, render_state, prof);
      }
    }
  }
}

void TFragment::discard_tree_cache() {
  m_textures = nullptr;
  for (int geom = 0; geom < GEOM_MAX; ++geom) {
    for (auto& tree : m_cached_trees[geom]) {
      if (tree.kind != tfrag3::TFragmentTreeKind::INVALID) {
        glBindTexture(GL_TEXTURE_1D, tree.time_of_day_texture);
        glDeleteTextures(1, &tree.time_of_day_texture);
        glDeleteBuffers(1, &tree.single_draw_index_buffer);
        glDeleteBuffers(1, &tree.index_buffer);
        glDeleteVertexArrays(1, &tree.vao);
      }
    }
    m_cached_trees[geom].clear();
  }
}

namespace {

float frac(float in) {
  return in - (int)in;
}

void debug_vis_draw(int first_root,
                    int tree,
                    int num,
                    int depth,
                    const std::vector<tfrag3::VisNode>& nodes,
                    std::vector<TFragment::DebugVertex>& verts_out) {
  for (int ki = 0; ki < num; ki++) {
    auto& node = nodes.at(ki + tree - first_root);
    ASSERT(node.child_id != 0xffff);
    math::Vector4f rgba{frac(0.4 * depth), frac(0.7 * depth), frac(0.2 * depth), 0.06};
    math::Vector3f center = node.bsphere.xyz();
    float rad = node.bsphere.w();
    math::Vector3f corners[8] = {center, center, center, center};
    corners[0].x() += rad;
    corners[1].x() += rad;
    corners[2].x() -= rad;
    corners[3].x() -= rad;

    corners[0].y() += rad;
    corners[1].y() -= rad;
    corners[2].y() += rad;
    corners[3].y() -= rad;

    for (int i = 0; i < 4; i++) {
      corners[i + 4] = corners[i];
      corners[i].z() += rad;
      corners[i + 4].z() -= rad;
    }

    if (true) {
      for (int i : {0, 4}) {
        verts_out.push_back({corners[0 + i], rgba});
        verts_out.push_back({corners[1 + i], rgba});
        verts_out.push_back({corners[2 + i], rgba});

        verts_out.push_back({corners[1 + i], rgba});  // 0
        verts_out.push_back({corners[3 + i], rgba});
        verts_out.push_back({corners[2 + i], rgba});
      }

      for (int i : {2, 6, 7, 2, 3, 7, 0, 4, 5, 0, 5, 1, 0, 6, 4, 0, 6, 2, 1, 3, 7, 1, 5, 7}) {
        verts_out.push_back({corners[i], rgba});
      }

      constexpr int border0[12] = {0, 4, 6, 2, 2, 6, 3, 7, 0, 1, 2, 3};
      constexpr int border1[12] = {1, 5, 7, 3, 0, 4, 1, 5, 4, 5, 6, 7};
      rgba.w() = 1.0;

      for (int i = 0; i < 12; i++) {
        auto p0 = corners[border0[i]];
        auto p1 = corners[border1[i]];
        auto diff = (p1 - p0).normalized();
        math::Vector3f px = diff.z() == 0 ? math::Vector3f{1, 0, 1} : math::Vector3f{0, 1, 1};
        auto off = diff.cross(px) * 2000;

        verts_out.push_back({p0 + off, rgba});
        verts_out.push_back({p0 - off, rgba});
        verts_out.push_back({p1 - off, rgba});

        verts_out.push_back({p0 + off, rgba});
        verts_out.push_back({p1 + off, rgba});
        verts_out.push_back({p1 - off, rgba});
      }
    }

    if (node.flags) {
      debug_vis_draw(first_root, node.child_id, node.num_kids, depth + 1, nodes, verts_out);
    }
  }
}

}  // namespace

void TFragment::render_tree_cull_debug(const TfragRenderSettings& settings,
                                       SharedRenderState* render_state,
                                       ScopedProfilerNode& prof) {
  // generate debug verts:
  m_debug_vert_data.clear();
  auto& tree = m_cached_trees.at(settings.tree_idx).at(lod());

  debug_vis_draw(tree.vis->first_root, tree.vis->first_root, tree.vis->num_roots, 1,
                 tree.vis->vis_nodes, m_debug_vert_data);

  render_state->shaders[ShaderId::TFRAG3_NO_TEX].activate();
  glUniformMatrix4fv(
      pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3_NO_TEX].id(), "camera"), 1,
      GL_FALSE, settings.camera.camera[0].data());
  glUniform4f(
      pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3_NO_TEX].id(), "hvdf_offset"),
      settings.camera.hvdf_off[0], settings.camera.hvdf_off[1], settings.camera.hvdf_off[2],
      settings.camera.hvdf_off[3]);
  glUniform1f(
      pc_uniform_loc(render_state->shaders[ShaderId::TFRAG3_NO_TEX].id(), "fog_constant"),
      settings.camera.fog.x());
  // glDisable(GL_DEPTH_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // ?
  glDepthMask(GL_FALSE);

  glBindVertexArray(m_debug_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_debug_verts);

  int remaining = m_debug_vert_data.size();
  int start = 0;

  while (remaining > 0) {
    int to_do = std::min(DEBUG_TRI_COUNT * 3, remaining);

    glBufferSubData(GL_ARRAY_BUFFER, 0, to_do * sizeof(DebugVertex),
                    m_debug_vert_data.data() + start);
    glDrawArrays(GL_TRIANGLES, 0, to_do);
    prof.add_draw_call();
    prof.add_tri(to_do / 3);

    remaining -= to_do;
    start += to_do;
  }
}
