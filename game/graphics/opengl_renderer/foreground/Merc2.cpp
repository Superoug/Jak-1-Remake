#include "Merc2.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/FileUtil.h"
#include "common/util/fnv.h"
#include "common/util/string_util.h"
#include "common/util/simd_util.h"

#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#include "third-party/imgui/imgui.h"

namespace {
// PC-REMAKE 17/10 : voir eye_tex dans la boucle de dessin.
void pc_eye_texture_linear() {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}
}  // namespace

void qa_glerr_check(const char* where);  // OpenGLRenderer.cpp — sonde [QA-GLERR]

/* Merc 2 renderer:
 The merc2 renderer is the main "foreground" renderer, which draws characters, collectables,
 and even some water.

 The PC format renderer does the usual tricks of buffering stuff head of time as much as possible.
 The main trick here is to buffer up draws and upload "bones" (skinning matrix) for many draws all
 at once.

 The other tricky part is "mod vertices", which may be modified by the game.
 We know ahead of time which vertices could be modified, and have a way to upload only those
 vertices.

 Each "merc model" corresponds to a merc-ctrl in game. There's one merc-ctrl per LOD of an
 art-group. So generally, this will be something like "jak" or "orb" or "some enemy".

 Each model is made up of "effect"s. There are a number of per-effect settings, like environment
 mapping. Generally, the purpose of an "effect" is to divide up a model into parts that should be
 rendered with a different configuration.

 Within each model, there are fragments. These correspond to how much data can be uploaded to VU1
 memory. For the most part, fragments are not considered by the PC renderer. The only exception is
 updating vertices - we must read the data from the game, which is stored in fragments.

 Per level, there is an FR3 file loaded by the loader. Each merc renderer can access multiple
 levels.
*/

/*!
 * Remaining ideas for optimization:
 * - port blerc to C++, do it in the rendering thread and avoid the lock.
 * - combine envmap draws per effect (might require some funky indexing stuff, or multidraw)
 * - smaller vertex formats for mod-vertex
 * - AVX version of vertex conversion math
 * - eliminate the "copy" step of vertex modification
 * - batch uploading the vertex modification data
 */

std::mutex g_merc_data_mutex;

// PC-REMAKE 21/09 (defini plus bas, utilise dans handle_setup_dma)
static bool compute_world_normal_from_merc(const math::Vector4f* camera_matrix,
                                           const math::Vector4f* perspective,
                                           float* out9,
                                           float* out16 = nullptr);

Merc2* Merc2::s_shadow_instance = nullptr;

void Merc2::shadow_hook(void* shader_lib) {
  if (s_shadow_instance) {
    s_shadow_instance->render_shadow_casters(shader_lib);
  }
}

// ---------------------------------------------------------------------------
// 21/10 v3 : CARTES DE DETAIL DES MATIERES — une hauteur 256 x 256 raccordable par famille de
// matiere (merc2.frag F_*), generee au demarrage et filtree comme une vraie texture (mipmaps,
// anisotrope) : le detail reste net de pres et se fond proprement au loin (pas de scintillement,
// contrairement au bruit calcule par pixel). Couche = famille.
namespace {
constexpr int kDetailN = 256;
constexpr int kDetailLayers = 17;

u32 det_hash(int x, int y, int s) {
  u32 h = (u32)x * 374761393u + (u32)y * 668265263u + (u32)s * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float det_rnd(int x, int y, int s) {
  return (det_hash(x, y, s) & 0xffffff) / 16777216.f;
}
// bruit de valeur raccordable : px x py cellules sur la tuile (allonge si px != py ; au plus
// une cellule pour 2 pixels, sinon le bruit se replie en grille)
float det_bruit2(float x, float y, int px, int py, int s) {
  const float fx = x * px / kDetailN, fy = y * py / kDetailN;
  const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
  float tx = fx - ix, ty = fy - iy;
  tx = tx * tx * (3.f - 2.f * tx);
  ty = ty * ty * (3.f - 2.f * ty);
  auto wx = [px](int i) { return ((i % px) + px) % px; };
  auto wy = [py](int i) { return ((i % py) + py) % py; };
  const float a = det_rnd(wx(ix), wy(iy), s), b = det_rnd(wx(ix + 1), wy(iy), s);
  const float c = det_rnd(wx(ix), wy(iy + 1), s), d = det_rnd(wx(ix + 1), wy(iy + 1), s);
  return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}
float det_bruit(float x, float y, int p, int s) {
  return det_bruit2(x, y, p, p, s);
}
// cellules (Worley) raccordables : distance au germe le plus proche (0..~1)
float det_cellule(float x, float y, int p, int s, float* id = nullptr) {
  const float fx = x * p / kDetailN, fy = y * p / kDetailN;
  const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
  float best = 9.f;
  auto w = [p](int i) { return ((i % p) + p) % p; };
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const int cx = ix + dx, cy = iy + dy;
      const float gx = cx + 0.15f + 0.7f * det_rnd(w(cx), w(cy), s);
      const float gy = cy + 0.15f + 0.7f * det_rnd(w(cx), w(cy), s + 7);
      const float d = std::sqrt((gx - fx) * (gx - fx) + (gy - fy) * (gy - fy));
      if (d < best) {
        best = d;
        if (id) {
          *id = det_rnd(w(cx), w(cy), s + 13);
        }
      }
    }
  }
  return best;
}
float det_hauteur(int famille, float x, float y) {
  const float pi2 = 6.2831853f;
  switch (famille) {
    case 1: {  // peau : pores fins et grain doux
      const float pores = 1.f - std::min(1.f, det_cellule(x, y, 48, 1) * 2.2f);
      return 0.55f + 0.25f * det_bruit(x, y, 16, 2) - 0.35f * pores * pores;
    }
    case 2: {  // tissu : trame croisee (chaine et trame, dessus / dessous)
      const float per = 16.f;
      const int cx = (int)std::floor(x / per), cy = (int)std::floor(y / per);
      const float u = x / per - cx, v = y / per - cy;
      const bool dessus = ((cx + cy) & 1) == 0;
      const float fil_x = 0.5f - 0.5f * std::cos(pi2 * v);  // fil horizontal
      const float fil_y = 0.5f - 0.5f * std::cos(pi2 * u);
      const float h = dessus ? std::max(fil_x * (0.6f + 0.4f * std::sin(3.14159f * u)), fil_y * 0.6f)
                             : std::max(fil_y * (0.6f + 0.4f * std::sin(3.14159f * v)), fil_x * 0.6f);
      return h * 0.85f + 0.15f * det_bruit(x, y, 32, 3);
    }
    case 3: {  // cuir : grain en petits galets, plis
      float id = 0.f;
      const float c = det_cellule(x, y, 24, 4, &id);
      return 0.75f - 0.6f * c * c + 0.15f * id + 0.2f * det_bruit(x, y, 6, 5);
    }
    case 4:  // metal : brossage (traits longs selon x) et voile doux
      return 0.5f + 0.35f * (det_bruit2(x, y, 4, 96, 6) - 0.5f) + 0.15f * det_bruit(x, y, 8, 7);
    case 5:  // cheveux : meches fines (selon y)
      return 0.5f + 0.4f * (det_bruit2(x, y, 96, 6, 8) - 0.5f) + 0.2f * (det_bruit2(x, y, 24, 3, 9) - 0.5f);
    case 6:  // fourrure : poils couches (selon y) en touffes
      return 0.4f + 0.4f * det_bruit2(x, y, 64, 12, 10) + 0.2f * det_bruit(x, y, 12, 11);
    case 8: {  // plume : barbes paralleles et rachis
      const float barbe = 0.5f + 0.5f * std::sin(pi2 * (x / 6.f + 0.3f * det_bruit(x, y, 8, 12)));
      return 0.3f + 0.5f * barbe + 0.2f * det_bruit(x, y, 16, 13);
    }
    case 10:  // bois : fibres longues (selon y), sillons et noeuds
      return 0.4f + 0.3f * det_bruit2(x, y, 48, 4, 14) + 0.2f * det_bruit2(x, y, 12, 2, 25) +
             0.15f * (1.f - std::min(1.f, det_cellule(x, y, 3, 15) * 1.6f));
    case 11: {  // ecaille : ecailles bombees
      float id = 0.f;
      const float c = det_cellule(x, y, 20, 16, &id);
      return std::max(0.f, 1.f - c * 1.4f) * (0.8f + 0.2f * id);
    }
    case 12:  // vegetal : nervures et cellules
      return 0.5f + 0.3f * det_bruit(x, y, 12, 17) - 0.3f * std::exp(-det_cellule(x, y, 10, 18) * 6.f);
    case 13:  // pierre, glace : rugueux
      return 0.25f * det_bruit(x, y, 4, 19) + 0.35f * det_bruit(x, y, 16, 20) +
             0.4f * det_bruit(x, y, 48, 21);
    case 14:  // corne, dents, ongles : stries de croissance
      return 0.5f + 0.35f * std::sin(pi2 * (y / 10.f + 0.4f * det_bruit(x, y, 4, 22))) * 0.5f +
             0.15f * det_bruit(x, y, 32, 23);
    case 15:  // corde : torons tordus
      return 0.5f + 0.5f * std::sin(pi2 * (x + y) / 24.f) * 0.8f + 0.1f * det_bruit(x, y, 32, 24);
    default:
      return 0.5f;
  }
}
}  // namespace

GLuint pc_make_detail_array() {
  std::vector<u8> data((size_t)kDetailN * kDetailN * 4 * kDetailLayers);
  for (int l = 0; l < kDetailLayers; l++) {
    u8* out = &data[(size_t)l * kDetailN * kDetailN * 4];
    for (int y = 0; y < kDetailN; y++) {
      for (int x = 0; x < kDetailN; x++) {
        const float h = std::clamp(det_hauteur(l, (float)x, (float)y), 0.f, 1.f);
        const u8 v = (u8)std::lround(h * 255.f);
        u8* p = &out[(y * kDetailN + x) * 4];
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
      }
    }
  }
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kDetailN, kDetailN, kDetailLayers, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, data.data());
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
  float aniso = 1.f;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
  glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY, std::min(aniso, 8.f));
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
  return tex;
}

