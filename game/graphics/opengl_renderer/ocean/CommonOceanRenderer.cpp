#include "CommonOceanRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/util/FileUtil.h"

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

constexpr int OCEAN_TEX_TBP_JAK1 = 8160;  // todo
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

// EAU DU JEU DE BASE
u64 g_water_murk_frame = ~0ull;  // image ou l'eau a pose sa brume (le post saute la sienne)
// cartes du rivage (OpenGLRenderer.cpp)
extern unsigned int g_rivage_tex[2];
extern float g_rivage_box[8];
extern int g_rivage_n;
// Jak dans l'eau (OpenGLRenderer.cpp / Merc2.cpp)
extern float g_jak_eau[2];
extern float g_jak_mouille[4];
namespace {
struct WaterDepthCopy {
  GLuint fbo = 0;
  GLuint tex = 0;
  int w = 0, h = 0;
  void ensure(int width, int height) {
    if (fbo && w == width && h == height) {
      return;
    }
    if (fbo) {
      glDeleteFramebuffers(1, &fbo);
      glDeleteTextures(1, &tex);
    }
    w = width;
    h = height;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // meme format que la profondeur du rendu
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
  }
};
WaterDepthCopy g_water_depth;

// copie la profondeur de la cible de rendu
bool water_depth_capture(SharedRenderState* render_state) {
  const int w = render_state->render_fb_w, h = render_state->render_fb_h;
  if (!g_world_from_clip_ok || w <= 0 || h <= 0) {
    return false;
  }
  GLint prev_draw = 0, prev_read = 0, prev_tex = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  if (scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  g_water_depth.ensure(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_draw);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_water_depth.fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  return true;
}
}  // namespace

// EAU VIVANTE (eau_vivante.vert)
float g_eau_temps = 0.f;  // temps de la houle (s), lu par pc_eau_houle (Jak flotte dessus)
float g_eau_dose[2] = {1.5f, 1.f};  // x = houle, y = ondes de la simulation
bool g_eau_active = false;  // surface vivante dessinee cette image (post : eclairage)
namespace {
constexpr int kEauN = 256;  // sommets par cote de la grille
constexpr float kEauRayon = 110.f;  // m
constexpr int kCoulN = 160;  // carte des couleurs : cases
constexpr float kCoulPas = 1.5f;  // m par case
struct EauVivante {
  Shader* prog = nullptr;
  GLuint vao = 0, vbo = 0, ibo = 0;
  GLsizei n_idx = 0;
  GLuint tex_coul = 0;
  std::vector<u8> coul;
  std::vector<float> monde;  // x, y, z (m) par sommet de la mer d'origine
  float coul_box[4] = {0, 0, 0, 0};
  bool pret = false;
  // simulation des ondes autour de Jak (eau_sim.frag)
  Shader* prog_sim = nullptr;
  GLuint sim_tex[2] = {0, 0}, sim_fbo[2] = {0, 0}, vao_vide = 0;
  int sim_cur = 0;
  float sim_x0 = 0.f, sim_z0 = 0.f;
  bool sim_ok = false, sim_vierge = true;
  // Jak a l'image precedente
  float jx = 0.f, jy = 0.f, jz = 0.f, jpieds = 0.f, jetat = 0.f;
  bool jvalide = false;
};
EauVivante g_eau;
constexpr int kSimN = 512;
constexpr float kSimCote = 51.2f;  // m (texel de 10 cm)

void eau_init() {
  if (g_eau.prog) {
    return;
  }
  g_eau.prog = new Shader("eau_vivante", GameVersion::Jak1, "", "ocean_common");
  std::vector<float> v;
  v.reserve(kEauN * kEauN * 2);
  for (int j = 0; j < kEauN; j++) {
    for (int i = 0; i < kEauN; i++) {
      v.push_back(-1.f + 2.f * i / (kEauN - 1));
      v.push_back(-1.f + 2.f * j / (kEauN - 1));
    }
  }
  std::vector<u32> idx;
  idx.reserve((kEauN - 1) * (kEauN - 1) * 6);
  for (int j = 0; j < kEauN - 1; j++) {
    for (int i = 0; i < kEauN - 1; i++) {
      u32 a = j * kEauN + i, b = a + 1, c = a + kEauN, d = c + 1;
      idx.insert(idx.end(), {a, c, b, b, c, d});
    }
  }
  g_eau.n_idx = (GLsizei)idx.size();
  glGenVertexArrays(1, &g_eau.vao);
  glBindVertexArray(g_eau.vao);
  glGenBuffers(1, &g_eau.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g_eau.vbo);
  glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glGenBuffers(1, &g_eau.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_eau.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(u32), idx.data(), GL_STATIC_DRAW);
  glBindVertexArray(0);
  g_eau.coul.assign(kCoulN * kCoulN * 4, 0);
  glGenTextures(1, &g_eau.tex_coul);
  glBindTexture(GL_TEXTURE_2D, g_eau.tex_coul);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCoulN, kCoulN, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               g_eau.coul.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // simulation
  g_eau.prog_sim = new Shader("eau_sim", GameVersion::Jak1);
  glGenVertexArrays(1, &g_eau.vao_vide);
  std::vector<float> zero(kSimN * kSimN * 2, 0.f);
  for (int i = 0; i < 2; i++) {
    glGenTextures(1, &g_eau.sim_tex[i]);
    glBindTexture(GL_TEXTURE_2D, g_eau.sim_tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, kSimN, kSimN, 0, GL_RG, GL_FLOAT, zero.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g_eau.sim_fbo[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_eau.sim_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eau.sim_tex[i],
                           0);
  }
  g_eau.sim_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// un pas de la simulation des ondes (une image)
void eau_sim_pas(const float* camp, float mer_m, float t) {
  (void)t;
  if (!g_eau.sim_ok || !g_eau.prog_sim || !g_eau.prog_sim->okay()) {
    return;
  }
  const float jx = g_grass_push[0] / 4096.f, jy = g_grass_push[1] / 4096.f,
              jz = g_grass_push[2] / 4096.f;
  const float etat = g_jak_eau[0];
  const float surf = etat > 0.5f ? g_jak_eau[1] / 4096.f : mer_m;
  const float pieds = g_jak_mouille[3] / 4096.f;
  const bool jak_pres = (jx != 0.f || jz != 0.f) && std::abs(jy - mer_m) < 12.f;
  const float cx = jak_pres ? jx : camp[0] / 4096.f, cz = jak_pres ? jz : camp[2] / 4096.f;
  const float pas = kSimCote / kSimN;
  const float x0 = std::floor((cx - kSimCote * 0.5f) / pas) * pas;
  const float z0 = std::floor((cz - kSimCote * 0.5f) / pas) * pas;
  float du = (x0 - g_eau.sim_x0) / kSimCote, dv = (z0 - g_eau.sim_z0) / kSimCote;
  if (g_eau.sim_vierge || std::abs(du) > 0.4f || std::abs(dv) > 0.4f) {
    du = dv = 2.f;  // tout est hors de l'ancienne grille : eau calme
    g_eau.sim_vierge = false;
  }
  g_eau.sim_x0 = x0;
  g_eau.sim_z0 = z0;

  // sources (uv, rayon uv, hauteur ajoutee en m)
  float src[16];
  int n = 0;
  auto ajouter = [&](float x, float z, float rayon, float force) {
    if (n >= 4) {
      return;
    }
    src[n * 4 + 0] = (x - x0) / kSimCote;
    src[n * 4 + 1] = (z - z0) / kSimCote;
    src[n * 4 + 2] = rayon / kSimCote;
    src[n * 4 + 3] = force;
    n++;
  };
  if (jak_pres) {
    float vx = 0.f, vz = 0.f, vy = 0.f;
    if (g_eau.jvalide) {
      vx = (jx - g_eau.jx) * 60.f;
      vz = (jz - g_eau.jz) * 60.f;
      vy = (jy - g_eau.jy) * 60.f;
    }
    const float vit = std::min(std::sqrt(vx * vx + vz * vz), 10.f);
    if (vit > 25.f) {
      // teleportation
    } else if (etat > 0.5f) {
      // il nage (ou patauge)
      const float k = etat > 1.5f ? 1.f : 0.5f;
      const float avance = std::min(vit, 6.f);
      ajouter(jx, jz, 0.75f, -(0.0022f * avance) * k);
      ajouter(jx, jz, 0.6f, -std::clamp(vy, -3.f, 3.f) * 0.0018f * k);
      // entree dans l'eau (chute, plongeon)
      const bool entre = g_eau.jvalide && (g_eau.jetat < 0.5f ||
                                           (g_eau.jpieds > surf + 0.25f && pieds <= surf + 0.25f));
      if (entre) {
        const float f = std::clamp(0.06f + 0.022f * std::max(-vy, 0.f), 0.06f, 0.3f);
        ajouter(jx, jz, 0.9f, -f);
        ajouter(jx, jz, 2.2f, f * 0.25f);
      }
    } else if (g_eau.jvalide && g_eau.jetat > 0.5f) {
      // il sort de l'eau (saut)
      ajouter(jx, jz, 0.6f, 0.04f);
    }
  }
  g_eau.jx = jx;
  g_eau.jy = jy;
  g_eau.jz = jz;
  g_eau.jpieds = pieds;
  g_eau.jetat = etat;
  g_eau.jvalide = jak_pres;

  GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_unit = 0, prev_tex = 0;
  GLint vp[4];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const int nxt = g_eau.sim_cur ^ 1;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_eau.sim_fbo[nxt]);
  glViewport(0, 0, kSimN, kSimN);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  g_eau.prog_sim->activate();
  const GLuint id = g_eau.prog_sim->id();
  glActiveTexture(GL_TEXTURE25);
  glBindTexture(GL_TEXTURE_2D, g_eau.sim_tex[g_eau.sim_cur]);
  glUniform1i(pc_uniform_loc(id, "sim_prec"), 25);
  glUniform4f(pc_uniform_loc(id, "sim_decal"), du, dv, 0.f, 0.f);
  glUniform4f(pc_uniform_loc(id, "sim_box"), x0, z0, kSimCote, 1.f / kSimN);
  // c^2 = 0,14 (texels)
  glUniform4f(pc_uniform_loc(id, "sim_param"), 0.14f, 0.993f, 1.f / 60.f, 0.12f);
  glUniform4fv(pc_uniform_loc(id, "sim_src"), 4, src);
  glUniform1i(pc_uniform_loc(id, "sim_n"), n);
  glUniform1i(pc_uniform_loc(id, "riv_tex0"), 20);
  glUniform1i(pc_uniform_loc(id, "riv_tex1"), 21);
  glUniform4fv(pc_uniform_loc(id, "riv_box"), 2, g_rivage_box);
  glUniform1i(pc_uniform_loc(id, "riv_n"), g_rivage_n);
  glBindVertexArray(g_eau.vao_vide);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  g_eau.sim_cur = nxt;
  glBindVertexArray(prev_vao);
  glUseProgram(prev_prog);
  glActiveTexture(prev_unit);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  if (blend) glEnable(GL_BLEND);
  if (depth) glEnable(GL_DEPTH_TEST);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);
  (void)prev_tex;
}
}  // namespace

// hauteur de la houle (m) au point (x, z en m), sans les ondes
float pc_eau_houle(float x, float z) {
  if (!g_eau.pret) {
    return 0.f;
  }
  static const float L[6] = {8.5f, 13.f, 19.f, 28.f, 41.f, 63.f};
  static const float DIR[6] = {0.95f, 0.35f, 0.70f, 0.15f, 0.55f, 0.42f};
  static const float AMP[6] = {0.035f, 0.05f, 0.07f, 0.09f, 0.11f, 0.13f};
  static const float PHI[6] = {0.f, 1.7f, 4.1f, 2.6f, 5.3f, 3.4f};
  float h = 0.f;
  for (int i = 0; i < 6; i++) {
    const float k = 6.2831853f / L[i];
    const float w = std::sqrt(9.81f * k);
    const float ph = k * (std::cos(DIR[i]) * x + std::sin(DIR[i]) * z) - w * g_eau_temps + PHI[i];
    h += AMP[i] * std::sin(ph);
  }
  return h * g_eau_dose[0];
}

CommonOceanRenderer::CommonOceanRenderer() {
  // rayon du proche étendu de 34 m à ~220 m (le trou de transition complet)
  m_vertices.resize(4096 * 160);
  for (auto& buf : m_indices) {
    buf.resize(4096 * 160);
  }

  // create OpenGL objects
  glGenBuffers(1, &m_ogl.vertex_buffer);
  glGenBuffers(NUM_BUCKETS, m_ogl.index_buffer);
  glGenVertexArrays(1, &m_ogl.vao);

  // set up the vertex array
  glBindVertexArray(m_ogl.vao);
  for (int i = 0; i < NUM_BUCKETS; i++) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[i]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices[i].size() * sizeof(u32), nullptr,
                 GL_STREAM_DRAW);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(Vertex), nullptr, GL_STREAM_DRAW);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,                            // location 0 in the shader
                        3,                            // 3 floats per vert
                        GL_FLOAT,                     // floats
                        GL_TRUE,                      // normalized, ignored,
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, xyz)  // offset in array
  );

  // rgba
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,                             // location 1 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // u8
                        GL_TRUE,                       // normalized (255 becomes 1)
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  //
  );

  // stq
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,                            // location 2 in the shader
                        3,                            // 2 floats per vert
                        GL_FLOAT,                     // floats
                        GL_FALSE,                     // normalized, ignored
                        sizeof(Vertex),               //
                        (void*)offsetof(Vertex, stq)  // offset in array
  );

  // byte data
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3,                            // location 3 in the shader
                         4,                            //
                         GL_UNSIGNED_BYTE,             // u8's
                         sizeof(Vertex),               //
                         (void*)offsetof(Vertex, fog)  // offset in array
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

