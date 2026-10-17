#include <algorithm>
#include <cstdlib>
#include <unordered_map>

#include "OpenGLRenderer.h"

#include <cmath>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/BlitDisplays.h"
#include "game/graphics/opengl_renderer/DepthCue.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/ProgressRenderer.h"
#include "game/graphics/opengl_renderer/ShadowRenderer.h"
#include "game/graphics/opengl_renderer/SkyRenderer.h"
#include "game/graphics/opengl_renderer/TextureUploadHandler.h"
#include "game/graphics/opengl_renderer/VisDataHandler.h"
#include "game/graphics/opengl_renderer/Warp.h"
#include "game/graphics/opengl_renderer/background/Hfrag.h"
#include "game/graphics/opengl_renderer/background/Shrub.h"
#include "game/graphics/opengl_renderer/background/TFragment.h"
#include "game/graphics/opengl_renderer/background/Tie3.h"
#include "game/graphics/opengl_renderer/foreground/Generic2.h"
#include "game/graphics/opengl_renderer/foreground/Generic2BucketRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Merc2BucketRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Shadow2.h"
#include "game/graphics/opengl_renderer/ocean/OceanMidAndFar.h"
#include "game/graphics/opengl_renderer/ocean/OceanNear.h"
#include "game/graphics/opengl_renderer/sprite/Sprite3.h"
#include "game/graphics/pipelines/opengl.h"

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_stdlib.h"

// for the vif callback
#include "game/kernel/common/kmachine.h"
#include "game/runtime.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include "common/util/string_util.h"

namespace {
std::string g_current_renderer;
}

/*!
 * OpenGL Error callback. If we do something invalid, this will be called.
 */
void GLAPIENTRY opengl_error_callback(GLenum source,
                                      GLenum type,
                                      GLuint id,
                                      GLenum severity,
                                      GLsizei /*length*/,
                                      const GLchar* message,
                                      const void* /*userParam*/) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
    lg::debug("[{}] OpenGL notification 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source,
              type, message);
  } else if (severity == GL_DEBUG_SEVERITY_LOW) {
    lg::info("[{}] OpenGL message 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
             message);
  } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
    lg::warn("[{}] OpenGL warn 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
             message);
  } else if (severity == GL_DEBUG_SEVERITY_HIGH) {
    lg::error("[{}] OpenGL error 0x{:X} S{:X} T{:X}: {}", g_current_renderer, id, source, type,
              message);
    // ASSERT(false);
  }
}

OpenGLRenderer::OpenGLRenderer(std::shared_ptr<TexturePool> texture_pool,
                               std::shared_ptr<Loader> loader,
                               GameVersion version)
    : m_render_state(texture_pool, loader, version),
      m_collide_renderer(version),
      m_version(version) {
  // requires OpenGL 4.3
#ifndef __APPLE__
  // setup OpenGL errors
  glEnable(GL_DEBUG_OUTPUT);
  glDebugMessageCallback(opengl_error_callback, nullptr);
  // disable specific errors
  const GLuint gl_error_ignores_api_other[1] = {0x20071};  // some annoying nvidia message
  glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 1,
                        &gl_error_ignores_api_other[0], GL_FALSE);
#endif

  lg::info("OpenGL context version: {}", (const char*)glGetString(GL_VERSION));
  lg::info("OpenGL context renderer: {}", (const char*)glGetString(GL_RENDERER));
  lg::info("OpenGL context vendor: {}", (const char*)glGetString(GL_VENDOR));
  lg::info("OpenGL context shading language version: {}",
           (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

  // set up screen draw

  glGenVertexArrays(1, &screen_vao);
  glGenBuffers(1, &screen_vbo);

  struct Vertex {
    float x, y;
  };
  constexpr std::array<Vertex, 4> vertices = {
      Vertex{-1, -1},
      Vertex{-1, 1},
      Vertex{1, -1},
      Vertex{1, 1},
  };

  glBindVertexArray(screen_vao);
  glBindBuffer(GL_ARRAY_BUFFER, screen_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, vertices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,               // location 0 in the shader
                        2,               // 2 floats per vert
                        GL_FLOAT,        // floats
                        GL_TRUE,         // normalized, ignored,
                        sizeof(Vertex),  //
                        nullptr          //
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  // end set up screen draw

  const tfrag3::Level* common_level = nullptr;
  {
    auto p = scoped_prof("load-common");
    common_level = &m_render_state.loader->load_common(*m_render_state.texture_pool, "GAME");
  }

  // initialize all renderers
  switch (m_version) {
    case GameVersion::Jak1:
      break;
    case GameVersion::Jak2:
    case GameVersion::Jak3:
    case GameVersion::JakX:
      m_texture_animator =
          std::make_shared<TextureAnimator>(m_render_state.shaders, common_level, m_version);
      break;
    default:
      ASSERT(false);
  }

  m_merc2 = std::make_shared<Merc2>(m_render_state.shaders, anim_slot_array());
  m_generic2 = std::make_shared<Generic2>(m_render_state.shaders);

  // initialize all renderers
  auto p = scoped_prof("init-bucket-renderers");
  switch (m_version) {
    case GameVersion::Jak1:
      init_bucket_renderers_jak1();
      break;
    case GameVersion::Jak2:
      init_bucket_renderers_jak2();
      break;
    case GameVersion::Jak3:
    case GameVersion::JakX:
      init_bucket_renderers_jak3();
      break;
    default:
      ASSERT(false);
  }

  // [QA-TEST 25/08] interrupteur d'enquete : QA_DISABLE="motif1,motif2" eteint
  // tout renderer dont le nom contient un des motifs (chasse au dessinateur du
  // decor lointain). Sans la variable, aucun effet.
  if (const char* dis = getenv("QA_DISABLE")) {
    std::string spec(dis);
    for (auto& br : m_bucket_renderers) {
      if (!br) continue;
      std::string n = br->name();
      size_t start = 0;
      while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string pat = spec.substr(start, comma == std::string::npos ? std::string::npos
                                                                        : comma - start);
        if (!pat.empty() && n.find(pat) != std::string::npos) {
          br->set_enabled(false);
          lg::warn("[QA-DISABLE] renderer eteint : {}", n);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
  }
}

void OpenGLRenderer::init_bucket_renderers_jak3() {
  using namespace jak3;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);
  {
    auto p = scoped_prof("render-inits");

    m_blit_displays =
        init_bucket_renderer<BlitDisplays>("blit", BucketCategory::OTHER, BucketId::BLIT_START);
    init_bucket_renderer<VisDataHandler>("vis", BucketCategory::OTHER, BucketId::BUCKET_2);

    // 4
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-pre", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SKY_PRE, m_texture_animator);
    init_bucket_renderer<DirectRenderer>("sky", BucketCategory::OTHER, BucketId::SKY, 1024 * 8);

    init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                         BucketId::OCEAN_MID_FAR);
    // 7 (hack for progress menu box)
    init_bucket_renderer<DirectRenderer>("progress-hack", BucketCategory::OTHER,
                                         BucketId::PROGRESS_HACK, 0x8000);

    // 8 (in tfrag category for now, just for stat reporting.)
    init_bucket_renderer<Hfrag>("hfrag", BucketCategory::TFRAG, BucketId::HFRAG);

    // 10
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      // 10
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-tfrag", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, i),
          m_texture_animator);
      // 11
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-l{}-tfrag", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_TFRAG, BucketId::TFRAG_L1_TFRAG, i),
          std::vector{tfrag3::TFragmentTreeKind::NORMAL}, false, i, anim_slot_array());
      Tie3* tie = init_bucket_renderer<Tie3>(
          fmt::format("tie-l{}-tfrag", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_TFRAG, BucketId::TIE_L1_TFRAG, i), i,
          anim_slot_array());
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-tfrag", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_TFRAG, BucketId::ETIE_L1_TFRAG, i), tie,
          tfrag3::TieCategory::NORMAL_ENVMAP);

      // 17
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-tfrag", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-tfrag", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_TFRAG, BucketId::GMERC_L1_TFRAG, i), m_generic2,
          Generic2::Mode::NORMAL);

      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-shrub", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, i),
          m_texture_animator);
      init_bucket_renderer<Shrub>(
          fmt::format("shrub-l{}-shrub", i), BucketCategory::SHRUB,
          GET_BUCKET_ID_FOR_LIST(BucketId::SHRUB_L0_SHRUB, BucketId::SHRUB_L1_SHRUB, i));
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-shrub", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-shrub", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_SHRUB, BucketId::GMERC_L1_SHRUB, i), m_generic2,
          Generic2::Mode::NORMAL);

      // 230
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-alpha", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, i),
          m_texture_animator);
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-t-l{}-alpha", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_ALPHA, BucketId::TFRAG_L1_ALPHA, i),
          std::vector{tfrag3::TFragmentTreeKind::TRANS}, false, i, anim_slot_array());
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("tie-t-l{}-alpha", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_ALPHA, BucketId::TIE_L1_ALPHA, i), tie,
          tfrag3::TieCategory::TRANS);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-alpha", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_ALPHA, BucketId::ETIE_L1_ALPHA, i), tie,
          tfrag3::TieCategory::TRANS_ENVMAP);
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-alpha", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-alpha", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_ALPHA, BucketId::GMERC_L1_ALPHA, i), m_generic2,
          Generic2::Mode::NORMAL);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("tie-w-l{}-water", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_WATER, BucketId::TIE_L1_WATER, i), tie,
          tfrag3::TieCategory::WATER);
      init_bucket_renderer<Tie3AnotherCategory>(
          fmt::format("etie-l{}-water", i), BucketCategory::TIE,
          GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_WATER, BucketId::ETIE_L1_WATER, i), tie,
          tfrag3::TieCategory::WATER_ENVMAP);
    }

    // 340
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-tfrag", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_TFRAG, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-tfrag", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_TFRAG, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-tfrag", BucketCategory::GENERIC,
                                                 BucketId::GMERC_LCOM_TFRAG, m_generic2,
                                                 Generic2::Mode::NORMAL);
    // 345
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-shrub", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SHRUB, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-shrub", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_SHRUB, m_merc2);

    // 350
    init_bucket_renderer<Shadow2>("shadow", BucketCategory::OTHER, BucketId::SHADOW);

    // 351
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-pris", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, i),
          m_texture_animator);
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-pris2", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, i),
          m_texture_animator);

      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-pris", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-pris", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS, BucketId::GMERC_L1_PRIS, i), m_generic2,
          Generic2::Mode::NORMAL);

      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-pris", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_PRIS, BucketId::GMERC2_L1_PRIS, i), m_generic2,
          Generic2::Mode::PRIM);

      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-pris2", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-pris2", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS2, BucketId::GMERC_L1_PRIS2, i), m_generic2,
          Generic2::Mode::NORMAL);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-pris2", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_PRIS2, BucketId::GMERC2_L1_PRIS2, i),
          m_generic2, Generic2::Mode::PRIM);
    }

    // 401
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-pris", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_PRIS, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-pris", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_PRIS, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-pris", BucketCategory::GENERIC,
                                                 BucketId::GMERC_LCOM_PRIS, m_generic2,
                                                 Generic2::Mode::NORMAL);

    init_bucket_renderer<Generic2BucketRenderer>("gmerc2-lcom-pris", BucketCategory::GENERIC,
                                                 BucketId::GMERC2_LCOM_PRIS, m_generic2,
                                                 Generic2::Mode::PRIM);

    // 461
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-post", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_SKY_POST, m_texture_animator);
    init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);

    // 463
    for (int i = 0; i < LEVEL_MAX; i++) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
      // 463
      init_bucket_renderer<TextureUploadHandler>(
          fmt::format("tex-l{}-water", i), BucketCategory::TEX,
          GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, i),
          m_texture_animator);
      init_bucket_renderer<Merc2BucketRenderer>(
          fmt::format("merc-l{}-water", i), BucketCategory::MERC,
          GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, i), m_merc2);
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc-l{}-water", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_WATER, BucketId::GMERC_L1_WATER, i), m_generic2,
          Generic2::Mode::NORMAL);
      // 466
      init_bucket_renderer<TFragment>(
          fmt::format("tfrag-l{}-water", i), BucketCategory::TFRAG,
          GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_WATER, BucketId::TFRAG_L1_WATER, i),
          std::vector{tfrag3::TFragmentTreeKind::WATER}, false, i, anim_slot_array());
      init_bucket_renderer<Generic2BucketRenderer>(
          fmt::format("gmerc2-l{}-water", i), BucketCategory::GENERIC,
          GET_BUCKET_ID_FOR_LIST(BucketId::GMERC2_L0_WATER, BucketId::GMERC2_L1_WATER, i),
          m_generic2, Generic2::Mode::PRIM);
    }

    // 563
    init_bucket_renderer<TextureUploadHandler>("tex-lcom-water", BucketCategory::TEX,
                                               BucketId::TEX_LCOM_WATER, m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-water", BucketCategory::MERC,
                                              BucketId::MERC_LCOM_WATER, m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>("generic2-lcom-water", BucketCategory::GENERIC,
                                                 BucketId::GMERC2_LCOM_WATER, m_generic2,
                                                 Generic2::Mode::PRIM);

    // 568
    init_bucket_renderer<TextureUploadHandler>("tex-sprite", BucketCategory::TEX,
                                               BucketId::TEX_SPRITE, m_texture_animator);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-1", BucketCategory::GENERIC,
                                                 BucketId::GENERIC_SPRITE_1, m_generic2,
                                                 Generic2::Mode::PRIM);
    init_bucket_renderer<Sprite3>("particles", BucketCategory::SPRITE, BucketId::PARTICLES);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-2", BucketCategory::GENERIC,
                                                 BucketId::GENERIC_SPRITE_2, m_generic2,
                                                 Generic2::Mode::PRIM);
    init_bucket_renderer<Generic2BucketRenderer>("generic-sprite-3", BucketCategory::OTHER,
                                                 BucketId::GENERIC_SPRITE_3, m_generic2,
                                                 Generic2::Mode::LIGHTNING);

    init_bucket_renderer<Shadow2>("shadow2", BucketCategory::OTHER, BucketId::SHADOW2);
    init_bucket_renderer<Shadow2>("shadow3", BucketCategory::OTHER, BucketId::SHADOW3);
    // 575
    init_bucket_renderer<TextureUploadHandler>("tex-warp", BucketCategory::TEX, BucketId::TEX_WARP,
                                               m_texture_animator);
    init_bucket_renderer<Warp>("generic-warp", BucketCategory::GENERIC, BucketId::GENERIC_WARP,
                               m_generic2);

    init_bucket_renderer<TextureUploadHandler>("debug-no-zbuf1", BucketCategory::OTHER,
                                               BucketId::DEBUG_NO_ZBUF1, m_texture_animator, true);
    // 578
    init_bucket_renderer<TextureUploadHandler>("tex-hud-hud-alpha", BucketCategory::TEX,
                                               BucketId::TEX_HUD_HUD_ALPHA, m_texture_animator,
                                               true);
    init_bucket_renderer<ProgressRenderer>("hud-draw-hud-alpha", BucketCategory::OTHER,
                                           BucketId::HUD_DRAW_HUD_ALPHA, 0x8000);
    init_bucket_renderer<TextureUploadHandler>("tex-hud-pris2", BucketCategory::TEX,
                                               BucketId::TEX_HUD_PRIS2, m_texture_animator, true);
    init_bucket_renderer<ProgressRenderer>("hud-draw-pris2", BucketCategory::OTHER,
                                           BucketId::HUD_DRAW_PRIS2, 0x8000);
    init_bucket_renderer<ProgressRenderer>("progress", BucketCategory::OTHER, BucketId::BUCKET582,
                                           0x8000);

    // 583
    init_bucket_renderer<DirectRenderer>("debug", BucketCategory::OTHER, BucketId::DEBUG, 0x8000);
    // 584
    init_bucket_renderer<DirectRenderer>("debug-no-zbuf2", BucketCategory::OTHER,
                                         BucketId::DEBUG_NO_ZBUF2, 0x8000);
    init_bucket_renderer<DirectRenderer>("debug-menu", BucketCategory::OTHER, BucketId::DEBUG_MENU,
                                         0x8000);

    auto eye_renderer = std::make_unique<EyeRenderer>("eyes", 0);
    m_render_state.eye_renderer = eye_renderer.get();
    m_jak3_eye_renderer = std::move(eye_renderer);

    // for any unset renderers, just set them to an EmptyBucketRenderer.
    for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
      if (!m_bucket_renderers[i]) {
        init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i),
                                                  BucketCategory::OTHER, i);
      }

      m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
      m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak3);
    }
    m_jak3_eye_renderer->init_shaders(m_render_state.shaders);
    m_jak3_eye_renderer->init_textures(*m_render_state.texture_pool, GameVersion::Jak3);
  }
}