Merc2::Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array)
    : m_anim_slot_array(anim_slot_array) {
  // PC-REMAKE: let the sun shadow map pull characters and props in
  s_shadow_instance = this;
  g_sun_shadow_merc_hook = &Merc2::shadow_hook;
  ASSERT(fnv64("the quick brown fox jumps over the lazy dog") == 0x7404cea13ff89bb0);

  // Set up main vertex array. This will point to the data stored in the .FR3 level file, and will
  // be uploaded to the GPU by the Loader.
  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);

  // Bone buffer to store skinning matrices for multiple draws
  glGenBuffers(1, &m_bones_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);

  // zero initialize the bone buffer.
  std::vector<u8> temp(MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f));
  glBufferData(GL_UNIFORM_BUFFER, MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f), temp.data(),
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  // annoyingly, glBindBufferRange can have alignment restrictions that vary per platform.
  // the GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT gives us the minimum alignment for views into the bone
  // buffer. The bone buffer stores things per-16-byte "quadword".
  GLint val;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &val);
  if (val <= 16) {
    // somehow doubt this can happen, but just in case
    m_opengl_buffer_alignment = 1;
  } else {
    m_opengl_buffer_alignment = val / 16;  // number of bone vectors
    if (m_opengl_buffer_alignment * 16 != (u32)val) {
      ASSERT_MSG(false,
                 fmt::format("opengl uniform buffer alignment is {}, which is strange\n", val));
    }
  }

  // initialize draw buffers, these will store lists of draws to flush.
  for (int i = 0; i < MAX_LEVELS; i++) {
    auto& draws = m_level_draw_buckets.emplace_back();
    draws.draws.resize(MAX_DRAWS_PER_LEVEL);
    draws.envmap_draws.resize(MAX_ENVMAP_DRAWS_PER_LEVEL);
  }

  m_mod_vtx_temp.resize(MAX_MOD_VTX);
  m_mod_vtx_unpack_temp.resize(MAX_MOD_VTX * 2);

  for (auto& x : m_effect_debug_mask) {
    x = true;
  }

  init_shaders(shaders);
  m_detail_tex = pc_make_detail_array();
}

// 21/10 : FAMILLE DE MATIERE d'une texture de personnage, d'apres son nom (merc2.frag F_*).
// coquilles = fourrure opaque qui recoit les couches de poils (pas les franges trans, ni
// les longues meches deja modelisees, ni les passages fourrure -> peau)
u8 Merc2::texture_famille(const LevelData* lev, s32 texture, bool* coquilles) {
  *coquilles = false;
  if (!lev || !lev->level || texture < 0 || texture >= (s32)lev->level->textures.size()) {
    return 0;
  }
  // 21/10 v2 : table revue texture par texture (shaders/personnages_matieres.txt :
  // nom famille coquilles) ; les regles par mots-cles ne servent qu'aux textures absentes
  static const std::unordered_map<std::string, std::pair<u8, bool>> table = [] {
    std::unordered_map<std::string, std::pair<u8, bool>> t;
    auto path = file_util::get_file_path({Shader::shader_folder, "personnages_matieres.txt"});
    if (file_util::file_exists(path)) {
      for (auto& ligne : str_util::split(file_util::read_text_file(path), '\n')) {
        if (ligne.empty() || ligne[0] == '#') {
          continue;
        }
        auto parts = str_util::split(ligne, ' ');
        if (parts.size() >= 3) {
          t[parts[0]] = {(u8)std::atoi(parts[1].c_str()), parts[2] == "1"};
        }
      }
    }
    lg::info("PC-REMAKE personnages : {} matieres de textures", t.size());
    return t;
  }();
  auto& cache = m_tex_famille[lev->load_id];
  if (cache.empty()) {
    if (m_tex_famille.size() > 64) {
      m_tex_famille.clear();
      return texture_famille(lev, texture, coquilles);
    }
    cache.resize(lev->level->textures.size() * 2, 0);
    for (size_t i = 0; i < lev->level->textures.size(); i++) {
      std::string n = lev->level->textures[i].debug_name;
      for (auto& c : n) {
        c = (char)std::tolower((unsigned char)c);
      }
      auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
      // mots courts : seulement entre separateurs (ear ne doit pas trouver gear)
      auto mot = [&](const char* k) {
        const size_t l = strlen(k);
        for (size_t p = n.find(k); p != std::string::npos; p = n.find(k, p + 1)) {
          const bool g = p == 0 || !std::isalpha((unsigned char)n[p - 1]);
          const bool d = p + l >= n.size() || !std::isalpha((unsigned char)n[p + l]);
          if (g && d) {
            return true;
          }
        }
        return false;
      };
      u8 f = 0;
      bool coq = false;
      auto ti = table.find(lev->level->textures[i].debug_name);
      if (ti != table.end()) {
        f = ti->second.first;
        coq = ti->second.second;
      } else if (has("eyelid") || has("eyebrow") || has("brow")) {
        f = 1;
      } else if (has("eye") || has("iris") || has("pupil")) {
        f = 7;
      } else if (has("fur") && has("skin")) {
        f = 1;
      } else if (has("fur")) {
        f = 6;
        coq = !has("trans") && !has("long") && !has("end");
      } else if (has("hair") || has("hilite")) {
        f = 5;
      } else if (has("feather") || has("feath")) {
        f = 8;
      } else if (has("skin") || has("flesh") || has("face") || has("finger") || has("dullwhite") ||
                 mot("hand") || mot("lip") || mot("nose") || mot("cheek") || mot("toe") ||
                 mot("foot") || mot("feet") || mot("ear") || mot("arm") || mot("leg")) {
        f = 1;
      } else if (has("leather") || has("belt") || has("strap") || has("boot") || has("glove") ||
                 has("saddle") || has("pouch") || has("sandal") || has("shoe")) {
        f = 3;
      } else if (has("armor") || has("metal") || has("steel") || has("iron") || has("gold") ||
                 has("brass") || has("buckle") || has("rivet") || has("chain") || has("nail") ||
                 has("target") || has("blade") || has("ring")) {
        f = 4;
      } else if (has("rock") || has("stone") || has("boulder") || has("cliff") || has("brick")) {
        // PC-REMAKE : objets remodeles (rochers, marches de pierre)
        f = 13;
      } else if (has("rope")) {
        f = 15;
      } else if (has("wood") || has("bark") || has("logend") || mot("log") || has("stick") ||
                 has("staff") || has("plank") || has("branch") || has("stump") || has("twig")) {
        // 21/10 v2 : bois (rondin de Samos, batons, bateaux) — surface irreguliere et ecorce
        f = 10;
      } else if (has("shiny") || has("glass") || has("lens") || has("goggle")) {
        f = 9;
      } else if (has("cotton") || has("cloth") || has("coat") || has("wrap") || has("pants") ||
                 has("shirt") || has("robe") || has("scarf") || has("sleeve") || has("sash") ||
                 has("cape") || has("dress") || has("wool") || has("fabric") || has("canvas") ||
                 has("tunic") || has("apron") || has("bandana") || has("hat")) {
        f = 2;
      }
      cache[i * 2] = f;
      cache[i * 2 + 1] = coq ? 1 : 0;
    }
  }
  *coquilles = cache[texture * 2 + 1] != 0;
  return cache[texture * 2];
}

// emplacements des uniformes de tous les programmes du rendu des personnages (au demarrage et
// apres un rechargement a chaud des shaders : `touch /tmp/pc_reload_perso`)
void Merc2::init_shaders(ShaderLibrary& shaders) {
  init_shader_common(shaders[ShaderId::MERC2], &m_merc_uniforms, true);
  init_shader_common(shaders[ShaderId::EMERC], &m_emerc_uniforms, false);
  m_emerc_uniforms.fade = pc_uniform_loc(shaders[ShaderId::EMERC].id(), "fade");
  // 21/10 : PERSONNAGES REMODELES
  auto remake_locs = [](Shader& sh, Uniforms* u) {
    u->remake_perso = pc_uniform_loc(sh.id(), "remake_perso");
    u->remake_famille = pc_uniform_loc(sh.id(), "remake_famille");
  };
  remake_locs(shaders[ShaderId::MERC2], &m_merc_uniforms);
  for (auto sid : {ShaderId::MERC2, ShaderId::MERC2_TESS, ShaderId::MERC2_FUR}) {
    if (shaders[sid].okay()) {
      shaders[sid].activate();
      glUniform1i(pc_uniform_loc(shaders[sid].id(), "remake_details"), 5);
    }
  }
  remake_locs(shaders[ShaderId::EMERC], &m_emerc_uniforms);  // absentes : -1, sans effet
  if (shaders[ShaderId::MERC2_TESS].okay()) {
    init_shader_common(shaders[ShaderId::MERC2_TESS], &m_merc_tess_uniforms, true);
    remake_locs(shaders[ShaderId::MERC2_TESS], &m_merc_tess_uniforms);
  }
  if (shaders[ShaderId::EMERC_TESS].okay()) {
    init_shader_common(shaders[ShaderId::EMERC_TESS], &m_emerc_tess_uniforms, false);
    m_emerc_tess_uniforms.fade = pc_uniform_loc(shaders[ShaderId::EMERC_TESS].id(), "fade");
    remake_locs(shaders[ShaderId::EMERC_TESS], &m_emerc_tess_uniforms);
  }
  if (shaders[ShaderId::MERC2_FUR].okay()) {
    init_shader_common(shaders[ShaderId::MERC2_FUR], &m_merc_fur_uniforms, true);
    remake_locs(shaders[ShaderId::MERC2_FUR], &m_merc_fur_uniforms);
  }
}

Merc2::~Merc2() {
  for (auto& x : m_mod_vtx_buffers) {
    glDeleteBuffers(1, &x.vertex);
    glDeleteVertexArrays(1, &x.vao);
  }

  glDeleteBuffers(1, &m_bones_buffer);
  glDeleteVertexArrays(1, &m_vao);
}

/*!
 * Modify vertices for blerc.
 */
void blerc_avx(const u32* i_data,
               const u32* i_data_end,
               const tfrag3::BlercFloatData* floats,
               const float* weights,
               tfrag3::MercVertex* out,
               float multiplier) {
  // store a table of weights. It's faster to load the 16-bytes of weights than load and broadcast
  // the float.
  __m128 weights_table[Merc2::kMaxBlerc];
  for (int i = 0; i < Merc2::kMaxBlerc; i++) {
    weights_table[i] = _mm_set1_ps(weights[i] * multiplier);
  }

  // loop over vertices
  while (i_data != i_data_end) {
    // load the base position
    __m128 pos = _mm_load_ps(floats->v);
    __m128 nrm = _mm_load_ps(floats->v + 4);
    floats++;

    // loop over targets
    while (*i_data != tfrag3::Blerc::kTargetIdxTerminator) {
      // get the weights for this target, from the game data.
      __m128 weight_multiplier = weights_table[*i_data];
      // get the pos/normal offset for this target.
      __m128 posm = _mm_load_ps(floats->v);
      __m128 nrmm = _mm_load_ps(floats->v + 4);
      floats++;

      // apply weights and add
      posm = _mm_mul_ps(posm, weight_multiplier);
      nrmm = _mm_mul_ps(nrmm, weight_multiplier);
      pos = _mm_add_ps(pos, posm);
      nrm = _mm_add_ps(nrm, nrmm);

      i_data++;
    }
    i_data++;

    // store final position/normal.
    _mm_store_ps(out[*i_data].pos, pos);
    _mm_store_ps(out[*i_data].normal, nrm);
    i_data++;
  }
}
namespace {
float blerc_multiplier = 1.f;
}