CommonOceanRenderer::~CommonOceanRenderer() {
  glDeleteBuffers(1, &m_ogl.vertex_buffer);
  glDeleteBuffers(3, m_ogl.index_buffer);
  glDeleteVertexArrays(1, &m_ogl.vao);
}

void CommonOceanRenderer::init_for_near() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void CommonOceanRenderer::kick_from_near(const u8* data) {
  // QA : chaque xgkick du programme VU proche passe ici
  {
    static int qa_kicks = 0;
    if ((qa_kicks++ % 2000) == 0) {
      lg::warn("[QA] ocean kick_from_near invoque (n° {})", qa_kicks);
    }
  }
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    if (tag.nreg() == 3) {
      ASSERT(tag.pre());
      if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
        handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
      } else {
        handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
      }
      offset += 16 * 3 * tag.nloop();
    } else if (tag.nreg() == 1) {
      handle_near_adgif(data, offset, tag.nloop());
      offset += 16 * 1 * tag.nloop();
    } else {
      ASSERT(false);
    }

    eop = tag.eop();
  }
}

void CommonOceanRenderer::handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop) {
  m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
  bool reset_last = false;
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;
    bool adc = upper & (1ull << 47);
    if (!adc) {
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = false;
    } else {
      if (reset_last) {
        m_next_free_index[m_current_bucket] -= 3;
      }
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
      m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
      reset_last = true;
    }
  }
}

