#include "OceanMid.h"

#include "common/log/log.h"

static bool is_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

OceanMid::OceanMid() {
  for (auto& x : m_vu_data) {
    x.fill(999.);
  }
  vu.vf25 = Vf(1, 1, 1, 1);
}

// PC-REMAKE EAU TRANSPARENTE (25/08) : pont vers OceanNear — le mid accumule
// ses sommets au bucket 4 mais se dessine au bucket ocean-near (63), APRÈS
// Jak, en transparent. Pointeur posé chaque frame par run(), consommé par
// OceanNear::render via flush_deferred (même frame, même thread de rendu).
OceanMid* g_ocean_mid_deferred = nullptr;

// [QA-EAU] : bilan toutes les 300 frames — si des mids accumules ne sont
// JAMAIS dessines (drop), l'eau disparait ces frames-la. A retirer une fois
// le clignotement regle.
int qa_eau_flush = 0;
int qa_eau_drop = 0;

void OceanMid::flush_deferred(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!m_flush_pending) {
    return;
  }
  m_flush_pending = false;
  qa_eau_flush++;
  m_common_ocean_renderer.flush_mid_transparent(render_state, prof);
}

// PC-REMAKE QA : compte les programmes VU exécutés par image pour vérifier que
// la transition proche/large (107/275) tourne réellement en jeu.
static int qa_mid_frames = 0;
static int qa_c46 = 0, qa_c73 = 0, qa_c107 = 0, qa_c275 = 0, qa_c41 = 0, qa_c43 = 0;

void OceanMid::run(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  // un différé de la frame précédente qui n'aurait jamais été dessiné (frame
  // avortée) est simplement abandonné — ses sommets vont être réécrits
  if (m_flush_pending) {
    qa_eau_drop++;  // [QA-EAU] une frame d'eau a ete PERDUE (jamais dessinee)
  }
  m_flush_pending = false;
  {
    static int qa_eau_frames = 0;
    if (++qa_eau_frames >= 300) {
      lg::info("[QA-EAU] mid differe : {} dessines, {} PERDUS sur {} frames", qa_eau_flush,
               qa_eau_drop, qa_eau_frames);
      qa_eau_frames = 0;
      qa_eau_flush = 0;
      qa_eau_drop = 0;
    }
  }
  m_common_ocean_renderer.init_for_mid();
  // first is setting base and offset
  {
    auto base_offset_tag = dma.read_and_advance();
    ASSERT(base_offset_tag.size_bytes == 0);
    auto base = base_offset_tag.vifcode0();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    auto offset = base_offset_tag.vifcode1();
    ASSERT(offset.kind == VifCode::Kind::OFFSET);
    ASSERT(offset.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  // next is constants
  {
    auto constants = dma.read_and_advance();
    ASSERT(constants.size_bytes == sizeof(Constants));
    ASSERT(constants.vifcode0().kind == VifCode::Kind::STCYCL);  // for whatever reason they do this
    auto unpack = constants.vifcode1();
    ASSERT(VifCodeUnpack(unpack).addr_qw == Vu1Data::CONSTANTS);
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }

  // next is call 0
  {
    auto call0 = dma.read_and_advance();
    ASSERT(call0.vifcode0().kind == VifCode::Kind::STCYCL);
    auto c = call0.vifcode1();
    ASSERT(c.kind == VifCode::Kind::MSCALF);
    ASSERT(c.immediate == 0);
    run_call0();
  }

  while (!is_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      // ASSERT(up.use_tops_flag);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
      ASSERT(16 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }
      ASSERT(4 * v1.num == data.size_bytes);

    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        // cl = 4
        // wl = 2
        u32 addr_off = 4 * (i / 2) + i % 2;
        memcpy(m_vu_data + addr + addr_off, temp, 16);
      }
      ASSERT(8 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          qa_c46++;
          run_call46_vu2c();
          break;
        case 73:
          qa_c73++;
          run_call73_vu2c();
          break;
        case 107:
          qa_c107++;
          run_call107_vu2c();
          break;
        case 275:
          qa_c275++;
          run_call275_vu2c();
          break;
        default:
          lg::warn("unknown call1: {}", v1.immediate);
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          qa_c41++;
          run_call41_vu2c();
          break;
        case 43:
          qa_c43++;
          run_call43_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown call2: {}", v0.immediate));
      }
    } else {
      ASSERT_MSG(false, fmt::format("{} {}", data.vifcode0().print(), data.vifcode1().print()));
    }
  }
  // PC-REMAKE EAU TRANSPARENTE (25/08) : ne PAS dessiner ici (bucket 4, avant
  // Jak — la surface gravait sa profondeur et coupait le joueur immergé à la
  // ligne de flottaison). Le dessin part au bucket ocean-near via
  // flush_deferred. Ligne d'origine, à restaurer pour revenir en arrière :
  // m_common_ocean_renderer.flush_mid(render_state, prof);
  m_flush_pending = true;
  g_ocean_mid_deferred = this;
  // 17/10 : retenir les textures MAINTENANT (fraiches) pour le dessin differe
  m_common_ocean_renderer.capture_mid_textures(render_state);
  qa_mid_frames++;
  if (qa_mid_frames >= 300) {
    lg::info(
        "[QA] ocean VU sur {} images : large 46={} 73={} | transition 107={} 275={} | flush "
        "41={} 43={}",
        qa_mid_frames, qa_c46, qa_c73, qa_c107, qa_c275, qa_c41, qa_c43);
    qa_mid_frames = 0;
    qa_c46 = qa_c73 = qa_c107 = qa_c275 = qa_c41 = qa_c43 = 0;
  }
}