void Merc2::model_mod_blerc_draws(int num_effects,
                                  const tfrag3::MercModel* model,
                                  const LevelData* lev,
                                  ModBuffers* mod_opengl_buffers,
                                  const float* blerc_weights,
                                  MercDebugStats* stats) {
  // loop over effects.
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    // grab opengl buffer
    auto opengl_buffers = alloc_mod_vtx_buffer(lev);
    mod_opengl_buffers[ei] = opengl_buffers;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the correct vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // do blerc math
    const auto* f_data = effect.mod.blerc.float_data.data();
    const u32* i_data = effect.mod.blerc.int_data.data();
    const u32* i_data_end = i_data + effect.mod.blerc.int_data.size();
    blerc_avx(i_data, i_data_end, f_data, blerc_weights, m_mod_vtx_temp.data(), blerc_multiplier);
    if (model->pc_character) {
      // 21/10 : la normale soudee (pad0) ne vient pas des formes de blerc
      for (size_t vi = 0; vi < effect.mod.vertices.size(); vi++) {
        m_mod_vtx_temp[vi].pad0 = effect.mod.vertices[vi].pad0;
      }
    }

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      glBindBuffer(GL_ARRAY_BUFFER, opengl_buffers.vertex);
      glBufferData(GL_ARRAY_BUFFER, effect.mod.vertices.size() * sizeof(tfrag3::MercVertex),
                   m_mod_vtx_temp.data(), GL_DYNAMIC_DRAW);
    }
  }
}

// We can run into a problem where adding a PC model would overflow the
// preallocated draw/bone buffers.
// So we break this part into two functions:
// - init_pc_model, which doesn't allocate bones/draws

void Merc2::model_mod_draws(int num_effects,
                            const tfrag3::MercModel* model,
                            const LevelData* lev,
                            const u8* input_data,
                            const DmaTransfer& setup,
                            ModBuffers* mod_opengl_buffers,
                            MercDebugStats* stats) {
  auto p = scoped_prof("update-verts");

  // loop over effects. Mod vertices are done per effect (possibly a bad idea?)
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    prof().begin_event("start1");
    // grab opengl buffer
    auto opengl_buffers = alloc_mod_vtx_buffer(lev);
    mod_opengl_buffers[ei] = opengl_buffers;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // check that we have enough room for unpack
    if (effect.mod.expect_vidx_end > MAX_MOD_VTX) {
      fmt::print("More mod vertices (temp) than MAX_MOD_VTX. {} > {}\n", effect.mod.expect_vidx_end,
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the "correct" vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // get pointers to the fragment and fragment control data
    u32 goal_addr;
    memcpy(&goal_addr, input_data + 4 * ei, 4);
    const u8* ee0 = setup.data - setup.data_offset;
    const u8* merc_effect = ee0 + goal_addr;
    u16 frag_cnt;
    memcpy(&frag_cnt, merc_effect + 18, 2);
    ASSERT(frag_cnt >= effect.mod.fragment_mask.size());
    u32 frag_goal;
    memcpy(&frag_goal, merc_effect, 4);
    u32 frag_ctrl_goal;
    memcpy(&frag_ctrl_goal, merc_effect + 4, 4);
    const u8* frag = ee0 + frag_goal;
    const u8* frag_ctrl = ee0 + frag_ctrl_goal;

    // loop over frags
    u32 vidx = 0;
    // u32 st_vif_add = model->st_vif_add;
    float xyz_scale = model->xyz_scale;
    prof().end_event();
    {
      // we're going to look at data that the game may be modifying.
      // in the original game, they didn't have any lock, but I think that the
      // scratchpad access from the EE would effectively block the VIF1 DMA, so you'd
      // hopefully never get a partially updated model (which causes obvious holes).
      // this lock is not ideal, and can block the rendering thread while blerc_execute runs,
      // which can take up to 2ms on really blerc-heavy scenes
      std::unique_lock<std::mutex> lk(g_merc_data_mutex);
      [[maybe_unused]] int frags_done = 0;
      auto p = scoped_prof("vert-math");

      // loop over fragments
      for (u32 fi = 0; fi < effect.mod.fragment_mask.size(); fi++) {
        frags_done++;
        u8 mat_xfer_count = frag_ctrl[3];

        // we create a mask of fragments to skip because they have no vertices.
        // the indexing data assumes that we skip the other fragments.
        if (effect.mod.fragment_mask[fi]) {
          // read fragment metadata
          u8 unsigned_four_count = frag_ctrl[0];
          u8 lump_four_count = frag_ctrl[1];
          u32 mm_qwc_off = frag[10];
          float float_offsets[3];
          memcpy(float_offsets, &frag[mm_qwc_off * 16], 12);
          u32 my_u4_count = ((unsigned_four_count + 3) / 4) * 16;
          u32 my_l4_count = my_u4_count + ((lump_four_count + 3) / 4) * 16;

          // loop over vertices in the fragment and unpack
          for (u32 w = my_u4_count / 4; w < (my_l4_count / 4) - 2; w += 3) {
            // positions
            u32 q0w = 0x4b010000 + frag[w * 4 + (0 * 4) + 3];
            u32 q1w = 0x4b010000 + frag[w * 4 + (1 * 4) + 3];
            u32 q2w = 0x4b010000 + frag[w * 4 + (2 * 4) + 3];

            // normals
            u32 q0z = 0x47800000 + frag[w * 4 + (0 * 4) + 2];
            u32 q1z = 0x47800000 + frag[w * 4 + (1 * 4) + 2];
            u32 q2z = 0x47800000 + frag[w * 4 + (2 * 4) + 2];

            // uvs
            u32 q2x = model->st_vif_add + frag[w * 4 + (2 * 4) + 0];
            u32 q2y = model->st_vif_add + frag[w * 4 + (2 * 4) + 1];

            auto* pos_array = m_mod_vtx_unpack_temp[vidx].pos;
            memcpy(&pos_array[0], &q0w, 4);
            memcpy(&pos_array[1], &q1w, 4);
            memcpy(&pos_array[2], &q2w, 4);
            pos_array[0] += float_offsets[0];
            pos_array[1] += float_offsets[1];
            pos_array[2] += float_offsets[2];
            pos_array[0] *= xyz_scale;
            pos_array[1] *= xyz_scale;
            pos_array[2] *= xyz_scale;

            auto* nrm_array = m_mod_vtx_unpack_temp[vidx].nrm;
            memcpy(&nrm_array[0], &q0z, 4);
            memcpy(&nrm_array[1], &q1z, 4);
            memcpy(&nrm_array[2], &q2z, 4);
            nrm_array[0] += -65537;
            nrm_array[1] += -65537;
            nrm_array[2] += -65537;

            auto* uv_array = m_mod_vtx_unpack_temp[vidx].uv;
            memcpy(&uv_array[0], &q2x, 4);
            memcpy(&uv_array[1], &q2y, 4);
            uv_array[0] += model->st_magic;
            uv_array[1] += model->st_magic;

            vidx++;
          }
        }

        // next control
        frag_ctrl += 4 + 2 * mat_xfer_count;

        // next frag
        u32 mm_qwc_count = frag[11];
        frag += mm_qwc_count * 16;
      }

      // sanity check
      if (effect.mod.expect_vidx_end != vidx) {
        fmt::print("---------- BAD {}/{}\n", effect.mod.expect_vidx_end, vidx);
        ASSERT(false);
      }
    }

    {
      auto pp = scoped_prof("copy");
      // now copy the data in merc original vertex order to the output.
      for (u32 vi = 0; vi < effect.mod.vertices.size(); vi++) {
        u32 addr = effect.mod.vertex_lump4_addr[vi];
        if (addr < vidx) {
          memcpy(&m_mod_vtx_temp[vi], &m_mod_vtx_unpack_temp[addr], 32);
          m_mod_vtx_temp[vi].st[0] = m_mod_vtx_unpack_temp[addr].uv[0];
          m_mod_vtx_temp[vi].st[1] = m_mod_vtx_unpack_temp[addr].uv[1];
          // 21/10 : normale soudee (pad0) des personnages, ecrasee par la copie de 32 octets
          m_mod_vtx_temp[vi].pad0 = effect.mod.vertices[vi].pad0;
        }
      }
    }

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      auto pp = scoped_prof("update-verts-upload");
      glBindBuffer(GL_ARRAY_BUFFER, opengl_buffers.vertex);
      glBufferData(GL_ARRAY_BUFFER, effect.mod.vertices.size() * sizeof(tfrag3::MercVertex),
                   m_mod_vtx_temp.data(), GL_DYNAMIC_DRAW);
    }
  }
}

/*!
 * Setup draws for a model, given the DMA data generated by the GOAL code.
 */
