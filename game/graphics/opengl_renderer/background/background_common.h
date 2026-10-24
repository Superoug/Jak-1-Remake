#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

// PC-REMAKE: grass interaction. World-space position of Jak (xyz, in game units
// where 1 meter = 4096) and push strength (w, 0..1). Written by the GOAL kernel
// binding pc-set-grass-push! each frame, read by the Shrub renderer to bend
// vegetation vertices around the player. Same cross-thread pattern as
// g_want_screenshot (worst case is one frame of slightly stale data).
extern float g_grass_push[4];
extern float g_grass_clear[4];  // 20/10 : herbe couchee pendant les cinematiques

// PC-REMAKE: presentation post effects (bloom / saturation / warmth), written
// by the GOAL binding pc-set-post-fx!, read by do_pcrtc_effects. All values
// are neutral at zero. x=bloom intensity, y=bloom threshold offset,
// z=saturation offset, w=warmth.
extern float g_post_fx[4];

// PC-REMAKE: sun shadow mapping. xyz = normalized world-space direction the
// sunLIGHT travels (pointing down), w = shadow strength (0 = disabled).
// Written by the GOAL binding pc-set-sun-shadow!.
extern float g_sun_shadow_params[4];

// PC-REMAKE: toon / cel-shading post pass (x=posterize, y=edges, z=levels).
// Written by pc-set-toon-fx!, read by do_pcrtc_effects. Neutral at zero.
extern float g_toon_fx[4];

// PC-REMAKE 25/08 : reglages de qualite des effets (pour
// réduire ou enlever certains reflets). x = reflets SSR (0 off, 1 leger
// 6 pas, 2 complet 10 pas), y = halos ecran (0/1), z = ombres de contact
// (0/1). DEFAUT 2/1/1 = rendu valide, strictement identique. Ecrit par le
// binding GOAL pc-set-fx-quality!, lu par do_pcrtc_effects.
extern float g_fx_quality[4];

// PC-REMAKE — rebuilt lighting rig.
//   g_light_fx : x = god-ray strength, y = directional sun relight,
//                z = hemisphere ambient, w = specular sheen. Neutral at zero.
//   g_sky_color / g_ground_color : the two halves of the hemisphere ambient
//                (sky above, bounced ground below), rgb + unused.
// All written from GOAL so the rig can be tuned live and follows time of day.
extern float g_light_fx[4];
extern float g_sky_color[4];
extern float g_ground_color[4];
// x = night amount (0 day .. 1 night), y = lamp reach, z = lamp brightness
extern float g_night_fx[4];
// PC-REMAKE : réglage EN DIRECT du vent des palmiers (pc-set-palm-wind!)
// x = échelle houle TIE (1 = validé), y = échelle battement feuilles TIE,
// z = amplitude houle palmiers de terrain en unités (0 = figés), w = réserve
extern float g_palm_wind[4];
// PC-REMAKE : scintillement de l'eau (reflets du soleil/de la lune,
// 17/08). x = niveau de la mer (unités monde, depuis *ocean-map*),
// y = force de jour, z = force de nuit, w rempli côté rendu (temps en s).
// Posé chaque frame par pc-set-water-glitter! (main.gc) ; x=0 et forces 0
// quand le niveau n'a pas d'océan.
extern float g_water_glitter[4];
// PROJECTEUR VIOLET : peint en magenta les draws désignés par le CONTENU du
// fichier témoin /tmp/palm_debug — idx:A-B (plage d'index de textures du
// niveau) ou une sous-chaîne de nom de texture. Fichier absent ou vide = rien.
// Sert à identifier à chaud, sans redémarrage, quel draw porte un objet visé
// (chasse aux palmiers penchés, 17/08).
extern float g_palm_debug;
extern std::string g_palm_debug_spec;
extern u32 g_palm_debug_seq;
void update_palm_debug_flag();
// PC-REMAKE 21/09 : detail intrinseque des sols (voir tfrag3.frag) — gain
// global (fichier /tmp/pc_detail : un flottant, absent = 1.0, 0 = off) et
// regle par nom de texture (1 = sol/roche, 0 = autre)
extern float g_pc_detail_gain;
extern float g_pc_force_opaque;
void update_detail_gain();
float pc_detail_texture_amount(const std::string& debug_name);
bool palm_debug_matches(s32 tex_idx, const std::string& name);

// PC-REMAKE local lamps. Emissive props register themselves each frame while
// they are drawn, so the light list follows whatever is actually on screen.
constexpr int kMaxRemakeLamps = 48;    // uploaded to the shader (24 → 48 : une
                                       // mer de lave mérite plus de 24 sources)
constexpr int kMaxRemakePending = 384; // collected before the nearest are picked
                                       // (128 → 384 : les lumières émissives
                                       // scannées par niveau s'y ajoutent)