void CommonOceanRenderer::handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop) {
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  // :regs0 (gif-reg-id st) :regs1 (gif-reg-id rgbaq) :regs2 (gif-reg-id xyzf2)
  for (u32 i = 0; i < loop; i++) {
    auto& dest_vert = m_vertices[m_next_free_vertex++];

    // stq
    memcpy(dest_vert.stq.data(), data + offset, 12);
    offset += 16;

    // rgba
    dest_vert.rgba[0] = data[offset];
    dest_vert.rgba[1] = data[offset + 4];
    dest_vert.rgba[2] = data[offset + 8];
    dest_vert.rgba[3] = data[offset + 12];
    offset += 16;

    // xyz
    u32 x = 0, y = 0;
    memcpy(&x, data + offset, 4);
    memcpy(&y, data + offset + 4, 4);

    u64 upper;
    memcpy(&upper, data + offset + 8, 8);
    u32 z = (upper >> 4) & 0xffffff;
    offset += 16;

    dest_vert.xyz[0] = (float)(x << 16) / (float)UINT32_MAX;
    dest_vert.xyz[1] = (float)(y << 16) / (float)UINT32_MAX;
    dest_vert.xyz[2] = (float)(z << 8) / (float)UINT32_MAX;

    u8 f = (upper >> 36);
    dest_vert.fog = f;

    auto vidx = m_next_free_vertex - 1;

    if (ind_of_fan_start == UINT32_MAX) {
      ind_of_fan_start = vidx;
    } else {
      if (fan_running) {
        // hack to draw fans with strips. this isn't efficient, but fans happen extremely rarely
        // (you basically have to put the camera intersecting the ocean and looking fwd)
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = UINT32_MAX;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = vidx - 1;
        m_indices[m_current_bucket][m_next_free_index[m_current_bucket]++] = ind_of_fan_start;
      } else {
        fan_running = true;
      }
    }
  }
}