void Merc2::handle_pc_model(const DmaTransfer& setup,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& proff,
                            MercDebugStats* stats) {
  auto p = scoped_prof("init-pc");

  // the format of the data is:
  //  ;; name   (128 char, 8 qw)
  //  ;; lights (7 qw x 1)
  //  ;; matrix slot string (128 char, 8 qw)
  //  ;; matrices (7 qw x N)
  //  ;; flags    (num-effects, effect-alpha-ignore, effect-disable)
  //  ;; fades    (u32 x N), padding to qw aligned
  //  ;; pointers (u32 x N), padding

  // Get the name
  const u8* input_data = setup.data;
  ASSERT(strlen((const char*)input_data) < 127);
  char name[128];
  strcpy(name, (const char*)setup.data);
  input_data += 128;

  // Look up the model by name in the loader.
  // This will return a reference to this model's data, plus a reference to the level's data
  // for stuff shared between models of the same level
  auto model_ref = render_state->loader->get_merc_model(name);
  if (!model_ref) {
    // it can fail, if the game is faster than the loader. In this case, we just don't draw.
    stats->num_missing_models++;
    return;
  }

  // next, we need to check if we have enough room to draw this effect.
  const LevelData* lev = model_ref->level;
  const tfrag3::MercModel* model = model_ref->model;

  // each model uses only 1 light.
  if (m_next_free_light >= MAX_LIGHTS) {
    fmt::print("MERC2 out of lights, consider increasing MAX_LIGHTS\n");
    flush_draw_buckets(render_state, proff, stats);
  }

  // models use many bones. First check if we need to flush:
  int bone_count = model->max_bones + 1;
  if (m_next_free_bone_vector + m_opengl_buffer_alignment + bone_count * 8 >
      MAX_SHADER_BONE_VECTORS) {
    fmt::print("MERC2 out of bones, consider increasing MAX_SHADER_BONE_VECTORS\n");
    flush_draw_buckets(render_state, proff, stats);
  }

  // also sanity check that we have enough to draw the model
  if (m_opengl_buffer_alignment + bone_count * 8 > MAX_SHADER_BONE_VECTORS) {
    fmt::print(
        "MERC2 doesn't have enough bones to draw a model, increase MAX_SHADER_BONE_VECTORS\n");
    ASSERT_NOT_REACHED();
  }

  // next, we need to find a bucket that holds draws for this level (will have the right buffers
  // bound for drawing)
  LevelDrawBucket* lev_bucket = nullptr;
  for (u32 i = 0; i < m_next_free_level_bucket; i++) {
    if (m_level_draw_buckets[i].level == lev) {
      lev_bucket = &m_level_draw_buckets[i];
      break;
    }
  }

  if (!lev_bucket) {
    // no existing bucket, allocate a new one.
    if (m_next_free_level_bucket >= m_level_draw_buckets.size()) {
      // out of room, flush
      // fmt::print("MERC2 out of levels, consider increasing MAX_LEVELS\n");
      flush_draw_buckets(render_state, proff, stats);
    }
    // alloc a new one
    lev_bucket = &m_level_draw_buckets[m_next_free_level_bucket++];
    lev_bucket->reset();
    lev_bucket->level = lev;
  }

  // next check draws:
  if (lev_bucket->next_free_draw + model->max_draws >= lev_bucket->draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of draws, consider increasing MAX_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(render_state, proff, stats);
    if (model->max_draws >= lev_bucket->draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 draw buffer not big enough");
    }
  }

  // same for envmap draws
  if (lev_bucket->next_free_envmap_draw + model->max_draws >= lev_bucket->envmap_draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of envmap draws, consider increasing MAX_ENVMAP_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(render_state, proff, stats);
    if (model->max_draws >= lev_bucket->envmap_draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 envmap draw buffer not big enough");
    }
  }

  // Next part of input data is the lights
  VuLights current_lights;
  memcpy(&current_lights, input_data, sizeof(VuLights));
  input_data += sizeof(VuLights);

  u64 uses_water = 0;
  if (render_state->version == GameVersion::Jak1) {
    // jak 1 figures out water at runtime sadly
    memcpy(&uses_water, input_data, 8);
    input_data += 16;
  }

  // Next part is the matrix slot string. The game sends us a bunch of bone matrices,
  // but they may not be in order, or include all bones. The matrix slot string tells
  // us which bones go where. (the game doesn't go in order because it follows the merc format)
  ShaderMercMat skel_matrix_buffer[MAX_SKEL_BONES];
  auto* matrix_array = (const u32*)(input_data + 128);
  int i;
  for (i = 0; i < 128; i++) {
    if (input_data[i] == 0xff) {  // indicates end of string.
      break;
    }
    // read goal addr of matrix (matrix data isn't known at merc dma time, bones runs after)
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    const u8* real_addr = setup.data - setup.data_offset + addr;
    ASSERT(input_data[i] < MAX_SKEL_BONES);
    // get the matrix data
    memcpy(&skel_matrix_buffer[input_data[i]], real_addr, sizeof(MercMat));
  }
  input_data += 128 + 16 * i;

  // Next part is some flags
  struct PcMercFlags {
    u64 enable_mask;
    u64 ignore_alpha_mask;
    u8 effect_count;
    u8 bitflags;
  };
  auto* flags = (const PcMercFlags*)input_data;
  int num_effects = flags->effect_count;  // mostly just a sanity check
  ASSERT(num_effects < kMaxEffect);
  u64 current_ignore_alpha_bits = flags->ignore_alpha_mask;  // shader settings
  u64 current_effect_enable_bits = flags->enable_mask;       // mask for game to disable an effect
  bool model_uses_mod = flags->bitflags & 1;  // if we should update vertices from game.
  bool model_disables_fog = flags->bitflags & 2;
  bool model_uses_pc_blerc = flags->bitflags & 4;
  bool model_disables_envmap = flags->bitflags & 8;
  bool model_no_texture = flags->bitflags & 16;
  input_data += 32;

  float blerc_weights[kMaxBlerc];
  if (model_uses_pc_blerc) {
    memcpy(blerc_weights, input_data, kMaxBlerc * sizeof(float));
    input_data += kMaxBlerc * sizeof(float);
  }

  // Next is "fade data", indicating the color/intensity of envmap effect
  u8 fade_buffer[4 * kMaxEffect];
  for (int ei = 0; ei < num_effects; ei++) {
    for (int j = 0; j < 4; j++) {
      fade_buffer[ei * 4 + j] = input_data[ei * 4 + j];
    }
  }
  input_data += (((num_effects * 4) + 15) / 16) * 16;

  // Next is pointers to merc data, needed so we can update vertices

  // custom models are likely to have a different number of effects than what GOAL reports, update
  // the count here (after reading DMA) so we don't potentially go out of bounds when we do
  // blerc/mod draws
  if (model->effects.at(0).all_draws.at(0).no_strip) {
    num_effects = model->effects.size();
  }

  // will hold opengl buffers for the updated vertices
  ModBuffers mod_opengl_buffers[kMaxEffect];
  if (model_uses_pc_blerc) {
    model_mod_blerc_draws(num_effects, model, lev, mod_opengl_buffers, blerc_weights, stats);
  } else if (model_uses_mod) {  // only if we've enabled, this path is slow.
    model_mod_draws(num_effects, model, lev, input_data, setup, mod_opengl_buffers, stats);
  }

  // stats
  stats->num_models++;
  for (const auto& effect : model_ref->model->effects) {
    bool envmap = effect.has_envmap && !model_disables_envmap;
    stats->num_effects++;
    stats->num_predicted_draws += effect.all_draws.size();
    if (envmap) {
      stats->num_envmap_effects++;
      stats->num_predicted_draws += effect.all_draws.size();
    }
    for (const auto& draw : effect.all_draws) {
      stats->num_predicted_tris += draw.num_triangles;
      if (envmap) {
        stats->num_predicted_tris += draw.num_triangles;
      }
    }
  }

  if (stats->collect_debug_model_list) {
    auto& d = stats->model_list.emplace_back();
    d.name = model->name;
    d.level = model_ref->level->level->level_name;
    for (auto& e : model->effects) {
      auto& de = d.effects.emplace_back();
      de.envmap = e.has_envmap;
      de.envmap_mode = e.envmap_mode;
      for (auto& draw : e.all_draws) {
        auto& dd = de.draws.emplace_back();
        dd.mode = draw.mode;
        dd.num_tris = draw.num_triangles;
      }
    }
  }

  // allocate bones in shared bone buffer to be sent to GPU at flush-time
  u32 first_bone = alloc_bones(bone_count, skel_matrix_buffer);

  // allocate lights
  if (current_lights.w1) {
    if (render_state->version != GameVersion::Jak3) {
      current_lights.w1 = 0;  // force off merc fade in jak2/1 - a bunch of stuff uses this
    }
  }
  u32 lights = alloc_lights(current_lights);
  stats->num_lights++;

  u64 hash = fnv64(model->name);

  DrawArgs args;
  args.lev_bucket = lev_bucket;
  args.jak1_water_mode = uses_water;
  args.disable_fog = model_disables_fog;
  args.hash = hash;
  args.lights = lights;
  args.first_bone = first_bone;
  args.no_texture = render_state->version == GameVersion::Jak3 && model_no_texture;
  args.character = model->pc_character && m_remake.actif;
  args.objet = !args.character && model->pc_remodele && m_remake.actif && m_remake.objets;
  args.lev = lev;
  args.orbe = model->name.rfind("money", 0) == 0;
  // 26/10 : les lacs de lave (village du volcan, ogre) sont des modeles d'eau animee
  args.lave = model->name.rfind("water-anim", 0) == 0 && model->name.find("lava") != std::string::npos;

  // loop over effects, creating draws for each
  for (size_t ei = 0; ei < model->effects.size(); ei++) {
    args.fade = fade_buffer + 4 * ei;

    // game has disabled it?
    if (!(current_effect_enable_bits & (1ull << ei))) {
      continue;
    }

    // imgui menu disabled it?
    if (!m_effect_debug_mask[ei]) {
      continue;
    }

    bool ignore_alpha = !!(current_ignore_alpha_bits & (1ull << ei));
    args.ignore_alpha = ignore_alpha;
    auto& effect = model->effects[ei];

    bool should_envmap = effect.has_envmap && !model_disables_envmap;
    bool should_mod = (model_uses_pc_blerc || model_uses_mod) && effect.has_mod_draw;

    if (should_mod) {
      // draw as two parts, fixed and mod

      // do fixed draws:
      for (auto& fdraw : effect.mod.fix_draw) {
        alloc_normal_draw(fdraw, args);
        if (should_envmap) {
          try_alloc_envmap_draw(fdraw, effect.envmap_mode, effect.envmap_texture, args);
        }
      }

      // do mod draws
      for (auto& mdraw : effect.mod.mod_draw) {
        auto n = alloc_normal_draw(mdraw, args);
        // modify the draw, set the mod flag and point it to the opengl buffer
        n->flags |= MOD_VTX;
        n->mod_vtx_buffer = mod_opengl_buffers[ei];
        if (should_envmap) {
          auto e = try_alloc_envmap_draw(mdraw, effect.envmap_mode, effect.envmap_texture, args);
          if (e) {
            e->flags |= MOD_VTX;
            e->mod_vtx_buffer = mod_opengl_buffers[ei];
          }
        }
      }
    } else {
      // no mod, just do all_draws
      for (auto& draw : effect.all_draws) {
        if (should_envmap) {
          try_alloc_envmap_draw(draw, effect.envmap_mode, effect.envmap_texture, args);
        }
        alloc_normal_draw(draw, args);
      }
    }
  }
}

void Merc2::draw_debug_window(MercDebugStats* stats) {
  ImGui::Text("Models   : %d", stats->num_models);
  ImGui::Text("Effects  : %d", stats->num_effects);
  ImGui::Text("Draws (p): %d", stats->num_predicted_draws);
  ImGui::Text("Tris  (p): %d", stats->num_predicted_tris);
  ImGui::Text("Bones    : %d", stats->num_bones_uploaded);
  ImGui::Text("Lights   : %d", stats->num_lights);
  ImGui::Text("Dflush   : %d", stats->num_draw_flush);

  ImGui::Text("EEffects : %d", stats->num_envmap_effects);
  ImGui::Text("ETris    : %d", stats->num_envmap_tris);

  ImGui::Text("Uploads  : %d", stats->num_uploads);
  ImGui::Text("Upload kB: %d", stats->num_upload_bytes / 1024);

  ImGui::Checkbox("Debug", &stats->collect_debug_model_list);

  ImGui::SliderFloat("blerc-nightmare", &blerc_multiplier, -3, 3);

  if (stats->collect_debug_model_list) {
    for (int i = 0; i < kMaxEffect; i++) {
      ImGui::Checkbox(fmt::format("e{:02d}", i).c_str(), &m_effect_debug_mask[i]);
    }

    for (const auto& model : stats->model_list) {
      if (ImGui::TreeNode(model.name.c_str())) {
        ImGui::Text("Level: %s\n", model.level.c_str());
        for (const auto& e : model.effects) {
          for (const auto& d : e.draws) {
            ImGui::Text("%s", d.mode.to_string().c_str());
          }
          ImGui::Separator();
        }
        ImGui::TreePop();
      }
    }
  }
}