void OpenGLRenderer::init_bucket_renderers_jak2() {
  using namespace jak2;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);

  // 0
  init_bucket_renderer<VisDataHandler>("vis", BucketCategory::OTHER, BucketId::BUCKET_2);
  m_blit_displays =
      init_bucket_renderer<BlitDisplays>("blit", BucketCategory::OTHER, BucketId::BUCKET_3);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-pre", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SKY_PRE, m_texture_animator);
  init_bucket_renderer<DirectRenderer>("sky-draw", BucketCategory::OTHER, BucketId::SKY_DRAW, 1024);
  init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                       BucketId::OCEAN_MID_FAR);

  for (int i = 0; i < LEVEL_MAX; ++i) {
#define GET_BUCKET_ID_FOR_LIST(bkt1, bkt2, idx) ((int)(bkt1) + ((int)(bkt2) - (int)(bkt1)) * (idx))
    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-tfrag", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, i),
        m_texture_animator);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-l{}-tfrag", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_L0_TFRAG, BucketId::TFRAG_L1_TFRAG, i),
        std::vector{tfrag3::TFragmentTreeKind::NORMAL}, false, i, anim_slot_array());
    Tie3* tie = init_bucket_renderer<Tie3>(
        fmt::format("tie-l{}-tfrag", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_L0_TFRAG, BucketId::TIE_L1_TFRAG, i), i,
        anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-l{}-tfrag", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_L0_TFRAG, BucketId::ETIE_L1_TFRAG, i), tie,
        tfrag3::TieCategory::NORMAL_ENVMAP);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-tfrag", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-tfrag", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_TFRAG, BucketId::GMERC_L1_TFRAG, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-shrub", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, i),
        m_texture_animator);
    init_bucket_renderer<Shrub>(
        fmt::format("shrub-l{}-shrub", i), BucketCategory::SHRUB,
        GET_BUCKET_ID_FOR_LIST(BucketId::SHRUB_L0_SHRUB, BucketId::SHRUB_L1_SHRUB, i));
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-shrub", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-shrub", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_SHRUB, BucketId::GMERC_L1_SHRUB, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-alpha", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, i),
        m_texture_animator);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-t-l{}-alpha", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_T_L0_ALPHA, BucketId::TFRAG_T_L1_ALPHA, i),
        std::vector{tfrag3::TFragmentTreeKind::TRANS}, false, i, anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("tie-t-l{}-alpha", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_T_L0_ALPHA, BucketId::TIE_T_L1_ALPHA, i), tie,
        tfrag3::TieCategory::TRANS);
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-t-l{}-alpha", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_T_L0_ALPHA, BucketId::ETIE_T_L1_ALPHA, i), tie,
        tfrag3::TieCategory::TRANS_ENVMAP);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-alpha", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-alpha", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_ALPHA, BucketId::GMERC_L1_ALPHA, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-pris", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-pris", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-pris", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS, BucketId::GMERC_L1_PRIS, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-pris2", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-pris2", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-pris2", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_PRIS2, BucketId::GMERC_L1_PRIS2, i), m_generic2,
        Generic2::Mode::NORMAL);

    init_bucket_renderer<TextureUploadHandler>(
        fmt::format("tex-l{}-water", i), BucketCategory::TEX,
        GET_BUCKET_ID_FOR_LIST(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, i),
        m_texture_animator);
    init_bucket_renderer<Merc2BucketRenderer>(
        fmt::format("merc-l{}-water", i), BucketCategory::MERC,
        GET_BUCKET_ID_FOR_LIST(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, i), m_merc2);
    init_bucket_renderer<Generic2BucketRenderer>(
        fmt::format("gmerc-l{}-water", i), BucketCategory::GENERIC,
        GET_BUCKET_ID_FOR_LIST(BucketId::GMERC_L0_WATER, BucketId::GMERC_L1_WATER, i), m_generic2,
        Generic2::Mode::NORMAL);
    init_bucket_renderer<TFragment>(
        fmt::format("tfrag-w-l{}-alpha", i), BucketCategory::TFRAG,
        GET_BUCKET_ID_FOR_LIST(BucketId::TFRAG_W_L0_WATER, BucketId::TFRAG_W_L1_WATER, i),
        std::vector{tfrag3::TFragmentTreeKind::WATER}, false, i, anim_slot_array());
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("tie-w-l{}-water", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::TIE_W_L0_WATER, BucketId::TIE_W_L1_WATER, i), tie,
        tfrag3::TieCategory::WATER);
    init_bucket_renderer<Tie3AnotherCategory>(
        fmt::format("etie-w-l{}-water", i), BucketCategory::TIE,
        GET_BUCKET_ID_FOR_LIST(BucketId::ETIE_W_L0_WATER, BucketId::ETIE_W_L1_WATER, i), tie,
        tfrag3::TieCategory::WATER_ENVMAP);
#undef GET_BUCKET_ID_FOR_LIST
  }
  // 180
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-tfrag", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_TFRAG, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-tfrag", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_TFRAG, m_merc2);
  // 190
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-shrub", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SHRUB, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-shrub", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_SHRUB, m_merc2);
  init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-tfrag", BucketCategory::GENERIC,
                                               BucketId::GMERC_LCOM_TFRAG, m_generic2,
                                               Generic2::Mode::NORMAL);
  init_bucket_renderer<Shadow2>("shadow", BucketCategory::OTHER, BucketId::SHADOW);
  // 220
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-pris", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_PRIS, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-pris", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_PRIS, m_merc2);
  init_bucket_renderer<Generic2BucketRenderer>("gmerc-lcom-pris", BucketCategory::GENERIC,
                                               BucketId::GMERC_LCOM_PRIS, m_generic2,
                                               Generic2::Mode::NORMAL);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-water", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_WATER, m_texture_animator);
  init_bucket_renderer<Merc2BucketRenderer>("merc-lcom-water", BucketCategory::MERC,
                                            BucketId::MERC_LCOM_WATER, m_merc2);
  init_bucket_renderer<TextureUploadHandler>("tex-lcom-sky-post", BucketCategory::TEX,
                                             BucketId::TEX_LCOM_SKY_POST, m_texture_animator);
  // 310
  init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);
  init_bucket_renderer<TextureUploadHandler>("tex-all-sprite", BucketCategory::TEX,
                                             BucketId::TEX_ALL_SPRITE, m_texture_animator);
  init_bucket_renderer<Sprite3>("particles", BucketCategory::SPRITE, BucketId::PARTICLES);
  init_bucket_renderer<Shadow2>("shadow2", BucketCategory::OTHER, BucketId::SHADOW2);
  init_bucket_renderer<Generic2BucketRenderer>("effects", BucketCategory::OTHER, BucketId::EFFECTS,
                                               m_generic2, Generic2::Mode::LIGHTNING);
  init_bucket_renderer<TextureUploadHandler>("tex-all-warp", BucketCategory::TEX,
                                             BucketId::TEX_ALL_WARP, m_texture_animator);
  init_bucket_renderer<Warp>("warp", BucketCategory::GENERIC, BucketId::GMERC_WARP, m_generic2);
  init_bucket_renderer<TextureUploadHandler>("debug-no-zbuf1", BucketCategory::OTHER,
                                             BucketId::DEBUG_NO_ZBUF1, m_texture_animator, true);
  init_bucket_renderer<TextureUploadHandler>("tex-all-map", BucketCategory::TEX,
                                             BucketId::TEX_ALL_MAP, m_texture_animator, true);
  // 320
  init_bucket_renderer<ProgressRenderer>("progress", BucketCategory::OTHER, BucketId::PROGRESS,
                                         0x1000);
  init_bucket_renderer<DirectRenderer>("screen-filter", BucketCategory::OTHER,
                                       BucketId::SCREEN_FILTER, 256);
  init_bucket_renderer<TextureUploadHandler>("subtitle", BucketCategory::OTHER, BucketId::SUBTITLE,
                                             m_texture_animator, true);
  init_bucket_renderer<DirectRenderer>("debug2", BucketCategory::OTHER, BucketId::DEBUG2, 0x8000);
  init_bucket_renderer<DirectRenderer>("debug-no-zbuf2", BucketCategory::OTHER,
                                       BucketId::DEBUG_NO_ZBUF2, 0x8000);
  init_bucket_renderer<DirectRenderer>("debug3", BucketCategory::OTHER, BucketId::DEBUG3, 0x2000);

  auto eye_renderer = std::make_unique<EyeRenderer>("eyes", 0);
  m_render_state.eye_renderer = eye_renderer.get();
  m_jak2_eye_renderer = std::move(eye_renderer);

  {
    auto p = scoped_prof("render-inits");
    // for now, for any unset renderers, just set them to an EmptyBucketRenderer.
    for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
      if (!m_bucket_renderers[i]) {
        init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i),
                                                  BucketCategory::OTHER, i);
      }

      m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
      m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak2);
    }
    m_jak2_eye_renderer->init_shaders(m_render_state.shaders);
    m_jak2_eye_renderer->init_textures(*m_render_state.texture_pool, GameVersion::Jak2);
  }
}

/*!
 * Construct bucket renderers.  We can specify different renderers for different buckets
 */
