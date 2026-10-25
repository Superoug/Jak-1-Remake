#pragma once
#include "game/graphics/opengl_renderer/BucketRenderer.h"

struct MercDebugStats {
  int num_models = 0;
  int num_missing_models = 0;
  int num_chains = 0;
  int num_effects = 0;
  int num_predicted_draws = 0;
  int num_predicted_tris = 0;
  int num_bones_uploaded = 0;
  int num_lights = 0;
  int num_draw_flush = 0;

  int num_envmap_effects = 0;
  int num_envmap_tris = 0;

  int num_upload_bytes = 0;
  int num_uploads = 0;

  struct DrawDebug {
    DrawMode mode;
    int num_tris;
  };
  struct EffectDebug {
    bool envmap = false;
    DrawMode envmap_mode;
    std::vector<DrawDebug> draws;
  };
  struct ModelDebug {
    std::string name;
    std::string level;
    std::vector<EffectDebug> effects;
  };

  std::vector<ModelDebug> model_list;

  bool collect_debug_model_list = false;
};

class Merc2 {
 public:
  Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array);
  ~Merc2();
  void draw_debug_window(MercDebugStats* stats);
  void render(DmaFollower& dma,
              SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              MercDebugStats* stats);
  static constexpr int kMaxBlerc = 40;

 private:
  const std::vector<GLuint>* m_anim_slot_array;
  enum MercDataMemory {
    LOW_MEMORY = 0,
    BUFFER_BASE = 442,
    // this negative offset is what broke jak graphics in Dobiestation for a long time.
    BUFFER_OFFSET = -442
  };

  struct LowMemory {
    u8 tri_strip_tag[16];
    u8 ad_gif_tag[16];
    math::Vector4f hvdf_offset;
    math::Vector4f perspective[4];
    math::Vector4f fog;
  } m_low_memory;
  static_assert(sizeof(LowMemory) == 0x80);

  struct VuLights {
    math::Vector3f direction0;
    u32 w0;  // 12
    math::Vector3f direction1;
    u32 w1;  // 28
    math::Vector3f direction2;
    u32 w2;  // 44
    math::Vector4f color0;
    math::Vector4f color1;
    math::Vector4f color2;
    math::Vector4f ambient;
  };

  void handle_pc_model(const DmaTransfer& setup,
                       SharedRenderState* render_state,
                       ScopedProfilerNode& prof,
                       MercDebugStats* stats);
  u32 alloc_lights(const VuLights& lights);

  struct ModBuffers {
    GLuint vao, vertex;
  };

  static constexpr int kMaxEffect = 64;
  bool m_effect_debug_mask[kMaxEffect];

  struct MercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
  };

  struct ShaderMercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
    math::Vector4f pad;
    std::string to_string() const;
  };
  u32 alloc_bones(int count, ShaderMercMat* data);
  static constexpr int MAX_SKEL_BONES = 128;
  static constexpr int BONE_VECTORS_PER_BONE = 7;
  static constexpr int MAX_SHADER_BONE_VECTORS = 1024 * 32;  // ??

  static constexpr int MAX_LEVELS = 3;
  static constexpr int MAX_DRAWS_PER_LEVEL = 2048 * 2;
  static constexpr int MAX_ENVMAP_DRAWS_PER_LEVEL = MAX_DRAWS_PER_LEVEL;

  math::Vector4f m_shader_bone_vector_buffer[MAX_SHADER_BONE_VECTORS];

  struct Uniforms {
    GLuint light_direction[3];
    GLuint light_color[3];
    GLuint light_ambient;

    GLuint hvdf_offset;
    GLuint fog;

    GLuint tbone;
    GLuint nbone;

    GLuint fog_color;
    GLuint perspective_matrix;
    GLuint sun_light_from_cam;  // PC-REMAKE: character sun shadows

    GLuint ignore_alpha;
    GLuint decal;

    GLuint gfx_hack_no_tex;

    GLuint fade;

    // 21/10 : personnages remodeles
    GLuint remake_perso;
    GLuint remake_famille;
  };

  Uniforms m_merc_uniforms, m_emerc_uniforms;
  // 21/10 : PERSONNAGES REMODELES — programmes decoupes (base, reflets) et fourrure en couches
  Uniforms m_merc_tess_uniforms, m_emerc_tess_uniforms, m_merc_fur_uniforms;
  struct RemakeToggles {
    bool actif = true;      // /tmp/qa_perso_off : tout le remodelage coupe
    bool objets = true;     // /tmp/qa_obj_off : matieres des objets remodeles coupees
    bool decoupe = true;    // /tmp/qa_perso_notess : sans decoupage
    bool fourrure = true;   // /tmp/qa_perso_nofur : sans fourrure
  } m_remake;
  // familles de matiere des textures, par niveau charge (cle : load_id)
  std::unordered_map<u64, std::vector<u8>> m_tex_famille;
  u8 texture_famille(const LevelData* lev, s32 texture, bool* coquilles);
  // surfaces reflechissantes des textures (pc_reflect_texture_kind), par niveau charge
  std::unordered_map<u64, std::vector<u8>> m_tex_reflet;
  u8 texture_reflet(const LevelData* lev, s32 texture);
  void init_shaders(ShaderLibrary& shaders);
  GLuint m_detail_tex = 0;  // 21/10 v3 : cartes de detail des matieres (tableau, unite 5)
  int m_shaders_seq = 0;

  void init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights);
  void handle_setup_dma(DmaFollower& dma, SharedRenderState* render_state);
  void handle_all_dma(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof,
                      MercDebugStats* stats);
  void handle_merc_chain(DmaFollower& dma,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof,
                         MercDebugStats* stats);

  void switch_to_merc2(SharedRenderState* render_state);
  void switch_to_emerc(SharedRenderState* render_state);

  GLuint m_vao;

  void setup_merc_vao();

  std::vector<ModBuffers> m_mod_vtx_buffers;
  u32 m_next_mod_vtx_buffer = 0;

  static constexpr int MAX_MOD_VTX = UINT16_MAX;
  std::vector<tfrag3::MercVertex> m_mod_vtx_temp;

  struct UnpackTempVtx {
    float pos[4];
    float nrm[4];
    float uv[2];
  };
  std::vector<UnpackTempVtx> m_mod_vtx_unpack_temp;

  ModBuffers alloc_mod_vtx_buffer(const LevelData* lev);

  GLuint m_bones_buffer;

  enum DrawFlags {
    IGNORE_ALPHA = 1,
    MOD_VTX = 2,
    NO_TEXTURE = 4,
    CHARACTER = 8,   // 21/10 : personnage remodele
    FUR_SHELLS = 16, // 21/10 : fourrure en couches au-dessus de ce dessin
    OBJET = 32,      // PC-REMAKE : objet remodele (rendu des matieres, sans decoupage ni fourrure)
  };

  struct Draw {
    u32 first_index;
    u32 index_count;
    DrawMode mode;
    s32 texture;
    u32 num_triangles;
    u16 first_bone;
    u16 light_idx;
    u8 flags;
    ModBuffers mod_vtx_buffer;
    u8 fade[4];
    // no strip hack for custom models
    u8 no_strip;
    u8 famille;  // 21/10 : matiere (merc2.frag F_*), 0 = neutre
    u8 reflet;   // 21/10 : 1 = orbe de precurseur (reflet renforce, emerc.frag)
    u8 lave;     // 26/10 : 1 = nappe de lave (merc2.frag, lave qui ondule)
    u8 mouille;  // 25/10 : 1 = Jak ou Daxter (merc2.frag, personnage mouille)
    u8 miroir;   // metal et verre : 0 rien, 1 metal, 2 metal poli, 3 verre (reflets du post)
    u64 hash;
  };

  // PC-REMAKE: one entry per merc draw, kept until the NEXT frame's shadow
  // pass. The bone buffer on the GPU is only rewritten when the draws are
  // flushed, so at that point it still holds exactly these poses.
  struct ShadowCaster {
    unsigned int vertices;
    unsigned int indices;
    u32 first_index;
    u32 index_count;
    u16 first_bone;
    u8 no_strip;
  };
  std::vector<ShadowCaster> m_shadow_casters;
  float m_shadow_light_from_cam[16] = {};
  bool m_shadow_light_valid = false;
  // PC-REMAKE 21/09 : la matrice de normales merc->monde est valide ce frame
  bool m_world_normal_valid = false;
  static Merc2* s_shadow_instance;

 public:
  static void shadow_hook(void* shader_lib);
  void render_shadow_casters(void* shader_lib);

 private:
  struct LevelDrawBucket {
    const LevelData* level = nullptr;
    std::vector<Draw> draws;
    std::vector<Draw> envmap_draws;
    u32 next_free_draw = 0;
    u32 next_free_envmap_draw = 0;

    void reset() {
      level = nullptr;
      next_free_draw = 0;
      next_free_envmap_draw = 0;
    }
  };

  struct DrawArgs {
    LevelDrawBucket* lev_bucket;
    const u8* fade;
    bool jak1_water_mode;
    bool ignore_alpha;
    bool disable_fog;
    bool no_texture;
    u64 hash;
    u32 lights;
    u32 first_bone;
    bool character = false;          // 21/10
    bool objet = false;              // PC-REMAKE : objet remodele
    bool orbe = false;               // 21/10 : orbe de precurseur (money)
    bool lave = false;               // 26/10 : nappe de lave animee (water-anim-*-lava)
    bool mouille = false;            // 25/10 : Jak ou Daxter (peut etre mouille)
    const LevelData* lev = nullptr;  // 21/10 : familles de matiere des textures
  };

  Draw* alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args);

  Draw* try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                              const DrawMode& envmap_mode,
                              u32 envmap_texture,
                              const DrawArgs& args);

  void do_draws(const Draw* draw_array,
                const LevelData* lev,
                u32 num_draws,
                const Uniforms& uniforms,
                ScopedProfilerNode& prof,
                bool set_fade,
                SharedRenderState* render_state);

  static constexpr int MAX_LIGHTS = 1024;
  VuLights m_lights_buffer[MAX_LIGHTS];
  u32 m_next_free_light = 0;

  std::vector<LevelDrawBucket> m_level_draw_buckets;
  u32 m_next_free_level_bucket = 0;
  u32 m_next_free_bone_vector = 0;
  size_t m_opengl_buffer_alignment = 0;

  void flush_draw_buckets(SharedRenderState* render_state,
                          ScopedProfilerNode& prof,
                          MercDebugStats* stats);
  void model_mod_draws(int num_effects,
                       const tfrag3::MercModel* model,
                       const LevelData* lev,
                       const u8* input_data,
                       const DmaTransfer& setup,
                       ModBuffers* mod_opengl_buffers,
                       MercDebugStats* stats);
  void model_mod_blerc_draws(int num_effects,
                             const tfrag3::MercModel* model,
                             const LevelData* lev,
                             ModBuffers* mod_opengl_buffers,
                             const float* blerc_weights,
                             MercDebugStats* stats);
};