void CommonOceanRenderer::handle_near_adgif(const u8* data, u32 offset, u32 count) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < count; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
        if (s) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        if (!reg.tcc()) {
          m_current_bucket = VertexBucket::RGB_TEXTURE;
        }
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
        GsAlpha reg(value);
        if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
            reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
            reg.c_mode() == GsAlpha::BlendMode::DEST && reg.d_mode() == GsAlpha::BlendMode::DEST) {
          m_current_bucket = VertexBucket::ENV_MAP;
        }
      } break;

      case GsRegisterAddress::FRAME_1: {
        u32 mask = value >> 32;
        if (mask) {
          m_current_bucket = VertexBucket::ALPHA;
        }
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (m_current_bucket == VertexBucket::ENV_MAP) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void CommonOceanRenderer::flush_near(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  // diagnostic de la zone plate autour du joueur
  {
    static int qa_near = 0;
    static u32 qa_max_verts = 0;
    qa_max_verts = std::max(qa_max_verts, m_next_free_vertex);
    if ((qa_near++ % 300) == 0) {
      lg::warn("[QA] ocean flush_near tourne (max {} sommets sur l'intervalle)", qa_max_verts);
      qa_max_verts = 0;
    }
  }
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  render_state->shaders[ShaderId::OCEAN_COMMON].activate();
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
  // "Spyro water": animated sea (time + strength)
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "water_fx"),
              (float)(render_state->frame_idx % 100000) / 60.f, 1.f, 0.f, 0.f);

  glDepthMask(GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glDepthFunc(GL_GEQUAL);

  for (int bucket = 0; bucket < 3; bucket++) {
    switch (bucket) {
      case 0: {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 0);
        // même filtrage que le large (le proche couvre ~220 m maintenant)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
        glUniform1f(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "alpha_mult"),
            1.f);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 1);
        break;
      case 2:
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 2);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[bucket] * sizeof(u32),
                 m_indices[bucket].data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[bucket], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[bucket]);
  }
}

void reverse_indices(u32* indices, u32 count);  // définie plus bas dans ce fichier

void CommonOceanRenderer::capture_mid_textures(SharedRenderState* render_state) {
  auto tbp = render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
  m_mid_ocean_tex = render_state->texture_pool->lookup(tbp);
  m_mid_envmap_tex = render_state->texture_pool->lookup(m_envmap_tex);
}