void OpenGLRenderer::init_bucket_renderers_jak1() {
  using namespace jak1;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);
  m_bucket_categories.resize((int)BucketId::MAX_BUCKETS, BucketCategory::OTHER);

  std::vector<tfrag3::TFragmentTreeKind> normal_tfrags = {tfrag3::TFragmentTreeKind::NORMAL,
                                                          tfrag3::TFragmentTreeKind::LOWRES};
  std::vector<tfrag3::TFragmentTreeKind> dirt_tfrags = {tfrag3::TFragmentTreeKind::DIRT};
  std::vector<tfrag3::TFragmentTreeKind> ice_tfrags = {tfrag3::TFragmentTreeKind::ICE};
  auto sky_gpu_blender = std::make_shared<SkyBlendGPU>();
  auto sky_cpu_blender = std::make_shared<SkyBlendCPU>();

  //-------------
  // PRE TEXTURE
  //-------------
  // 0 : ??
  // 1 : ??
  // 2 : ??
  // 3 : SKY_DRAW
  init_bucket_renderer<SkyRenderer>("sky", BucketCategory::OTHER, BucketId::SKY_DRAW);
  // 4 : OCEAN_MID_AND_FAR
  init_bucket_renderer<OceanMidAndFar>("ocean-mid-far", BucketCategory::OCEAN,
                                       BucketId::OCEAN_MID_AND_FAR);

  //-----------------------
  // LEVEL 0 tfrag texture
  //-----------------------
  // 5 : TFRAG_TEX_LEVEL0
  init_bucket_renderer<TextureUploadHandler>("l0-tfrag-tex", BucketCategory::TEX,
                                             BucketId::TFRAG_TEX_LEVEL0, m_texture_animator);
  // 6 : TFRAG_LEVEL0
  init_bucket_renderer<TFragment>("l0-tfrag-tfrag", BucketCategory::TFRAG, BucketId::TFRAG_LEVEL0,
                                  normal_tfrags, false, 0, anim_slot_array());
  // 7 : TFRAG_NEAR_LEVEL0
  // 8 : TIE_NEAR_LEVEL0
  // 9 : TIE_LEVEL0
  init_bucket_renderer<Tie3WithEnvmapJak1>("l0-tfrag-tie", BucketCategory::TIE,
                                           BucketId::TIE_LEVEL0, 0);
  // 10 : MERC_TFRAG_TEX_LEVEL0
  init_bucket_renderer<Merc2BucketRenderer>("l0-tfrag-merc", BucketCategory::MERC,
                                            BucketId::MERC_TFRAG_TEX_LEVEL0, m_merc2);
  // 11 : GMERC_TFRAG_TEX_LEVEL0
  init_bucket_renderer<Generic2BucketRenderer>("l0-tfrag-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_TFRAG_TEX_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 1 tfrag texture
  //-----------------------
  // 12 : TFRAG_TEX_LEVEL1
  init_bucket_renderer<TextureUploadHandler>("l1-tfrag-tex", BucketCategory::TEX,
                                             BucketId::TFRAG_TEX_LEVEL1, m_texture_animator);
  // 13 : TFRAG_LEVEL1
  init_bucket_renderer<TFragment>("l1-tfrag-tfrag", BucketCategory::TFRAG, BucketId::TFRAG_LEVEL1,
                                  normal_tfrags, false, 1, anim_slot_array());
  // 14 : TFRAG_NEAR_LEVEL1
  // 15 : TIE_NEAR_LEVEL1
  // 16 : TIE_LEVEL1
  init_bucket_renderer<Tie3WithEnvmapJak1>("l1-tfrag-tie", BucketCategory::TIE,
                                           BucketId::TIE_LEVEL1, 1);
  // 17 : MERC_TFRAG_TEX_LEVEL1
  init_bucket_renderer<Merc2BucketRenderer>("l1-tfrag-merc", BucketCategory::MERC,
                                            BucketId::MERC_TFRAG_TEX_LEVEL1, m_merc2);
  // 18 : GMERC_TFRAG_TEX_LEVEL1
  init_bucket_renderer<Generic2BucketRenderer>("l1-tfrag-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_TFRAG_TEX_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 0 shrub texture
  //-----------------------
  // 19 : SHRUB_TEX_LEVEL0
  init_bucket_renderer<TextureUploadHandler>("l0-shrub-tex", BucketCategory::TEX,
                                             BucketId::SHRUB_TEX_LEVEL0, m_texture_animator);
  // 20 : SHRUB_NORMAL_LEVEL0
  init_bucket_renderer<Shrub>("l0-shrub", BucketCategory::SHRUB, BucketId::SHRUB_NORMAL_LEVEL0);
  // 21 : ???
  // 22 : SHRUB_BILLBOARD_LEVEL0
  // 23 : SHRUB_TRANS_LEVEL0
  // 24 : SHRUB_GENERIC_LEVEL0
  init_bucket_renderer<Generic2BucketRenderer>("l0-shrub-generic", BucketCategory::GENERIC,
                                               BucketId::SHRUB_GENERIC_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 1 shrub texture
  //-----------------------
  // 25 : SHRUB_TEX_LEVEL1
  init_bucket_renderer<TextureUploadHandler>("l1-shrub-tex", BucketCategory::TEX,
                                             BucketId::SHRUB_TEX_LEVEL1, m_texture_animator);
  // 26 : SHRUB_NORMAL_LEVEL1
  init_bucket_renderer<Shrub>("l1-shrub", BucketCategory::SHRUB, BucketId::SHRUB_NORMAL_LEVEL1);
  // 27 : ???
  // 28 : SHRUB_BILLBOARD_LEVEL1
  // 29 : SHRUB_TRANS_LEVEL1
  // 30 : SHRUB_GENERIC_LEVEL1
  init_bucket_renderer<Generic2BucketRenderer>("l1-shrub-generic", BucketCategory::GENERIC,
                                               BucketId::SHRUB_GENERIC_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);

  //-----------------------
  // LEVEL 0 alpha texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-alpha-tex", BucketCategory::TEX,
                                             BucketId::ALPHA_TEX_LEVEL0, m_texture_animator);  // 31
  init_bucket_renderer<SkyBlendHandler>("l0-alpha-sky-blend-and-tfrag-trans", BucketCategory::OTHER,
                                        BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, 0,
                                        sky_gpu_blender, sky_cpu_blender, anim_slot_array());  // 32
  // 33
  init_bucket_renderer<TFragment>("l0-alpha-tfrag", BucketCategory::TFRAG,
                                  BucketId::TFRAG_DIRT_LEVEL0, dirt_tfrags, false, 0,
                                  anim_slot_array());  // 34
  // 35
  init_bucket_renderer<TFragment>("l0-alpha-tfrag-ice", BucketCategory::TFRAG,
                                  BucketId::TFRAG_ICE_LEVEL0, ice_tfrags, false, 0,
                                  anim_slot_array());
  // 37

  //-----------------------
  // LEVEL 1 alpha texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-alpha-tex", BucketCategory::TEX,
                                             BucketId::ALPHA_TEX_LEVEL1, m_texture_animator);  // 38
  init_bucket_renderer<SkyBlendHandler>("l1-alpha-sky-blend-and-tfrag-trans", BucketCategory::OTHER,
                                        BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, 1,
                                        sky_gpu_blender, sky_cpu_blender, anim_slot_array());  // 39
  // 40
  init_bucket_renderer<TFragment>("l1-alpha-tfrag-dirt", BucketCategory::TFRAG,
                                  BucketId::TFRAG_DIRT_LEVEL1, dirt_tfrags, false, 1,
                                  anim_slot_array());  // 41
  // 42
  init_bucket_renderer<TFragment>("l1-alpha-tfrag-ice", BucketCategory::TFRAG,
                                  BucketId::TFRAG_ICE_LEVEL1, ice_tfrags, false, 1,
                                  anim_slot_array());
  // 44

  init_bucket_renderer<Merc2BucketRenderer>("common-alpha-merc", BucketCategory::MERC,
                                            BucketId::MERC_AFTER_ALPHA, m_merc2);

  init_bucket_renderer<Generic2BucketRenderer>("common-alpha-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_ALPHA, m_generic2,
                                               Generic2::Mode::NORMAL);                     // 46
  init_bucket_renderer<ShadowRenderer>("shadow", BucketCategory::OTHER, BucketId::SHADOW);  // 47

  //-----------------------
  // LEVEL 0 pris texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-pris-tex", BucketCategory::TEX,
                                             BucketId::PRIS_TEX_LEVEL0, m_texture_animator);  // 48
  init_bucket_renderer<Merc2BucketRenderer>("l0-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_PRIS_LEVEL0, m_merc2);  // 49
  init_bucket_renderer<Generic2BucketRenderer>("l0-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);  // 50

  //-----------------------
  // LEVEL 1 pris texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-pris-tex", BucketCategory::TEX,
                                             BucketId::PRIS_TEX_LEVEL1, m_texture_animator);  // 51
  init_bucket_renderer<Merc2BucketRenderer>("l1-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_PRIS_LEVEL1, m_merc2);  // 52
  init_bucket_renderer<Generic2BucketRenderer>("l1-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);  // 53

  // other renderers may output to the eye renderer
  m_render_state.eye_renderer = init_bucket_renderer<EyeRenderer>(
      "common-pris-eyes", BucketCategory::OTHER, BucketId::MERC_EYES_AFTER_PRIS);  // 54

  // hack: set to merc2 for debugging
  init_bucket_renderer<Merc2BucketRenderer>("common-pris-merc", BucketCategory::MERC,
                                            BucketId::MERC_AFTER_PRIS, m_merc2);  // 55
  init_bucket_renderer<Generic2BucketRenderer>("common-pris-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_PRIS, m_generic2,
                                               Generic2::Mode::NORMAL);  // 56

  //-----------------------
  // LEVEL 0 water texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l0-water-tex", BucketCategory::TEX,
                                             BucketId::WATER_TEX_LEVEL0, m_texture_animator);  // 57
  init_bucket_renderer<Merc2BucketRenderer>("l0-water-merc", BucketCategory::MERC,
                                            BucketId::MERC_WATER_LEVEL0, m_merc2);  // 58
  init_bucket_renderer<Generic2BucketRenderer>("l0-water-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_WATER_LEVEL0, m_generic2,
                                               Generic2::Mode::NORMAL);  // 59

  //-----------------------
  // LEVEL 1 water texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("l1-water-tex", BucketCategory::TEX,
                                             BucketId::WATER_TEX_LEVEL1, m_texture_animator);  // 60
  init_bucket_renderer<Merc2BucketRenderer>("l1-water-merc", BucketCategory::MERC,
                                            BucketId::MERC_WATER_LEVEL1, m_merc2);  // 61
  init_bucket_renderer<Generic2BucketRenderer>("l1-water-generic", BucketCategory::GENERIC,
                                               BucketId::GENERIC_WATER_LEVEL1, m_generic2,
                                               Generic2::Mode::NORMAL);  // 62

  init_bucket_renderer<OceanNear>("ocean-near", BucketCategory::OCEAN, BucketId::OCEAN_NEAR);  // 63

  //-----------------------
  // DEPTH CUE
  //-----------------------
  init_bucket_renderer<DepthCue>("depth-cue", BucketCategory::OTHER, BucketId::DEPTH_CUE);  // 64

  //-----------------------
  // COMMON texture
  //-----------------------
  init_bucket_renderer<TextureUploadHandler>("common-tex", BucketCategory::TEX,
                                             BucketId::PRE_SPRITE_TEX, m_texture_animator);  // 65

  init_bucket_renderer<Sprite3>("sprite", BucketCategory::SPRITE, BucketId::SPRITE);  // 66

  init_bucket_renderer<DirectRenderer>("debug", BucketCategory::OTHER, BucketId::DEBUG, 0x20000);
  init_bucket_renderer<DirectRenderer>("debug-no-zbuf", BucketCategory::OTHER,
                                       BucketId::DEBUG_NO_ZBUF, 0x8000);
  // an extra custom bucket!
  init_bucket_renderer<DirectRenderer>("subtitle", BucketCategory::OTHER, BucketId::SUBTITLE, 6000);

  // for now, for any unset renderers, just set them to an EmptyBucketRenderer.
  for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
    if (!m_bucket_renderers[i]) {
      init_bucket_renderer<EmptyBucketRenderer>(fmt::format("bucket-{}", i), BucketCategory::OTHER,
                                                (BucketId)i);
    }

    m_bucket_renderers[i]->init_shaders(m_render_state.shaders);
    m_bucket_renderers[i]->init_textures(*m_render_state.texture_pool, GameVersion::Jak1);
  }
  sky_cpu_blender->init_textures(*m_render_state.texture_pool, m_version);
  sky_gpu_blender->init_textures(*m_render_state.texture_pool, m_version);
}

namespace {
Fbo make_fbo(int w, int h, int msaa, bool make_zbuf_and_stencil) {
  Fbo result;
  bool use_multisample = msaa > 1;

  // make framebuffer object
  glGenFramebuffers(1, &result.fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, result.fbo_id);
  result.valid = true;

  // make texture that will hold the colors of the framebuffer
  GLuint tex;
  glGenTextures(1, &tex);
  result.tex_id = tex;
  glActiveTexture(GL_TEXTURE0);
  if (use_multisample) {
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, msaa, GL_RGBA8, w, h, GL_TRUE);
  } else {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  // make depth and stencil buffers that will hold the... depth and stencil buffers
  if (make_zbuf_and_stencil) {
    GLuint zbuf;
    glGenRenderbuffers(1, &zbuf);
    result.zbuf_stencil_id = zbuf;
    glBindRenderbuffer(GL_RENDERBUFFER, zbuf);
    if (use_multisample) {
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa, GL_DEPTH24_STENCIL8, w, h);
    } else {
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zbuf);
  }
  // attach texture to framebuffer as target for colors

  if (use_multisample) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, tex, 0);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  }

  // PC-REMAKE 21/09 : G-buffer de normales sur l'attachement 1 du framebuffer
  // principal. Il n'est PAS dans la liste de dessin par defaut (glDrawBuffers(1)) :
  // les renderers PS2 emules n'ecrivent que la sortie 0, et une sortie non
  // ecrite serait indefinie. Les renderers qui ont une normale (tfrag, tie,
  // merc) l'activent le temps de leurs draws (pc_normal_target).
  if (make_zbuf_and_stencil) {
    GLuint ntex;
    glGenTextures(1, &ntex);
    result.normal_tex_id = ntex;
    if (use_multisample) {
      glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ntex);
      glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, msaa, GL_RGBA8, w, h, GL_TRUE);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D_MULTISAMPLE, ntex,
                             0);
    } else {
      glBindTexture(GL_TEXTURE_2D, ntex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, ntex, 0);
    }
  }

  GLenum render_targets[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, render_targets);
  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("Failed to setup framebuffer: {} {} {} {} ", w, h, msaa, make_zbuf_and_stencil);
    switch (status) {
      case GL_FRAMEBUFFER_UNDEFINED:
        lg::print("GL_FRAMEBUFFER_UNDEFINED\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER\n");
        break;
      case GL_FRAMEBUFFER_UNSUPPORTED:
        lg::print("GL_FRAMEBUFFER_UNSUPPORTED\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE\n");
        break;
      case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
        lg::print("GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS\n");
        break;
    }
    ASSERT(false);
  }

  result.multisample_count = msaa;
  result.multisampled = use_multisample;
  result.is_window = false;
  result.width = w;
  result.height = h;

  return result;
}
}  // namespace

void OpenGLRenderer::blit_display(ScopedProfilerNode& prof) {
  if (m_blit_displays) {
    m_blit_displays->do_copy_back(&m_render_state, prof);
  }
}

/*!
 * Main render function. This is called from the gfx loop with the chain passed from the game.
 */
namespace {
// PC-REMAKE: colour target used to replay the post pass for screenshots.
struct PostCapture {
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
};
PostCapture g_post_capture;
}  // namespace

// PC-REMAKE 14/10 : SONDE [QA-PROF] (passe d'optimisation). `touch /tmp/qa_prof`
// -> glFinish apres chaque bucket (temps GPU reel de chaque etape) et, toutes les
// 240 images, le cout moyen des 30 noeuds les plus chers dans le journal. Inerte
// sans le fichier (un stat() toutes les 60 images).
namespace {
bool g_qa_prof = false;
int g_qa_prof_poll = 0;
int g_qa_prof_frames = 0;
std::unordered_map<std::string, std::pair<double, u32>> g_qa_prof_acc;  // ms, draws
void qa_prof_walk(const ProfilerNode& n, const std::string& path, int depth) {
  auto& e = g_qa_prof_acc[path];
  e.first += n.stats().duration * 1000.0;
  e.second += n.stats().draw_calls;
  if (depth < 4) {
    for (const auto& c : n.children()) {
      qa_prof_walk(c, path + "/" + c.name(), depth + 1);
    }
  }
}
// PC-REMAKE 14/10 : SONDE [QA-GPU] — temps GPU REEL de chaque bucket et du post par
// requetes GL_TIME_ELAPSED (lues 4 images plus tard : aucun blocage, aucune
// serialisation, contrairement a [QA-PROF]). `touch /tmp/qa_gpu`.
struct QaGpuTimer {
  static constexpr int kRing = 4;
  static constexpr int kSlots = 96;
  GLuint q[kRing][kSlots] = {};
  bool used[kRing][kSlots] = {};
  std::string names[kSlots];
  double acc[kSlots] = {};
  bool init = false;
  bool active = false;
  bool open = false;
  int ring = 0;
  int poll = 0;
  int frames = 0;
  void begin_frame() {
    if ((poll++ % 60) == 0) {
      active = fs::exists("/tmp/qa_gpu");
    }
    if (!active) {
      return;
    }
    if (!init) {
      glGenQueries(kRing * kSlots, &q[0][0]);
      init = true;
    }
    // la tranche du ring qu'on va reecrire a ete emise il y a kRing images
    for (int i = 0; i < kSlots; i++) {
      if (!used[ring][i]) {
        continue;
      }
      GLuint64 ns = 0;
      glGetQueryObjectui64v(q[ring][i], GL_QUERY_RESULT, &ns);
      acc[i] += ns * 1e-6;
      used[ring][i] = false;
    }
  }
  void begin(int slot, const std::string& name) {
    if (!active || open || slot < 0 || slot >= kSlots) {
      return;
    }
    names[slot] = name;
    glBeginQuery(GL_TIME_ELAPSED, q[ring][slot]);
    used[ring][slot] = true;
    open = true;
  }
  void end() {
    if (open) {
      glEndQuery(GL_TIME_ELAPSED);
      open = false;
    }
  }
  void end_frame() {
    if (!active) {
      return;
    }
    ring = (ring + 1) % kRing;
    if (++frames >= 240 + kRing) {
      std::vector<std::pair<double, int>> v;
      double total = 0;
      for (int i = 0; i < kSlots; i++) {
        if (acc[i] > 0) {
          v.push_back({acc[i], i});
          total += acc[i];
        }
      }
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      const int n = 240;
      std::string out =
          fmt::format("[QA-GPU] temps GPU moyen sur {} images : total {:.3f} ms\n", n, total / n);
      for (size_t k = 0; k < v.size() && k < 24; k++) {
        out += fmt::format("  {:7.3f} ms  {}\n", v[k].first / n, names[v[k].second]);
      }
      lg::warn("{}", out);
      for (auto& a : acc) {
        a = 0;
      }
      frames = kRing;
    }
  }
};
QaGpuTimer g_qa_gpu;
}  // namespace
// [QA-GPU] 14/10 : decoupe le temps du bucket en cours en segments nommes (appelable
// depuis les renderers : la requete ouverte est fermee, une nouvelle commence)
void qa_gpu_split(int slot, const char* name) {
  if (!g_qa_gpu.active || !g_qa_gpu.open) {
    return;
  }
  g_qa_gpu.end();
  g_qa_gpu.begin(slot, name);
}
// [QA-GLERR] 14/10 : glGetError apres chaque grande etape de l'image (appelable aussi
// depuis les renderers : qa_glerr_check("...") ne coute rien sans /tmp/qa_glerr)
bool g_qa_glerr = false;
int g_qa_glerr_logged = 0;
void qa_glerr_check(const char* where) {
  if (!g_qa_glerr) {
    return;
  }
  GLenum e = glGetError();
  if (e != GL_NO_ERROR && g_qa_glerr_logged < 60) {
    g_qa_glerr_logged++;
    lg::warn("[QA-GLERR] 0x{:x} dans {}", (int)e, where);
  }
}
namespace {
}  // namespace

