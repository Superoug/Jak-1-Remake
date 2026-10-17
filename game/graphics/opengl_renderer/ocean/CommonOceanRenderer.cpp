#include "CommonOceanRenderer.h"

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

constexpr int OCEAN_TEX_TBP_JAK1 = 8160;  // todo
constexpr int OCEAN_TEX_TBP_JAK2 = 672;

// PC-REMAKE 17/10 : EAU DU JEU DE BASE. La brume sous-marine etait posee par le post sur
// TOUTE l'eau, surface comprise : un aplat bleu-vert qui effacait les vagues et la couleur
// des artistes (qui suit l'heure : cyan le jour, bleu profond la nuit). Elle est maintenant
// calculee au dessin de la surface (mid transparent, bucket shader 3) : seule la part vue A
// TRAVERS l'eau (le fond) est voilee, selon l'epaisseur d'eau traversee, lue dans une copie
// de la profondeur de la scene prise juste avant l'eau. La surface garde sa couleur, ses
// vagues et son reflet d'origine ; les reflets du remake (post) restent par-dessus.
u64 g_water_murk_frame = ~0ull;  // image ou l'eau a pose sa brume (le post saute la sienne)
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
    // meme format que la profondeur du rendu (sinon la copie echoue sans bruit)
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

// copie la profondeur de la cible de rendu (sans l'eau, qui n'ecrit pas la profondeur)
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

CommonOceanRenderer::CommonOceanRenderer() {
  // PC-REMAKE : rayon du proche étendu de 34 m à ~220 m (le trou de transition
  // complet) — l'ancien 4096*10 débordait dès le village (ASSERT « add more
  // vertices »). ×16 : ~10 Mo, sans enjeu sur M1 Pro.
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
  // PC-REMAKE QA : chaque xgkick du programme VU proche passe ici
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
  // diagnostic de la zone plate autour du joueur : le rendu
  // PROCHE tourne-t-il seulement ? — max de sommets vu sur l'intervalle
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
  // PC-REMAKE "Spyro water": animated sea (time + strength)
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
        // PC-REMAKE : même filtrage que le large (le proche couvre ~220 m
        // maintenant) — GL_LINEAR seul aplatissait la texture au loin
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

// PC-REMAKE EAU TRANSPARENTE (25/08) : le mid dessiné APRÈS Jak, en vraie
// transparence. Trois passes sur les mêmes sommets :
//   A. surface (bucket shader 3) : blend alpha classique, RGB seulement —
//      l'alpha du shader est une VRAIE transparence (rampe rivage × distance) ;
//   B. dépôt d'alpha (bucket shader 5) : écrit UNIQUEMENT l'alpha destination
//      avec la formule d'origine PS2 (motif alphamod) pour doser le reflet —
//      le même rôle que la passe « alpha » du near des artistes ;
//   C. reflet envmap additif (bucket shader 4) : inchangé (DST_ALPHA, ONE).
// Jamais d'écriture de profondeur ; GEQUAL comme le near — un rocher devant
// masque l'eau, Jak immergé reçoit le voile.
void CommonOceanRenderer::flush_mid_transparent(SharedRenderState* render_state,
                                                ScopedProfilerNode& prof) {
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
  // 17/10 : brume de l'eau posee par la surface (voir g_water_murk_frame)
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
    // y = densite de la brume par metre d'eau traverse. 17/10 : 0.5 -> 0.2 — les rochers
    // sous 1 a 3 m d'eau n'etaient plus qu'un aplat cyan sans texture
    glUniform4f(pc_uniform_loc(id, "water_murk"), murk ? 1.f : 0.f, 0.2f,
                (float)render_state->render_fb_w, (float)render_state->render_fb_h);
    // Couleur de la PLEINE MER des artistes (sommets du large, alpha plein) : pres des
    // cotes le large est transparent (alpha 0, couleur noire — le near prenait le relais
    // sur PS2) ; l'eau profonde y prend cette couleur au lieu du noir local. Moyenne des
    // sommets de la surface de cette image, a l'echelle de fragment_color (bucket 3 : x2).
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
    }
  }

  glDepthMask(GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);

  // même astuce que flush_mid : envmap en ordre inverse + alpha destination 0
  reverse_indices(m_indices[1].data(), m_next_free_index[1]);

  // ---- passe A : surface transparente (RGB), alpha du framebuffer préservé
  {
    auto tbp = render_state->version == GameVersion::Jak1 ? OCEAN_TEX_TBP_JAK1 : OCEAN_TEX_TBP_JAK2;
    // 17/10 : la texture retenue au bucket du large (voir capture_mid_textures)
    // (original : auto tex = render_state->texture_pool->lookup(tbp);)
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
    // 17/10 : avec la brume de l'eau, melange a DEUX SOURCES : final = couleur + fond *
    // transmission (ocean_common.frag, color_t) — l'eau filtre le fond au lieu de le
    // recouvrir. Sans brume : blend alpha d'origine.
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

    // ---- passe B : dépôt d'alpha (dose du reflet), mêmes sommets
    glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_COMMON].id(), "bucket"),
                5);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glDisable(GL_BLEND);
    glDrawElements(GL_TRIANGLE_STRIP, m_next_free_index[0], GL_UNSIGNED_INT, nullptr);
    prof.add_draw_call();
    prof.add_tri(m_next_free_index[0]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
  }

  // ---- passe C : reflet envmap additif, identique à flush_mid
  {
    // 17/10 : idem, retenue au bucket du large
    // (original : auto tex = render_state->texture_pool->lookup(m_envmap_tex);)
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
  // PC-REMAKE "Spyro water": animated sea (time + strength)
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
