#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

// grass interaction
extern float g_grass_push[4];
extern float g_grass_clear[4];  // herbe couchee pendant les cinematiques

// presentation post effects (bloom / saturation / warmth)
extern float g_post_fx[4];

// sun shadow mapping
extern float g_sun_shadow_params[4];

// toon / cel-shading post pass (x=posterize, y=edges, z=levels)
extern float g_toon_fx[4];

// reglages de qualite des effets (pour réduire ou enlever certains reflets)
extern float g_fx_quality[4];

// rebuilt lighting rig
extern float g_light_fx[4];
extern float g_sky_color[4];
extern float g_ground_color[4];
// x = night amount
extern float g_night_fx[4];
// réglage EN DIRECT du vent des palmiers (pc-set-palm-wind!) x = échelle houle TIE (1 = validé)
extern float g_palm_wind[4];
// scintillement de l'eau (reflets du soleil/de la lune, )
extern float g_water_glitter[4];
// PROJECTEUR VIOLET
extern float g_palm_debug;
extern std::string g_palm_debug_spec;
extern u32 g_palm_debug_seq;
void update_palm_debug_flag();
// detail intrinseque des sols (voir tfrag3.frag)
extern float g_pc_detail_gain;
extern float g_pc_force_opaque;
void update_detail_gain();
float pc_detail_texture_amount(const std::string& debug_name);
bool palm_debug_matches(s32 tex_idx, const std::string& name);

// local lamps
constexpr int kMaxRemakeLamps = 48;  // uploaded to the shader (24 → 48 : une
                                       // mer de lave mérite plus de 24 sources)
constexpr int kMaxRemakePending = 384;  // collected before the nearest are picked
                                       // 128 → 384 : les lumières émissives scannées par niveau s'y ajoutent
void pc_remake_lamps_begin_frame();
// le jeu valide sa liste de lampes en envoyant l'image
void pc_remake_lamps_commit();
// TRACES DANS LA NEIGE
extern float g_snow_step[4];
void pc_snow_trail_frame(unsigned long long frame_idx);
unsigned int pc_snow_trail_tex();
unsigned int pc_sand_tex();  // empreintes dans le sable (post_processing.frag)
// grille 128 x 128 (cases de 0,8 m)
unsigned int pc_snow_cells_tex();
// le sol de neige a ete vraiment creuse cette image (passe decoupee de TFragment)
extern bool g_snow_geom_drawn;
// image ou la neige decoupee a ete dessinee
extern u64 g_snow_geom_frame;
// incremente a chaque rechargement a chaud des shaders des personnages
extern int g_pc_merc_shaders_seq;
// faisceaux d'eco (segments monde)
constexpr int kMaxRemakeBeams = 12;
void pc_remake_add_beam(float x0, float y0, float z0, float x1, float y1, float z1);
int pc_remake_beam_count();
const float* pc_remake_beams();  // 2 x vec4 par faisceau (extremites, w = 0)
void pc_remake_remove_static_lamp(long long id);
void pc_remake_add_lamp(float x, float y, float z, float radius, float r, float g, float b,
                        float intensity, int priority);
// Static lamps: fixtures bolted to the world (street lamps)
void pc_remake_add_static_lamp(long long id, float x, float y, float z, float radius, float r,
                               float g, float b, float intensity, int priority);
int pc_remake_lamp_count();
const float* pc_remake_lamp_pos();
const float* pc_remake_lamp_color();
// keep only the kMaxRemakeLamps lamps closest to the camera; a field of eco orbs must not crowd out
void pc_remake_select_lamps(const float* cam_xyz);