void OpenGLRenderer::render(DmaFollower dma, const RenderOptions& settings_in) {
  {
    static int poll = 0;
    if ((poll++ % 60) == 0) {
      g_qa_glerr = fs::exists("/tmp/qa_glerr");
    }
  }
  qa_glerr_check("hors rendu (image precedente : ImGui, swap, capture...)");
  g_qa_gpu.begin_frame();
  // /tmp/qa_prof_cpu : meme releve SANS glFinish = temps PROCESSEUR de soumission
  static bool qa_prof_cpu = false;
  if ((g_qa_prof_poll++ % 60) == 0) {
    qa_prof_cpu = fs::exists("/tmp/qa_prof_cpu");
    g_qa_prof = qa_prof_cpu || fs::exists("/tmp/qa_prof");
  }
  RenderOptions settings_copy;
  const RenderOptions* settings_ptr = &settings_in;
  if (g_qa_prof && !qa_prof_cpu && !settings_in.gpu_sync) {
    settings_copy = settings_in;
    settings_copy.gpu_sync = true;
    settings_ptr = &settings_copy;
  }
  const RenderOptions& settings = *settings_ptr;
  pc_remake_lamps_begin_frame();  // PC-REMAKE: publish the lamps GOAL registered
  m_profiler.clear();
  m_render_state.reset();
  m_render_state.ee_main_memory = g_ee_main_mem;
  m_render_state.offset_of_s7 = offset_of_s7();

  {
    g_current_renderer = "frame-setup";
    auto prof = m_profiler.root()->make_scoped_child("frame-setup");
    g_qa_gpu.begin(90, "frame-setup");
    setup_frame(settings);
    g_qa_gpu.end();
    qa_glerr_check("frame-setup");
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  {
    g_current_renderer = "loader";
    auto prof = m_profiler.root()->make_scoped_child("loader");
    if (m_last_pmode_alp == 0 && settings.pmode_alp_register != 0 && m_enable_fast_blackout_loads) {
      // blackout, load everything and don't worry about frame rate
      m_render_state.loader->update_blocking(*m_render_state.texture_pool);

    } else {
      m_render_state.loader->update(*m_render_state.texture_pool);
    }
    qa_glerr_check("loader");
  }

  // render the buckets!
  {
    auto prof = m_profiler.root()->make_scoped_child("buckets");
    dispatch_buckets(dma, prof, settings.gpu_sync);
    // 14/10 : passe d'ombres du soleil DIFFEREE, en une seule bascule (voir
    // SunShadowMap::flush_deferred) — avant le post, apres tous les casteurs
    {
      auto sp = prof.make_scoped_child("ombres-differees");
      g_sun_shadow.flush_deferred(&m_render_state.shaders, m_render_state.frame_idx);
      qa_glerr_check("passe d'ombres differee");
    }
    if (m_texture_animator) {
      // if animation requests weren't made, assume the level is unloaded and the textures should
      // reset.
      m_texture_animator->clear_stale_textures(m_render_state.frame_idx);
    }
  }

  // blit framebuffer so that it can be used as a texture by the game later
  {
    g_current_renderer = "blit-display";
    auto prof = m_profiler.root()->make_scoped_child("blit-display");
    blit_display(prof);
  }

  // apply effects done with PCRTC registers, as well as blit the framebuffer to the window and
  // apply brightness/contrast
  {
    g_current_renderer = "pcrtc";
    auto prof = m_profiler.root()->make_scoped_child("pcrtc");
    qa_glerr_check("buckets / blit-display");
    do_pcrtc_effects(settings.pmode_alp_register, settings.brightness_contrast_color,
                     settings.brightness_contrast_alpha, &m_render_state, prof);
    qa_glerr_check("pcrtc (post)");
    g_qa_gpu.end_frame();
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  m_last_pmode_alp = settings.pmode_alp_register;

  if (settings.save_screenshot) {
    g_current_renderer = "screenshot";
    auto prof = m_profiler.root()->make_scoped_child("screenshot");
    int read_buffer;
    int x, y, w, h, fbo_id;

    if (settings.internal_res_screenshot) {
      Fbo* screenshot_src;
      // can't screenshot from a multisampled buffer directly -
      if (m_fbo_state.resources.resolve_buffer.valid) {
        screenshot_src = &m_fbo_state.resources.resolve_buffer;
        read_buffer = GL_COLOR_ATTACHMENT0;
      } else {
        screenshot_src = m_fbo_state.render_fbo;
        read_buffer = GL_FRONT;
      }
      w = screenshot_src->width;
      h = screenshot_src->height;
      x = 0;
      y = 0;
      fbo_id = screenshot_src->fbo_id;
      // PC-REMAKE: that buffer holds the raw scene, BEFORE the post pass — so a
      // screenshot taken from it shows none of the lighting, bloom, fog or
      // tonemap the player actually sees. Replay the post pass into an
      // offscreen colour buffer and capture that instead.
      g_post_capture.ensure(w, h);
      do_pcrtc_effects(settings.pmode_alp_register, settings.brightness_contrast_color,
                       settings.brightness_contrast_alpha, &m_render_state, prof,
                       g_post_capture.fbo, w, h);
      fbo_id = g_post_capture.fbo;
      read_buffer = GL_COLOR_ATTACHMENT0;
    } else {
      read_buffer = GL_BACK;
      w = settings.draw_region_width;
      h = settings.draw_region_height;
      x = m_render_state.draw_offset_x;
      y = m_render_state.draw_offset_y;
      fbo_id = 0;  // window
    }
    finish_screenshot(settings.screenshot_path, w, h, x, y, fbo_id, read_buffer,
                      settings.quick_screenshot);
  }

  if (settings.draw_render_debug_window) {
    g_current_renderer = "render-window";
    auto prof = m_profiler.root()->make_scoped_child("render-window");
    draw_renderer_selection_window();
    // add a profile bar for the imgui stuff
    // vif_interrupt_callback(0);
    if (settings.gpu_sync) {
      glFinish();
    }
  }

  if (settings.draw_loader_window) {
    g_current_renderer = "loader-window";
    m_render_state.loader->draw_debug_window();
  }

  m_profiler.finish();
  if (g_qa_prof) {
    qa_prof_walk(*m_profiler.root(), "frame", 0);
    if (++g_qa_prof_frames >= 240) {
      std::vector<std::pair<std::string, std::pair<double, u32>>> v(g_qa_prof_acc.begin(),
                                                                     g_qa_prof_acc.end());
      std::sort(v.begin(), v.end(),
                [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
      std::string out = fmt::format("[QA-PROF] moyenne sur {} images (glFinish par bucket)\n",
                                    g_qa_prof_frames);
      for (size_t i = 0; i < v.size() && i < 30; i++) {
        out += fmt::format("  {:7.3f} ms {:6.1f} draws  {}\n", v[i].second.first / g_qa_prof_frames,
                           double(v[i].second.second) / g_qa_prof_frames, v[i].first);
      }
      lg::warn("{}", out);
      g_qa_prof_acc.clear();
      g_qa_prof_frames = 0;
    }
  }
  //  if (m_profiler.root_time() > 0.018) {
  //    fmt::print("Slow frame: {:.2f} ms\n", m_profiler.root_time() * 1000);
  //    fmt::print("{}\n", m_profiler.to_string());
  //  }

  if (settings.draw_profiler_window) {
    g_current_renderer = "profiler-window";
    m_profiler.draw();
  }

  if (settings.draw_small_profiler_window) {
    g_current_renderer = "small-profiler-window";
    SmallProfilerStats stats;
    stats.draw_calls = m_profiler.root()->stats().draw_calls;
    stats.triangles = m_profiler.root()->stats().triangles;
    for (int i = 0; i < (int)BucketCategory::MAX_CATEGORIES; i++) {
      stats.time_per_category[i] = m_category_times[i];
    }
    m_small_profiler.draw(m_render_state.load_status_debug, stats);
  }

  if (settings.draw_subtitle_editor_window) {
    g_current_renderer = "subtitle-editor-window";
    if (m_subtitle_editor == nullptr) {
      m_subtitle_editor = new SubtitleEditor();
    }
    m_subtitle_editor->draw_window();
  }

  if (settings.draw_filters_window) {
    g_current_renderer = "filters-window";
    m_filters_menu.draw_window();
  }

  if (settings.gpu_sync) {
    g_current_renderer = "gpu-sync";
    glFinish();
  }

  g_current_renderer = "end";
}

/*!
 * Draw the per-renderer debug window
 */
void OpenGLRenderer::draw_renderer_selection_window() {
  ImGui::Begin("Renderer Debug");

  ImGui::Checkbox("Use old single-draw", &m_render_state.no_multidraw);
  ImGui::SliderFloat("Fog Adjust", &m_render_state.fog_intensity, 0, 10);
  ImGui::Checkbox("Sky CPU", &m_render_state.use_sky_cpu);
  ImGui::Checkbox("Occlusion Cull", &m_render_state.use_occlusion_culling);
  ImGui::Checkbox("Blackout Loads", &m_enable_fast_blackout_loads);

  if (m_texture_animator && ImGui::TreeNode("Texture Animator")) {
    m_texture_animator->draw_debug_window();
    ImGui::TreePop();
  }

  ImGui::InputText("Renderer Filter", &m_renderer_filter);

  for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
    auto renderer = m_bucket_renderers[i].get();
    if (!m_renderer_filter.empty() && !str_util::contains(renderer->name(), m_renderer_filter)) {
      continue;
    }
    if (renderer && !renderer->empty()) {
      ImGui::PushID(i);
      if (ImGui::TreeNode(renderer->name_and_id().c_str())) {
        ImGui::Checkbox("Enable", &renderer->enabled());
        renderer->draw_debug_window();
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
  }
  if (ImGui::TreeNode("Texture Pool")) {
    m_render_state.texture_pool->draw_debug_window();
    ImGui::TreePop();
  }
  if (m_jak2_eye_renderer) {
    if (ImGui::TreeNode("Eyes")) {
      m_jak2_eye_renderer->draw_debug_window();
      ImGui::TreePop();
    }
  }
  if (m_jak3_eye_renderer) {
    if (ImGui::TreeNode("Eyes")) {
      m_jak3_eye_renderer->draw_debug_window();
      ImGui::TreePop();
    }
  }
  ImGui::End();
}

/*!
 * Pre-render frame setup.
 */
void OpenGLRenderer::setup_frame(const RenderOptions& settings) {
  // SDL controls the window framebuffer, so we just update the size:
  auto& window_fb = m_fbo_state.resources.window;

  bool window_resized = window_fb.width != settings.window_framebuffer_width ||
                        window_fb.height != settings.window_framebuffer_height;
  window_fb.valid = true;
  window_fb.is_window = true;
  window_fb.fbo_id = 0;
  window_fb.width = settings.window_framebuffer_width;
  window_fb.height = settings.window_framebuffer_height;
  window_fb.multisample_count = 1;
  window_fb.multisampled = false;

  // see if the render FBO is still applicable
  if (settings.save_screenshot || window_resized || !m_fbo_state.render_fbo ||
      !m_fbo_state.render_fbo->matches(settings.game_res_w, settings.game_res_h,
                                       settings.msaa_samples)) {
    // doesn't match, set up a new one for these settings
    lg::info("FBO Setup: requested {}x{}, msaa {}", settings.game_res_w, settings.game_res_h,
             settings.msaa_samples);

    // clear old framebuffers
    m_fbo_state.resources.render_buffer.clear();
    m_fbo_state.resources.resolve_buffer.clear();

    // NOTE: we will ALWAYS render the game to a separate framebuffer instead of directly to the
    // window framebuffer.

    // create a fbo to render to, with the desired settings
    m_fbo_state.resources.render_buffer =
        make_fbo(settings.game_res_w, settings.game_res_h, settings.msaa_samples, true);
    m_fbo_state.render_fbo = &m_fbo_state.resources.render_buffer;

    if (settings.msaa_samples != 1) {
      lg::info("FBO Setup: using second temporary buffer: res: {}x{}", settings.game_res_w,
               settings.game_res_h);

      // we'll need a temporary fbo to do the msaa resolve step
      // non-multisampled, and doesn't need z/stencil
      m_fbo_state.resources.resolve_buffer =
          make_fbo(settings.game_res_w, settings.game_res_h, 1, false);
    } else {
      lg::info("FBO Setup: not using second temporary buffer");
    }
  }

  ASSERT_MSG(settings.game_res_w > 0 && settings.game_res_h > 0,
             fmt::format("Bad viewport size from game_res: {}x{}\n", settings.game_res_w,
                         settings.game_res_h));

  ASSERT_MSG(!m_fbo_state.render_fbo->is_window, "window fbo");

  if (m_version == GameVersion::Jak1) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_fbo_state.resources.window.width, m_fbo_state.resources.window.height);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClearDepth(0.0);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClearDepth(0.0);
    glClearStencil(0);
    glDepthMask(GL_TRUE);
    // Note: could rely on sky renderer to clear depth and color, but this causes problems with
    // letterboxing
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // PC-REMAKE 21/09 : le G-buffer de normales s'efface a 0 (alpha 0 = « pas de
    // normale ici », le post retombe alors sur sa reconstruction par profondeur)
    g_pc_normal_target_ok = m_fbo_state.render_fbo->normal_tex_id.has_value();
    if (g_pc_normal_target_ok) {
      const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
      glDrawBuffers(2, both);
      const float zeros[4] = {0.f, 0.f, 0.f, 0.f};
      glClearBufferfv(GL_COLOR, 1, zeros);
      const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
      glDrawBuffers(1, one);
    }
    glDisable(GL_BLEND);
    m_render_state.stencil_dirty = false;
  }
  // jak 2 does the clear in BlitDisplays.cpp

  // setup the draw region to letterbox later
  m_render_state.draw_region_w = settings.draw_region_width;
  m_render_state.draw_region_h = settings.draw_region_height;

  // center the letterbox
  m_render_state.draw_offset_x =
      (settings.window_framebuffer_width - m_render_state.draw_region_w) / 2;
  m_render_state.draw_offset_y =
      (settings.window_framebuffer_height - m_render_state.draw_region_h) / 2;

  m_render_state.render_fb = m_fbo_state.render_fbo->fbo_id;

  if (m_render_state.draw_region_w <= 0 || m_render_state.draw_region_h <= 0) {
    // trying to draw to 0 size region... opengl doesn't like this.
    m_render_state.draw_region_w = 320;
    m_render_state.draw_region_h = 240;
  }

  m_render_state.render_fb_x = 0;
  m_render_state.render_fb_y = 0;
  m_render_state.render_fb_w = settings.game_res_w;
  m_render_state.render_fb_h = settings.game_res_h;
  glViewport(0, 0, settings.game_res_w, settings.game_res_h);
}

// PC-REMAKE 22/09 : instantane de la scene AVANT les sprites 2D / HUD / textes
// (voir pc_prehud_snapshot) — le post (reflets d'eau, rayons) lit cette copie
// pour ne pas refleter les informations a l'ecran.
void pc_prehud_snapshot(GLuint src_fbo, int w, int h, bool multisampled);
GLuint pc_prehud_texture();
// PC-REMAKE 12/10 : bloom large (source prise par Sprite3 avant le HUD)
GLuint pc_bloom_run(GameVersion version,
                    bool has_depth,
                    bool has_normals,
                    bool reload,
                    u64 frame_idx);
GLuint pc_hud_ref_texture();

void OpenGLRenderer::dispatch_buckets_jak1(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  // [QA] 13/10 : meme interrupteur que QA_DISABLE, mais A CHAUD — le contenu de
  // /tmp/qa_disable (motifs separes par des virgules) eteint les renderers dont le
  // nom contient un motif ; fichier absent = tous allumes. Inerte sans le fichier.
  {
    static int qa_dis_poll = 0;
    static std::string qa_dis_spec = "\x01";
    if ((qa_dis_poll++ % 30) == 0) {
      std::string spec;
      if (FILE* f = fopen("/tmp/qa_disable", "r")) {
        char buf[512] = {};
        size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        spec.assign(buf, nread);
        while (!spec.empty() && (spec.back() == '\n' || spec.back() == ' ')) {
          spec.pop_back();
        }
      }
      if (spec != qa_dis_spec && !(qa_dis_spec == "\x01" && spec.empty())) {
        qa_dis_spec = spec;
        for (auto& br : m_bucket_renderers) {
          if (!br) {
            continue;
          }
          const std::string n = br->name();
          bool off = false;
          size_t start = 0;
          while (start <= spec.size() && !spec.empty()) {
            size_t comma = spec.find(',', start);
            std::string pat = spec.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!pat.empty() && n.find(pat) != std::string::npos) {
              off = true;
            }
            if (comma == std::string::npos) {
              break;
            }
            start = comma + 1;
          }
          br->set_enabled(!off);
          if (off) {
            lg::warn("[QA-DISABLE] a chaud : {}", n);
          }
        }
      } else if (qa_dis_spec == "\x01") {
        qa_dis_spec = spec;
      }
    }
  }

  m_render_state.buckets_base =
      dma.current_tag_offset() + 16;  // offset by 1 qw for the initial call
  m_render_state.next_bucket = m_render_state.buckets_base;
  m_render_state.bucket_for_vis_copy = (int)jak1::BucketId::TFRAG_LEVEL0;
  m_render_state.num_vis_to_copy = jak1::LEVEL_MAX;

  // Find the default regs buffer
  auto initial_call_tag = dma.current_tag();
  ASSERT(initial_call_tag.kind == DmaTag::Kind::CALL);
  auto initial_call_default_regs = dma.read_and_advance();
  ASSERT(initial_call_default_regs.transferred_tag == 0);  // should be a nop.
  m_render_state.default_regs_buffer = dma.current_tag_offset();
  auto default_regs_tag = dma.current_tag();
  ASSERT(default_regs_tag.kind == DmaTag::Kind::CNT);
  ASSERT(default_regs_tag.qwc == 10);
  // TODO verify data in here.
  auto default_data = dma.read_and_advance();
  ASSERT(default_data.size_bytes > 148);
  memcpy(m_render_state.fog_color.data(), default_data.data + 144, 4);
  auto default_ret_tag = dma.current_tag();
  ASSERT(default_ret_tag.qwc == 0);
  ASSERT(default_ret_tag.kind == DmaTag::Kind::RET);
  dma.read_and_advance();

  // now we should point to the first bucket!
  ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
  m_render_state.next_bucket += 16;

  // loop over the buckets!
  bool prehud_done = false;
  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    // PC-REMAKE 22/09 : juste avant le bucket des sprites (particules 3D + HUD 2D,
    // suivi des textes), on copie la scene pour le post (reflets sans HUD)
    if (!prehud_done && m_bucket_categories[bucket_id] == BucketCategory::SPRITE &&
        m_fbo_state.render_fbo) {
      prehud_done = true;
      pc_prehud_snapshot(m_fbo_state.render_fbo->fbo_id, m_fbo_state.render_fbo->width,
                         m_fbo_state.render_fbo->height, m_fbo_state.render_fbo->multisampled);
    }
    // lg::info("Render: {} start", g_current_renderer);
    // [QA-GLERR] 14/10 : `touch /tmp/qa_glerr` -> glGetError apres chaque bucket ; les
    // 40 premieres erreurs sont journalisees avec le bucket fautif (inerte sinon).
    static bool qa_glerr = false;
    static int qa_glerr_poll = 0, qa_glerr_logged = 0;
    if (bucket_id == 0 && (qa_glerr_poll++ % 60) == 0) {
      qa_glerr = fs::exists("/tmp/qa_glerr");
      if (qa_glerr) {
        GLenum e0 = glGetError();
        if (e0 != GL_NO_ERROR && qa_glerr_logged < 40) {
          qa_glerr_logged++;
          lg::warn("[QA-GLERR] 0x{:x} AVANT les buckets (frame-setup / loader / image precedente)",
                   (int)e0);
        }
      }
    }
    g_qa_gpu.begin((int)bucket_id, renderer->name_and_id());
    renderer->render(dma, &m_render_state, bucket_prof);
    g_qa_gpu.end();
    if (qa_glerr) {
      GLenum e = glGetError();
      if (e != GL_NO_ERROR && qa_glerr_logged < 40) {
        qa_glerr_logged++;
        lg::warn("[QA-GLERR] 0x{:x} dans {}", (int)e, renderer->name_and_id());
      }
    }
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    vif_interrupt_callback(bucket_id);
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id == 31 - 1 && Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }
  }

  // TODO ending data.
}