// EAU TRANSPARENTE
void CommonOceanRenderer::flush_mid_transparent(SharedRenderState* render_state,
                                                ScopedProfilerNode& prof) {
  float eau_deep[3] = {0.30f, 0.62f, 0.66f};  // couleur de la pleine mer (eau vivante)
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  render_state->shaders[ShaderId::OCEAN_COMMON].activate();
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "water_fx"),
              (float)(render_state->frame_idx % 100000) / 60.f, 1.f, 0.f, 0.f);
  // brume de l'eau posee par la surface (voir g_water_murk_frame)
  {
    const auto id = render_state->shaders[ShaderId::OCEAN_COMMON].id();
    const bool murk = water_depth_capture(render_state);
    if (murk) {
      glActiveTexture(GL_TEXTURE19);
      glBindTexture(GL_TEXTURE_2D, g_water_depth.tex);
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(pc_uniform_loc(id, "water_depth"), 19);
      glUniformMatrix4fv(pc_uniform_loc(id, "water_inv_camera"), 1, GL_FALSE, g_world_from_clip);
      glUniform4f(pc_uniform_loc(id, "water_cam"), render_state->camera_pos[0],
                  render_state->camera_pos[1], render_state->camera_pos[2], 0.f);
      g_water_murk_frame = render_state->frame_idx;
    }
    // y = densite de la brume par metre d'eau traverse
    glUniform4f(pc_uniform_loc(id, "water_murk"), murk ? 1.f : 0.f, 0.2f,
                (float)render_state->render_fb_w, (float)render_state->render_fb_h);
    // cartes du rivage -> unites 20 et 21
    for (int i = 0; i < 2; i++) {
      glActiveTexture(GL_TEXTURE20 + i);
      glBindTexture(GL_TEXTURE_2D, i < g_rivage_n ? g_rivage_tex[i] : 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(pc_uniform_loc(id, "riv_tex0"), 20);
    glUniform1i(pc_uniform_loc(id, "riv_tex1"), 21);
    glUniform4fv(pc_uniform_loc(id, "riv_box"), 2, g_rivage_box);
    glUniform1i(pc_uniform_loc(id, "riv_n"), murk ? g_rivage_n : 0);
    // Couleur de la PLEINE MER des artistes (sommets du large, alpha plein)
    {
      static float deep[3] = {0.30f, 0.62f, 0.66f};
      double acc[3] = {0, 0, 0};
      int n = 0;
      const u32 count = m_next_free_index[0];
      for (u32 i = 0; i < count; i += 3) {
        const u32 vi = m_indices[0][i];
        if (vi == UINT32_MAX || vi >= m_next_free_vertex) {
          continue;
        }
        const auto& v = m_vertices[vi];
        if (v.rgba[3] < 100) {
          continue;  // cellules cotieres (transparentes) : couleur sans valeur
        }
        for (int c = 0; c < 3; c++) {
          acc[c] += v.rgba[c] * (2.0 / 255.0);
        }
        n++;
      }
      if (n > 16) {
        for (int c = 0; c < 3; c++) {
          deep[c] = (float)(acc[c] / n);
        }
      }
      glUniform4f(pc_uniform_loc(id, "water_deep"), deep[0], deep[1], deep[2], 0.f);
      for (int c = 0; c < 3; c++) {
        eau_deep[c] = deep[c];
      }
    }
  }

  // EAU VIVANTE (voir g_eau)
  bool vive = false;
  const float* camp = render_state->camera_pos.data();
  const float mer_m = g_water_glitter[0] / 4096.f;
  {
    // meme horloge que le post
    g_eau_temps = (float)(render_state->frame_idx % 216000) / 60.f;
    const float cam_y = camp[1] / 4096.f;
    vive = g_world_from_clip_ok &&
           g_water_murk_frame == render_state->frame_idx && g_rivage_n > 0 &&
           (g_water_glitter[1] > 0.001f || g_water_glitter[2] > 0.001f) && cam_y > mer_m + 0.15f &&
           cam_y < mer_m + 80.f;
    if (vive) {
      eau_init();
      vive = g_eau.prog->okay();
    }
    g_eau.pret = vive;
    g_eau_active = vive;
  }
  if (vive) {
    const float cx = camp[0] / 4096.f, cz = camp[2] / 4096.f;
    const float x0 = std::floor(cx / kCoulPas) * kCoulPas - kCoulN * kCoulPas * 0.5f;
    const float z0 = std::floor(cz / kCoulPas) * kCoulPas - kCoulN * kCoulPas * 0.5f;
    g_eau.coul_box[0] = x0;
    g_eau.coul_box[1] = z0;
    g_eau.coul_box[2] = 1.f / (kCoulN * kCoulPas);
    g_eau.coul_box[3] = 1.f / (kCoulN * kCoulPas);
    // positions monde des sommets (m)
    g_eau.monde.resize(m_next_free_vertex * 3);
    const float* Mi = g_world_from_clip;
    for (u32 vi = 0; vi < m_next_free_vertex; vi++) {
      const auto& v = m_vertices[vi];
      const float nx = (v.xyz[0] - 0.5f) * 16.f;
      const float ny = -(v.xyz[1] - 0.5f) * 32.f * (512.f / 448.f);
      const float nz = v.xyz[2] * 2.f - 1.f;
      float w4[4];
      for (int r = 0; r < 4; r++) {
        w4[r] = Mi[r] * nx + Mi[4 + r] * ny + Mi[8 + r] * nz + Mi[12 + r];
      }
      float* o = &g_eau.monde[vi * 3];
      if (std::abs(w4[3]) < 1e-12f) {
        o[0] = o[1] = o[2] = std::numeric_limits<float>::quiet_NaN();
        continue;
      }
      o[0] = (w4[0] / w4[3] + camp[0]) / 4096.f;
      o[1] = (w4[1] / w4[3] + camp[1]) / 4096.f;
      o[2] = (w4[2] / w4[3] + camp[2]) / 4096.f;
      if (std::abs(o[1] - mer_m) > 1.5f) {
        o[0] = std::numeric_limits<float>::quiet_NaN();
      }
    }
    // fond : la pleine mer (couleur moyenne), alpha plein
    for (int i = 0; i < kCoulN * kCoulN; i++) {
      u8* c = &g_eau.coul[i * 4];
      for (int k = 0; k < 3; k++) {
        c[k] = (u8)std::clamp(eau_deep[k] * 255.f / 2.f, 0.f, 255.f);
      }
      c[3] = 128;
    }
    // triangles des bandes de la surface (seau 0)
    const u32 count = m_next_free_index[0];
    for (u32 k = 0; k + 2 < count; k++) {
      const u32 ia = m_indices[0][k], ib = m_indices[0][k + 1], ic = m_indices[0][k + 2];
      if (ia == UINT32_MAX || ib == UINT32_MAX || ic == UINT32_MAX || ia >= m_next_free_vertex ||
          ib >= m_next_free_vertex || ic >= m_next_free_vertex) {
        continue;
      }
      const float* pa = &g_eau.monde[ia * 3];
      const float* pb = &g_eau.monde[ib * 3];
      const float* pc = &g_eau.monde[ic * 3];
      if (std::isnan(pa[0]) || std::isnan(pb[0]) || std::isnan(pc[0])) {
        continue;
      }
      // en cases de la carte
      const float ax = (pa[0] - x0) / kCoulPas, az = (pa[2] - z0) / kCoulPas;
      const float bx = (pb[0] - x0) / kCoulPas, bz = (pb[2] - z0) / kCoulPas;
      const float cxx = (pc[0] - x0) / kCoulPas, czz = (pc[2] - z0) / kCoulPas;
      const float det = (bx - ax) * (czz - az) - (cxx - ax) * (bz - az);
      if (std::abs(det) < 1e-6f) {
        continue;
      }
      const int i0 = std::max(0, (int)std::floor(std::min({ax, bx, cxx})));
      const int i1 = std::min(kCoulN - 1, (int)std::ceil(std::max({ax, bx, cxx})));
      const int j0 = std::max(0, (int)std::floor(std::min({az, bz, czz})));
      const int j1 = std::min(kCoulN - 1, (int)std::ceil(std::max({az, bz, czz})));
      if (i0 > i1 || j0 > j1) {
        continue;
      }
      const auto& va = m_vertices[ia];
      const auto& vb = m_vertices[ib];
      const auto& vc = m_vertices[ic];
      for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
          const float px = i + 0.5f, pz = j + 0.5f;
          const float u = ((px - ax) * (czz - az) - (cxx - ax) * (pz - az)) / det;
          const float w = ((bx - ax) * (pz - az) - (px - ax) * (bz - az)) / det;
          if (u < -0.02f || w < -0.02f || u + w > 1.02f) {
            continue;
          }
          u8* c = &g_eau.coul[(j * kCoulN + i) * 4];
          for (int q = 0; q < 4; q++) {
            const float val = va.rgba[q] * (1.f - u - w) + vb.rgba[q] * u + vc.rgba[q] * w;
            c[q] = (u8)std::clamp(val + 0.5f, 0.f, 255.f);
          }
        }
      }
    }
    glBindTexture(GL_TEXTURE_2D, g_eau.tex_coul);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCoulN, kCoulN, GL_RGBA, GL_UNSIGNED_BYTE,
                    g_eau.coul.data());
    // les cartes du rivage sont liees aux unites 20 et 21 plus haut
    eau_sim_pas(camp, mer_m, g_eau_temps);
  }
  glUniform4f(pc_uniform_loc(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "eau_vivante"),
              vive ? 1.f : 0.f, kEauRayon * 0.92f, kEauRayon * 0.25f, 0.f);
  // dessin de la surface vivante (memes uniformes que la mer d'origine), puis retour a celle-ci
  auto dessiner_eau = [&](int seau) {
    const GLuint id = g_eau.prog->id();
    g_eau.prog->activate();
    glUniform4f(pc_uniform_loc(id, "fog_color"), render_state->fog_color[0] / 255.f,
                render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                render_state->fog_intensity / 255);
    glUniform4f(pc_uniform_loc(id, "water_fx"), (float)(render_state->frame_idx % 100000) / 60.f,
                1.f, 0.f, 0.f);
    glUniform1i(pc_uniform_loc(id, "water_depth"), 19);
    glUniformMatrix4fv(pc_uniform_loc(id, "water_inv_camera"), 1, GL_FALSE, g_world_from_clip);
    glUniform4f(pc_uniform_loc(id, "water_cam"), camp[0], camp[1], camp[2], 0.f);
    glUniform4f(pc_uniform_loc(id, "water_murk"), 1.f, 0.2f, (float)render_state->render_fb_w,
                (float)render_state->render_fb_h);
    glUniform1i(pc_uniform_loc(id, "riv_tex0"), 20);
    glUniform1i(pc_uniform_loc(id, "riv_tex1"), 21);
    glUniform4fv(pc_uniform_loc(id, "riv_box"), 2, g_rivage_box);
    glUniform1i(pc_uniform_loc(id, "riv_n"), g_rivage_n);
    glUniform4f(pc_uniform_loc(id, "water_deep"), eau_deep[0], eau_deep[1], eau_deep[2], 0.f);
    glUniform1i(pc_uniform_loc(id, "tex_T0"), 0);
    glUniform1i(pc_uniform_loc(id, "bucket"), seau);
    glUniform4f(pc_uniform_loc(id, "eau_vivante"), 2.f, kEauRayon * 0.92f, kEauRayon * 0.25f, 0.f);
    glUniformMatrix4fv(pc_uniform_loc(id, "clip_from_world"), 1, GL_FALSE, g_clip_from_world);
    glUniform4f(pc_uniform_loc(id, "eau_grille"), camp[0] / 4096.f, mer_m, camp[2] / 4096.f,
                kEauRayon);
    // w = vagues de rivage en relief
    glUniform4f(pc_uniform_loc(id, "eau_houle"), g_eau_temps, g_eau_dose[0], g_eau_dose[1], 0.f);
    glActiveTexture(GL_TEXTURE23);
    glBindTexture(GL_TEXTURE_2D, g_eau.tex_coul);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(pc_uniform_loc(id, "eau_couleurs"), 23);
    glUniform4fv(pc_uniform_loc(id, "eau_couleurs_box"), 1, g_eau.coul_box);
    glActiveTexture(GL_TEXTURE24);
    glBindTexture(GL_TEXTURE_2D, g_eau.sim_tex[g_eau.sim_cur]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(pc_uniform_loc(id, "eau_sim"), 24);
    glUniform4f(pc_uniform_loc(id, "eau_sim_box"), g_eau.sim_x0, g_eau.sim_z0, 1.f / kSimCote,
                g_eau.sim_ok ? 1.f : 0.f);
    glBindVertexArray(g_eau.vao);
    glDrawElements(GL_TRIANGLES, g_eau.n_idx, GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(g_eau.n_idx / 3);
    render_state->shaders[ShaderId::OCEAN_COMMON].activate();
    glBindVertexArray(m_ogl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  };

  glDepthMask(GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);

  // même astuce que flush_mid
  reverse_indices(m_indices[1].data(), m_next_free_index[1]);

  // --- passe A
  {
    auto tbp = render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
    // la texture retenue au bucket du large (voir capture_mid_textures)
    auto tex = m_mid_ocean_tex ? m_mid_ocean_tex : render_state->texture_pool->lookup(tbp);
    if (!tex) {
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "tex_T0"),
                0);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"),
                3);
    // avec la brume de l'eau, melange a DEUX SOURCES
    if (g_water_murk_frame == render_state->frame_idx) {
      glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
    } else {
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    }
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[0]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[0] * sizeof(u32), m_indices[0].data(),
                 GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[0], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[0]);
    // la surface vivante, meme melange
    if (vive) {
      dessiner_eau(3);
      glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"),
                  3);
    }

    // --- passe B
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"),
                5);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[0], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[0]);
    if (vive) {
      dessiner_eau(5);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
  }

  // --- passe C
  {
    // idem, retenue au bucket du large
    auto tex = m_mid_envmap_tex ? m_mid_envmap_tex : render_state->texture_pool->lookup(m_envmap_tex);
    if (!tex) {
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"),
                4);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[1] * sizeof(u32), m_indices[1].data(),
                 GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[1], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[1]);
  }
}