// lumières émissives
namespace tfrag3 {
struct Level;
}
void pc_emissive_scan_level(const tfrag3::Level& lvl);
void pc_emissive_drop_level(const std::string& level_name);
// versé dans la collecte des lampes à chaque image
void pc_emissive_fold_into_all();
// boost d'auto-illumination pour une texture (0 = pas émissive)
float pc_emissive_texture_boost(const std::string& debug_name);
// lave animee (tfrag3.frag, lava_kind)
float pc_lava_texture_kind(const std::string& debug_name);
// surfaces reflechissantes (reflets du post, post_processing.frag)
float pc_reflect_texture_kind(const std::string& debug_name);
// reflets du verre au dessin (tfrag3.frag)
extern float g_reflet_fwd[4];
// SCULPTURE DU DECOR
using PcSculptMap = tfrag3::PcSculptHeight;  // chargee dans common (aussi pour la geometrie)
const PcSculptMap* pc_sculpt_map(const std::string& debug_name);
void pc_sculpt_register_texture(GLuint color_tex, const std::string& debug_name);
void pc_sculpt_forget_texture(GLuint color_tex);
void pc_sculpt_bind(GLuint color_tex, GLuint program);
// CARTES DE NORMALES DES SOLS (<nom>.nrm, out/jak1/normales puis custom_assets/jak1/normales)
void pc_normal_register_texture(GLuint color_tex, const std::string& debug_name);
void pc_normal_forget_texture(GLuint color_tex);
void pc_normal_bind(GLuint color_tex, GLuint program);
// COUCHE DE DETAIL DES SOLS (custom_assets/jak1/sol_detail, outil details_sol.py)
void pc_det_register_texture(GLuint color_tex, const std::string& debug_name);
void pc_det_forget_texture(GLuint color_tex);
void pc_det_bind(GLuint color_tex, GLuint program);
bool pc_sol_melange_off();
float pc_sol_kind(const std::string& debug_name);
bool pc_vert_off();
float pc_fronde_dose();
bool pc_is_palm_leaf_texture(const std::string& n);
bool pc_remake_mat_inverse(const float* m, float* inv);
// inverse of the background vertex transform; (ndc,1) ->
extern float g_world_from_clip[16];
extern float g_clip_from_world[16];
extern bool g_world_from_clip_ok;

// depth-aware post effects
extern float g_depth_fx[4];

// real-time ocean wave simulation replacing the 2001 precomputed 64-frame loop
struct OceanSim {
  static constexpr int N = 32;
  float cur[N * N] = {};
  float prev[N * N] = {};
  unsigned int rng = 0x12345u;
  float swell_phase = 0.f;
  bool enabled = true;
  float gain = 1.15f;  // wave size vs the original swell; the same grid
                          // drives buoyancy, so this stays close to 1
  bool logged = false;

  float frand();  // 0..1
  void step_and_export(float* dst_1024);
};
extern OceanSim g_ocean_sim;

// Sun shadow map machinery (lazy-initialized on first use, all on the render thread)
struct SunShadowMap {
  static constexpr int kSize = 4096;
  unsigned int fbo = 0;
  unsigned int tex = 0;
  float light_vp[16];  // column-major, world -> light clip
  bool ready = false;
  bool rendered_this_frame = false;
  unsigned long long last_frame_stamp = ~0ull;
  // ANCRAGE (ombres qui bougeaient pendant les déplacements)
  bool anchor_valid = false;
  float anchor_pos[3] = {0.f, 0.f, 0.f};
  float anchor_sun[3] = {0.f, -1.f, 0.f};
  // DOUBLE TAMPON (ombres qui disparaissaient très vite et revenaient)
  float receive_vp[16] = {0};
  bool pingpong_primed = false;

  // perf: the static scenery
  unsigned int static_fbo = 0;
  unsigned int static_tex = 0;
  bool static_ready = false;  // the cache holds a full static layer
  bool static_dirty = true;  // the cache must be rebuilt this frame
  bool static_pass_cleared = false;  // a rebuild started this frame
  bool cache_mode = false;  // false = replays directs (chemin éprouvé)
  int frames_since_static = 0;
  float cached_vp[16] = {0};

  bool enabled() const;
  // compute the ortho light matrix; no-op if already done for this frame_idx
  void maybe_update_for_frame(const float* cam_trans /*vec4*/, unsigned long long frame_idx);
  // compute the ortho light matrix for this frame (call once per frame)
  void update_matrix(const float* cam_trans /*vec4*/);
  // static scenery replay: returns false when the cache is fresh
  bool begin_static_replay(void* shader_lib);
  // dynamic casters (wind crowns): always render, into the working map
  void begin_dynamic_pass(void* shader_lib);
  void end_depth_pass();
  // bind the map + uniforms for a receiving shader program
  void bind_for_receive(unsigned int program) const;
  // a appeler quand les ombres sont COUPEES (interieurs, nuit)
  void bind_disabled(unsigned int program);
  // carte et matrice telles que lues par les receveurs
  unsigned int read_tex() const { return (pingpong_primed && !cache_mode) ? static_tex : tex; }
  const float* read_vp() const { return (pingpong_primed && !cache_mode) ? receive_vp : light_vp; }

  // PASSE D'OMBRES DIFFEREE
  struct DeferredDraws {
    unsigned int vao = 0, ebo = 0, mode = 0;
    unsigned int tex = 0;  // vent : texture a test d'alpha ; 0 = casteur statique
    std::vector<int> counts;
    std::vector<void*> offsets;
    std::vector<std::array<float, 16>> mats;  // vent : matrice lumiere par plage
  };
  std::vector<DeferredDraws> deferred_static, deferred_wind;
  bool qa_immediate = false;
  bool deferred() const { return !cache_mode && !qa_immediate; }
  void flush_deferred(void* shader_lib, unsigned long long frame_idx);