void Merc2::init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights) {
  auto id = shader.id();
  shader.activate();
  if (include_lights) {
    uniforms->light_direction[0] = pc_uniform_loc(id, "light_dir0_fade");
    uniforms->light_direction[1] = pc_uniform_loc(id, "light_dir1_fade_en");
    uniforms->light_direction[2] = pc_uniform_loc(id, "light_dir2");
    uniforms->light_color[0] = pc_uniform_loc(id, "light_col0");
    uniforms->light_color[1] = pc_uniform_loc(id, "light_col1");
    uniforms->light_color[2] = pc_uniform_loc(id, "light_col2");
    uniforms->light_ambient = pc_uniform_loc(id, "light_ambient");
  }

  uniforms->hvdf_offset = pc_uniform_loc(id, "hvdf_offset");

  uniforms->fog = pc_uniform_loc(id, "fog_constants");
  uniforms->decal = pc_uniform_loc(id, "decal_enable");

  uniforms->fog_color = pc_uniform_loc(id, "fog_color");
  uniforms->perspective_matrix = pc_uniform_loc(id, "perspective_matrix");
  // PC-REMAKE: character sun shadows
  uniforms->sun_light_from_cam = pc_uniform_loc(id, "sun_light_from_cam");
  uniforms->ignore_alpha = pc_uniform_loc(id, "ignore_alpha");

  uniforms->gfx_hack_no_tex = pc_uniform_loc(id, "gfx_hack_no_tex");
}

void Merc2::switch_to_merc2(SharedRenderState* render_state) {
  render_state->shaders[ShaderId::MERC2].activate();

  // set uniforms that we know from render_state
  glUniform4f(m_merc_uniforms.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  glUniform1i(m_merc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}

void Merc2::switch_to_emerc(SharedRenderState* render_state) {
  render_state->shaders[ShaderId::EMERC].activate();
  // set uniforms that we know from render_state
  glUniform4f(m_emerc_uniforms.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  glUniform1i(m_emerc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}

/*!
 * Main merc2 rendering.
 */
void Merc2::render(DmaFollower& dma,
                   SharedRenderState* render_state,
                   ScopedProfilerNode& prof,
                   MercDebugStats* stats) {
  if (m_shaders_seq != g_pc_merc_shaders_seq) {
    m_shaders_seq = g_pc_merc_shaders_seq;
    init_shaders(render_state->shaders);
  }
  bool hack = stats->collect_debug_model_list;
  *stats = {};
  stats->collect_debug_model_list = hack;
  if (stats->collect_debug_model_list) {
    stats->model_list.clear();
  }

  switch_to_merc2(render_state);

  {
    auto pp = scoped_prof("handle-all-dma");
    // iterate through the dma chain, filling buckets
    handle_all_dma(dma, render_state, prof, stats);
  }

  {
    auto pp = scoped_prof("flush-buckets");
    // flush buckets to draws
    flush_draw_buckets(render_state, prof, stats);
  }
}

u32 Merc2::alloc_lights(const VuLights& lights) {
  ASSERT(m_next_free_light < MAX_LIGHTS);
  u32 light_idx = m_next_free_light;
  m_lights_buffer[m_next_free_light++] = lights;
  static_assert(sizeof(VuLights) == 7 * 16);
  return light_idx;
}

std::string Merc2::ShaderMercMat::to_string() const {
  return fmt::format("tmat:\n{}\n{}\n{}\n{}\n", tmat[0].to_string_aligned(),
                     tmat[1].to_string_aligned(), tmat[2].to_string_aligned(),
                     tmat[3].to_string_aligned());
}

/*!
 * Main MERC2 function to handle DMA
 */
void Merc2::handle_all_dma(DmaFollower& dma,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof,
                           MercDebugStats* stats) {
  // process the first tag. this is just jumping to the merc-specific dma.
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
  // if we reach here, there's stuff to draw
  // this handles merc-specific setup DMA
  handle_setup_dma(dma, render_state);

  // handle each merc transfer
  while (dma.current_tag_offset() != render_state->next_bucket) {
    handle_merc_chain(dma, render_state, prof, stats);
  }
  ASSERT(dma.current_tag_offset() == render_state->next_bucket);
}

namespace {
void set_uniform(GLuint uniform, const math::Vector3f& val) {
  glUniform3f(uniform, val.x(), val.y(), val.z());
}
void set_uniform(GLuint uniform, const math::Vector4f& val) {
  glUniform4f(uniform, val.x(), val.y(), val.z(), val.w());
}
}  // namespace

void Merc2::handle_setup_dma(DmaFollower& dma, SharedRenderState* render_state) {
  auto first = dma.read_and_advance();

  // 10 quadword setup packet
  ASSERT(first.size_bytes == 10 * 16);

  // transferred vifcodes
  {
    auto vif0 = first.vifcode0();
    auto vif1 = first.vifcode1();
    // STCYCL 4, 4
    ASSERT(vif0.kind == VifCode::Kind::STCYCL);
    auto vif0_st = VifCodeStcycl(vif0);
    ASSERT(vif0_st.cl == 4 && vif0_st.wl == 4);
    // STMOD
    ASSERT(vif1.kind == VifCode::Kind::STMOD);
    ASSERT(vif1.immediate == 0);
  }

  // 1 qw with 4 vifcodes.
  u32 vifcode_data[4];
  memcpy(vifcode_data, first.data, 16);
  {
    auto vif0 = VifCode(vifcode_data[0]);
    ASSERT(vif0.kind == VifCode::Kind::BASE);
    ASSERT(vif0.immediate == MercDataMemory::BUFFER_BASE);
    auto vif1 = VifCode(vifcode_data[1]);
    ASSERT(vif1.kind == VifCode::Kind::OFFSET);
    ASSERT((s16)vif1.immediate == MercDataMemory::BUFFER_OFFSET);
    auto vif2 = VifCode(vifcode_data[2]);
    ASSERT(vif2.kind == VifCode::Kind::NOP);
    auto vif3 = VifCode(vifcode_data[3]);
    ASSERT(vif3.kind == VifCode::Kind::UNPACK_V4_32);
    VifCodeUnpack up(vif3);
    ASSERT(up.addr_qw == MercDataMemory::LOW_MEMORY);
    ASSERT(!up.use_tops_flag);
    ASSERT(vif3.num == 8);
  }

  // 8 qw's of low memory data
  memcpy(&m_low_memory, first.data + 16, sizeof(LowMemory));

  // PC-REMAKE: matrix taking merc VIEW-space vertices to sun-shadow clip space
  float sun_mat[16];
  bool sun_ok = render_state->has_pc_data &&
                compute_sun_light_from_camera(render_state->camera_matrix, render_state->camera_pos,
                                              m_low_memory.perspective, sun_mat);

  m_shadow_light_valid = sun_ok;
  if (sun_ok) {
    for (int i = 0; i < 16; i++) {
      m_shadow_light_from_cam[i] = sun_mat[i];
    }
  }

  // PC-REMAKE 21/09 : normales monde pour le G-buffer
  float nrm_mat[9];
  float monde_mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // 26/10 : lave
  bool nrm_ok = render_state->has_pc_data &&
                compute_world_normal_from_merc(render_state->camera_matrix, m_low_memory.perspective,
                                               nrm_mat, monde_mat);
  if (!nrm_ok) {
    for (int i = 0; i < 9; i++) {
      nrm_mat[i] = (i % 4 == 0) ? 1.f : 0.f;
    }
  }
  m_world_normal_valid = nrm_ok;

  switch_to_merc2(render_state);
  set_uniform(m_merc_uniforms.hvdf_offset, m_low_memory.hvdf_offset);
  set_uniform(m_merc_uniforms.fog, m_low_memory.fog);
  glUniformMatrix4fv(m_merc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &m_low_memory.perspective[0].x());
  glUniformMatrix3fv(
      pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "world_nrm_from_merc"), 1,
      GL_FALSE, nrm_mat);
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "write_normals"),
              nrm_ok ? 1.f : 0.f);
  // PC-REMAKE 22/09 : eclairage par pixel des personnages (merc2.frag) ;
  // `touch /tmp/qa_gouraud` pour revoir le Gouraud d'origine (A/B a chaud)
  static int ppl_poll = 0;
  static float ppl = 1.f;
  if ((ppl_poll++ & 31) == 0) {
    ppl = fs::exists("/tmp/qa_gouraud") ? 0.f : 1.f;
  }
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "per_pixel_light"), ppl);
  // 26/10 : lave qui ondule (merc2.vert / merc2.frag) : position de la camera et horloge, les
  // memes que la lave du decor (TFragment, wind_time)
  glUniform3f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "lava_cam"),
              render_state->camera_pos[0], render_state->camera_pos[1],
              render_state->camera_pos[2]);
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "lava_time"),
              (float)(render_state->frame_idx % 216000) / 60.f);
  glUniformMatrix4fv(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(),
                                    "lava_world_from_merc"),
                     1, GL_FALSE, monde_mat);
  glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "remake_force"), 1.f);
  {
    auto id = render_state->shaders[ShaderId::MERC2].id();
    if (sun_ok) {
      glUniformMatrix4fv(m_merc_uniforms.sun_light_from_cam, 1, GL_FALSE, sun_mat);
      g_sun_shadow.bind_for_receive(id);
    } else {
      // 21/09 : sampler toujours pose sur l'unite 14 (voir bind_disabled)
      g_sun_shadow.bind_disabled(id);
    }
  }
  switch_to_emerc(render_state);
  // 21/10 : normales merc -> monde, pour le reflet des orbes
  glUniformMatrix3fv(pc_uniform_loc(render_state->shaders[ShaderId::EMERC].id(),
                                    "world_nrm_from_merc"),
                     1, GL_FALSE, nrm_mat);
  set_uniform(m_emerc_uniforms.hvdf_offset, m_low_memory.hvdf_offset);
  set_uniform(m_emerc_uniforms.fog, m_low_memory.fog);
  glUniformMatrix4fv(m_emerc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &m_low_memory.perspective[0].x());
  float sun_view[4] = {0.f, 1.f, 0.f, 0.f};  // 21/10 : reutilisee par EMERC_TESS
  {
    // PC-REMAKE: sun glint on reflective (metal) materials. The sun direction
    // is rotated into this shader's space with the camera basis, and the
    // strength follows the shadow setting so the whole sun rig stays coherent.
    auto id = render_state->shaders[ShaderId::EMERC].id();
    float sx = g_sun_shadow_params[0], sy = g_sun_shadow_params[1], sz = g_sun_shadow_params[2];
    float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len > 1e-4f && render_state->has_pc_data) {
      // the light travels along g_sun_shadow_params; we want the direction TO
      // the sun, hence the negation
      float wx = -sx / len, wy = -sy / len, wz = -sz / len;
      const auto& C = render_state->camera_matrix;
      float vx = -(C[0][0] * wx + C[1][0] * wy + C[2][0] * wz);
      float vy = -(C[0][1] * wx + C[1][1] * wy + C[2][1] * wz);
      float vz = -(C[0][2] * wx + C[1][2] * wy + C[2][2] * wz);
      glUniform4f(pc_uniform_loc(id, "sun_dir_view"), vx, vy, vz, 1.f);
      // 21/10 : soleil en monde (reflet des orbes, emerc.frag)
      glUniform4f(pc_uniform_loc(id, "sun_dir_world"), wx, wy, wz, 1.f);
      sun_view[0] = vx;
      sun_view[1] = vy;
      sun_view[2] = vz;
      sun_view[3] = 1.f;
    } else {
      glUniform4f(pc_uniform_loc(id, "sun_dir_view"), 0.f, 1.f, 0.f, 0.f);
    }
    // 18/08 : le glint suit la FORCE du soleil (w des ombres), pas seulement sa
    // direction — sinon les objets reflechissants gardaient leur reflet de
    // plein soleil en PLEINE NUIT (18/08).
    glUniform1f(pc_uniform_loc(id, "glint_strength"),
                0.85f * std::min(1.f, g_sun_shadow_params[3] / 0.5f));
  }

  // 21/10 : PERSONNAGES REMODELES — memes reglages par image que MERC2 / EMERC pour les
  // programmes decoupes et la fourrure. Interrupteurs de comparaison : /tmp/qa_perso_off (tout),
  // /tmp/qa_perso_notess (decoupage), /tmp/qa_perso_nofur (fourrure).
  {
    static int poll = 0;
    static bool qa_split = false;
    if ((poll++ & 63) == 0) {
      m_remake.actif = !fs::exists("/tmp/qa_perso_off");
      m_remake.objets = !fs::exists("/tmp/qa_obj_off");
#ifdef __APPLE__
      // 26/10 : le pilote OpenGL du Mac emule la tessellation : ~8 ms par image au village pour
      // une poignee de personnages, MEME sans aucune decoupe (cout du pipeline, pas des
      // triangles). Decoupage coupe par defaut sur Mac (/tmp/qa_perso_tess pour le revoir) ;
      // ailleurs (Windows, Linux, Steam Deck), tessellation native : inchange.
      // (original : m_remake.decoupe = !fs::exists("/tmp/qa_perso_notess");)
      m_remake.decoupe = fs::exists("/tmp/qa_perso_tess");
#else
      m_remake.decoupe = !fs::exists("/tmp/qa_perso_notess");
#endif
      m_remake.fourrure = !fs::exists("/tmp/qa_perso_nofur");
      qa_split = fs::exists("/tmp/qa_perso_split");
    }
    // essai /tmp/qa_perso_split : rendu d'origine a gauche, matieres a droite (meme image)
    render_state->shaders[ShaderId::MERC2].activate();
    float split = 0.f;
    if (qa_split) {
      GLint vp[4];
      glGetIntegerv(GL_VIEWPORT, vp);
      split = vp[0] + vp[2] * 0.5f;
    }
    glUniform1f(pc_uniform_loc(render_state->shaders[ShaderId::MERC2].id(), "remake_split"),
                split);
    auto commun = [&](ShaderId sid, Uniforms& u) {
      auto& sh = render_state->shaders[sid];
      sh.activate();
      glUniform4f(u.fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                  render_state->fog_intensity / 255);
      glUniform1i(u.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
      set_uniform(u.hvdf_offset, m_low_memory.hvdf_offset);
      set_uniform(u.fog, m_low_memory.fog);
      glUniformMatrix4fv(u.perspective_matrix, 1, GL_FALSE, &m_low_memory.perspective[0].x());
      // longueur visee d'un morceau : 0,6 % de la distance, 6 coupes au plus par arete
      glUniform2f(pc_uniform_loc(sh.id(), "decoupe"), m_remake.decoupe ? 0.006f : 0.f, 6.f);
      return sh.id();
    };
    for (auto [sid, u] : {std::pair<ShaderId, Uniforms*>{ShaderId::MERC2_TESS, &m_merc_tess_uniforms},
                          {ShaderId::MERC2_FUR, &m_merc_fur_uniforms}}) {
      if (!render_state->shaders[sid].okay()) {
        continue;
      }
      auto id = commun(sid, *u);
      glUniformMatrix3fv(pc_uniform_loc(id, "world_nrm_from_merc"), 1, GL_FALSE, nrm_mat);
      glUniform1f(pc_uniform_loc(id, "write_normals"), nrm_ok ? 1.f : 0.f);
      glUniform1f(pc_uniform_loc(id, "per_pixel_light"), ppl);
      glUniform1f(pc_uniform_loc(id, "remake_force"), 1.f);
      glUniform1f(pc_uniform_loc(id, "remake_split"), split);
      // fourrure : poils de 1 cm, brins de 3 mm, 16 couches douces (v1 : brins de 1,7 mm — plus
      // petits qu'un pixel en jeu, scintillement)
      glUniform4f(pc_uniform_loc(id, "fourrure"), 42.f, 12.f, 16.f, 0.f);
      if (sun_ok) {
        glUniformMatrix4fv(u->sun_light_from_cam, 1, GL_FALSE, sun_mat);
        g_sun_shadow.bind_for_receive(id);
      } else {
        g_sun_shadow.bind_disabled(id);
      }
    }
    if (render_state->shaders[ShaderId::EMERC_TESS].okay()) {
      auto id = commun(ShaderId::EMERC_TESS, m_emerc_tess_uniforms);
      glUniform4f(pc_uniform_loc(id, "sun_dir_view"), sun_view[0], sun_view[1], sun_view[2],
                  sun_view[3]);
      glUniform1f(pc_uniform_loc(id, "glint_strength"),
                  0.85f * std::min(1.f, g_sun_shadow_params[3] / 0.5f));
    }
  }

  // 1 qw with another 4 vifcodes.
  u32 vifcode_final_data[4];
  memcpy(vifcode_final_data, first.data + 16 + sizeof(LowMemory), 16);
  {
    ASSERT(VifCode(vifcode_final_data[0]).kind == VifCode::Kind::FLUSHE);
    ASSERT(vifcode_final_data[1] == 0);
    ASSERT(vifcode_final_data[2] == 0);
    VifCode mscal(vifcode_final_data[3]);
    ASSERT(mscal.kind == VifCode::Kind::MSCAL);
    ASSERT(mscal.immediate == 0);
  }

  // TODO: process low memory initialization

  if (render_state->version == GameVersion::Jak1) {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 32);  // setting up test register.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  } else {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 48);  // setting up test/zbuf register.
    // todo z write mask stuff.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  }
}