void OceanMid::run_jak2(DmaFollower& dma,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof) {
  m_common_ocean_renderer.init_for_mid();
  // first is setting base and offset
  {
    auto base_offset_tag = dma.read_and_advance();
    ASSERT(base_offset_tag.size_bytes == 0);
    auto base = base_offset_tag.vifcode0();
    ASSERT(base.kind == VifCode::Kind::BASE);
    ASSERT(base.immediate == VU1_INPUT_BUFFER_BASE);
    auto offset = base_offset_tag.vifcode1();
    ASSERT(offset.kind == VifCode::Kind::OFFSET);
    ASSERT(offset.immediate == VU1_INPUT_BUFFER_OFFSET);
  }

  // next is constants
  {
    auto constants = dma.read_and_advance();
    ASSERT(constants.size_bytes == sizeof(Constants));
    ASSERT(constants.vifcode0().kind == VifCode::Kind::STCYCL);  // for whatever reason they do this
    auto unpack = constants.vifcode1();
    ASSERT(VifCodeUnpack(unpack).addr_qw == Vu1Data::CONSTANTS);
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }

  // next is call 0
  {
    auto call0 = dma.read_and_advance();
    ASSERT(call0.vifcode0().kind == VifCode::Kind::STCYCL);
    auto c = call0.vifcode1();
    ASSERT(c.kind == VifCode::Kind::MSCALF);
    ASSERT(c.immediate == 0);
    run_call0();
  }

  while (!is_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      // ASSERT(up.use_tops_flag);
      u16 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      ASSERT(addr + v1.num <= 1024);
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
      ASSERT(16 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }
      ASSERT(4 * v1.num == data.size_bytes);

    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      ASSERT(up.use_tops_flag);
      ASSERT(up.is_unsigned);
      u16 addr = up.addr_qw + get_upload_buffer();
      ASSERT(addr + v1.num <= 1024);

      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = data.data[4 * i + j];
        }
        // cl = 4
        // wl = 2
        u32 addr_off = 4 * (i / 2) + i % 2;
        memcpy(m_vu_data + addr + addr_off, temp, 16);
      }
      ASSERT(8 * v1.num == data.size_bytes);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          run_call46_vu2c();
          break;
        case 73:
          run_call73_vu2c_jak2();
          break;
        case 107:
          run_call107_vu2c_jak2();
          break;
        case 275:
          run_call275_vu2c_jak2();
          break;
        default:
          lg::warn("unknown call1: {}", v1.immediate);
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          run_call41_vu2c();
          break;
        case 43:
          run_call43_vu2c();
          break;
        default:
          ASSERT_MSG(false, fmt::format("unknown call2: {}", v0.immediate));
      }
    } else {
      ASSERT_MSG(false, fmt::format("{} {}", data.vifcode0().print(), data.vifcode1().print()));
    }
  }
  m_common_ocean_renderer.flush_mid(render_state, prof);
}

void OceanMid::run_call0() {
  run_call0_vu2c();
}

void OceanMid::xgkick(u16 addr) {
  m_common_ocean_renderer.kick_from_mid((const u8*)&m_vu_data[addr]);
}