 private:
  // once per frame: seed the working map from the static cache and render the dynamic character casters
  void ensure_frame_init(void* shader_lib);
  int m_saved_viewport[4] = {0, 0, 0, 0};
  int m_saved_fbo = 0;
};
extern SunShadowMap g_sun_shadow;

// G-buffer de normales
extern bool g_pc_normal_target_ok;
void pc_normal_target(bool on);

// characters and props are drawn long after the background
extern void (*g_sun_shadow_merc_hook)(void* shader_lib);

// build the matrix that takes a merc VIEW-space vertex straight to sun-shadow clip space
bool compute_sun_light_from_camera(const math::Vector4f* camera_matrix /*4*/,
                                   const math::Vector4f& camera_pos,
                                   const math::Vector4f* perspective /*4*/,
                                   float* out16);

struct GoalBackgroundCameraData {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};

// data passed from game to PC renderers
// the GOAL code assumes this memory layout.
struct TfragPcPortData {
  GoalBackgroundCameraData camera;
  char level_name[32];
};
static_assert(sizeof(TfragPcPortData) == 16 * 25);

// inputs to background renderers.
struct TfragRenderSettings {
  GoalBackgroundCameraData camera;
  int tree_idx;
  bool debug_culling = false;
  const u8* occlusion_culling = nullptr;
};

enum class DoubleDrawKind { NONE, AFAIL_NO_DEPTH_WRITE };

struct DoubleDraw {
  DoubleDrawKind kind = DoubleDrawKind::NONE;
  float aref_first = 0.;
  float aref_second = 0.;
  float color_mult = 1.;
};

DoubleDraw setup_tfrag_shader(SharedRenderState* render_state, DrawMode mode, ShaderId shader);
DoubleDraw setup_opengl_from_draw_mode(DrawMode mode, u32 tex_unit, bool mipmap);

void first_tfrag_draw_setup(const GoalBackgroundCameraData& settings,
                            SharedRenderState* render_state,
                            ShaderId shader);
// SOL DE NEIGE DEFORMABLE
bool pc_is_snow_ground_texture(const std::string& name);
// la passe decoupee (TFRAG3_SNOW) est-elle possible cette image ?
bool pc_snow_tess_available(SharedRenderState* render_state);
// prepare TFRAG3_SNOW (memes reglages que TFRAG3, carte des traces sur l'unite 7, patchs de 3 sommets)
GLuint pc_snow_tess_begin(const GoalBackgroundCameraData& camera, SharedRenderState* render_state);


void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out);

void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out);
bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes);

// visibilite des noeuds pour la CARTE D'OMBRES du soleil
void shadow_vis_from_light(const float* light_vp,
                           const std::vector<tfrag3::VisNode>& nodes,
                           std::vector<u8>& out);
// ajoute les plages d'indices visibles des draws [first, end) (fusion des plages contigues)
u64 append_shadow_ranges(const std::vector<tfrag3::StripDraw>& draws,
                         size_t first,
                         size_t end,
                         const std::vector<u8>& vis,
                         std::vector<GLsizei>& counts,
                         std::vector<void*>& offsets);

void update_render_state_from_pc_settings(SharedRenderState* state, const TfragPcPortData& data);

void make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                 GLsizei* counts_out,
                                 void** index_offsets_out,
                                 const std::vector<tfrag3::ShrubDraw>& draws);

u32 make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                GLsizei* counts_out,
                                void** index_offsets_out,
                                const std::vector<tfrag3::StripDraw>& draws);

u32 make_multidraws_from_vis_string(std::pair<int, int>* draw_ptrs_out,
                                    GLsizei* counts_out,
                                    void** index_offsets_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::StripDraw>& draws,
                                const u32* idx_in,
                                u32* num_tris_out);

u32 make_index_list_from_vis_string(std::pair<int, int>* group_out,
                                    u32* idx_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data,
                                    const u32* idx_in,
                                    u32* num_tris_out);

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::ShrubDraw>& draws,
                                const u32* idx_in);

u32 make_multidraws_from_vis_and_proto_string(std::pair<int, int>* draw_ptrs_out,
                                              GLsizei* counts_out,
                                              void** index_offsets_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data);

u32 make_index_list_from_vis_and_proto_string(std::pair<int, int>* group_out,
                                              u32* idx_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data,
                                              const u32* idx_in,
                                              u32* num_tris_out);