void CommonOceanRenderer::kick_from_mid(const u8* data) {
  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    // unpack registers.
    // faster to do it once outside of the nloop loop.
    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.nreg() == 1) {
        ASSERT(!tag.pre());
        ASSERT(tag.nloop() == 5);
        handle_mid_adgif(data, offset);
        offset += 5 * 16;
      } else {
        ASSERT(tag.nreg() == 3);
        ASSERT(tag.pre());
        m_current_bucket = GsPrim(tag.prim()).abe() ? 1 : 0;

        int count = tag.nloop();
        if (GsPrim(tag.prim()).kind() == GsPrim::Kind::TRI_STRIP) {
          handle_near_vertex_gif_data_strip(data, offset, tag.nloop());
        } else {
          handle_near_vertex_gif_data_fan(data, offset, tag.nloop());
        }
        offset += 3 * 16 * count;
        // todo handle.
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}

void CommonOceanRenderer::handle_mid_adgif(const u8* data, u32 offset) {
  u32 most_recent_tbp = 0;

  for (u32 i = 0; i < 5; i++) {
    u64 value;
    GsRegisterAddress addr;
    memcpy(&value, data + offset + 16 * i, sizeof(u64));
    memcpy(&addr, data + offset + 16 * i + 8, sizeof(GsRegisterAddress));
    switch (addr) {
      case GsRegisterAddress::MIPTBP1_1:
      case GsRegisterAddress::MIPTBP2_1:
        // ignore this, it's just mipmapping settings
        break;
      case GsRegisterAddress::TEX1_1: {
        GsTex1 reg(value);
        ASSERT(reg.mmag());
      } break;
      case GsRegisterAddress::CLAMP_1: {
        bool s = value & 0b001;
        bool t = value & 0b100;
        ASSERT(s == t);
      } break;
      case GsRegisterAddress::TEX0_1: {
        GsTex0 reg(value);
        ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
        most_recent_tbp = reg.tbp0();
      } break;
      case GsRegisterAddress::ALPHA_1: {
      } break;

      default:
        lg::debug("reg: {}", register_address_name(addr));
        break;
    }
  }

  if (most_recent_tbp != OCEAN_TEX_TBP_JAK2) {
    m_envmap_tex = most_recent_tbp;
  }

  if (m_vertices.size() - 128 < m_next_free_vertex) {
    ASSERT(false);  // add more vertices.
  }
}