void pc_remake_lamps_begin_frame();
// PC-REMAKE 20/10 : TRACES DANS LA NEIGE. Carte flottante torique (1024 x 1024 texels de 10 cm)
// centree sur Jak : R = distance au chemin de Jak (m), G = hauteur des pieds (m), B = instant du
// pas (s), A = profondeur tassee (m). Le profil du sillon est calcule par les shaders
// (tfrag3_snow.tese, post_processing.frag). Mise a jour une fois par image ; 0 si inactive.
extern float g_snow_step[4];
void pc_snow_trail_frame(unsigned long long frame_idx);
unsigned int pc_snow_trail_tex();
// grille 128 x 128 (cases de 0,8 m) : instant du dernier pas qui touche chaque case
unsigned int pc_snow_cells_tex();
// le sol de neige a ete vraiment creuse cette image (passe decoupee de TFragment) : le post
// ne simule plus le relief (parallaxe), il garde la teinte et l'ombre du sillon
extern bool g_snow_geom_drawn;
// 22/10 : image ou la neige decoupee a ete dessinee (la capture d'ecran rejoue le post dans
// la meme image : le drapeau seul, remis a zero apres le premier post, la faisait mentir)
extern u64 g_snow_geom_frame;
// 21/10 : incremente a chaque rechargement a chaud des shaders des personnages
extern int g_pc_merc_shaders_seq;
// PC-REMAKE 20/10 : faisceaux d'eco (segments monde) — lumiere en tube dans le post.
constexpr int kMaxRemakeBeams = 12;
void pc_remake_add_beam(float x0, float y0, float z0, float x1, float y1, float z1);
int pc_remake_beam_count();
const float* pc_remake_beams();  // 2 x vec4 par faisceau (extremites, w = 0)
void pc_remake_remove_static_lamp(long long id);
void pc_remake_add_lamp(float x, float y, float z, float radius, float r, float g, float b,
                        float intensity, int priority);
// Static lamps: fixtures bolted to the world (street lamps). Registered once,
// keyed by their entity address, and kept for good — a lamp must not go out
// just because the game paused its actor while the player walked away.
void pc_remake_add_static_lamp(long long id, float x, float y, float z, float radius, float r,
                               float g, float b, float intensity, int priority);
int pc_remake_lamp_count();
const float* pc_remake_lamp_pos();
const float* pc_remake_lamp_color();
// keep only the kMaxRemakeLamps lamps closest to the camera; a field of eco
// orbs must not crowd out the village lamps behind it
void pc_remake_select_lamps(const float* cam_xyz);

// PC-REMAKE lumières émissives (18/08) : au chargement d'un niveau, la
// géométrie tfrag est balayée à la recherche des textures qui brillent dans le
// jeu d'origine (lave, braises, flammes, lueurs précurseurs, lampes…) ; chaque
// amas devient une vraie source pour la passe différée, de la couleur moyenne
// de sa texture. objectif : chaque objet censé émettre de la
// lumière éclaire son environnement.
namespace tfrag3 {
struct Level;
}
void pc_emissive_scan_level(const tfrag3::Level& lvl);
void pc_emissive_drop_level(const std::string& level_name);
// versé dans la collecte des lampes à chaque image (appelé par begin_frame)
void pc_emissive_fold_into_all();
// boost d'auto-illumination pour une texture (0 = pas émissive) : la surface
// garde sa pleine luminosité, ni nuit ni ombre dessus — la lave EST sa lumière
float pc_emissive_texture_boost(const std::string& debug_name);
// 22/10 : lave animee (tfrag3.frag, lava_kind) — 0 = rien, 1 = croute craquelee (seule la lave
// des fissures coule), 2 = lave en fusion (toute la surface coule)
float pc_lava_texture_kind(const std::string& debug_name);
// PC-REMAKE : SCULPTURE DU DECOR — carte de hauteur par texture (custom_assets/jak1/sculpture/<nom>.hgt,
// ecrite par l'outil sculpture_decor.py) : relief par pixel avec parallaxe dans tfrag3.frag.
// QA_NO_SCULPT=1 au lancement ou /tmp/qa_no_sculpt a chaud = coupe (A/B).
using PcSculptMap = tfrag3::PcSculptHeight;  // chargee dans common (aussi pour la geometrie)
const PcSculptMap* pc_sculpt_map(const std::string& debug_name);
void pc_sculpt_register_texture(GLuint color_tex, const std::string& debug_name);
void pc_sculpt_forget_texture(GLuint color_tex);
void pc_sculpt_bind(GLuint color_tex, GLuint program);
bool pc_sol_melange_off();
float pc_sol_kind(const std::string& debug_name);
bool pc_vert_off();
float pc_fronde_dose();
bool pc_is_palm_leaf_texture(const std::string& n);
bool pc_remake_mat_inverse(const float* m, float* inv);
// inverse of the background vertex transform; (ndc,1) -> (world - cam_trans, 1)
extern float g_world_from_clip[16];
extern float g_clip_from_world[16];
extern bool g_world_from_clip_ok;