void OpenGLRenderer::dispatch_buckets_jak2(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  m_render_state.buckets_base = dma.current_tag_offset();  // starts at 0 in jak 2
  m_render_state.next_bucket = m_render_state.buckets_base + 16;
  m_render_state.bucket_for_vis_copy = (int)jak2::BucketId::BUCKET_2;
  m_render_state.num_vis_to_copy = jak2::LEVEL_MAX;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    // lg::info("Render: {} start", g_current_renderer);
    renderer->render(dma, &m_render_state, bucket_prof);
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    vif_interrupt_callback(bucket_id + 1);
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id + 1 == (int)jak2::BucketId::TEX_L0_ALPHA &&
        Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }
  }
  vif_interrupt_callback(m_bucket_renderers.size());

  // TODO ending data.
}

void OpenGLRenderer::dispatch_buckets_jak3(DmaFollower dma,
                                           ScopedProfilerNode& prof,
                                           bool sync_after_buckets) {
  // The first thing the DMA chain should be a call to a common default-registers chain.
  // this chain resets the state of the GS. After this is buckets
  m_category_times.fill(0);

  m_render_state.buckets_base = dma.current_tag_offset();  // starts at 0 in jak 2
  m_render_state.next_bucket = m_render_state.buckets_base + 16;
  m_render_state.bucket_for_vis_copy = (int)jak3::BucketId::BUCKET_2;
  m_render_state.num_vis_to_copy = jak3::LEVEL_MAX;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    auto bucket_prof = prof.make_scoped_child(renderer->name_and_id());
    g_current_renderer = renderer->name_and_id();
    // lg::info("Render: {} start", g_current_renderer);
    renderer->render(dma, &m_render_state, bucket_prof);
    if (sync_after_buckets) {
      auto pp = scoped_prof("finish");
      glFinish();
    }

    // lg::info("Render: {} end", g_current_renderer);
    //  should have ended at the start of the next chain
    ASSERT(dma.current_tag_offset() == m_render_state.next_bucket);
    m_render_state.next_bucket += 16;
    vif_interrupt_callback(bucket_id + 1);
    m_category_times[(int)m_bucket_categories[bucket_id]] += bucket_prof.get_elapsed_time();

    // hack to draw the collision mesh in the middle the drawing
    if (bucket_id + 1 == (int)jak3::BucketId::TEX_L0_ALPHA &&
        Gfx::g_global_settings.collision_enable) {
      auto p = prof.make_scoped_child("collision-draw");
      m_collide_renderer.render(&m_render_state, p);
    }

    if (bucket_id == (int)jak3::BucketId::TEX_HUD_HUD_ALPHA) {
      auto p = prof.make_scoped_child("color-filter");
      m_blit_displays->apply_color_filter(&m_render_state, p);
    }
  }
  vif_interrupt_callback(m_bucket_renderers.size());

  // TODO ending data.
}

/*!
 * This function finds buckets and dispatches them to the appropriate part.
 */
void OpenGLRenderer::dispatch_buckets(DmaFollower dma,
                                      ScopedProfilerNode& prof,
                                      bool sync_after_buckets) {
  g_current_renderer = "dispatch-buckets pre";

  m_render_state.version = m_version;
  m_render_state.frame_idx++;
  switch (m_version) {
    case GameVersion::Jak1:
      dispatch_buckets_jak1(dma, prof, sync_after_buckets);
      break;
    case GameVersion::Jak2:
      dispatch_buckets_jak2(dma, prof, sync_after_buckets);
      break;
    case GameVersion::Jak3:
    case GameVersion::JakX:
      dispatch_buckets_jak3(dma, prof, sync_after_buckets);
      break;
    default:
      ASSERT(false);
  }

  g_current_renderer = "dispatch-buckets post";
}

#ifdef _WIN32
void win_print_last_error(const std::string& msg) {
  LPSTR lpMsgBuf;

  FormatMessage(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);

  lg::error("[OpenGLRenderer] {} Win Err: {}", msg, lpMsgBuf);
}

void copy_texture_to_clipboard(int width, int height, const std::vector<u32>& texture_data) {
  std::vector<u32> data(texture_data);

  // BGR -> RGB
  for (auto& px : data) {
    u8 r = px >> 0;
    u8 g = px >> 8;
    u8 b = px >> 16;
    u8 a = px >> 24;
    px = (a << 24) | (r << 16) | (g << 8) | (b << 0);
  }

  // Calculate the total size of the image data
  size_t image_size = data.size() * sizeof(u32);

  // BMP/DIB file header
  BITMAPINFOHEADER header;
  header.biSize = sizeof(header);
  header.biWidth = width;
  header.biHeight = height;
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  header.biSizeImage = 0;
  header.biXPelsPerMeter = 0;
  header.biYPelsPerMeter = 0;
  header.biClrUsed = 0;
  header.biClrImportant = 0;

  // Open the clipboard
  if (!OpenClipboard(NULL)) {
    win_print_last_error("Failed to open the clipboard.");
    return;
  }

  // Empty the clipboard
  if (!EmptyClipboard()) {
    win_print_last_error("Failed to empty the clipboard.");
    CloseClipboard();
    return;
  }

  // Create a global memory object to hold the image data
  HGLOBAL hClipboardData = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + image_size);
  if (hClipboardData == NULL) {
    win_print_last_error("Failed to allocate memory for clipboard data.");
    CloseClipboard();
    return;
  }

  // Get a pointer to the global memory object
  void* pData = GlobalLock(hClipboardData);
  if (pData == NULL) {
    win_print_last_error("Failed to lock clipboard memory.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Copy the image data into the global memory object
  memcpy(pData, &header, sizeof(header));
  memcpy((char*)pData + sizeof(header), data.data(), image_size);

  // Unlock the global memory object
  if (!GlobalUnlock(hClipboardData) && GetLastError() != NO_ERROR) {
    win_print_last_error("Failed to unlock memory.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Set the image data to clipboard
  if (!SetClipboardData(CF_DIB, hClipboardData)) {
    win_print_last_error("Failed to set clipboard data.");
    CloseClipboard();
    GlobalFree(hClipboardData);
    return;
  }

  // Close the clipboard
  CloseClipboard();
  GlobalFree(hClipboardData);

  lg::info("Image data copied to clipboard successfully!");
}
#endif

/*!
 * Take a screenshot!
 */
void OpenGLRenderer::finish_screenshot(const std::string& output_name,
                                       int width,
                                       int height,
                                       int x,
                                       int y,
                                       GLuint fbo,
                                       int read_buffer,
                                       bool quick_screenshot) {
  std::vector<u32> buffer(width * height);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  GLint oldbuf, oldreadbuf;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldbuf);
  glGetIntegerv(GL_READ_BUFFER, &oldreadbuf);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glReadBuffer(read_buffer);
  glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());

  // set alpha. our renderers mess this up in a way that isn't relevant to the final framebuffer.
  for (auto& px : buffer) {
    px |= 0xff000000;
  }

#ifdef _WIN32
  if (quick_screenshot) {
    // copy to clipboard (windows only)
    copy_texture_to_clipboard(width, height, buffer);
  }
#else
  (void)quick_screenshot;
  lg::error("screenshot to clipboard NYI non-Windows");
#endif

  // flip upside down in place
  for (int h = 0; h < height / 2; h++) {
    for (int w = 0; w < width; w++) {
      std::swap(buffer[h * width + w], buffer[(height - h - 1) * width + w]);
    }
  }

  file_util::write_rgba_png(output_name, buffer.data(), width, height);
  glReadBuffer(oldreadbuf);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, oldbuf);
}

// PC-REMAKE: defined in background/background_common.cpp
extern float g_post_fx[4];
extern float g_toon_fx[4];
extern float g_depth_fx[4];
extern float g_light_fx[4];
extern float g_grade_fx[4];
extern float g_grade_amt2[4];
extern float g_sky_color[4];
extern float g_ground_color[4];
extern float g_sun_shadow_params[4];
extern float g_night_fx[4];