void CommonOceanRenderer::init_for_mid() {
  m_next_free_vertex = 0;
  for (auto& x : m_next_free_index) {
    x = 0;
  }
}

void reverse_indices(u32* indices, u32 count) {
  if (count) {
    for (u32 a = 0, b = count - 1; a < b; a++, b--) {
      std::swap(indices[a], indices[b]);
    }
  }
}

void CommonOceanRenderer::flush_mid(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  {
    static int qa_mid = 0;
    if ((qa_mid++ % 300) == 0) {
      lg::warn("[QA] ocean flush_mid tourne ({} sommets)", m_next_free_vertex);
    }
  }
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBufferData(GL_ARRAY_BUFFER, m_next_free_vertex * sizeof(Vertex), m_vertices.data(),
               GL_STREAM_DRAW);
  render_state->shaders[ShaderId::OCEAN_COMMON].activate();
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "fog_color"),
              render_state->fog_color[0] / 255.f, render_state->fog_color[1] / 255.f,
              render_state->fog_color[2] / 255.f, render_state->fog_intensity / 255);
  // "Spyro water": animated sea (time + strength)
  glUniform4f(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "water_fx"),
              (float)(render_state->frame_idx % 100000) / 60.f, 1.f, 0.f, 0.f);

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  glDisable(GL_BLEND);

  // note:
  // there are some places where the game draws the same section of ocean twice, in this order:
  // - low poly mesh with ocean texture
  // - low poly mesh with envmap texture
  // - high poly mesh with ocean texture (overwrites previous draw)
  // - high poly mesh with envmap texture (overwrites previous draw)

  // we draw all ocean textures together and all envmap textures togther. luckily, there's a trick
  // we can use to get the same result.
  // first, we'll draw all ocean textures. The high poly mesh is drawn second, so it wins.
  // then, we'll draw all envmaps, but with two changes:
  // - first, we draw it in reverse, so the high poly versions are drawn first
  // - second, we'll modify the shader to set alpha = 0 of the destination. when the low poly
  //    version is drawn on top, it won't draw at all because of the blending mode
  //    (s_factor = DST_ALPHA, d_factor = 1)

  // draw it in reverse
  reverse_indices(m_indices[1].data(), m_next_free_index[1]);

  for (int bucket = 0; bucket < 2; bucket++) {
    switch (bucket) {
      case 0: {
        auto tbp =
            render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
        auto tex = render_state->texture_pool->lookup(tbp);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "tex_T0"), 0);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 3);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }

      break;
      case 1:
        glEnable(GL_BLEND);
        auto tex = render_state->texture_pool->lookup(m_envmap_tex);
        if (!tex) {
          tex = render_state->texture_pool->get_placeholder_texture();
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, *tex);

        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1i(
            glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"), 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        break;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer[bucket]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_free_index[bucket] * sizeof(u32),
                 m_indices[bucket].data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[bucket], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[bucket]);
  }
}