// PC-REMAKE: depth-aware post effects — x=SSAO strength, y=aerial fog density,
// z=far DOF strength, w=filmic tonemap amount. Neutral at zero.
// Written by pc-set-depth-fx!, read by do_pcrtc_effects.
extern float g_depth_fx[4];

// PC-REMAKE: real-time ocean wave simulation replacing the 2001 precomputed
// 64-frame loop.
//
// FORMAT (read from ocean-get-height in ocean.gc, not guessed): *ocean-heights*
// is a 32x32 grid of FLOATS, indexed (z * 32 + x), each a height offset in game
// units added to the ocean plane. The same grid feeds BOTH the rendered mesh
// and the height queries used for buoyancy — so writing here changes what the
// water looks like AND how things float on it.
//
// The simulation self-calibrates: it measures the amplitude the game itself
// just produced and matches it, so no magic constants are involved.
struct OceanSim {
  static constexpr int N = 32;
  float cur[N * N] = {};
  float prev[N * N] = {};
  unsigned int rng = 0x12345u;
  float swell_phase = 0.f;
  bool enabled = true;
  float gain = 1.15f;     // wave size vs the original swell; the same grid
                          // drives buoyancy, so this stays close to 1
  bool logged = false;

  float frand();  // 0..1
  void step_and_export(float* dst_1024);
};
extern OceanSim g_ocean_sim;

// Sun shadow map machinery (lazy-initialized on first use, all on the render
// thread). The depth pass is replayed inside the background renderers right
// after their normal draws, reusing the same vis/index state.
struct SunShadowMap {
  static constexpr int kSize = 4096;
  unsigned int fbo = 0;
  unsigned int tex = 0;
  float light_vp[16];  // column-major, world -> light clip
  bool ready = false;
  bool rendered_this_frame = false;
  unsigned long long last_frame_stamp = ~0ull;
  // ANCRAGE (17/08, ombres qui bougeaient pendant les déplacements) :
  // la matrice n'est plus recalculée chaque frame en suivant la caméra ; elle
  // est GELÉE sur une ancre et re-posée seulement sur événements rares
  // (caméra loin de l'ancre, soleil ayant assez tourné). Entre deux ancres,
  // les ombres sont immobiles au sol et la carte est identique frame à frame.
  bool anchor_valid = false;
  float anchor_pos[3] = {0.f, 0.f, 0.f};
  float anchor_sun[3] = {0.f, -1.f, 0.f};
  // DOUBLE TAMPON (17/08, ombres qui disparaissaient très vite et revenaient)
  // : les receveurs lisent TOUJOURS la carte COMPLÈTE de la frame précédente
  // (receive_vp = la matrice avec laquelle elle a été rendue), pendant que les
  // casteurs remplissent l'autre carte avec la matrice du jour. Échange au
  // début de chaque frame. Sans ça, les arbres dessinés après les premiers
  // replays échantillonnaient une carte à moitié reconstruite au moindre
  // changement de matrice (re-ancrage, soleil) → trou d'ombres d'une frame.
  float receive_vp[16] = {0};
  bool pingpong_primed = false;

  // PC-REMAKE perf: the static scenery (TIE + TFRAG full-level replays, ~3.7M
  // indices) only changes when the snapped light matrix moves. It is cached in
  // its own depth texture and blitted into the working map each frame; only the
  // dynamic casters (characters, wind-blown crowns) re-render every frame.
  unsigned int static_fbo = 0;
  unsigned int static_tex = 0;
  bool static_ready = false;         // the cache holds a full static layer
  bool static_dirty = true;          // the cache must be rebuilt this frame
  bool static_pass_cleared = false;  // a rebuild started this frame
  bool cache_mode = false;           // false = replays directs (chemin éprouvé)
  int frames_since_static = 0;
  float cached_vp[16] = {0};

