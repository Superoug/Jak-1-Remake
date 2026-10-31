#pragma once

#include <optional>

#include "common/util/FilteredValue.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/pipelines/opengl.h"

class Shrub : public BucketRenderer {
 public:
  Shrub(const std::string& name, int my_id);
  ~Shrub();
  void init_shaders(ShaderLibrary& shaders) override;

  bool setup_for_level(const std::string& level, SharedRenderState* render_state);
  void render_all_trees(const TfragRenderSettings& settings,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);
  void render_tree(int idx,
                   const TfragRenderSettings& settings,
                   SharedRenderState* render_state,
                   ScopedProfilerNode& prof);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 private:
  void update_load(const LevelData* loader_data);
  void discard_tree_cache();

  struct Tree {
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint single_draw_index_buffer;
    GLuint time_of_day_texture;
    GLuint vao;
    u32 vert_count;
    const std::vector<tfrag3::ShrubDraw>* draws = nullptr;
    const std::vector<tfrag3::TieWindInstance>* instance_info = nullptr;
    // per-draw "is vegetation" flags (by texture name)
    std::vector<u8> veg_flags;
    // 1 = objet RIGIDE (planches, banc, corde, rocher)
    std::vector<u8> facet_flags;
    // 1 = vegetation (herbe, feuilles)
    std::vector<u8> veg_mark;
    // projecteur violet (spec /tmp/palm_debug)
    const std::vector<tfrag3::Texture>* tex_names = nullptr;
    unsigned int palm_debug_seq_built = 0;
    std::vector<char> draw_palm_debug;
    const tfrag3::PackedTimeOfDay* colors = nullptr;
    const u32* index_data = nullptr;
    // TRI DE VISIBILITE PAR CASES
    struct Cell {
      math::Vector4f bsphere;  // monde, rayon = demi-diagonale + marge vent/poussee
    };
    struct Segment {
      u32 first_index = 0;  // dans reordered_indices
      u32 count = 0;
      u32 cell = 0;
    };
    std::vector<Cell> cells;
    std::vector<std::vector<Segment>> draw_segments;  // par draw, tries par case
    std::vector<std::pair<u32, u32>> draw_full_range;  // bloc complet du draw
    std::vector<u32> reordered_indices;
    std::vector<u8> cell_visible;
    std::vector<bool> proto_vis_mask;
    std::unordered_map<std::string, std::vector<u32>> proto_name_to_idx;

    struct {
      u32 draws = 0;
      u32 wind_draws = 0;
      Filtered<float> cull_time;
      Filtered<float> index_time;
      Filtered<float> tod_time;
      Filtered<float> setup_time;
      Filtered<float> draw_time;
      Filtered<float> tree_time;
    } perf;
  };

  struct {
    GLuint decal;
    GLuint grass_push;  // player position + strength for grass bending
    GLuint grass_clear;  // herbe couchee pendant les cinematiques
    GLuint grass_wind;  // ambient sway (time + amplitude)
    GLuint shrub_facet;  // normale de facette (1) ou verticale (0)
  } m_uniforms;

  std::vector<Tree> m_trees;
  std::string m_level_name;
  const std::vector<GLuint>* m_textures;
  u64 m_load_id = -1;

  std::vector<math::Vector<u8, 4>> m_color_result;

  static constexpr int TIME_OF_DAY_COLOR_COUNT = 8192;
  bool m_has_level = false;

  static void build_cells(Tree& out, const tfrag3::ShrubTree& tree);
  // /tmp/qa_no_shrub_cull = tri par cases coupe (A/B)
  bool m_cull_enabled = true;
  int m_cull_poll = 0;
  struct {
    u64 frames = 0;
    u64 segs_total = 0;
    u64 segs_drawn = 0;
  } m_cull_stats;

  struct Cache {
    std::vector<std::pair<int, int>> draw_idx_temp;
    std::vector<u32> index_temp;
    std::vector<std::pair<int, int>> multidraw_offset_per_stripdraw;
    std::vector<GLsizei> multidraw_count_buffer;
    std::vector<void*> multidraw_index_offset_buffer;
  } m_cache;
  TfragPcPortData m_pc_port_data;
  const u8* m_proto_vis_data = nullptr;
  int m_proto_vis_data_size = 0;
};