namespace {
bool tag_is_nothing_next(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}
}  // namespace

void Merc2::handle_merc_chain(DmaFollower& dma,
                              SharedRenderState* render_state,
                              ScopedProfilerNode& prof,
                              MercDebugStats* stats) {
  while (tag_is_nothing_next(dma)) {
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
  }
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  auto init = dma.read_and_advance();
  int skip_count = 2;
  if (render_state->version >= GameVersion::Jak2) {
    skip_count = 1;
  }

  while (init.vifcode1().kind == VifCode::Kind::PC_PORT) {
    // flush_pending_model(render_state, prof);
    handle_pc_model(init, render_state, prof, stats);
    for (int i = 0; i < skip_count; i++) {
      auto link = dma.read_and_advance();
      ASSERT(link.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(link.vifcode1().kind == VifCode::Kind::NOP);
      ASSERT(link.size_bytes == 0);
    }
    init = dma.read_and_advance();
  }

  if (init.vifcode0().kind == VifCode::Kind::FLUSHA) {
    int num_skipped = 0;
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
      num_skipped++;
    }
    ASSERT(num_skipped < 4);
    return;
  }
}

/*!
 * Queue up some bones to be included in the bone buffer.
 * Returns the index of the first bone vector.
 */
u32 Merc2::alloc_bones(int count, ShaderMercMat* data) {
  u32 first_bone_vector = m_next_free_bone_vector;
  ASSERT(count * 8 + first_bone_vector <= MAX_SHADER_BONE_VECTORS);

  // model should have under 128 bones.
  ASSERT(count <= MAX_SKEL_BONES);

  // iterate over each bone we need
  for (int i = 0; i < count; i++) {
    auto& skel_mat = data[i];
    auto* shader_mat = &m_shader_bone_vector_buffer[m_next_free_bone_vector];
    int bv = 0;

    // and copy to the large bone buffer.
    for (int j = 0; j < 4; j++) {
      shader_mat[bv++] = skel_mat.tmat[j];
    }

    for (int j = 0; j < 3; j++) {
      shader_mat[bv++] = skel_mat.nmat[j];
    }

    m_next_free_bone_vector += 8;
  }

  auto b0 = m_next_free_bone_vector;
  m_next_free_bone_vector += m_opengl_buffer_alignment - 1;
  m_next_free_bone_vector /= m_opengl_buffer_alignment;
  m_next_free_bone_vector *= m_opengl_buffer_alignment;
  ASSERT(b0 <= m_next_free_bone_vector);
  ASSERT(first_bone_vector + count * 8 <= m_next_free_bone_vector);
  return first_bone_vector;
}

Merc2::ModBuffers Merc2::alloc_mod_vtx_buffer(const LevelData* lev) {
  if (m_next_mod_vtx_buffer >= m_mod_vtx_buffers.size()) {
    GLuint b;
    glGenBuffers(1, &b);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    setup_merc_vao();
    m_mod_vtx_buffers.push_back({vao, b});
  }
  return m_mod_vtx_buffers[m_next_mod_vtx_buffer++];
}

Merc2::Draw* Merc2::try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                                          const DrawMode& envmap_mode,
                                          u32 envmap_texture,
                                          const DrawArgs& args) {
  bool nonzero_fade = false;
  for (int i = 0; i < 4; i++) {
    if (args.fade[i]) {
      nonzero_fade = true;
      break;
    }
  }
  if (!nonzero_fade) {
    return nullptr;
  }

  Draw* draw = &args.lev_bucket->envmap_draws[args.lev_bucket->next_free_envmap_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = envmap_mode;
  draw->hash = 0;
  if (args.jak1_water_mode) {
    draw->mode.enable_ab();
    draw->mode.disable_depth_write();
  }
  draw->texture = envmap_texture;
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  draw->famille = 0;
  draw->reflet = args.orbe ? 1 : 0;
  draw->lave = 0;
  if (args.character) {
    draw->flags |= CHARACTER;
    // la passe des reflets suit la meme surface que la base (bois deforme compris)
    bool coquilles = false;
    draw->famille =
        mdraw.eye_id != 0xff ? 7 : texture_famille(args.lev, mdraw.tree_tex_id, &coquilles);
  } else if (args.objet && mdraw.eye_id == 0xff) {
    draw->flags |= OBJET;
    bool coquilles = false;
    draw->famille = texture_famille(args.lev, mdraw.tree_tex_id, &coquilles);
  }
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = args.fade[i];
  }
  return draw;
}