  bool enabled() const;
  // compute the ortho light matrix; no-op if already done for this frame_idx
  // (uses SharedRenderState::frame_idx so it runs once regardless of bucket count)
  void maybe_update_for_frame(const float* cam_trans /*vec4*/, unsigned long long frame_idx);
  // compute the ortho light matrix for this frame (call once per frame)
  void update_matrix(const float* cam_trans /*vec4*/);
  // static scenery replay: returns false when the cache is fresh (skip the
  // replay entirely — this is the fast path)
  bool begin_static_replay(void* shader_lib);
  // dynamic casters (wind crowns): always render, into the working map
  void begin_dynamic_pass(void* shader_lib);
  void end_depth_pass();
  // bind the map + uniforms for a receiving shader program
  void bind_for_receive(unsigned int program) const;
  // 21/09 : a appeler quand les ombres sont COUPEES (interieurs, nuit) — pose
  // quand meme le sampler sur l'unite 14 avec une vraie texture de profondeur.
  // Sans ca, `sun_shadow_tex` (sampler2DShadow) reste sur l'unite 0 = celle de
  // tex_T0 (sampler2D) : deux types de sampler sur la meme unite = programme
  // invalide, le pilote Apple REJETTE tous les draws TFRAG3/SHRUB/MERC2 -> ecran
  // titre sans terrain, logo reduit a sa passe envmap (boot direct au titre).
  void bind_disabled(unsigned int program);
  // 20/10 : carte et matrice telles que lues par les receveurs (rayons volumetriques du post)
  unsigned int read_tex() const { return (pingpong_primed && !cache_mode) ? static_tex : tex; }
  const float* read_vp() const { return (pingpong_primed && !cache_mode) ? receive_vp : light_vp; }

  // PC-REMAKE 14/10 : PASSE D'OMBRES DIFFEREE. Chaque arbre (terrain, objets, vent)
  // basculait vers la carte d'ombres puis revenait a l'image principale : une
  // dizaine de bascules par image, ~2,6 ms de PROCESSEUR au village (pilote Mac).
  // Les dessins sont desormais ENREGISTRES pendant l'image et rejoues en une seule
  // passe en fin d'image (flush_deferred, appele par OpenGLRenderer::render apres
  // les buckets). Ombres identiques : la carte ecrite pendant l'image N n'est lue
  // qu'a l'image N+1 (double tampon). `touch /tmp/qa_shadow_immediate` = ancien
  // chemin (A/B).
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
  // once per frame: seed the working map from the static cache and render the
  // dynamic character casters on top (poses from the previous frame)
  void ensure_frame_init(void* shader_lib);
  int m_saved_viewport[4] = {0, 0, 0, 0};
  int m_saved_fbo = 0;
};
extern SunShadowMap g_sun_shadow;

// PC-REMAKE 21/09 : G-buffer de normales. Vrai quand le framebuffer principal
// possede l'attachement 1 ; `pc_normal_target(true)` l'ajoute a la liste de
// dessin le temps des draws d'un renderer dont le shader ecrit `out_normal`,
// `pc_normal_target(false)` le retire (les renderers PS2 emules n'ecrivent que
// la sortie 0 : une sortie non ecrite serait indefinie).
extern bool g_pc_normal_target_ok;
void pc_normal_target(bool on);

// PC-REMAKE: characters and props are drawn long after the background, so they
// cannot be in the shadow map by the time the background samples it. They
// register here instead and are added right after the map is cleared, using the
// previous frame's poses — one frame behind, which nobody can see.
extern void (*g_sun_shadow_merc_hook)(void* shader_lib);

// PC-REMAKE: build the matrix that takes a merc VIEW-space vertex straight to
// sun-shadow clip space (see the derivation in background_common.cpp).
// Returns false when shadows are off or the camera matrix is degenerate.
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
// 20/10 : SOL DE NEIGE DEFORMABLE. Textures du sol de neige — terrain (TFragment) ET
// monticules de neige epaisse (TIE, poses par-dessus le terrain au centre des etendues)
bool pc_is_snow_ground_texture(const std::string& name);
// la passe decoupee (TFRAG3_SNOW) est-elle possible cette image ?
bool pc_snow_tess_available(SharedRenderState* render_state);
// prepare TFRAG3_SNOW (memes reglages que TFRAG3, carte des traces sur l'unite 7, patchs de 3
// sommets) ; renvoie le programme. Dessiner ensuite les draws de neige en GL_PATCHES.
GLuint pc_snow_tess_begin(const GoalBackgroundCameraData& camera, SharedRenderState* render_state);


void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out);

void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out);
bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes);

// PC-REMAKE 14/10 : visibilite des noeuds pour la CARTE D'OMBRES du soleil. Le rejeu
// des casteurs statiques dessinait le niveau ENTIER a chaque image (~3 M d'indices
// au village) alors que la carte ne couvre que 240 m autour de l'ancre : tout ce qui
// tombe hors de son carre (ou de sa tranche de profondeur) etait coupe par le GPU
// apres avoir paye ses sommets. Ombres identiques, travail en moins.
void shadow_vis_from_light(const float* light_vp,
                           const std::vector<tfrag3::VisNode>& nodes,
                           std::vector<u8>& out);
// ajoute les plages d'indices visibles des draws [first, end) (fusion des plages
// contigues) ; renvoie le nombre d'indices retenus
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
