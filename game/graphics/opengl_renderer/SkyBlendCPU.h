#pragma once

#include "common/dma/dma_chain_read.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SkyBlendCommon.h"
#include "game/graphics/pipelines/opengl.h"

// declared here so the tests can call them
void blend_sky_initial_fast(u8 intensity, u8* out, const u8* in, u32 size);
void blend_sky_fast(u8 intensity, u8* out, const u8* in, u32 size);

class SkyBlendCPU {
 public:
  SkyBlendCPU();
  ~SkyBlendCPU();

  SkyBlendStats do_sky_blends(DmaFollower& dma,
                              SharedRenderState* render_state,
                              ScopedProfilerNode& prof);
  void init_textures(TexturePool& tex_pool, GameVersion version);

 private:
  static constexpr int m_sizes[2] = {32, 64};
  std::vector<u8> m_texture_data[2];

  struct TexInfo {
    GLuint gl;
    u32 tbp;
    GpuTexture* tex;
  } m_textures[2];

  // PC-REMAKE 14/10 : TROIS textures GL par tampon (ciel, nuages), utilisees a tour
  // de role. Reecrire par glTexImage2D une texture que le GPU lit encore (le ciel de
  // l'image en vol) forcait le pilote Apple a faire ATTENDRE le processeur : 3 a
  // 4,5 ms par image mesures au village. Une seule mise en ligne par tampon et par
  // image, dans une texture que le GPU n'utilise plus.
  static constexpr int kRing = 3;
  GLuint m_ring[2][kRing] = {};
  int m_ring_cur[2] = {0, 0};
};