// PC-REMAKE: lazily-created FBO holding a resolved (non-MSAA) copy of the
// scene depth, sampled by the post shader for SSAO / aerial fog / DOF.
namespace {
// PC-REMAKE 21/09 : resolution (MSAA -> simple) du G-buffer de normales pour le post
struct NormalResolve {
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
};
NormalResolve g_normal_resolve;
// PC-REMAKE 22/09 : copie de la scene avant HUD (meme mecanique que les normales)
NormalResolve g_prehud_resolve;
bool g_prehud_valid = false;
}  // namespace
// [QA-COPIES] 14/10 : temps PROCESSEUR des copies d'image de l'etape des sprites
struct QaCopies {
  double t[3] = {};
  int n = 0;
};
QaCopies g_qa_copies;
double g_qa_copies_depth_ms = 0;
void qa_copies_tick() {
  if (++g_qa_copies.n == 900) {
    lg::info("[QA-COPIES] ms/image CPU : avant sprites {:.3f} | G-buffer HUD {:.3f} (dont "
             "profondeur {:.3f}) | avant HUD {:.3f}",
             g_qa_copies.t[0] / 900, g_qa_copies.t[1] / 900, g_qa_copies_depth_ms / 900,
             g_qa_copies.t[2] / 900);
    g_qa_copies = {};
    g_qa_copies_depth_ms = 0;
  }
}
void pc_prehud_snapshot(GLuint src_fbo, int w, int h, bool multisampled) {
  (void)multisampled;
  Timer qa_t;
  g_prehud_resolve.ensure(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_prehud_resolve.fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  qa_glerr_check("copie avant sprites (pc_prehud_snapshot)");
  glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
  g_prehud_valid = true;
  g_qa_copies.t[0] += qa_t.getMs();
  qa_copies_tick();
}
GLuint pc_prehud_texture() {
  return g_prehud_valid ? g_prehud_resolve.tex : 0;
}

// ===================================================================
// PC-REMAKE 12/10 : BLOOM LARGE (« rendu remake » — sur les images de reference,
// le portail, les spirales vertes, la lave rayonnent d'un grand halo
// doux ; notre bloom d'origine ne portait qu'a 6-14 pixels). Chaine de mips
// 1/2 -> 1/32 : reduction 13 points (tri de ce qui rayonne + Karis au premier
// niveau), remontee en tente additive. Source : la scene APRES les particules du
// monde et AVANT les sprites du HUD (Sprite3::render_jak1) — le HUD et les textes
// ne rayonnent jamais. Dose = curseur Bloom du menu Remake (0 = coupe).
// A/B : `touch /tmp/qa_no_widebloom` le coupe a chaud.
// ===================================================================
namespace {
struct BloomTarget {
  GLuint fbo = 0;
  GLuint tex = 0;
  int w = 0;
  int h = 0;
};
constexpr int kBloomLevels = 5;
BloomTarget g_bloom_src;
BloomTarget g_bloom_lv[kBloomLevels];
bool g_bloom_src_fresh = false;
int g_bloom_age = 1000;  // images depuis le dernier resultat calcule
u64 g_bloom_frame = ~0ull;  // image du dernier calcul (la capture d'ecran rejoue le post)

void bloom_ensure(BloomTarget& b, int w, int h, bool hdr) {
  if (b.tex && b.w == w && b.h == h) {
    return;
  }
  if (b.tex) {
    glDeleteTextures(1, &b.tex);
    glDeleteFramebuffers(1, &b.fbo);
  }
  b.w = w;
  b.h = h;
  glGenTextures(1, &b.tex);
  glBindTexture(GL_TEXTURE_2D, b.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA,
               hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &b.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
  GLenum db = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &db);
}

bool bloom_off_by_file() {
  static int poll = 0;
  static bool off = false;
  if ((poll++ & 31) == 0) {
    off = fs::exists("/tmp/qa_no_widebloom");
  }
  return off;
}
}  // namespace

// Appele par Sprite3 juste avant les sprites du HUD : copie de la scene (resout le
// MSAA). Etat GL rendu tel quel (FBO, texture liee, ciseaux).
extern bool g_pc_normal_target_ok;
// 13/10 : profondeur + normales de la scene SANS le HUD (defini plus bas)
void pc_hud_gbuffer_capture(GLuint src_fbo, int w, int h);

void pc_bloom_snapshot(GLuint src_fbo, int w, int h) {
  if (w > 0 && h > 0) {
    pc_hud_gbuffer_capture(src_fbo, w, h);
  }
  if (w <= 0 || h <= 0 || bloom_off_by_file()) {
    return;
  }
  Timer qa_t;
  GLint prev_draw = 0, prev_read = 0, prev_tex = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  bloom_ensure(g_bloom_src, w, h, false);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  if (scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_bloom_src.fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  qa_glerr_check("copie avant HUD (pc_bloom_snapshot)");
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  g_bloom_src_fresh = true;
  g_qa_copies.t[2] += qa_t.getMs();
}

// Scene sans HUD de CETTE image (0 si les sprites n'ont pas tourne : menu pause,
// chargement — une reference perimee ferait passer tout le decor pour du HUD).
GLuint pc_hud_ref_texture() {
  return (g_bloom_age == 0 && g_bloom_src.tex) ? g_bloom_src.tex : 0;
}

// Calcule la chaine et rend la texture du bloom (niveau 1/2), ou 0. A appeler dans
// le post, profondeur deja liee sur l'unite 15 et normales sur la 13, VAO plein
// ecran lie. Programme, FBO, viewport, ciseaux et melange sont restaures ; la
// texture liee a l'unite 0 ne l'est PAS (l'appelant la relie).
GLuint pc_bloom_run(GameVersion version,
                    bool has_depth,
                    bool has_normals,
                    bool reload,
                    u64 frame_idx) {
  static Shader* s_down = nullptr;
  static Shader* s_up = nullptr;
  if (reload || !s_down) {
    delete s_down;
    delete s_up;
    s_down = new Shader("bloom_down", version);
    s_up = new Shader("bloom_up", version);
  }
  if (!s_down->okay() || !s_up->okay() || bloom_off_by_file()) {
    g_bloom_age = 1000;
    return 0;
  }
  if (!g_bloom_src_fresh) {
    // meme image rejouee (capture d'ecran) : resultat ET reference HUD valides
    if (g_bloom_frame == frame_idx && g_bloom_age == 0 && g_bloom_lv[0].tex) {
      return g_bloom_lv[0].tex;
    }
    // image sans sprites (chargement...) : on garde le dernier resultat un instant
    g_bloom_age++;
    return (g_bloom_age < 8 && g_bloom_lv[0].tex) ? g_bloom_lv[0].tex : 0;
  }
  g_bloom_src_fresh = false;
  g_bloom_age = 0;
  g_bloom_frame = frame_idx;

  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  GLint prev_fbo = 0, prev_prog = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  GLint bsrc = 0, bdst = 0, beq = 0;
  glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
  glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  if (scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  glDisable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);

  const int w = std::max(1, g_bloom_src.w / 2);
  const int h = std::max(1, g_bloom_src.h / 2);
  for (int i = 0; i < kBloomLevels; i++) {
    bloom_ensure(g_bloom_lv[i], std::max(1, w >> i), std::max(1, h >> i), true);
  }

  // 1. reduction (le premier niveau trie ce qui rayonne)
  s_down->activate();
  const GLuint pd = s_down->id();
  glUniform1i(glGetUniformLocation(pd, "src_tex"), 0);
  glUniform1i(glGetUniformLocation(pd, "depth_tex"), 15);
  glUniform1i(glGetUniformLocation(pd, "normal_tex"), 13);
  glUniform1i(glGetUniformLocation(pd, "pre_tex"), 12);
  const float has_pre = pc_prehud_texture() ? 1.f : 0.f;  // lie sur l'unite 12 par le post
  const BloomTarget* src = &g_bloom_src;
  for (int i = 0; i < kBloomLevels; i++) {
    const BloomTarget& dst = g_bloom_lv[i];
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glBindTexture(GL_TEXTURE_2D, src->tex);
    glUniform4f(glGetUniformLocation(pd, "bloom_p"), i == 0 ? 1.f : 0.f, has_depth ? 1.f : 0.f,
                has_normals ? 1.f : 0.f, has_pre);
    glUniform2f(glGetUniformLocation(pd, "src_texel"), 1.f / src->w, 1.f / src->h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    src = &dst;
  }

  // 2. remontee : chaque niveau s'ajoute au niveau plus grand
  s_up->activate();
  const GLuint pu = s_up->id();
  glUniform1i(glGetUniformLocation(pu, "src_tex"), 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  for (int i = kBloomLevels - 1; i > 0; i--) {
    const BloomTarget& s = g_bloom_lv[i];
    const BloomTarget& d = g_bloom_lv[i - 1];
    glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
    glViewport(0, 0, d.w, d.h);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glUniform2f(glGetUniformLocation(pu, "src_texel"), 1.f / s.w, 1.f / s.h);
    glUniform1f(glGetUniformLocation(pu, "up_weight"), 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // 3. etat rendu a l'appelant
  glBlendFunc(bsrc, bdst);
  glBlendEquation(beq);
  if (!blend) {
    glDisable(GL_BLEND);
  }
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glUseProgram(prev_prog);
  return g_bloom_lv[0].tex;
}
namespace {

struct DepthResolve {
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
    // MUST match the render FBO's depth format exactly (GL_DEPTH24_STENCIL8):
    // glBlitFramebuffer refuses a depth blit between different depth formats,
    // and it fails silently, leaving this texture cleared to zero.
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
};
DepthResolve g_depth_resolve;
// 13/10 (textes affiches dans un rectangle transparent) : les
// quads des textes et du HUD ecrivent la PROFONDEUR (et laissent les normales du
// G-buffer indefinies) sur tout leur rectangle. Le post, qui lit profondeur et
// normales APRES le HUD, traitait ce rectangle comme une surface collee a la
// camera : autre eclairage, autre brume -> un cadre visible autour de chaque
// texte. Copie prise JUSTE AVANT les sprites du HUD (Sprite3), utilisee par le
// post a la place.
DepthResolve g_hud_depth;
NormalResolve g_hud_normal;
bool g_hud_gbuf_pending = false;
bool g_hud_gbuf_has_normals = false;
u64 g_hud_gbuf_frame = ~0ull;
}  // namespace

void pc_hud_gbuffer_capture(GLuint src_fbo, int w, int h) {
  Timer qa_t;
  GLint prev_draw = 0, prev_read = 0, prev_tex = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  if (scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  qa_glerr_check("avant capture G-buffer HUD (sprites 3D)");
  g_hud_depth.ensure(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_hud_depth.fbo);
  {
    // PERF 14/10 : PROFONDEUR SEULE — le post ne lit jamais le stencil de cette copie,
    // et le pilote Apple fait la copie profondeur+stencil d'un tampon MSAA par un
    // chemin lent (0,88 ms de processeur par image contre 0,25 ms, mesure au village).
    // (original : GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)
    // `touch /tmp/qa_depth_stencil` = ancien chemin (A/B).
    static bool depth_only = true;
    static int poll = 0;
    if ((poll++ % 64) == 0) {
      depth_only = !fs::exists("/tmp/qa_depth_stencil");
    }
    Timer qa_d;
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                      depth_only ? GL_DEPTH_BUFFER_BIT : (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT),
                      GL_NEAREST);
    g_qa_copies_depth_ms += qa_d.getMs();
  }
  qa_glerr_check("copie profondeur avant HUD");
  g_hud_gbuf_has_normals = g_pc_normal_target_ok;
  if (g_hud_gbuf_has_normals) {
    g_hud_normal.ensure(w, h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_hud_normal.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    qa_glerr_check("copie normales avant HUD");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
  }
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  g_hud_gbuf_pending = true;
  g_qa_copies.t[1] += qa_t.getMs();
}

// PERF 14/10 : POST EN DEUX PASSES (voir post_processing.frag, POST_PASS1/2). Chaque
// reglage du post est envoye au programme courant ET, en mode scinde, au programme de
// la 2e passe (glProgramUniform, GL 4.1). `touch /tmp/qa_post_single` = passe unique
// d'origine (A/B).
namespace {
GLuint g_post_prog_a = 0;  // programme qui recoit les reglages (passe unique ou passe 1)
GLuint g_post_prog_b = 0;  // passe 2 (0 = pas de passe 2)
Shader g_post_p1, g_post_p2;
bool g_post_split_built = false;
GLuint g_lit_fbo = 0, g_lit_tex = 0;
int g_lit_w = 0, g_lit_h = 0;
void ensure_lit_target(int w, int h) {
  if (g_lit_fbo && g_lit_w == w && g_lit_h == h) {
    return;
  }
  if (g_lit_fbo) {
    glDeleteFramebuffers(1, &g_lit_fbo);
    glDeleteTextures(1, &g_lit_tex);
  }
  g_lit_w = w;
  g_lit_h = h;
  GLint prev_tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGenTextures(1, &g_lit_tex);
  glBindTexture(GL_TEXTURE_2D, g_lit_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  GLint prev_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  glGenFramebuffers(1, &g_lit_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g_lit_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lit_tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
}
}  // namespace
#define POST_U(suffix, name, ...)                                                   \
  do {                                                                              \
    glProgramUniform##suffix(g_post_prog_a, pc_uniform_loc(g_post_prog_a, name),    \
                             __VA_ARGS__);                                          \
    if (g_post_prog_b) {                                                            \
      glProgramUniform##suffix(g_post_prog_b, pc_uniform_loc(g_post_prog_b, name),  \
                               __VA_ARGS__);                                        \
    }                                                                               \
  } while (0)

void OpenGLRenderer::do_pcrtc_effects(float alp,
                                      int brightness_contrast_color,
                                      int brightness_contrast_alpha,
                                      SharedRenderState* render_state,
                                      ScopedProfilerNode& prof,
                                      unsigned int dst_fbo,
                                      int vp_w,
                                      int vp_h) {
  Fbo* window_blit_src = nullptr;
  // [QA-GPU] 14/10 : sous-passes du post (seulement la passe ecran, pas la capture)
  const bool qa_gpu_here = dst_fbo == 0;
  if (qa_gpu_here) {
    g_qa_gpu.begin(84, "post: setup + resolution MSAA couleur");
  }
  if (m_fbo_state.resources.resolve_buffer.valid) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_state.resources.resolve_buffer.fbo_id);
    glBlitFramebuffer(0,                                            // srcX0
                      0,                                            // srcY0
                      m_fbo_state.render_fbo->width,                // srcX1
                      m_fbo_state.render_fbo->height,               // srcY1
                      0,                                            // dstX0
                      0,                                            // dstY0
                      m_fbo_state.resources.resolve_buffer.width,   // dstX1
                      m_fbo_state.resources.resolve_buffer.height,  // dstY1
                      GL_COLOR_BUFFER_BIT,                          // mask
                      GL_LINEAR                                     // filter
    );
    window_blit_src = &m_fbo_state.resources.resolve_buffer;
  } else {
    window_blit_src = &m_fbo_state.resources.render_buffer;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  // PERF 25/08 : le post rendait TOUJOURS a la resolution de la FENETRE (8,4
  // Mpx retina) — la resolution interne n'y changeait rien. Quand le jeu rend
  // plus petit que la zone d'affichage, le post rend desormais dans un FBO a la
  // resolution du JEU puis un blit lineaire (quasi gratuit) monte vers la
  // fenetre. A resolution native : chemin d'origine strictement inchange.
  static Fbo s_post_fbo;
  const int post_game_w = m_fbo_state.render_fbo->width;
  const int post_game_h = m_fbo_state.render_fbo->height;
  const bool use_post_fbo =
      (vp_w <= 0 && vp_h <= 0 && post_game_w < render_state->draw_region_w);
  if (use_post_fbo && !s_post_fbo.matches(post_game_w, post_game_h, 1)) {
    s_post_fbo.clear();
    s_post_fbo = make_fbo(post_game_w, post_game_h, 1, false);
  }
  const GLuint post_target = use_post_fbo ? s_post_fbo.fbo_id : dst_fbo;
  if (use_post_fbo) {
    glViewport(0, 0, post_game_w, post_game_h);
  } else if (vp_w > 0 && vp_h > 0) {
    glViewport(0, 0, vp_w, vp_h);
  } else {
    glViewport(render_state->draw_offset_x, render_state->draw_offset_y,
               render_state->draw_region_w, render_state->draw_region_h);
  }
  glBindTexture(GL_TEXTURE_2D, *window_blit_src->tex_id);
  glBindFramebuffer(GL_FRAMEBUFFER, post_target);

  glBindVertexArray(screen_vao);
  glBindBuffer(GL_ARRAY_BUFFER, screen_vbo);

  float color = (float)brightness_contrast_color / 128.0f;
  float alpha = (float)brightness_contrast_alpha / 128.0f;
  // [QA] 12/10 (outillage) : rechargement a chaud du post — `touch /tmp/pc_reload_post`
  // recompile post_processing.frag sans relancer le jeu. Inerte sans le fichier ; un
  // shader qui ne compile pas laisse l'ancien en place (erreur dans le journal).
  bool post_reloaded = false;  // 12/10 : les shaders du bloom large suivent
  {
    static int s_reload_poll = 0;
    if (++s_reload_poll >= 30) {
      s_reload_poll = 0;
      if (FILE* rf = fopen("/tmp/pc_reload_post", "r")) {
        fclose(rf);
        remove("/tmp/pc_reload_post");
        post_reloaded = true;
        Shader fresh("post_processing", render_state->version);
        if (fresh.okay()) {
          render_state->shaders[ShaderId::POST_PROCESSING] = fresh;
          lg::info("[QA] post_processing recharge");
        } else {
          lg::error("[QA] post_processing : echec de compilation, ancien shader garde");
        }
      }
      // [QA] 17/10 : meme chose pour la couleur de l'eau — `touch /tmp/pc_reload_ocean`
      // (l'ocean relit ses uniformes a chaque image, aucun etat a reconstruire).
      if (FILE* rf = fopen("/tmp/pc_reload_ocean", "r")) {
        fclose(rf);
        remove("/tmp/pc_reload_ocean");
        Shader fresh("ocean_common", render_state->version);
        if (fresh.okay()) {
          render_state->shaders[ShaderId::OCEAN_COMMON] = fresh;
          lg::info("[QA] ocean_common recharge");
        } else {
          lg::error("[QA] ocean_common : echec de compilation, ancien shader garde");
        }
      }
    }
  }
  // 14/10 : programmes de la version en DEUX PASSES (construits une fois, et a chaque
  // rechargement a chaud du post). Echec de compilation = passe unique d'origine.
  static bool post_single = false;
  {
    static int poll = 0;
    if ((poll++ % 64) == 0) {
      post_single = fs::exists("/tmp/qa_post_single");
    }
  }
  if (!g_post_split_built || post_reloaded) {
    g_post_split_built = true;
    Shader p1("post_processing", render_state->version, "POST_PASS1");
    Shader p2("post_processing", render_state->version, "POST_PASS2");
    if (p1.okay() && p2.okay()) {
      g_post_p1 = p1;
      g_post_p2 = p2;
    } else {
      lg::error("[QA] post en deux passes : echec de compilation, passe unique");
      g_post_p1 = Shader();
      g_post_p2 = Shader();
    }
  }
  const bool post_split = !post_single && g_post_p1.okay() && g_post_p2.okay();
  // (original : auto& shader = render_state->shaders[ShaderId::POST_PROCESSING];)
  auto& shader = post_split ? g_post_p1 : render_state->shaders[ShaderId::POST_PROCESSING];
  g_post_prog_a = shader.id();
  g_post_prog_b = post_split ? g_post_p2.id() : 0;
  shader.activate();
  POST_U(1i, "tex_T0", 0);
  if (brightness_contrast_color < 0) {
    // subtractive blend - note that color is already negative
    float color_neg = color * alpha;
    POST_U(4f, "color_mult", 1.0f, 1.0f, 1.0f, alpha);
    POST_U(4f, "color_add", color_neg, color_neg, color_neg,
                0.0f);
  } else {
    // additive blend
    POST_U(4f, "color_mult", 1.0f, 1.0f, 1.0f, alpha);
    POST_U(4f, "color_add", color, color, color, 0.0f);
  }
  // PC-REMAKE: bloom / saturation / warmth, driven from GOAL via pc-set-post-fx!
  // (storage in background_common.cpp; all values neutral at zero).
  POST_U(4fv, "post_fx", 1, g_post_fx);
  POST_U(4fv, "fx_quality", 1, g_fx_quality);
  POST_U(4fv, "toon_fx", 1, g_toon_fx);

  // PC-REMAKE rebuilt lighting rig: feed the sun's on-screen position (needed
  // for god rays and for the directional relight) plus the ambient palette.
  {
    float sun_screen[4] = {0.5f, 1.2f, 0.f, 0.f};
    float sx = g_sun_shadow_params[0], sy = g_sun_shadow_params[1],
          sz = g_sun_shadow_params[2];
    float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len > 1e-4f && m_render_state.has_pc_data) {
      // direction TO the sun (the parameters describe where its light travels)
      float ux = -sx / len, uy = -sy / len, uz = -sz / len;
      sun_screen[3] = std::max(0.f, uy);  // altitude: 0 at the horizon, 1 overhead
      // project a point far along that direction, with the SAME matrix the post
      // uses to unproject the depth buffer (g_clip_from_world = the PC shaders'
      // camera). 22/09 : the old code used m_render_state.camera_matrix (the
      // game's own matrix, not the one the shaders draw with) — sun_screen came
      // out at (1110, 1122) and the post's sun disc, god rays and flares were
      // NEVER drawn (sun_screen.z stayed 0).
      const float kFar = 4096.f * 4000.f;
      float vx = ux * kFar, vy = uy * kFar, vz = uz * kFar;
      const float* G = g_clip_from_world;
      float cx = G[0] * vx + G[4] * vy + G[8] * vz + G[12];
      float cy = G[1] * vx + G[5] * vy + G[9] * vz + G[13];
      float cw = G[3] * vx + G[7] * vy + G[11] * vz + G[15];
      if (cw > 1e-3f) {
        float ndc_x = cx / cw, ndc_y = cy / cw;
        sun_screen[0] = ndc_x * 0.5f + 0.5f;
        sun_screen[1] = ndc_y * 0.5f + 0.5f;
        bool on_screen = sun_screen[0] > -0.35f && sun_screen[0] < 1.35f &&
                         sun_screen[1] > -0.35f && sun_screen[1] < 1.35f;
        sun_screen[2] = on_screen ? 1.f : 0.f;
      }
    }
    // direction TOWARDS the sun, for the deferred N.L pass
    {
      float sd[4] = {0.f, 1.f, 0.f, 0.f};
      if (len > 1e-4f) {
        sd[0] = -sx / len;
        sd[1] = -sy / len;
        sd[2] = -sz / len;
        sd[3] = sd[1] > 0.02f ? 1.f : 0.f;  // light must travel downwards
      }
      POST_U(4fv, "sun_dir", 1, sd);
    }
    POST_U(4fv, "light_fx", 1, g_light_fx);
    POST_U(4fv, "grade_fx", 1, g_grade_fx);
    POST_U(4fv, "grade_amt2", 1, g_grade_amt2);
    POST_U(4fv, "sun_screen", 1, sun_screen);
    POST_U(4fv, "sky_color", 1, g_sky_color);
    POST_U(4fv, "ground_color", 1, g_ground_color);

    // Inverse of the exact background vertex transform, so the post pass can
    // rebuild a world position per pixel — what makes local lamps possible.
    float Ainv[16];
    if (g_world_from_clip_ok) {
      for (int i = 0; i < 16; i++) {
        Ainv[i] = g_world_from_clip[i];
      }
    } else {
      for (int i = 0; i < 16; i++) {
        Ainv[i] = (i % 5 == 0) ? 1.f : 0.f;
      }
    }
    POST_U(Matrix4fv, "inv_camera", 1, GL_FALSE, Ainv);
    POST_U(Matrix4fv, "clip_from_world", 1, GL_FALSE,
                       g_clip_from_world);
    POST_U(4f, "cam_trans", m_render_state.camera_pos[0],
                m_render_state.camera_pos[1], m_render_state.camera_pos[2], 0.f);

    // lamps collected this frame, narrowed down to the ones nearest the camera
    pc_remake_select_lamps(m_render_state.camera_pos.data());
    int lamp_n_all = pc_remake_lamp_count();
    // PERF 14/10 : TRI DES LAMPES pour le post (image identique). Le shader testait
    // les 48 lampes pour CHAQUE pixel (1,15 ms de GPU en plein jour au village, ou
    // presque aucune n'eclaire). On ne lui envoie que les lampes qui peuvent toucher
    // un pixel visible : eclairage possible (presence * intensite >= 0,001, memes
    // regles que le shader) ET sphere de portee dans le champ, OU halo possible
    // (memes portes que le shader). Le rang d'origine suit (lamp_orig) : le
    // vacillement des feux depend de lui. /tmp/qa_no_lamp_cull = toutes (A/B).
    static float c_pos[48 * 4], c_col[48 * 4], c_scr[48 * 4], c_orig[48], c_rect[48 * 4];
    int lamp_n = 0;
    {
      static bool lamp_cull = true;
      static int lamp_cull_poll = 0;
      if ((lamp_cull_poll++ % 64) == 0) {
        lamp_cull = !fs::exists("/tmp/qa_no_lamp_cull");
      }
      const float* lp = pc_remake_lamp_pos();
      const float* lc = pc_remake_lamp_color();
      const float* G = g_clip_from_world;
      const float camx = m_render_state.camera_pos[0];
      const float camy = m_render_state.camera_pos[1];
      const float camz = m_render_state.camera_pos[2];
      const float night = std::clamp(g_night_fx[0], 0.f, 1.f);
      const float interior = std::clamp(g_ground_color[3], 0.f, 1.f);
      const float hut = std::clamp(g_grade_amt2[3], 0.f, 1.f);
      const float lamp_hour = std::max(night, interior);
      const float glow_gate = std::max(night, hut) * std::clamp(g_night_fx[2], 0.f, 2.f);
      // plans du champ (lignes de clip_from_world, colonne majeure) : w+x, w-x, w+y,
      // w-y, et w (devant la camera)
      float planes[5][4];
      for (int k = 0; k < 4; k++) {
        const float r0 = G[k * 4 + 0], r1 = G[k * 4 + 1], r3 = G[k * 4 + 3];
        planes[0][k] = r3 + r0;
        planes[1][k] = r3 - r0;
        planes[2][k] = r3 + r1;
        planes[3][k] = r3 - r1;
        planes[4][k] = r3;
      }
      for (int i = 0; i < lamp_n_all && i < 48; i++) {
        const float inten = lc[i * 4 + 3];
        const bool world_em = inten < -8.f;
        const bool fixture = inten < -40.f;
        const bool eco = inten < 0.f && !world_em;
        // portee (memes regles que le shader)
        float reach = (inten < 0.f) ? 1.f : std::max(g_night_fx[1], 0.05f);
        if (inten >= 0.f) {
          reach = reach + (1.f - reach) * hut;
        }
        const float r = std::max(lp[i * 4 + 3] * reach, 1.f);
        // presence * intensite (pour le feu : toujours > 0)
        float pi;
        if (fixture) {
          pi = (-inten - 64.f) * (0.30f + 0.70f * lamp_hour);
        } else if (world_em) {
          pi = 1.f;
        } else if (eco) {
          pi = -inten * std::max(g_night_fx[2], 1.2f) * 0.45f * (0.85f + 0.15f * lamp_hour);
        } else {
          pi = lamp_hour * inten * std::max(g_night_fx[2], 0.f);
        }
        bool light = pi >= 0.001f;
        if (light) {
          const float vx = lp[i * 4 + 0] - camx, vy = lp[i * 4 + 1] - camy,
                      vz = lp[i * 4 + 2] - camz;
          for (auto& pl : planes) {
            const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
            const float d = pl[0] * vx + pl[1] * vy + pl[2] * vz + pl[3];
            if (len > 0.f && d < -(r * 1.02f + 4096.f) * len) {
              light = false;  // sphere de portee entierement hors du champ
              break;
            }
          }
        }
        // halo : projection ecran de la lampe, memes formules que l'ancien code du
        // shader (cadre elargi -0,3..1,3, rayon angulaire, fondu 30-85 m)
        float scr[4];
        {
          const float vx = lp[i * 4 + 0] - camx, vy = lp[i * 4 + 1] - camy,
                      vz = lp[i * 4 + 2] - camz;
          scr[0] = scr[1] = scr[2] = 0.f;
          scr[3] = -1.f;
          const float cx = G[0] * vx + G[4] * vy + G[8] * vz + G[12];
          const float cy = G[1] * vx + G[5] * vy + G[9] * vz + G[13];
          const float cw = G[3] * vx + G[7] * vy + G[11] * vz + G[15];
          if (cw >= 1e-5f) {
            const float u = (cx / cw) * 0.5f + 0.5f, v = (cy / cw) * 0.5f + 0.5f;
            if (u >= -0.3f && u <= 1.3f && v >= -0.3f && v <= 1.3f) {
              const float ldist = std::sqrt(vx * vx + vy * vy + vz * vz);
              const float rad =
                  std::clamp((4096.f * 3.2f) / std::max(ldist, 1.f), 0.006f, 0.075f);
              float t = std::clamp((ldist - 4096.f * 30.f) / (4096.f * 55.f), 0.f, 1.f);
              const float fade = 1.f - t * t * (3.f - 2.f * t);
              if (fade >= 0.01f) {
                scr[0] = u;
                scr[1] = v;
                scr[2] = rad;
                scr[3] = fade;
              }
            }
          }
        }
        const bool halo = scr[3] >= 0.f && (world_em || eco || glow_gate > 0.f);
        if (lamp_cull && !light && !halo) {
          continue;
        }
        for (int k = 0; k < 4; k++) {
          c_pos[lamp_n * 4 + k] = lp[i * 4 + k];
          c_col[lamp_n * 4 + k] = lc[i * 4 + k];
          c_scr[lamp_n * 4 + k] = scr[k];
        }
        c_orig[lamp_n] = (float)i;
        // 14/10 : rectangle ECRAN (uv) ou la lampe peut agir — eclairage (projection
        // de la boite de sa sphere de portee) + halo et reflets fantomes. Sert aux
        // tuiles (lamp_tiles, plus bas). Conservateur : un coin derriere la camera
        // = tout l'ecran.
        {
          float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
          auto add = [&](float ax, float ay, float bx, float by) {
            x0 = std::min(x0, ax);
            y0 = std::min(y0, ay);
            x1 = std::max(x1, bx);
            y1 = std::max(y1, by);
          };
          if (light) {
            const float cx0 = lp[i * 4 + 0] - camx, cy0 = lp[i * 4 + 1] - camy,
                        cz0 = lp[i * 4 + 2] - camz;
            const float rr = r * 1.02f + 4096.f;
            bool behind = false;
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (int c = 0; c < 8 && !behind; c++) {
              const float vx = cx0 + ((c & 1) ? rr : -rr), vy = cy0 + ((c & 2) ? rr : -rr),
                          vz = cz0 + ((c & 4) ? rr : -rr);
              const float px = G[0] * vx + G[4] * vy + G[8] * vz + G[12];
              const float py = G[1] * vx + G[5] * vy + G[9] * vz + G[13];
              const float pw = G[3] * vx + G[7] * vy + G[11] * vz + G[15];
              if (pw < 1e-3f) {
                behind = true;
                break;
              }
              const float u = (px / pw) * 0.5f + 0.5f, v = (py / pw) * 0.5f + 0.5f;
              u0 = std::min(u0, u);
              v0 = std::min(v0, v);
              u1 = std::max(u1, u);
              v1 = std::max(v1, v);
            }
            if (behind) {
              add(-1.f, -1.f, 2.f, 2.f);
            } else {
              add(u0, v0, u1, v1);
            }
          }
          if (scr[3] >= 0.f) {
            const float aspect = (float)m_fbo_state.render_fbo->width /
                                 std::max(1.f, (float)m_fbo_state.render_fbo->height);
            const float u = scr[0], v = scr[1], rad = scr[2];
            // disque du halo (rayon rad en hauteur, rad/aspect en largeur)
            add(u - rad / aspect, v - rad, u + rad / aspect, v + rad);
            // deux reflets fantomes sur l'axe lampe -> centre (k = 0,45 et 1,05)
            const float gscale = std::clamp(rad / 0.03f, 0.5f, 1.5f);
            for (int g = 0; g < 2; g++) {
              const float k = g == 0 ? 0.45f : 1.05f;
              const float grad = (g == 0 ? 0.020f : 0.034f) * gscale;
              const float gu = u + (0.5f - u) * k, gv = v + (0.5f - v) * k;
              add(gu - grad / aspect, gv - grad, gu + grad / aspect, gv + grad);
            }
          }
          c_rect[lamp_n * 4 + 0] = x0;
          c_rect[lamp_n * 4 + 1] = y0;
          c_rect[lamp_n * 4 + 2] = x1;
          c_rect[lamp_n * 4 + 3] = y1;
        }
        lamp_n++;
      }
      static int qa_n = 0;
      if ((qa_n++ % 600) == 0) {
        lg::info("[QA-LAMPES] post : {} lampes envoyees sur {}", lamp_n, lamp_n_all);
      }
    }
    float night_fx[4] = {g_night_fx[0], g_night_fx[1], g_night_fx[2], (float)lamp_n};
    POST_U(4fv, "night_fx", 1, night_fx);
    // scintillement de l'eau : niveau de la mer + forces jour/nuit + temps
    float wg[4] = {g_water_glitter[0], g_water_glitter[1], g_water_glitter[2],
                   (float)(m_render_state.frame_idx % 216000) / 60.f};
    POST_U(4fv, "water_glitter", 1, wg);
    // 17/10 : la surface de l'eau a deja pose sa brume cette image (CommonOceanRenderer)
    {
      extern u64 g_water_murk_frame;
      POST_U(1f, "water_murk_ocean", g_water_murk_frame == m_render_state.frame_idx ? 1.f : 0.f);
    }
    // [QA-GLITTER] 25/08 : preuve de la COURSE entre threads — main.gc desarme
    // chaque frame GOAL, draw-ocean re-arme ; si le rendu lit ENTRE les deux,
    // la frame se peint sans aucun effet d'eau (l'eau disparaissait). Compte
    // ce que le rendu LIT reellement. A retirer une fois l'eau reglee.
    {
      static int qa_wg_arm = 0, qa_wg_dis = 0, qa_wg_n = 0;
      if (wg[1] > 0.001f || wg[2] > 0.001f) {
        qa_wg_arm++;
      } else {
        qa_wg_dis++;
      }
      if (++qa_wg_n >= 300) {
        lg::info("[QA-GLITTER] lus par le rendu : {} armes / {} DESARMES sur 300", qa_wg_arm,
                 qa_wg_dis);
        qa_wg_n = qa_wg_arm = qa_wg_dis = 0;
      }
    }
    if (lamp_n > 0) {
      // note: name the first element explicitly — safer across drivers than
      // relying on the bare array name resolving to element 0
      // 14/10 : listes TRIEES (voir plus haut) — original : pc_remake_lamp_pos() /
      // pc_remake_lamp_color() en entier
      POST_U(4fv, "lamp_pos[0]", lamp_n, c_pos);
      POST_U(4fv, "lamp_color[0]", lamp_n, c_col);
      POST_U(1fv, "lamp_orig[0]", lamp_n, c_orig);
      // PERF 14/10 : TUILES DE LAMPES — 16 x 9 tuiles d'ecran, chacune avec le masque
      // (48 bits) des lampes dont le rectangle d'action la touche. Le shader ne
      // parcourt que ces lampes : ailleurs leur contribution valait exactement 0.
      // /tmp/qa_no_lamp_cull = toutes les lampes partout (A/B).
      {
        constexpr int TX = 16, TY = 9;
        static GLuint tiles[TX * TY * 2];
        static bool all_tiles = false;
        static int all_poll = 0;
        if ((all_poll++ % 64) == 0) {
          all_tiles = fs::exists("/tmp/qa_no_lamp_cull");
        }
        for (int t = 0; t < TX * TY * 2; t++) {
          tiles[t] = 0;
        }
        for (int l = 0; l < lamp_n; l++) {
          int tx0 = 0, ty0 = 0, tx1 = TX - 1, ty1 = TY - 1;
          if (!all_tiles) {
            const float* rc = &c_rect[l * 4];
            if (rc[0] > 1.f || rc[1] > 1.f || rc[2] < 0.f || rc[3] < 0.f) {
              continue;  // rien a l'ecran
            }
            tx0 = std::clamp((int)std::floor(rc[0] * TX), 0, TX - 1);
            ty0 = std::clamp((int)std::floor(rc[1] * TY), 0, TY - 1);
            tx1 = std::clamp((int)std::floor(rc[2] * TX), 0, TX - 1);
            ty1 = std::clamp((int)std::floor(rc[3] * TY), 0, TY - 1);
          }
          for (int ty = ty0; ty <= ty1; ty++) {
            for (int tx = tx0; tx <= tx1; tx++) {
              tiles[(ty * TX + tx) * 2 + (l >> 5)] |= 1u << (l & 31);
            }
          }
        }
        POST_U(2uiv, "lamp_tiles[0]", TX * TY, tiles);
      }
      // PERF 14/10 : position ECRAN de chaque lampe (uv, rayon du halo, fondu ; w < 0 =
      // sautee) calculee une fois par image dans le tri ci-dessus (c_scr) — le post la
      // recalculait pour chacun des 2 millions de pixels.
      POST_U(4fv, "lamp_scr[0]", lamp_n, c_scr);
    }
  }
  // PC-REMAKE: depth-aware post (SSAO / aerial fog / DOF). Resolve the scene
  // depth (possibly MSAA) into a texture and bind it on unit 15.
  POST_U(4fv, "depth_fx", 1, g_depth_fx);
  // 13/10 : G-buffer d'avant le HUD (voir g_hud_depth) valable pour CETTE image —
  // la capture d'ecran rejoue le post dans la meme image : meme copie
  if (g_hud_gbuf_pending) {
    g_hud_gbuf_pending = false;
    g_hud_gbuf_frame = render_state->frame_idx;
  }
  const bool hud_gbuf = g_hud_gbuf_frame == render_state->frame_idx &&
                        g_hud_depth.w == m_fbo_state.render_fbo->width &&
                        g_hud_depth.h == m_fbo_state.render_fbo->height;
  if (qa_gpu_here) {
    g_qa_gpu.end();
    g_qa_gpu.begin(85, "post: resolutions profondeur + normales");
  }
  if (g_depth_fx[0] > 0.001f || g_depth_fx[1] > 0.001f || g_depth_fx[2] > 0.001f) {
    int src_w = m_fbo_state.render_fbo->width;
    int src_h = m_fbo_state.render_fbo->height;
    GLuint dtex = g_hud_depth.tex;
    if (!hud_gbuf) {
      g_depth_resolve.ensure(src_w, src_h);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_depth_resolve.fbo);
      // 14/10 : profondeur seule (voir pc_hud_gbuffer_capture) —
      // original : GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT
      glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, src_w, src_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, post_target);  // back to OUR target, not always the window
      dtex = g_depth_resolve.tex;
    }
    glActiveTexture(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_2D, dtex);
    glActiveTexture(GL_TEXTURE0);
    POST_U(1i, "depth_tex", 15);
  }
  // PC-REMAKE 21/09 : G-buffer de normales -> unite 13 (normal_tex). Avec MSAA la
  // texture est multi-echantillons : on la resout par blit de l'attachement 1.
  float bloom_has_normals = 0.f;  // 12/10 : le bloom large lit aussi les drapeaux
  {
    float has_normals = 0.f;
    if (m_fbo_state.render_fbo->normal_tex_id.has_value()) {
      int src_w = m_fbo_state.render_fbo->width;
      int src_h = m_fbo_state.render_fbo->height;
      GLuint ntex = 0;
      if (hud_gbuf && g_hud_gbuf_has_normals) {
        ntex = g_hud_normal.tex;  // 13/10 : normales d'avant le HUD
      } else if (m_fbo_state.render_fbo->multisampled) {
        g_normal_resolve.ensure(src_w, src_h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_normal_resolve.fbo);
        glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, src_w, src_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_state.render_fbo->fbo_id);
        glReadBuffer(GL_COLOR_ATTACHMENT0);  // ne pas laisser l'etat de lecture sur l'att. 1
        glBindFramebuffer(GL_FRAMEBUFFER, post_target);
        ntex = g_normal_resolve.tex;
      } else {
        ntex = m_fbo_state.render_fbo->normal_tex_id.value();
      }
      glActiveTexture(GL_TEXTURE13);
      glBindTexture(GL_TEXTURE_2D, ntex);
      glActiveTexture(GL_TEXTURE0);
      POST_U(1i, "normal_tex", 13);
      has_normals = 1.f;
    }
    // PC-REMAKE 22/09 : scene sans HUD pour les reflets d'eau et les rayons
    {
      GLuint pt = pc_prehud_texture();
      glActiveTexture(GL_TEXTURE12);
      glBindTexture(GL_TEXTURE_2D, pt ? pt : 0);
      glActiveTexture(GL_TEXTURE0);
      POST_U(1i, "scene_tex", 12);
      POST_U(1f, "has_scene_tex", pt ? 1.f : 0.f);
    }
    // [QA] QA_NO_GBUFFER=1 : le post ignore le G-buffer (A/B contre la reconstruction)
    static const bool qa_no_gb = [] {
      const char* e = getenv("QA_NO_GBUFFER");
      return e && e[0] == '1';
    }();
    if (qa_no_gb) {
      has_normals = 0.f;
    }
    POST_U(1f, "has_normal_tex", has_normals);
    bloom_has_normals = has_normals;
    static const float qa_show_env = [] {
      const char* e = getenv("QA_SHOW_NORMALS");
      return (e && e[0] == '1') ? 1.f : 0.f;
    }();
    // 21/09 : bascule A CHAUD par fichier (touch /tmp/qa_show_normals), scrutee
    // toutes les 32 images — evite une relance du jeu pour un controle visuel
    static int qa_show_poll = 0;
    static float qa_show_file = 0.f;
    if ((qa_show_poll++ & 31) == 0) {
      qa_show_file = fs::exists("/tmp/qa_show_normals") ? 1.f : 0.f;
    }
    POST_U(1f, "qa_show_normals",
                std::max(qa_show_env, qa_show_file));
  }
  // PC-REMAKE 12/10 : BLOOM LARGE (voir pc_bloom_run) -> unite 11 (bloom_tex)
  {
    const bool bloom_has_depth =
        g_depth_fx[0] > 0.001f || g_depth_fx[1] > 0.001f || g_depth_fx[2] > 0.001f;
    if (qa_gpu_here) {
      g_qa_gpu.end();
      g_qa_gpu.begin(86, "post: bloom large (chaine de mips)");
    }
    GLuint btex = pc_bloom_run(render_state->version, bloom_has_depth, bloom_has_normals > 0.5f,
                               post_reloaded, render_state->frame_idx);
    if (qa_gpu_here) {
      g_qa_gpu.end();
    }
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D, btex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *window_blit_src->tex_id);  // la chaine a pris l'unite 0
    POST_U(1i, "bloom_tex", 11);
    POST_U(1f, "has_bloom_tex", btex ? 1.f : 0.f);
    // HUD intact : la scene sans HUD de cette image -> unite 10
    GLuint href = pc_hud_ref_texture();
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, href);
    glActiveTexture(GL_TEXTURE0);
    POST_U(1i, "hud_ref_tex", 10);
    POST_U(1f, "has_hud_ref", href ? 1.f : 0.f);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glActiveTexture(GL_TEXTURE0);
  if (qa_gpu_here) {
    g_qa_gpu.begin(87, "post: shader principal");
  }
  if (post_split) {
    // passe 1 -> texture flottante a la taille de la cible du post
    GLint cur_fbo = 0;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    ensure_lit_target(vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, g_lit_fbo);
    glViewport(0, 0, vp[2], vp[3]);
    shader.activate();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // passe 2 -> cible d'origine
    glBindFramebuffer(GL_FRAMEBUFFER, cur_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    g_post_p2.activate();
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, g_lit_tex);
    glActiveTexture(GL_TEXTURE0);
    glProgramUniform1i(g_post_p2.id(), pc_uniform_loc(g_post_p2.id(), "lit_tex"), 9);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (qa_gpu_here) {
    g_qa_gpu.end();
    g_qa_gpu.begin(88, "post: copie vers la fenetre + fin");
  }

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  // PERF 25/08 : remontee du FBO post (resolution jeu) vers la fenetre
  if (use_post_fbo) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_post_fbo.fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glBlitFramebuffer(0, 0, post_game_w, post_game_h, render_state->draw_offset_x,
                      render_state->draw_offset_y,
                      render_state->draw_offset_x + render_state->draw_region_w,
                      render_state->draw_offset_y + render_state->draw_region_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(render_state->draw_offset_x, render_state->draw_offset_y,
               render_state->draw_region_w, render_state->draw_region_h);
  }

  glEnable(GL_BLEND);
  if (alp < 1) {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);

    m_blackout_renderer.draw(Vector4f(0, 0, 0, 1.f - alp), render_state, prof);
  }
  glEnable(GL_DEPTH_TEST);
  if (qa_gpu_here) {
    g_qa_gpu.end();
  }
}