Merc2::Draw* Merc2::alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args) {
  Draw* draw = &args.lev_bucket->draws[args.lev_bucket->next_free_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = mdraw.mode;
  draw->hash = args.hash;
  if (args.jak1_water_mode) {
    draw->mode.set_ab(true);
    draw->mode.disable_depth_write();
  }

  if (args.disable_fog) {
    draw->mode.set_fog(false);
    // but don't toggle it the other way?
  }

  draw->texture = mdraw.eye_id == 0xff ? mdraw.tree_tex_id : (0xefffff00 | mdraw.eye_id);
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  if (args.ignore_alpha) {
    draw->flags |= IGNORE_ALPHA;
  }
  if (args.no_texture) {
    draw->flags |= NO_TEXTURE;
  }
  draw->famille = 0;
  draw->reflet = args.orbe ? 1 : 0;
  draw->lave = args.lave ? 1 : 0;
  if (args.character) {
    draw->flags |= CHARACTER;
    bool coquilles = false;
    draw->famille =
        mdraw.eye_id != 0xff ? 7 : texture_famille(args.lev, mdraw.tree_tex_id, &coquilles);
    if (coquilles && !draw->mode.get_ab_enable()) {
      draw->flags |= FUR_SHELLS;
    }
  } else if (args.objet && mdraw.eye_id == 0xff) {
    draw->flags |= OBJET;
    bool coquilles = false;
    draw->famille = texture_famille(args.lev, mdraw.tree_tex_id, &coquilles);
  }
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = 0;
  }
  return draw;
}

void Merc2::setup_merc_vao() {
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(4);
  glEnableVertexAttribArray(5);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);

  glVertexAttribPointer(0,                                        // location 0 in the shader
                        3,                                        // 3 values per vert
                        GL_FLOAT,                                 // floats
                        GL_FALSE,                                 // normalized
                        sizeof(tfrag3::MercVertex),               // stride
                        (void*)offsetof(tfrag3::MercVertex, pos)  // offset (0)
  );

  glVertexAttribPointer(1,                                              // location 1 in the
                        3,                                              // 3 values per vert
                        GL_FLOAT,                                       // floats
                        GL_FALSE,                                       // normalized
                        sizeof(tfrag3::MercVertex),                     // stride
                        (void*)offsetof(tfrag3::MercVertex, normal[0])  // offset (0)
  );

  glVertexAttribPointer(2,                                               // location 1 in the
                        3,                                               // 3 values per vert
                        GL_FLOAT,                                        // floats
                        GL_FALSE,                                        // normalized
                        sizeof(tfrag3::MercVertex),                      // stride
                        (void*)offsetof(tfrag3::MercVertex, weights[0])  // offset (0)
  );

  glVertexAttribPointer(3,                                          // location 1 in the shader
                        2,                                          // 3 values per vert
                        GL_FLOAT,                                   // floats
                        GL_FALSE,                                   // normalized
                        sizeof(tfrag3::MercVertex),                 // stride
                        (void*)offsetof(tfrag3::MercVertex, st[0])  // offset (0)
  );

  glVertexAttribPointer(4,                                            // location 1 in the shader
                        4,                                            // 3 values per vert
                        GL_UNSIGNED_BYTE,                             // floats
                        GL_TRUE,                                      // normalized
                        sizeof(tfrag3::MercVertex),                   // stride
                        (void*)offsetof(tfrag3::MercVertex, rgba[0])  // offset (0)
  );

  glVertexAttribIPointer(5,                                            // location 0 in the
                         4,                                            // 3 floats per vert
                         GL_UNSIGNED_BYTE,                             // u8's
                         sizeof(tfrag3::MercVertex),                   //
                         (void*)offsetof(tfrag3::MercVertex, mats[0])  // offset in array
  );

  // 21/10 : normale soudee des personnages (pad0, voir tfrag3::pc_pack_weld_normal)
  glEnableVertexAttribArray(6);
  glVertexAttribIPointer(6, 1, GL_UNSIGNED_INT, sizeof(tfrag3::MercVertex),
                         (void*)offsetof(tfrag3::MercVertex, pad0));
}

void Merc2::flush_draw_buckets(SharedRenderState* render_state,
                               ScopedProfilerNode& prof,
                               MercDebugStats* stats) {
  stats->num_draw_flush++;
  // PC-REMAKE: the caster list is NOT cleared here — this runs several times
  // per frame and the last call often has nothing left in it, which wiped the
  // list. The shadow pass clears it once it has replayed it. The cap is only a
  // safety valve for when the shadow map is off and nobody drains the list.
  if (m_shadow_casters.size() > 8192) {
    m_shadow_casters.clear();
  }
  for (u32 li = 0; li < m_next_free_level_bucket; li++) {
    const auto& lev_bucket = m_level_draw_buckets[li];
    const auto* lev = lev_bucket.level;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    setup_merc_vao();
    stats->num_bones_uploaded += m_next_free_bone_vector;

    glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, m_next_free_bone_vector * sizeof(math::Vector4f),
                    m_shader_bone_vector_buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // 21/10 v3 : cartes de detail des matieres (unite 5)
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_detail_tex);
    glActiveTexture(GL_TEXTURE0);
    switch_to_merc2(render_state);
    // PC-REMAKE 21/09 : la passe de base ecrit la normale monde dans le G-buffer ;
    // la passe envmap (EMERC, additive) ne doit pas y toucher
    if (m_world_normal_valid) {
      pc_normal_target(true);
    }
    do_draws(lev_bucket.draws.data(), lev, lev_bucket.next_free_draw, m_merc_uniforms, prof, false,
             render_state);
    pc_normal_target(false);
    if (lev_bucket.next_free_envmap_draw) {
      switch_to_emerc(render_state);
      do_draws(lev_bucket.envmap_draws.data(), lev, lev_bucket.next_free_envmap_draw,
               m_emerc_uniforms, prof, true, render_state);
    }

    // Record what to replay into the shadow map. Models with modified vertices
    // live in their own buffer that is recycled every frame, so they are left
    // out rather than risk replaying stale geometry.
    auto collect = [&](const Draw* arr, u32 n) {
      for (u32 di = 0; di < n; di++) {
        const auto& d = arr[di];
        if (d.flags & MOD_VTX) {
          continue;
        }
        m_shadow_casters.push_back({lev->merc_vertices, lev->merc_indices, d.first_index,
                                    d.index_count, d.first_bone, d.no_strip});
      }
    };
    collect(lev_bucket.draws.data(), lev_bucket.next_free_draw);
    collect(lev_bucket.envmap_draws.data(), lev_bucket.next_free_envmap_draw);
  }

  m_next_free_light = 0;
  m_next_free_bone_vector = 0;
  m_next_free_level_bucket = 0;
  m_next_mod_vtx_buffer = 0;
}

// PC-REMAKE: draw last frame's characters and props into the sun shadow map.
// This is what makes Jak, the villagers and the crates cast a shadow on the
// ground instead of floating on it. Runs once per frame, right after the map is
// cleared and before any scenery is drawn, so a single framebuffer switch is
// involved (switching per model cost 15x the frame time when the palm crowns
// were first done that way).
void Merc2::render_shadow_casters(void* shader_lib) {
  if (m_shadow_casters.empty() || !m_shadow_light_valid) {
    m_shadow_casters.clear();
    return;
  }
  auto* shaders = static_cast<ShaderLibrary*>(shader_lib);
  auto& shader = (*shaders)[ShaderId::MERC_SHADOW];
  shader.activate();
  glUniformMatrix4fv(pc_uniform_loc(shader.id(), "sun_light_from_cam"), 1, GL_FALSE,
                     m_shadow_light_from_cam);
  glUniformBlockBinding(shader.id(), glGetUniformBlockIndex(shader.id(), "ub_bones"), 1);

  // Skinned geometry needs a heavier depth bias than the scenery: a character
  // is made of many thin, curved surfaces almost parallel to the light, which
  // is exactly the case that self-shadows into acne. Restored afterwards so the
  // scenery keeps its own (tighter) offset.
  glPolygonOffset(2.f, 4.f);
  // This hook runs INSIDE another renderer's frame, between its VAO setup and
  // its draws. Leaving merc's VAO bound here made the caller bind its index
  // buffer into OUR VAO and then draw with OUR attribute layout — garbage
  // indices, empty shadow map, every cast shadow in the game gone. Save and
  // restore the whole binding.
  GLint saved_vao = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
  glBindVertexArray(m_vao);
  unsigned int bound_vtx = 0;
  for (const auto& c : m_shadow_casters) {
    if (c.vertices != bound_vtx) {
      glBindBuffer(GL_ARRAY_BUFFER, c.vertices);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.indices);
      setup_merc_vao();
      bound_vtx = c.vertices;
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                      sizeof(math::Vector4f) * c.first_bone, 128 * sizeof(ShaderMercMat));
    glDrawElements(c.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, c.index_count, GL_UNSIGNED_INT,
                   (void*)(sizeof(u32) * c.first_index));
    qa_glerr_check("merc2 draw #1 (ligne 1383)");  // [QA-GLERR] 14/10
  }
  m_shadow_casters.clear();
  glBindVertexArray(saved_vao);
}

// PC-REMAKE 21/09 : matrice (3x3) qui envoie une NORMALE de l'espace merc (celui
// des os, vue pre-perspective) vers le monde, pour le G-buffer de normales.
// Positions : monde = campos + (-A^-1 * P) * v (cf. compute_sun_light_from_camera)
// -> pour une normale, l'inverse-transposee de la partie lineaire de ce produit.
static bool compute_world_normal_from_merc(const math::Vector4f* camera_matrix,
                                           const math::Vector4f* perspective,
                                           float* out9,
                                           float* out16) {
  float A[16], P[16], Ainv[16], M[16];
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      A[c * 4 + r] = camera_matrix[c][r];
      P[c * 4 + r] = perspective[c][r];
    }
  }
  if (!pc_remake_mat_inverse(A, Ainv)) {
    return false;
  }
  for (int i = 0; i < 16; i++) {
    Ainv[i] = -Ainv[i];
  }
  // M = Ainv * P (column-major)
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float acc = 0.f;
      for (int k = 0; k < 4; k++) {
        acc += Ainv[k * 4 + r] * P[c * 4 + k];
      }
      M[c * 4 + r] = acc;
    }
  }
  // 26/10 : la matrice complete (position merc -> monde, homogene), pour la lave
  if (out16) {
    for (int i = 0; i < 16; i++) {
      out16[i] = M[i];
    }
  }
  // L = partie 3x3 (colonnes 0..2, lignes 0..2), inverse puis transposee
  float L[9];
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      L[c * 3 + r] = M[c * 4 + r];
    }
  }
  // inverse 3x3 (column-major : L[c*3+r])
  auto e = [&](int r, int c) { return L[c * 3 + r]; };
  float det = e(0, 0) * (e(1, 1) * e(2, 2) - e(1, 2) * e(2, 1)) -
              e(0, 1) * (e(1, 0) * e(2, 2) - e(1, 2) * e(2, 0)) +
              e(0, 2) * (e(1, 0) * e(2, 1) - e(1, 1) * e(2, 0));
  if (std::fabs(det) < 1e-20f) {
    return false;
  }
  float inv[9];  // inv[c*3+r] column-major
  float id = 1.f / det;
  // cofactors -> inverse = adj/det ; on veut inverse-TRANSPOSEE : out(r,c) = inv(c,r)
  inv[0 * 3 + 0] = (e(1, 1) * e(2, 2) - e(1, 2) * e(2, 1)) * id;
  inv[1 * 3 + 0] = (e(0, 2) * e(2, 1) - e(0, 1) * e(2, 2)) * id;
  inv[2 * 3 + 0] = (e(0, 1) * e(1, 2) - e(0, 2) * e(1, 1)) * id;
  inv[0 * 3 + 1] = (e(1, 2) * e(2, 0) - e(1, 0) * e(2, 2)) * id;
  inv[1 * 3 + 1] = (e(0, 0) * e(2, 2) - e(0, 2) * e(2, 0)) * id;
  inv[2 * 3 + 1] = (e(0, 2) * e(1, 0) - e(0, 0) * e(1, 2)) * id;
  inv[0 * 3 + 2] = (e(1, 0) * e(2, 1) - e(1, 1) * e(2, 0)) * id;
  inv[1 * 3 + 2] = (e(0, 1) * e(2, 0) - e(0, 0) * e(2, 1)) * id;
  inv[2 * 3 + 2] = (e(0, 0) * e(1, 1) - e(0, 1) * e(1, 0)) * id;
  // transposee : out[c*3+r] = inv[r*3+c]
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      out9[c * 3 + r] = inv[r * 3 + c];
    }
  }
  return true;
}

void Merc2::do_draws(const Draw* draw_array,
                     const LevelData* lev,
                     u32 num_draws,
                     const Uniforms& uniforms,
                     ScopedProfilerNode& prof,
                     bool set_fade,
                     SharedRenderState* render_state) {
  glBindVertexArray(m_vao);
  s32 last_tex = INT32_MIN;
  int last_light = -1;
  bool normal_vtx_buffer_bound = true;

  bool fog_on = true;

  // 21/10 : PERSONNAGES REMODELES — chaque dessin de personnage passe par le programme decoupe
  // (base ou reflets) ; les autres gardent MERC2 / EMERC. Le programme change a la volee.
  const ShaderId sid_base = set_fade ? ShaderId::EMERC : ShaderId::MERC2;
  const ShaderId sid_tess = set_fade ? ShaderId::EMERC_TESS : ShaderId::MERC2_TESS;
  const bool tess_ok =
      m_remake.actif && m_remake.decoupe && render_state->shaders[sid_tess].okay();
  const bool fur_ok = !set_fade && m_remake.actif && m_remake.fourrure &&
                      render_state->shaders[ShaderId::MERC2_FUR].okay();
  const Uniforms* U = &uniforms;
  ShaderId sid_cur = sid_base;
  float last_perso = -1.f;
  int last_famille = -1;
  int last_reflet = -1;
  int last_lave = -1;
  if (tess_ok) {
    glPatchParameteri(GL_PATCH_VERTICES, 3);
  }
  auto set_lights = [&](const Uniforms& u, u16 light_idx) {
    const auto& l0_dir = m_lights_buffer[light_idx].direction0;
    const auto& l1_dir = m_lights_buffer[light_idx].direction1;
    float fade = 1.f;
    float fade_enable = 0.f;
    if (m_lights_buffer[light_idx].w1) {
      fade = m_lights_buffer[light_idx].w2 / 128.f;
      fade_enable = 1.f;
    }
    math::Vector4f l0_dir_f(l0_dir.x(), l0_dir.y(), l0_dir.z(), fade);
    set_uniform(u.light_direction[0], l0_dir_f);
    math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), fade_enable);
    set_uniform(u.light_direction[1], l1_dir_f);
    set_uniform(u.light_direction[2], m_lights_buffer[light_idx].direction2);
    set_uniform(u.light_color[0], m_lights_buffer[light_idx].color0);
    set_uniform(u.light_color[1], m_lights_buffer[light_idx].color1);
    set_uniform(u.light_color[2], m_lights_buffer[light_idx].color2);
    set_uniform(u.light_ambient, m_lights_buffer[light_idx].ambient);
  };

  for (u32 di = 0; di < num_draws; di++) {
    auto& draw = draw_array[di];
    if (draw.flags & MOD_VTX) {
      glBindVertexArray(draw.mod_vtx_buffer.vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
      glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
      normal_vtx_buffer_bound = false;
    } else {
      if (!normal_vtx_buffer_bound) {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
        glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
        normal_vtx_buffer_bound = true;
      }
    }
    const bool perso = (draw.flags & CHARACTER) != 0;
    const bool objet = (draw.flags & OBJET) != 0;
    const bool tess = perso && tess_ok && draw.no_strip;
    {
      const ShaderId want = tess ? sid_tess : sid_base;
      if (want != sid_cur) {
        render_state->shaders[want].activate();
        U = tess ? (set_fade ? &m_emerc_tess_uniforms : &m_merc_tess_uniforms) : &uniforms;
        sid_cur = want;
        last_light = -1;
        last_perso = -1.f;
        last_famille = -1;
        last_reflet = -1;
        last_lave = -1;
        glUniform4f(U->fog_color, render_state->fog_color[0] / 255.f,
                    render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                    fog_on ? render_state->fog_intensity / 255 : 0);
      }
    }
    if (!set_fade) {
      const float pv = (perso || objet) ? 1.f : 0.f;
      if (pv != last_perso) {
        glUniform1f(U->remake_perso, pv);
        last_perso = pv;
      }
    }
    if ((perso || objet) && draw.famille != last_famille) {
      glUniform1i(U->remake_famille, draw.famille);
      last_famille = draw.famille;
    }
    if (!tess && draw.reflet != last_reflet) {
      glUniform1i(pc_uniform_loc(render_state->shaders[sid_base].id(), "reflet_orbe"), draw.reflet);
      last_reflet = draw.reflet;
    }
    if (!tess && !set_fade && draw.lave != last_lave) {
      glUniform1i(pc_uniform_loc(render_state->shaders[sid_base].id(), "lave_merc"), draw.lave);
      last_lave = draw.lave;
    }
    const GLenum prim = tess ? GL_PATCHES : (draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP);
    glUniform1i(U->ignore_alpha, draw.flags & DrawFlags::IGNORE_ALPHA);

    if (fog_on && !draw.mode.get_fog_enable()) {
      // on -> off
      glUniform4f(U->fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f, 0);
      fog_on = false;
    } else if (!fog_on && draw.mode.get_fog_enable()) {
      glUniform4f(U->fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                  render_state->fog_intensity / 255);
      fog_on = true;
    }
    bool use_mipmaps_for_filtering = true;
    // PC-REMAKE 17/10 : texture d'oeil = rendu 256 px agrandi a l'ecran ; le mode
    // sans filtrage de la PS2 la lisait au plus proche voisin (yeux pixelises).
    const bool eye_tex = (draw.texture & 0xffffff00) == 0xefffff00;
    if (draw.texture != last_tex) {
      if (draw.texture < (int)lev->textures.size() && draw.texture >= 0) {
        glBindTexture(GL_TEXTURE_2D, lev->textures.at(draw.texture));
      } else if ((draw.texture & 0xffffff00) == 0xefffff00) {
        if (render_state->version == GameVersion::Jak3 ||
            render_state->version == GameVersion::JakX) {
          auto maybe_eye =
              render_state->eye_renderer->lookup_eye_texture_hash(draw.hash, (draw.texture & 1));
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        } else {
          auto maybe_eye = render_state->eye_renderer->lookup_eye_texture(draw.texture & 0xff);
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        }

        use_mipmaps_for_filtering = false;
      } else if (draw.texture < 0) {
        int slot = -(draw.texture + 1);
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(slot));
      } else {
        fmt::print("Invalid draw.texture is {}, would have crashed.\n", draw.texture);
      }
      last_tex = draw.texture;
    }

    if ((int)draw.light_idx != last_light && !set_fade) {
      set_lights(*U, draw.light_idx);
      last_light = draw.light_idx;
    }

    glUniform1i(U->decal, draw.mode.get_decal());
    glUniform1i(U->gfx_hack_no_tex, (draw.flags & NO_TEXTURE) != 0);

    if (set_fade) {
      math::Vector4f fade =
          math::Vector4f(draw.fade[0], draw.fade[1], draw.fade[2], draw.fade[3]) / 255.f;
      set_uniform(U->fade, fade);
      ASSERT(draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST);
    }

    if (m_lights_buffer[draw.light_idx].w1 && !set_fade) {
      DrawMode mode = draw.mode;
      mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
      mode.set_ab(true);
      setup_opengl_from_draw_mode(mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      if (eye_tex) {
        pc_eye_texture_linear();
      }

      prof.add_draw_call(2);
      prof.add_tri(draw.num_triangles * 2);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      // draw rgb
      const auto& l1_dir = m_lights_buffer[draw.light_idx].direction1;
      math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), 1);
      set_uniform(U->light_direction[1], l1_dir_f);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
      glDrawElements(prim, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      qa_glerr_check("merc2 draw #2 (ligne 1573)");  // [QA-GLERR] 14/10
      // draw a
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      if (eye_tex) {
        pc_eye_texture_linear();
      }
      math::Vector4f l1_dir_f_off(l1_dir.x(), l1_dir.y(), l1_dir.z(), -1);
      set_uniform(U->light_direction[1], l1_dir_f_off);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
      glDrawElements(prim, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      qa_glerr_check("merc2 draw #3 (ligne 1580)");  // [QA-GLERR] 14/10
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    } else {
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      if (eye_tex) {
        pc_eye_texture_linear();
      }
      prof.add_draw_call();
      prof.add_tri(draw.num_triangles);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      glDrawElements(prim, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      qa_glerr_check("merc2 draw #4 (ligne 1590)");  // [QA-GLERR] 14/10

      // 21/10 : FOURRURE EN COUCHES au-dessus de ce dessin (meme maillage, meme pose)
      if (fur_ok && (draw.flags & FUR_SHELLS) && draw.no_strip) {
        const auto& fu = m_merc_fur_uniforms;
        render_state->shaders[ShaderId::MERC2_FUR].activate();
        set_lights(fu, draw.light_idx);
        glUniform4f(fu.fog_color, render_state->fog_color[0] / 255.f,
                    render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                    fog_on ? render_state->fog_intensity / 255 : 0);
        glUniform1i(fu.ignore_alpha, draw.flags & DrawFlags::IGNORE_ALPHA);
        glUniform1i(fu.decal, draw.mode.get_decal());
        glUniform1i(fu.gfx_hack_no_tex, (draw.flags & NO_TEXTURE) != 0);
        glUniform1f(fu.remake_perso, 1.f);
        glUniform1i(fu.remake_famille, draw.famille);
        // couches semi-transparentes : melange, sans ecrire la profondeur, l'alpha de l'image ni le
        // G-buffer de normales
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDepthMask(GL_FALSE);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUniform1i(fu.ignore_alpha, 1);
        prof.add_draw_call();
        glDrawElementsInstanced(GL_TRIANGLES, draw.index_count, GL_UNSIGNED_INT,
                                (void*)(sizeof(u32) * draw.first_index), 16);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
        render_state->shaders[sid_cur].activate();
      }
    }
  }

  if (!normal_vtx_buffer_bound) {
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
  }
}
