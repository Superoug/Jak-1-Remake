

#include "background_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_map>

// (22/09 : <unistd.h> retire — POSIX seulement, cassait la compilation Windows ;
// remove()/fopen() viennent de <cstdio>)
#include <cstdio>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"

#include "common/log/log.h"

#include "common/util/os.h"
#include "common/util/simd_util.h"

// PC-REMAKE: see background_common.h
float g_grass_push[4] = {0.f, 0.f, 0.f, 0.f};
float g_post_fx[4] = {0.f, 0.f, 0.f, 0.f};
// PC-REMAKE 25/08 : qualite des effets (reflets SSR / halos / ombres contact)
float g_fx_quality[4] = {2.f, 1.f, 1.f, 1.f};
float g_sun_shadow_params[4] = {0.3f, -0.85f, 0.25f, 0.f};  // strength 0 = off
float g_toon_fx[4] = {0.f, 0.f, 0.f, 0.f};                  // toon: neutral at zero
float g_depth_fx[4] = {0.f, 0.f, 0.f, 0.f};                 // depth post: neutral at zero
float g_light_fx[4] = {0.f, 0.f, 0.f, 0.f};                 // rebuilt lighting rig
// RENDU REMAKE (12/10) : x = dose, y = nettete, z = vignette, w = etalonnage
float g_grade_fx[4] = {1.0f, 0.35f, 0.5f, 1.0f};
// x = exposition, y = contraste, z = saturation
float g_grade_amt2[4] = {0.78f, 1.12f, 0.92f, 0.f};
float g_sky_color[4] = {0.55f, 0.70f, 1.00f, 0.f};
float g_ground_color[4] = {0.42f, 0.36f, 0.26f, 0.f};
float g_night_fx[4] = {0.f, 0.f, 0.f, 0.f};
// défauts calibrés au REPL (16/08) : houle douce
// (troncs quasi muets), battement de feuilles ×2 final, palmiers de terrain réveillés, feuilles shrub vivantes
// terrain (z) à ZÉRO : les palmiers penchés fondus dans le terrain restent
// comme dans le jeu de base ; battement des palmiers du village calibré ×1,5
// (16/08, dernier réglage en date)
// battement ×0,75 (échelle 1.2) : réglage définitif (17/08 soir,
// validé avec les palmiers penchés)
float g_palm_wind[4] = {0.7f, 1.2f, 0.f, 1.5f};
float g_water_glitter[4] = {0.f, 0.f, 0.f, 0.f};
float g_palm_debug = 0.f;
std::string g_palm_debug_spec;
u32 g_palm_debug_seq = 1;
// PC-REMAKE 21/09 : detail intrinseque des sols — au village, la texture du sol
// paraissait de basse qualite. Les sols sont des textures 128² etirees sur
// des metres ; le shader re-echantillonne la MEME texture a 4x et n'en garde que
// la haute frequence (grain), en luminance seulement : rien d'invente, couleurs
// intactes, fondu de 4 a 18 m. Regle par nom ; gain global par fichier.
// 22/09 QA : `touch /tmp/qa_opaque_foliage` force color.a = 1 sur les draws a
// test d'alpha (feuillages) — dit en une image si la translucidite vient d'un
// MELANGE actif sur ces draws
float g_pc_force_opaque = 0.f;
// 14/10 : 0 par defaut — le detail inventait un motif absent de l'original (pas
// fidele) ; reglable dans le menu Remake (original 21/09 : 1.4f)
float g_pc_detail_gain = 0.f;
float g_pc_detail_menu = 0.f;  // pose par GOAL (menu Remake « Detail des sols »)
void update_detail_gain() {
  static int ctr = 0;
  if ((ctr++ & 63) != 0) {
    return;
  }
  g_pc_force_opaque = fs::exists("/tmp/qa_opaque_foliage") ? 1.f : 0.f;
  FILE* f = fopen("/tmp/pc_detail", "r");
  if (f) {
    float v = 1.f;
    if (fscanf(f, "%f", &v) == 1) {
      g_pc_detail_gain = std::max(0.f, std::min(3.f, v));
    }
    fclose(f);
  } else {
    g_pc_detail_gain = std::max(0.f, std::min(3.f, g_pc_detail_menu));
  }
}
float pc_detail_texture_amount(const std::string& debug_name) {
  std::string n = debug_name;
  std::transform(n.begin(), n.end(), n.begin(), ::tolower);
  static const char* kNever[] = {"sky",   "cloud", "water", "glow",  "light", "lamp",
                                 "lores", "medres", "hud",  "envmap", "eye",  "leaf-",
                                 "palm",  "tree",  "grassclump", "bush", "hang"};
  for (const char* x : kNever) {
    if (n.find(x) != std::string::npos) {
      return 0.f;
    }
  }
  // sols et roches : grain plein ; le reste rien (bois, tissus, metal gardent
  // leur dessin — un motif repete 4x y ferait des mini-planches)
  static const char* kGround[] = {"ground", "sand",  "beach-0", "dirt",   "soil",  "grass",
                                  "floor",  "snow",  "rock",    "gravel", "pebble", "cobble",
                                  "path",   "mud",   "moss",    "cave",   "cliff",  "stone",
                                  "lava-ground", "rice-soil"};
  for (const char* x : kGround) {
    if (n.find(x) != std::string::npos) {
      return 1.f;
    }
  }
  return 0.f;
}

void update_palm_debug_flag() {
  static int ctr = 0;
  if ((ctr++ & 63) != 0) {
    return;
  }
  std::string spec;
  FILE* f = fopen("/tmp/palm_debug", "r");
  if (f) {
    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), f)) {
      spec = buf;
      while (!spec.empty() &&
             (spec.back() == '\n' || spec.back() == '\r' || spec.back() == ' ')) {
        spec.pop_back();
      }
    }
    fclose(f);
    g_palm_debug = 1.f;
  } else {
    g_palm_debug = 0.f;
    spec.clear();
  }
  if (spec != g_palm_debug_spec) {
    g_palm_debug_spec = spec;
    g_palm_debug_seq++;
    lg::info("PC-REMAKE projecteur violet : spec '{}'", spec);
  }
}
bool palm_debug_matches(s32 tex_idx, const std::string& name) {
  if (g_palm_debug < 0.5f || g_palm_debug_spec.empty()) {
    return false;
  }
  const auto& s = g_palm_debug_spec;
  if (s.rfind("idx:", 0) == 0) {
    int a = -1, b = -1;
    if (sscanf(s.c_str() + 4, "%d-%d", &a, &b) == 2) {
      return tex_idx >= a && tex_idx <= b;
    }
    return false;
  }
  return name.find(s) != std::string::npos;
}

// Double-buffered: GOAL fills `pending` while the render thread reads `active`,
// and the render thread swaps once per frame. That way the order in which the
// lamp processes run inside a game frame doesn't matter.
namespace {
float s_lamp_pos[kMaxRemakeLamps * 4];
float s_lamp_color[kMaxRemakeLamps * 4];
int s_lamp_count = 0;
float s_pending_pos[kMaxRemakePending * 4];
float s_pending_color[kMaxRemakePending * 4];
int s_pending_count = 0;
float s_all_pos[kMaxRemakePending * 4];
float s_all_color[kMaxRemakePending * 4];
int s_all_prio[kMaxRemakePending] = {};
int s_pending_prio[kMaxRemakePending] = {};
int s_all_count = 0;

// persistent fixtures, kept across frames (see pc_remake_add_static_lamp)
constexpr int kMaxStaticLamps = 96;
long long s_static_id[kMaxStaticLamps] = {};
float s_static_pos[kMaxStaticLamps * 4] = {};
float s_static_color[kMaxStaticLamps * 4] = {};
int s_static_prio[kMaxStaticLamps] = {};
int s_static_count = 0;
}  // namespace

void pc_remake_add_static_lamp(long long id, float x, float y, float z, float radius, float r,
                               float g, float b, float intensity, int priority) {
  int slot = -1;
  for (int i = 0; i < s_static_count; i++) {
    if (s_static_id[i] == id) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (s_static_count >= kMaxStaticLamps) {
      return;
    }
    slot = s_static_count++;
    s_static_id[slot] = id;
  }
  s_static_pos[slot * 4 + 0] = x;
  s_static_pos[slot * 4 + 1] = y;
  s_static_pos[slot * 4 + 2] = z;
  s_static_pos[slot * 4 + 3] = radius;
  s_static_color[slot * 4 + 0] = r;
  s_static_color[slot * 4 + 1] = g;
  s_static_color[slot * 4 + 2] = b;
  s_static_color[slot * 4 + 3] = intensity;
  s_static_prio[slot] = priority;
}

// PC-REMAKE 18/08 : retire une fixture par id — sans ca, la lueur d'un eco
// RAMASSE restait allumee pour toujours (bug trouve par l'enquete du 18/08 :
// s_static_count n'etait jamais decremete nulle part).
void pc_remake_remove_static_lamp(long long id) {
  for (int i = 0; i < s_static_count; i++) {
    if (s_static_id[i] == id) {
      int last = s_static_count - 1;
      s_static_id[i] = s_static_id[last];
      s_static_prio[i] = s_static_prio[last];
      for (int k = 0; k < 4; k++) {
        s_static_pos[i * 4 + k] = s_static_pos[last * 4 + k];
        s_static_color[i * 4 + k] = s_static_color[last * 4 + k];
      }
      s_static_count = last;
      return;
    }
  }
}

void pc_remake_lamps_begin_frame() {
  // The game and render threads are not locked in step, so an empty pending
  // list usually just means "GOAL hasn't run its lamp processes since the last
  // swap" — publishing that straight away would make the lights strobe. Hold
  // the previous set, and only drop it once the list has stayed empty long
  // enough to mean the lamps really are gone (level change).
  static int empty_frames = 0;
  if (s_pending_count > 0) {
    empty_frames = 0;
    s_all_count = s_pending_count;
    for (int i = 0; i < s_pending_count * 4; i++) {
      s_all_pos[i] = s_pending_pos[i];
      s_all_color[i] = s_pending_color[i];
    }
    for (int i = 0; i < s_pending_count; i++) {
      s_all_prio[i] = s_pending_prio[i];
    }
  } else if (++empty_frames > 20) {
    s_all_count = 0;
    s_lamp_count = 0;
  }
  s_pending_count = 0;
  // fold in the fixtures; the nearest-N selection sorts everything out, and a
  // lamp from another level simply never wins a slot because it is far away
  for (int i = 0; i < s_static_count && s_all_count < kMaxRemakePending; i++) {
    int j = s_all_count++;
    for (int k = 0; k < 4; k++) {
      s_all_pos[j * 4 + k] = s_static_pos[i * 4 + k];
      s_all_color[j * 4 + k] = s_static_color[i * 4 + k];
    }
    s_all_prio[j] = s_static_prio[i];
  }
  // et les lumières émissives scannées des niveaux chargés (lave, feux, lueurs)
  pc_emissive_fold_into_all();
}

void pc_remake_select_lamps(const float* cam_xyz) {
  // partial selection sort over the collected set: cheap at these counts and
  // it keeps the lamps that actually matter for what is on screen
  int n = s_all_count < kMaxRemakeLamps ? s_all_count : kMaxRemakeLamps;
  bool used[kMaxRemakePending] = {};
  // BONUS de priorité : ~40 m d'avance au carré — une fixture PROCHE garde son
  // avantage, mais une fixture à 2 km ne vole plus le slot d'une lave à 30 m.
  // (Avant : la priorité était un VETO absolu — les statics du village
  // remplissaient les 48 slots depuis l'autre bout du monde, et AUCUNE
  // émissive n'atteignait jamais le shader. Diagnostic 18/08 : « 48 slots —
  // 0 emissives, 0 eco, 48 normales ».)
  const float kPrioBonus = (4096.f * 40.f) * (4096.f * 40.f);
  for (int slot = 0; slot < n; slot++) {
    int best = -1;
    float best_score = 0.f;
    for (int i = 0; i < s_all_count; i++) {
      if (used[i]) {
        continue;
      }
      float dx = s_all_pos[i * 4 + 0] - cam_xyz[0];
      float dy = s_all_pos[i * 4 + 1] - cam_xyz[1];
      float dz = s_all_pos[i * 4 + 2] - cam_xyz[2];
      // measure from the edge of the lamp's reach, so a big village lamp wins
      // over a tiny orb that happens to be a little closer
      float d = dx * dx + dy * dy + dz * dz - s_all_pos[i * 4 + 3] * s_all_pos[i * 4 + 3];
      float score = d - (float)s_all_prio[i] * kPrioBonus;
      if (best < 0 || score < best_score) {
        best = i;
        best_score = score;
      }
    }
    if (best < 0) {
      break;
    }
    used[best] = true;
    for (int k = 0; k < 4; k++) {
      s_lamp_pos[slot * 4 + k] = s_all_pos[best * 4 + k];
      s_lamp_color[slot * 4 + k] = s_all_color[best * 4 + k];
    }
  }
  s_lamp_count = n;
  // QA : composition des élues (une fois toutes les 300 images)
  {
    static int qa_sel = 0;
    if ((qa_sel++ % 300) == 0) {
      int em = 0, eco = 0, pos = 0;
      for (int i = 0; i < n; i++) {
        float w = s_lamp_color[i * 4 + 3];
        if (w < -8.f) {
          em++;
        } else if (w < 0.f) {
          eco++;
        } else {
          pos++;
        }
      }
      // 12/10 : doublons (meme position) parmi les elues
      int dup = 0;
      for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
          if (s_lamp_pos[i * 4 + 0] == s_lamp_pos[j * 4 + 0] &&
              s_lamp_pos[i * 4 + 1] == s_lamp_pos[j * 4 + 1] &&
              s_lamp_pos[i * 4 + 2] == s_lamp_pos[j * 4 + 2]) {
            dup++;
            break;
          }
        }
      }
      lg::warn(
          "[QA] selection lampes : {} slots — {} emissives(-16), {} eco(-), {} normales(+), "
          "{} doublons (collecte {}, fixes {})",
          n, em, eco, pos, dup, s_all_count, s_static_count);
    }
  }
}

void pc_remake_add_lamp(float x, float y, float z, float radius, float r, float g, float b,
                        float intensity, int priority) {
  if (s_pending_count >= kMaxRemakePending) {
    return;
  }
  int i = s_pending_count++;
  s_pending_pos[i * 4 + 0] = x;
  s_pending_pos[i * 4 + 1] = y;
  s_pending_pos[i * 4 + 2] = z;
  s_pending_pos[i * 4 + 3] = radius;
  s_pending_color[i * 4 + 0] = r;
  s_pending_color[i * 4 + 1] = g;
  s_pending_color[i * 4 + 2] = b;
  s_pending_color[i * 4 + 3] = intensity;
  s_pending_prio[i] = priority;
}

int pc_remake_lamp_count() {
  return s_lamp_count;
}
const float* pc_remake_lamp_pos() {
  return s_lamp_pos;
}
const float* pc_remake_lamp_color() {
  return s_lamp_color;
}

// ===================== PC-REMAKE : lumières émissives scannées =====================
// objectif (18/08) : chaque objet du jeu qui est censé émettre
// de la lumière doit éclairer autour de lui — lave, torches, feux, murs
// précurseurs, éclairs du village de pierre. Au chargement d'un niveau on balaye
// la géométrie tfrag : les draws dont la texture est dans le registre ci-dessous
// sont regroupés en amas de ~24 m, et chaque amas devient une source ponctuelle
// pour la passe différée (le système des lampes). La couleur de chaque source
// est la couleur moyenne de sa texture — la lave rayonne SON orange, les glyphes
// précurseurs LEUR bleu, sans couleur codée en dur.

namespace {
struct EmissiveRule {
  const char* pattern;  // sous-chaîne du nom de texture (minuscules)
  float intensity;      // force de la source (le signe négatif = visible de jour
                        // est appliqué au moment du versement dans les lampes)
  float radius_m;       // portée en mètres
  float lift_m;         // surélévation du centre (la lumière au-dessus du sol)
  // 21/09 : la regle ne vaut que si les PIXELS luisent vraiment (veines bleues,
  // voile blanc) — certaines textures brillaient plus que d'autres sans raison
  // : les arches de pierre rouge gravee de la cite engloutie
  // s'appelaient « circuitpattern » et etaient boostees comme des circuits.
  bool needs_glow = false;
};
// Inventaire tiré des dumps du décompilateur (18/08) : lava-floor, fcn-coal,
// fcn-crackedfloor, cav-firebarrel-flame, mineshort-candle, vil2-sticklight-tip,
// cmn-precursor-blue-glow, gen-bluelight, sun-*-glow, vil-fireplace…
const EmissiveRule kEmissiveRules[] = {
    // famille FEU musclée (18/08 : la lave ne produisait pas de lumière) —
    // le canyon doit BAIGNER dans l'orange
    // « lava » generique EMBRASAIT tout le tube de lave : la-bas, TOUTES les
    // textures s'appellent lava-... (prefixe du niveau — meme lava-lrkr-metal,
    // le metal des lurkers !). Motifs chirurgicaux : la lave EN FUSION seule.
    {"lava-flow", 2.4f, 110.f, 2.5f},
    {"lava-floor", 2.4f, 110.f, 2.5f},
    {"lava-ground-hot", 1.6f, 40.f, 2.0f},
    {"magma", 2.4f, 110.f, 2.5f},
    // 70 m -> 24 m (18/08) : calibre pour les champs de braises du canyon de
    // feu, ce rayon noyait TOUTE la falaise du village de pierre, dont les
    // braseros portent la meme texture (les lampes du village en
    // concentrent beaucoup)
    {"-coal", 1.1f, 24.f, 1.5f},
    {"crackedfloor", 1.0f, 18.f, 1.2f},
    {"flame", 1.1f, 13.f, 1.0f},
    {"bonfire", 1.3f, 15.f, 1.0f},
    {"candle", 0.8f, 8.f, 0.6f},
    {"torch", 0.9f, 11.f, 0.8f},
    {"fireplace", 0.9f, 12.f, 1.0f},
    {"precursor-blue-glow", 1.0f, 15.f, 1.0f},
    {"precursor-bluelight", 1.0f, 15.f, 1.0f},
    {"bluelight", 0.9f, 13.f, 1.0f},
    {"light-blue", 0.9f, 13.f, 1.0f},
    {"sticklight", 1.0f, 12.f, 0.8f},
    {"pinklight", 0.9f, 12.f, 0.8f},
    {"palelight", 0.8f, 12.f, 0.8f},
    {"-glow", 0.9f, 13.f, 1.0f},
    {"ecovalve", 0.45f, 7.f, 0.8f},
    {"eco-valve", 0.45f, 7.f, 0.8f},
    {"-lamp", 1.0f, 13.f, 0.5f},
    // Inventaire COMPLET du jeu (18/08) — familles trouvées en balayant les 20+ niveaux :
    {"crystal", 1.25f, 18.f, 1.0f},        // grottes sombres, neige, village volcan
    {"pinkbeam", 1.3f, 18.f, 1.5f},       // rayons roses de la citadelle
    {"beambar", 1.2f, 15.f, 1.2f},
    {"hot-pipe", 1.0f, 12.f, 0.8f},       // cité engloutie
    {"hot-coil", 1.0f, 10.f, 0.8f},
    {"blueeco-flash", 1.0f, 12.f, 0.8f},  // plaques d'éco bleu (temple, rolling…)
    {"plat-eco", 0.9f, 10.f, 0.6f},       // plates-formes à éco
    {"darkeco", 0.9f, 12.f, 0.8f},        // éco noir luisant (misty, grottes)
    {"dark-eco", 0.8f, 9.f, 0.8f},        // caisses/barils d'éco noir
    {"dkecobomb", 1.1f, 14.f, 1.0f},      // bombe du boss final
    {"sun-orb", 0.9f, 10.f, 0.6f},        // orbes de la cité engloutie
    {"-light", 0.9f, 12.f, 0.8f},         // rol-light, fin-light, cmn-light…
    // 25/08 (objectif : les châteaux/vaisseaux d'éco doivent faire de la
    // VRAIE lumière) : les grandes structures précurseur portent leurs veines
    // de circuits et leurs écritures lumineuses — elles deviennent des lampes.
    {"circuitpattern", 1.15f, 16.f, 1.2f, true},
    {"tube-script", 0.7f, 10.f, 1.0f, true},
    {"precursor-glass", 0.9f, 12.f, 1.0f, true},
    // 26/08 (objets susceptibles de faire de la lumiere, tous
    // allumes) — inventaire complet des textures, manquants averes :
    {"thicklava", 2.2f, 90.f, 2.5f},          // lave epaisse du tube
    {"vil3-lava", 2.2f, 90.f, 2.5f},          // le lac de lave du village volcan
    {"lava-cracked-floor", 1.4f, 30.f, 1.5f}, // sol craquele (tiret != crackedfloor)
    {"lava-craked-floor", 1.4f, 30.f, 1.5f},  // typo d'origine des artistes
    {"lava-vent-fire", 1.6f, 18.f, 1.2f},     // events de feu
    {"energycoil", 1.0f, 12.f, 0.8f},         // bobines d'energie du tube
    {"energyrail", 0.8f, 10.f, 0.6f},         // rails d'energie
    {"boulderfire", 1.4f, 16.f, 1.0f},        // rochers enflammes du canyon
    {"firebarrel", 1.1f, 12.f, 0.8f},         // barils de feu des grottes
    {"multitileglow", 0.9f, 12.f, 1.0f},      // lueur du sage rouge
    {"smallblueglow", 0.9f, 12.f, 0.8f},      // lueurs bleues cite engloutie
    {"lightning", 1.2f, 14.f, 1.5f},          // eclairs des pylones (vil2)
    {"watchtower-lens", 1.4f, 16.f, 1.0f},    // lentilles des tours de guet (jungle)
    // 21/09 (tournee de tous les niveaux, tubes jaunes de la cite
    // sous-marine) : les EXTREMITES des tubes chauds sont peintes avec deux
    // aplats 4x4 (sun-yellow = le disque jaune, sun-red = sa couronne rouge) —
    // sans ces regles le tube brillait mais pas sa bouche. Prefixe « sun- » =
    // cite engloutie seulement (sunken/sunkenb).
    {"sun-yellow", 1.3f, 14.f, 1.0f},
    {"sun-red", 0.9f, 10.f, 0.8f},
    // 21/09 : le « lac de lave » du village volcan est en fait un BASSIN DE
    // SOUFRE (vil3-sulfurpool-center, jaune 177/171/57 d'origine) — sans regle,
    // relumiere + hemispherique le delavaient en blanc creme ; en source il
    // garde son jaune et rechauffe la rive.
    {"sulfurpool-center", 0.8f, 30.f, 0.5f},
};
// Faux amis : rebords métalliques « éclairés », bois des poteaux, ciels,
// végétation brûlée (éteinte, elle ne brille pas)…
const char* kEmissiveExcludes[] = {"metalrim", "metal-dark", "hilite", "-wood", "sky",
                                   "cloud",    "envmap",     "grate",  "burn"};

// PC-REMAKE 18/08 : seul le VRAI FEU garde sa pleine force en plein soleil.
// Une lampe, une lueur precurseur ou un cristal n'eclaire pas une falaise a
// midi — c'etait la teinte jaune-vert sur la falaise du village de pierre.
// GRANDES SURFACES de feu : elles eclairent de partout a la fois et ne
// s'eteignent pas au soleil. Une TORCHE n'en fait pas partie — c'est une
// petite flamme ponctuelle (torches du centre du village peu
// naturelles, 18/08) : elle doit sculpter comme une lampe et
// s'effacer de jour, tout en gardant une couleur de flamme.
// 26/08 (objectif : les objets d'eco bleu emettent comme les
// boules) : ces surfaces passent dans la FAMILLE ECO du shader — couleur
// bleue franche + depot additif, au lieu du luminaire discret.
bool emissive_pattern_is_blue_eco(const char* pattern) {
  static const char* kBlueEco[] = {"ecovalve",           "eco-valve",
                                   "plat-eco",           "blueeco-flash",
                                   "precursor-blue-glow", "precursor-bluelight",
                                   "watchtower-lens"};
  for (const char* f : kBlueEco) {
    if (std::strcmp(pattern, f) == 0) {
      return true;
    }
  }
  return false;
}

bool emissive_pattern_is_fire(const char* pattern) {
  static const char* kFire[] = {"lava-flow",       "lava-floor",        "lava-ground-hot",
                                "magma",           "-coal",             "crackedfloor",
                                "thicklava",       "vil3-lava",         "lava-cracked-floor",
                                "lava-craked-floor", "lava-vent-fire",  "boulderfire"};
  for (const char* f : kFire) {
    if (std::strcmp(pattern, f) == 0) {
      return true;
    }
  }
  return false;
}

// petites flammes : traitees comme des sources PONCTUELLES (directionnelles,
// discretes de jour) mais avec une couleur de feu plus franche qu'une lampe
bool emissive_pattern_is_flame(const char* pattern) {
  static const char* kFlame[] = {"flame", "bonfire", "candle", "torch", "fireplace"};
  for (const char* f : kFlame) {
    if (std::strcmp(pattern, f) == 0) {
      return true;
    }
  }
  return false;
}

struct PcScannedLight {
  float pos[4];    // xyz monde + rayon
  float color[4];  // rgb + intensité (positive ici)
  float weight;    // nombre de sommets de l'amas (pour le tri)
  bool fire;       // grande surface de feu (plein jour, enveloppante)
  bool flame;      // petite flamme : ponctuelle, mais couleur de feu
  bool blue_eco;   // objet a eco bleu : famille eco (couleur pleine + depot)
};
std::mutex s_emissive_mutex;
std::map<std::string, std::vector<PcScannedLight>> s_emissive_levels;

const EmissiveRule* emissive_rule_for(const std::string& lower_name) {
  for (const char* ex : kEmissiveExcludes) {
    if (lower_name.find(ex) != std::string::npos) {
      return nullptr;
    }
  }
  for (const auto& rule : kEmissiveRules) {
    if (lower_name.find(rule.pattern) != std::string::npos) {
      return &rule;
    }
  }
  return nullptr;
}

// 21/09 : une texture « circuit » ne luit que si elle contient des veines
// bleues/cyan claires (>= 2 % des texels) ou un voile clair (>= 20 %) — les
// variantes « -glow », les rubans bleus vil1/vil2/vil3, le verre precurseur.
// Les pierres rouges gravees (sun-, cit-, rol-, cmn-, jngb-) sont ecartees.
bool emissive_has_glow_pixels(const tfrag3::Texture& tex) {
  u32 total = 0, blue = 0, bright = 0;
  for (u32 px : tex.data) {
    float a = (float)((px >> 24) & 0xff);
    if (a < 48.f) {
      continue;
    }
    float r = (float)(px & 0xff) / 255.f, g = (float)((px >> 8) & 0xff) / 255.f,
          b = (float)((px >> 16) & 0xff) / 255.f;
    float v = std::max(r, std::max(g, b));
    float mn = std::min(r, std::min(g, b));
    float sat = v > 1e-4f ? (v - mn) / v : 0.f;
    float luma = 0.299f * r + 0.587f * g + 0.114f * b;
    total++;
    if (b > r * 1.15f && v > 0.45f && sat > 0.3f) {
      blue++;
    }
    if (luma > 0.65f) {
      bright++;
    }
  }
  if (total == 0) {
    return false;
  }
  return blue >= total / 50 + 1 || bright >= total / 5;
}
// verdict par nom (rempli par le scan au chargement, lu par le boost du rendu)
std::mutex s_glow_verdict_mutex;
std::unordered_map<std::string, bool> s_glow_verdict;

// Couleur moyenne des texels visibles, normalisée pour rester vive : une
// texture de braises presque noire donne quand même son orange.
void emissive_avg_color(const tfrag3::Texture& tex, float out[3]) {
  double sr = 0, sg = 0, sb = 0, sw = 0;
  for (u32 px : tex.data) {
    float a = (float)((px >> 24) & 0xff);
    if (a < 48.f) {
      continue;
    }
    float r = (float)(px & 0xff), g = (float)((px >> 8) & 0xff), b = (float)((px >> 16) & 0xff);
    // pondère par la luminosité : les texels ÉCLATANTS définissent la teinte
    float w = (0.299f * r + 0.587f * g + 0.114f * b) / 255.f;
    w = w * w;
    sr += r * w;
    sg += g * w;
    sb += b * w;
    sw += w;
  }
  if (sw <= 0.0) {
    out[0] = out[1] = out[2] = 1.f;
    return;
  }
  float r = (float)(sr / sw) / 255.f, g = (float)(sg / sw) / 255.f, b = (float)(sb / sw) / 255.f;
  // Teinte creusee (pow) puis normalisee. 2.2 -> 1.5 le 18/08 : a 2.2 la
  // moindre lampe rendait un ORANGE criard (trop jaune au lieu d'une
  // belle lumiere). Le blanchiment final se fait au versement,
  // par famille : une lampe eclaire BLANC CHAUD, la lave reste orange.
  r = std::pow(r, 1.5f);
  g = std::pow(g, 1.5f);
  b = std::pow(b, 1.5f);
  float mx = std::max(r, std::max(g, b));
  if (mx > 0.001f) {
    r /= mx;
    g /= mx;
    b /= mx;
  }
  out[0] = r;
  out[1] = g;
  out[2] = b;
}
}  // namespace

void pc_emissive_scan_level(const tfrag3::Level& lvl) {
  constexpr float kCell = 4096.f * 24.f;  // amas de 24 m
  struct Cell {
    double sx = 0, sy = 0, sz = 0, w = 0;
    const EmissiveRule* rule = nullptr;
    float color[3] = {1.f, 1.f, 1.f};
  };
  std::unordered_map<u64, Cell> cells;
  // cache par texture : règle + couleur moyenne
  std::unordered_map<s32, std::pair<const EmissiveRule*, std::array<float, 3>>> tex_cache;

  // même balayage pour le terrain (tfrag) ET les objets instanciés (TIE) —
  // lampes, foyers, murs précurseurs… sont du TIE, pas du terrain
  auto scan_draws = [&](const std::vector<tfrag3::StripDraw>& draws,
                        const std::vector<tfrag3::PreloadedVertex>& verts) {
    for (const auto& draw : draws) {
      if (draw.tree_tex_id < 0 || draw.tree_tex_id >= (s32)lvl.textures.size()) {
        continue;
      }
      auto it = tex_cache.find(draw.tree_tex_id);
      if (it == tex_cache.end()) {
        const auto& tex = lvl.textures[draw.tree_tex_id];
        std::string lower = tex.debug_name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        const EmissiveRule* rule = emissive_rule_for(lower);
        if (rule && rule->needs_glow) {
          bool ok = emissive_has_glow_pixels(tex);
          {
            std::lock_guard<std::mutex> g(s_glow_verdict_mutex);
            s_glow_verdict[lower] = ok;
          }
          if (!ok) {
            rule = nullptr;
          }
        }
        std::array<float, 3> col = {1.f, 1.f, 1.f};
        if (rule) {
          emissive_avg_color(tex, col.data());
        }
        it = tex_cache.emplace(draw.tree_tex_id, std::make_pair(rule, col)).first;
      }
      const EmissiveRule* rule = it->second.first;
      if (!rule) {
        continue;
      }
      const auto& col = it->second.second;
      auto accum = [&](const tfrag3::PreloadedVertex& v) {
        s64 cx = (s64)std::floor(v.x / kCell);
        s64 cy = (s64)std::floor(v.y / (kCell * 2.f));
        s64 cz = (s64)std::floor(v.z / kCell);
        u64 key =
            ((u64)(cx & 0x1fffff) << 42) | ((u64)(cy & 0x1fffff) << 21) | ((u64)(cz & 0x1fffff));
        auto& c = cells[key];
        c.sx += v.x;
        c.sy += v.y;
        c.sz += v.z;
        c.w += 1.0;
        c.rule = rule;
        c.color[0] = col[0];
        c.color[1] = col[1];
        c.color[2] = col[2];
      };
      // Échantillonne 1 sommet sur 4 : les centroïdes n'ont pas besoin de tout,
      // et les niveaux de lave (canyon de feu, tube) matchent des surfaces
      // entières — sans ça le fil du chargeur calerait plusieurs secondes.
      constexpr u32 kStride = 4;
      if (!draw.runs.empty()) {
        for (const auto& run : draw.runs) {
          for (u32 i = 0; i < run.length; i += kStride) {
            u32 vi = run.vertex0 + i;
            if (vi < verts.size()) {
              accum(verts[vi]);
            }
          }
        }
      } else {
        for (u32 k = 0; k < draw.plain_indices.size(); k += kStride) {
          u32 vi = draw.plain_indices[k];
          if (vi != UINT32_MAX && vi < verts.size()) {
            accum(verts[vi]);
          }
        }
      }
    }
  };
  for (const auto& cat : lvl.tfrag_trees) {
    for (const auto& tree : cat) {
      scan_draws(tree.draws, tree.unpacked.vertices);
    }
  }
  for (const auto& cat : lvl.tie_trees) {
    for (const auto& tree : cat) {
      scan_draws(tree.static_draws, tree.unpacked.vertices);
    }
  }

  std::vector<PcScannedLight> lights;
  lights.reserve(cells.size());
  for (auto& [key, c] : cells) {
    (void)key;
    if (c.w < 2.0) {
      continue;  // amas trop maigre : bruit (seuil ajusté au stride de 4)
    }
    PcScannedLight L;
    L.pos[0] = (float)(c.sx / c.w);
    L.pos[1] = (float)(c.sy / c.w) + c.rule->lift_m * 4096.f;
    L.pos[2] = (float)(c.sz / c.w);
    L.pos[3] = c.rule->radius_m * 4096.f;
    L.color[0] = c.color[0];
    L.color[1] = c.color[1];
    L.color[2] = c.color[2];
    // un amas dense rayonne plus fort, plafonné à la valeur de sa règle
    // (diviseur ajusté au stride de 4)
    L.color[3] = c.rule->intensity * std::min(1.f, 0.55f + (float)c.w / 40.f);
    L.weight = (float)c.w;
    L.fire = emissive_pattern_is_fire(c.rule->pattern);
    L.flame = emissive_pattern_is_flame(c.rule->pattern);
    L.blue_eco = emissive_pattern_is_blue_eco(c.rule->pattern);
    if (L.blue_eco) {
      // couleur d'eco bleue FRANCHE (la texture moyenne est trop delavee)
      L.color[0] = 0.25f;
      L.color[1] = 0.55f;
      L.color[2] = 1.00f;
      L.color[3] = std::max(L.color[3], 1.6f);
    }
    lights.push_back(L);
  }
  std::sort(lights.begin(), lights.end(),
            [](const PcScannedLight& a, const PcScannedLight& b) { return a.weight > b.weight; });
  if (lights.size() > 160) {
    lights.resize(160);
  }
  {
    std::lock_guard<std::mutex> g(s_emissive_mutex);
    s_emissive_levels[lvl.level_name] = lights;
  }
  lg::info("[QA] emissives : niveau '{}' -> {} sources ({} amas bruts)", lvl.level_name,
           lights.size(), cells.size());
  // DEBUG temporaire : si rien ne matche, montrer ce que contiennent les noms
  if (cells.empty()) {
    std::string sample;
    int shown = 0;
    for (const auto& t : lvl.textures) {
      if (shown++ >= 25) {
        break;
      }
      sample += t.debug_name + " | ";
    }
    lg::info("[QA] emissives : {} textures, echantillon : {}", lvl.textures.size(), sample);
    int ndraws = 0, nneg = 0, nbig = 0;
    for (const auto& cat : lvl.tfrag_trees) {
      for (const auto& tree : cat) {
        for (const auto& draw : tree.draws) {
          ndraws++;
          if (draw.tree_tex_id < 0) {
            nneg++;
          } else if (draw.tree_tex_id >= (s32)lvl.textures.size()) {
            nbig++;
          }
        }
      }
    }
    lg::info("[QA] emissives : {} draws tfrag ({} anim, {} hors bornes)", ndraws, nneg, nbig);
  }
}

void pc_emissive_drop_level(const std::string& level_name) {
  std::lock_guard<std::mutex> g(s_emissive_mutex);
  s_emissive_levels.erase(level_name);
}

float pc_emissive_texture_boost(const std::string& debug_name) {
  std::string lower = debug_name;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  const EmissiveRule* rule = emissive_rule_for(lower);
  if (!rule) {
    return 0.f;
  }
  if (rule->needs_glow) {
    // verdict du scan (pixels) ; inconnu = pas de boost
    std::lock_guard<std::mutex> g(s_glow_verdict_mutex);
    auto it = s_glow_verdict.find(lower);
    if (it == s_glow_verdict.end() || !it->second) {
      return 0.f;
    }
  }
  // dérivé de l'intensité de la règle — INCANDESCENCE : lave ~3.4, braises
  // ~2.7, lueurs bleues ~2.0 ; au-delà du seuil du bloom, la surface gagne
  // son halo automatiquement (v1 à ~2.0 : terne, 18/08)
  // objets trop brillants (18/08) : éclat propre réduit — les sources
  // se lisent, la LUMIÈRE portée fait le spectacle. Et l'incandescence est
  // reservee au feu/flammes : des dizaines de cristaux geants auto-illumines
  // delavaient tout village3 en blanc-lavande (tournee 18/08 soir).
  if (emissive_pattern_is_fire(rule->pattern) || emissive_pattern_is_flame(rule->pattern)) {
    return 1.0f + 0.45f * rule->intensity;
  }
  return 1.0f + 0.20f * rule->intensity;
}

void pc_emissive_fold_into_all() {
  std::lock_guard<std::mutex> g(s_emissive_mutex);
  // QA : combien d'émissives partent réellement vers la sélection ?
  static int qa_fold_frames = 0;
  int folded = 0;
  for (auto& [name, lights] : s_emissive_levels) {
    (void)name;
    for (auto& L : lights) {
      if (s_all_count >= kMaxRemakePending) {
        if ((qa_fold_frames++ % 300) == 0) {
          lg::warn("[QA] emissives fold : SATURATION pending, {} versees", folded);
        }
        return;
      }
      folded++;
      int j = s_all_count++;
      for (int k = 0; k < 4; k++) {
        s_all_pos[j * 4 + k] = L.pos[k];
        s_all_color[j * 4 + k] = L.color[k];
      }
      // encodage : -(i+16) = VRAI FEU (pleine puissance jour et nuit — la lave
      // ne s'eteint pas au soleil) ; -(i+64) = LUMINAIRE (lampe, lueur, cristal,
      // eco) qui s'attenue fortement en plein jour. Le shader lit le seuil -40.
      // BLANC CHAUD : une lampe, une lueur, un cristal eclairent presque
      // blanc (teinte a 40 %) ; le vrai feu garde son orange (teinte a 82 %)
      {
        // 19/08 : dans le modele lineaire, la teinte se rend proprement —
        // les luminaires du monde (cristaux, lueurs precurseurs) colorent
        // selon LEUR texture (0.75), le feu garde son orange (0.85)
        float keep = L.fire ? 0.85f : (L.flame ? 0.70f : 0.75f);
        for (int k = 0; k < 3; k++) {
          float c0 = s_all_color[j * 4 + k];
          s_all_color[j * 4 + k] = 1.f + (c0 - 1.f) * keep;
        }
      }
      s_all_color[j * 4 + 3] =
          L.blue_eco ? -std::min(L.color[3], 7.5f) : -(L.color[3] + (L.fire ? 16.f : 64.f));
      s_all_prio[j] = 1;
    }
  }
  if ((qa_fold_frames++ % 300) == 0) {
    lg::warn("[QA] emissives fold : {} versees dans la selection", folded);
  }
}
OceanSim g_ocean_sim;

float OceanSim::frand() {
  rng = rng * 1664525u + 1013904223u;
  return (float)(rng >> 8) / 16777216.f;
}

void OceanSim::step_and_export(float* dst_1024) {
  if (gain < 0.f) {
    return;  // bypass: the engine's original wave loop is left untouched
  }
  // ---- self-calibration: learn the amplitude the game just produced --------
  float mn = 1e30f, mx = -1e30f, mean = 0.f;
  for (int i = 0; i < N * N; i++) {
    mn = std::min(mn, dst_1024[i]);
    mx = std::max(mx, dst_1024[i]);
    mean += dst_1024[i];
  }
  mean /= (float)(N * N);
  float native_amp = 0.5f * (mx - mn);
  if (!(native_amp > 0.f) || native_amp > 1e6f) {
    return;  // unexpected contents: leave the engine's data alone
  }
  if (!logged) {
    logged = true;
    lg::info("PC-REMAKE ocean: native heights min={:.1f} max={:.1f} mean={:.1f} amp={:.1f}", mn, mx,
             mean, native_amp);
  }

  // ---- wave equation over a wrapping grid ---------------------------------
  constexpr float c2 = 0.22f;  // propagation speed^2 (stable below 0.25)
  constexpr float damping = 0.995f;
  float next[N * N];
  auto at = [&](int x, int y) -> float { return cur[((y + N) % N) * N + ((x + N) % N)]; };
  for (int y = 0; y < N; y++) {
    for (int x = 0; x < N; x++) {
      int i = y * N + x;
      float lap = at(x - 1, y) + at(x + 1, y) + at(x, y - 1) + at(x, y + 1) - 4.f * cur[i];
      next[i] = (2.f * cur[i] - prev[i] + c2 * lap) * damping;
    }
  }
  memcpy(prev, cur, sizeof(cur));
  memcpy(cur, next, sizeof(next));

  // directional swell: a slowly advancing wind front pumps parallel crests
  swell_phase += 0.05f;
  {
    int y0i = ((int)swell_phase % N + N) % N;
    for (int x = 0; x < N; x++) {
      int yy = (y0i + (int)(2.f * sinf(0.35f * (float)x + swell_phase * 0.15f)) + N) % N;
      cur[yy * N + x] += 0.55f;
    }
  }
  // wind chop riding on the swell
  for (int g = 0; g < 3; g++) {
    if (frand() < 0.5f) {
      int gx = (int)(frand() * N), gy = (int)(frand() * N);
      float a = 0.15f + frand() * 0.25f;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          cur[((gy + dy + N) % N) * N + ((gx + dx + N) % N)] +=
              a * expf(-(float)(dx * dx + dy * dy) * 0.7f);
        }
      }
    }
  }
  // Jak's wake: the sea reacts to the player moving through it
  if (g_grass_push[3] > 0.36f) {
    constexpr float kTile = 4096.f * 8.f;
    float fx = g_grass_push[0] / kTile, fz = g_grass_push[2] / kTile;
    int jx = (int)((fx - std::floor(fx)) * N);
    int jz = (int)((fz - std::floor(fz)) * N);
    cur[((jz + N) % N) * N + ((jx + N) % N)] -= 0.9f * (g_grass_push[3] - 0.3f);
  }

  // ---- write back, matched to the game's own amplitude --------------------
  float smn = 1e30f, smx = -1e30f;
  for (int i = 0; i < N * N; i++) {
    smn = std::min(smn, cur[i]);
    smx = std::max(smx, cur[i]);
  }
  float sim_amp = 0.5f * (smx - smn);
  if (sim_amp < 1e-5f) {
    return;  // simulation still settling
  }
  const float scale = (native_amp * gain) / sim_amp;
  const float sim_mid = 0.5f * (smx + smn);

  // This grid also answers the height queries used for BUOYANCY, so it is
  // smoothed before being written: sharp per-cell spikes would make anything
  // floating on the water jitter (they did).
  float smooth[N * N];
  for (int y = 0; y < N; y++) {
    for (int x = 0; x < N; x++) {
      float acc = 0.f;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          acc += cur[((y + dy + N) % N) * N + ((x + dx + N) % N)];
        }
      }
      smooth[y * N + x] = acc * (1.f / 9.f);
    }
  }
  for (int i = 0; i < N * N; i++) {
    dst_1024[i] = mean + (smooth[i] - sim_mid) * scale;
  }

}

// ---------------------------------------------------------------------------
// PC-REMAKE: characters (merc) receive sun shadows.
//
// Merc vertices arrive in VIEW space (the bone matrices bake the camera in),
// while the shadow map is indexed in world space. The background pipeline gives
// us the combined matrix A (perspective * view, as pc_camera) and the camera
// translation T, and merc gives us the perspective matrix P. From
//     clip = -(A * [world - T, 1])   and   clip = P * view_pos
// it follows that
//     light_clip = light_vp * Translate(T) * (-A^-1) * P * view_pos
// which collapses into the single matrix computed here.
// ---------------------------------------------------------------------------
namespace {
// column-major 4x4 helpers
void mat_mul(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float s = 0.f;
      for (int k = 0; k < 4; k++) {
        s += a[k * 4 + r] * b[c * 4 + k];
      }
      out[c * 4 + r] = s;
    }
  }
}

bool mat_inverse_impl(const float* m, float* inv) {
  float a[16];
  for (int i = 0; i < 16; i++) {
    a[i] = m[i];
  }
  for (int i = 0; i < 16; i++) {
    inv[i] = (i % 5 == 0) ? 1.f : 0.f;
  }
  for (int col = 0; col < 4; col++) {
    int piv = col;
    float best = std::fabs(a[col * 4 + col]);
    for (int r = col + 1; r < 4; r++) {
      float v = std::fabs(a[col * 4 + r]);
      if (v > best) {
        best = v;
        piv = r;
      }
    }
    if (best < 1e-9f) {
      return false;
    }
    if (piv != col) {
      for (int c = 0; c < 4; c++) {
        std::swap(a[c * 4 + col], a[c * 4 + piv]);
        std::swap(inv[c * 4 + col], inv[c * 4 + piv]);
      }
    }
    float d = a[col * 4 + col];
    for (int c = 0; c < 4; c++) {
      a[c * 4 + col] /= d;
      inv[c * 4 + col] /= d;
    }
    for (int r = 0; r < 4; r++) {
      if (r == col) {
        continue;
      }
      float f = a[col * 4 + r];
      if (f == 0.f) {
        continue;
      }
      for (int c = 0; c < 4; c++) {
        a[c * 4 + r] -= f * a[c * 4 + col];
        inv[c * 4 + r] -= f * inv[c * 4 + col];
      }
    }
  }
  return true;
}
}  // namespace

bool pc_remake_mat_inverse(const float* m, float* inv) {
  return mat_inverse_impl(m, inv);
}

bool compute_sun_light_from_camera(const math::Vector4f* camera_matrix,
                                   const math::Vector4f& camera_pos,
                                   const math::Vector4f* perspective,
                                   float* out16) {
  if (!g_sun_shadow.enabled() || !g_sun_shadow.ready) {
    return false;
  }
  float A[16], P[16];
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      A[c * 4 + r] = camera_matrix[c][r];
      P[c * 4 + r] = perspective[c][r];
    }
  }
  float Ainv[16];
  if (!mat_inverse_impl(A, Ainv)) {
    return false;
  }
  for (int i = 0; i < 16; i++) {
    Ainv[i] = -Ainv[i];  // the pipeline negates the transform
  }
  float step1[16];
  mat_mul(Ainv, P, step1);  // (-A^-1) * P

  float TR[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  TR[12] = camera_pos[0];
  TR[13] = camera_pos[1];
  TR[14] = camera_pos[2];
  float step2[16];
  mat_mul(TR, step1, step2);  // Translate(T) * ...

  // DOUBLE TAMPON : merc ÉCHANTILLONNE la carte → matrice de LECTURE (celle de
  // la carte complète de la frame précédente), jamais celle des casteurs
  mat_mul((g_sun_shadow.pingpong_primed && !g_sun_shadow.cache_mode) ? g_sun_shadow.receive_vp
                                                                     : g_sun_shadow.light_vp,
          step2, out16);
  return true;
}

// free-function bridge for the kernel binding (no graphics types in kmachine)
void ocean_sim_set_gain(float g) {
  g_ocean_sim.gain = g;
}

void ocean_sim_step_and_export(float* dst_1024) {
  // A/B switch: PC_REMAKE_OCEAN=0 keeps the original 2001 wave loop.
  static int s_enabled = -1;
  if (s_enabled < 0) {
    const char* e = getenv("PC_REMAKE_OCEAN");
    s_enabled = (e && e[0] == '0') ? 0 : 1;
  }
  if (g_ocean_sim.enabled && s_enabled) {
    g_ocean_sim.step_and_export(dst_1024);
  }
}
SunShadowMap g_sun_shadow;

bool g_pc_normal_target_ok = false;
void pc_normal_target(bool on) {
  if (!g_pc_normal_target_ok) {
    return;
  }
  if (on) {
    const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
  } else {
    const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, one);
  }
}
void (*g_sun_shadow_merc_hook)(void* shader_lib) = nullptr;

bool SunShadowMap::enabled() const {
  return g_sun_shadow_params[3] > 0.001f;
}

void SunShadowMap::maybe_update_for_frame(const float* cam_trans, unsigned long long frame_idx) {
  if (last_frame_stamp == frame_idx) {
    return;
  }
  last_frame_stamp = frame_idx;
  // DOUBLE TAMPON (17/08, ombres qui disparaissaient et revenaient en
  // mouvement) : la carte écrite la frame DERNIÈRE devient la carte de LECTURE
  // (avec receive_vp = la matrice qui l'a rendue), l'autre devient la cible des
  // casteurs de cette frame. Les receveurs lisent donc toujours une carte
  // COMPLÈTE et cohérente — plus jamais de trou d'une frame quand la matrice
  // bouge (re-ancrage ou soleil). Incompatible avec l'ancien cache statique
  // (cache_mode, off par défaut), qui garde alors l'ancien comportement.
  if (ready && !cache_mode) {
    std::swap(tex, static_tex);
    std::swap(fbo, static_fbo);
    std::memcpy(receive_vp, light_vp, sizeof(receive_vp));
    pingpong_primed = true;
  }
  // ANCRAGE : ne re-poser la matrice que sur événement rare — c'est ce qui rend
  // les ombres immobiles au sol pendant les déplacements du joueur.
  constexpr float kMeter = 4096.f;
  const float kMoveH = 8.f * kMeter;   // errance horizontale tolérée autour de l'ancre
  const float kMoveV = 12.f * kMeter;  // errance verticale tolérée
  // POSITION ANCRÉE, SOLEIL FLUIDE (v3) : les paliers de
  // soleil provoquaient une re-pose GLOBALE visible toutes les ~3 s (le soleil
  // du jeu est rapide). La vérité d'ingénierie : seule la POSITION doit être
  // gelée (c'est elle qui faisait ramper les ombres avec le joueur) ; le
  // soleil, lui, doit être suivi CONTINÛMENT — les ombres tournent alors
  // lentement et sans à-coups, comme dans la réalité.
  bool need = !ready || !anchor_valid;
  if (!need) {
    const float dx = cam_trans[0] - anchor_pos[0];
    const float dz = cam_trans[2] - anchor_pos[2];
    need = (dx * dx + dz * dz > kMoveH * kMoveH) ||
           (std::fabs(cam_trans[1] - anchor_pos[1]) > kMoveV);
  }
  if (need) {
    anchor_pos[0] = cam_trans[0];
    anchor_pos[1] = cam_trans[1];
    anchor_pos[2] = cam_trans[2];
    anchor_valid = true;
  }
  // matrice reconstruite chaque frame : position = l'ANCRE (stable), soleil =
  // continu ; la translation reste ancrée au texel dans update_matrix
  update_matrix(anchor_pos);
}

void SunShadowMap::update_matrix(const float* cam_trans) {
  // lazily create the depth FBOs (working map + static cache)
  if (!ready) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kSize, kSize, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    // seconde carte : partenaire du DOUBLE TAMPON (lue par les shaders après
    // échange) — paramètres identiques à la première, compare mode compris
    glGenTextures(1, &static_tex);
    glBindTexture(GL_TEXTURE_2D, static_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kSize, kSize, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &static_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, static_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, static_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ready = true;
  }

  // Sun direction: sheared top-down projection. The XZ footprint is axis
  // aligned (rock solid); sunlight slant comes from shearing X/Z by the sun's
  // horizontal components as depth decreases. Equivalent to a tilted ortho for
  // moderate elevations and much harder to get wrong.
  float dx = g_sun_shadow_params[0], dy = g_sun_shadow_params[1], dz = g_sun_shadow_params[2];
  if (dy > -0.2f) {
    dy = -0.2f;  // keep the sun meaningfully above the horizon
  }
  // shear per world unit of height: how far the shadow shifts horizontally
  float shear_x = -dx / dy;  // dy < 0
  float shear_z = -dz / dy;

  constexpr float kMeter = 4096.f;
  // 120 m de demi-côté (17/08, coupure visible — la coupure des ombres se voyait
  // depuis que la vue porte loin) ; texel ≈ 5,9 cm à 4096², le PCF lisse
  const float extent = 120.f * kMeter;     // half-size of the shadowed square
  const float up = 150.f * kMeter;         // top of the depth volume above camera
  const float depth_range = 300.f * kMeter;

  float cx = cam_trans[0], cy = cam_trans[1], cz = cam_trans[2];

  // Only the vertical is snapped here. Snapping cx/cz in WORLD space was the
  // bug: the horizontal light-space offset is not cx/cz, it is
  // (shear * top_y - c), and top_y moves with the camera height. Snapping one
  // and not the other left a sub-texel residue that shifted every frame, which
  // is what made the shadows swim with the camera. The real offset is snapped
  // further down instead, in light space, where the texel grid actually lives.
  const float texel = (2.f * extent) / (float)kSize;
  cy = std::floor(cy / (texel * 4.f)) * (texel * 4.f);  // coarser vertical grid

  // light space:
  //   u = (wx + shear_x * (top_y - wy) - cx) / extent
  //   v = (wz + shear_z * (top_y - wy) - cz) / extent
  //   d = (top_y - wy) / depth_range * 2 - 1     (near = high above)
  const float top_y = cy + up;
  const float su = 1.f / extent;
  const float sv = 1.f / extent;
  const float sd = 2.f / depth_range;

  // column-major: clip = M * world
  // u = su*wx - su*shear_x*wy + 0*wz + su*(shear_x*top_y - cx)
  light_vp[0] = su;
  light_vp[1] = 0.f;
  light_vp[2] = 0.f;
  light_vp[3] = 0.f;
  light_vp[4] = -su * shear_x;
  light_vp[5] = -sv * shear_z;
  light_vp[6] = -sd;
  light_vp[7] = 0.f;
  light_vp[8] = 0.f;
  light_vp[9] = sv;
  light_vp[10] = 0.f;
  light_vp[11] = 0.f;
  // Snap the translation to whole texels of the shadow map, in the [-1,1] clip
  // space it is expressed in. This is what pins the sampling grid to the world:
  // the map now steps texel by texel as the camera moves instead of sliding
  // continuously underneath the geometry.
  // Centre the volume at the CAMERA's height, not at the top of the depth
  // volume. u = su*(wx - cx) + su*shear*(top_y - wy); referencing the offset to
  // top_y meant that as soon as the sun tilted, the whole footprint slid by
  // shear * (top_y - cy) — up to three map widths. Shadows then only existed
  // near noon, when the shear is zero.
  const float ndc_texel = 2.f / (float)kSize;
  float off_u = su * (shear_x * cy - cx);
  float off_v = sv * (shear_z * cy - cz);
  off_u = std::floor(off_u / ndc_texel) * ndc_texel;
  off_v = std::floor(off_v / ndc_texel) * ndc_texel;
  light_vp[12] = off_u;
  light_vp[13] = off_v;
  light_vp[14] = sd * top_y - 1.f;
  light_vp[15] = 1.f;

  rendered_this_frame = false;

  // PC-REMAKE perf: static-cache bookkeeping. A rebuild that started last frame
  // is complete now (both TIE and TFRAG replayed into it during the frame).
  if (static_pass_cleared) {
    static_dirty = false;
    static_ready = true;
    frames_since_static = 0;
    static_pass_cleared = false;
  }
  frames_since_static++;
  // dirty when the texel-snapped matrix moved, plus a safety refresh so level
  // streaming (new chunks) reaches the cache within a couple of seconds
  if (memcmp(light_vp, cached_vp, sizeof(light_vp)) != 0 || frames_since_static > 120) {
    static_dirty = true;
  }
  // Le cache est DÉSACTIVÉ par défaut : à chaque franchissement de texel par la
  // caméra, une image mélangeait cache périmé et matrice fraîche → clignotement,
  // et la mesure n'a montré AUCUN gain fps. Replays complets
  // chaque frame = le chemin éprouvé. `touch /tmp/shadow_cache` pour ré-essayer.
  static int cache_check = 0;
  if ((cache_check++ & 63) == 0) {
    if (FILE* f = fopen("/tmp/shadow_cache", "r")) {
      fclose(f);
      cache_mode = true;
    } else {
      cache_mode = false;
    }
  }
  if (!cache_mode) {
    static_dirty = true;
    static_ready = false;  // le blit du cache saute aussi : clear + replays complets
  }
  // 14/10 : A/B de la passe differee (une lecture de fichier toutes les 64 images)
  if ((cache_check & 63) == 1) {
    qa_immediate = fs::exists("/tmp/qa_shadow_immediate");
  }
}

// Depth-only raster state shared by every shadow pass.
static void shadow_pass_raster_state() {
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(2.f, 4.f);
}

void SunShadowMap::ensure_frame_init(void* shader_lib) {
  if (rendered_this_frame) {
    return;
  }
  rendered_this_frame = true;
  int saved_fbo = 0;
  int saved_vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_fbo);
  glGetIntegerv(GL_VIEWPORT, saved_vp);
  // Unbind from the receive unit while we write (feedback loop = flicker).
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);

  // Seed the working map with the cached static scenery...
  if (static_ready) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, kSize, kSize, 0, 0, kSize, kSize, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  } else {
    // ...or clear it while the cache does not exist yet. The game runs the
    // PS2's REVERSED depth convention and leaves its global clear depth at 0 —
    // force 1 and restore (see the static replay clear as well).
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLfloat saved_clear_depth = 0.f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &saved_clear_depth);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(saved_clear_depth);
  }
  // Dynamic character/prop casters, poses from the previous (invisible) frame.
  glViewport(0, 0, kSize, kSize);
  shadow_pass_raster_state();
  if (g_sun_shadow_merc_hook) {
    g_sun_shadow_merc_hook(shader_lib);
  }
  glDisable(GL_POLYGON_OFFSET_FILL);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // hand the working map back to the receivers and restore the caller
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, tex);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
  glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]);
}

bool SunShadowMap::begin_static_replay(void* shader_lib) {
  ensure_frame_init(shader_lib);
  if (!static_dirty) {
    return false;  // cache is fresh — the caller skips its 3.7M-index replay
  }
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_saved_fbo);
  glGetIntegerv(GL_VIEWPORT, m_saved_viewport);
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  if (cache_mode) {
    glBindFramebuffer(GL_FRAMEBUFFER, static_fbo);
  } else {
    // chemin éprouvé : replays directs dans la carte reçue par les shaders
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  }
  glViewport(0, 0, kSize, kSize);
  shadow_pass_raster_state();
  if (cache_mode && !static_pass_cleared) {
    // first static replay of the rebuild: clear the cache and remember the
    // matrix it is built for
    GLfloat saved_clear_depth = 0.f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &saved_clear_depth);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(saved_clear_depth);
    memcpy(cached_vp, light_vp, sizeof(cached_vp));
    static_pass_cleared = true;
  }
  auto* shaders = static_cast<ShaderLibrary*>(shader_lib);
  auto& shader = (*shaders)[ShaderId::SHADOW_DEPTH];
  shader.activate();
  glUniformMatrix4fv(pc_uniform_loc(shader.id(), "light_vp"), 1, GL_FALSE, light_vp);
  // opaque caster by default; the wind pass raises this for cut-out foliage
  glUniform1f(pc_uniform_loc(shader.id(), "alpha_min"), 0.f);
  return true;
}

void SunShadowMap::begin_dynamic_pass(void* shader_lib) {
  ensure_frame_init(shader_lib);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_saved_fbo);
  glGetIntegerv(GL_VIEWPORT, m_saved_viewport);
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);  // the WORKING map, every frame
  glViewport(0, 0, kSize, kSize);
  shadow_pass_raster_state();
  auto* shaders = static_cast<ShaderLibrary*>(shader_lib);
  auto& shader = (*shaders)[ShaderId::SHADOW_DEPTH];
  shader.activate();
  glUniformMatrix4fv(pc_uniform_loc(shader.id(), "light_vp"), 1, GL_FALSE, light_vp);
  glUniform1f(pc_uniform_loc(shader.id(), "alpha_min"), 0.f);
}

void SunShadowMap::end_depth_pass() {
  // debug: dump the depth map to a PGM when the flag file exists
  {
    // PERF 25/08 : le fopen tournait A CHAQUE passe (plusieurs par frame) —
    // un syscall par passe pour un outil de debug. Sonde 1 passe sur 64.
    static int qa_gate = 0;
    static int cool = 0;
    if ((qa_gate++ & 63) != 0) {
      // pas de sonde cette passe
    } else if (cool > 0) {
      cool--;
    } else if (FILE* f = fopen("/tmp/dump_shadow", "r")) {
      fclose(f);
      remove("/tmp/dump_shadow");
      cool = 60;
      std::vector<float> buf(kSize * kSize);
      glBindTexture(GL_TEXTURE_2D, tex);
      // reading a depth texture while COMPARE_REF_TO_TEXTURE is on is undefined
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
      glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, buf.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
      FILE* out = fopen("/tmp/shadowmap.pgm", "wb");
      if (out) {
        fprintf(out, "P5\n%d %d\n255\n", kSize, kSize);
        for (int i = 0; i < kSize * kSize; i++) {
          unsigned char v = (unsigned char)(buf[i] * 255.f);
          fwrite(&v, 1, 1, out);
        }
        fclose(out);
        lg::warn("[QA] carte d'ombres exportee");
      }
    }
  }
  glDisable(GL_POLYGON_OFFSET_FILL);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // make the map available again to the receivers that follow
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, tex);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, m_saved_fbo);
  glViewport(m_saved_viewport[0], m_saved_viewport[1], m_saved_viewport[2], m_saved_viewport[3]);
}

void SunShadowMap::flush_deferred(void* shader_lib, unsigned long long frame_idx) {
  const bool active = enabled() && ready && deferred() && last_frame_stamp == frame_idx;
  if (!active) {
    deferred_static.clear();
    deferred_wind.clear();
    return;
  }
  // effacement de la carte de travail + personnages (crochet merc), comme avant
  ensure_frame_init(shader_lib);
  if (deferred_static.empty() && deferred_wind.empty()) {
    return;
  }
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_saved_fbo);
  glGetIntegerv(GL_VIEWPORT, m_saved_viewport);
  GLint saved_vao = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, kSize, kSize);
  shadow_pass_raster_state();
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  auto* shaders = static_cast<ShaderLibrary*>(shader_lib);
  auto& shader = (*shaders)[ShaderId::SHADOW_DEPTH];
  shader.activate();
  const GLint loc_vp = pc_uniform_loc(shader.id(), "light_vp");
  const GLint loc_amin = pc_uniform_loc(shader.id(), "alpha_min");
  // 1. decor statique (terrain + objets), memes plages que le rejeu immediat
  glUniformMatrix4fv(loc_vp, 1, GL_FALSE, light_vp);
  glUniform1f(loc_amin, 0.f);
  for (const auto& d : deferred_static) {
    glBindVertexArray(d.vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ebo);
    glMultiDrawElements(d.mode, d.counts.data(), GL_UNSIGNED_INT, d.offsets.data(),
                        (GLsizei)d.counts.size());
  }
  // 2. couronnes au vent (test d'alpha, une matrice par instance)
  if (!deferred_wind.empty()) {
    glUniform1f(loc_amin, 0.45f);  // lacy, not a blob
    glUniform1i(pc_uniform_loc(shader.id(), "tex_T0"), 0);
    glActiveTexture(GL_TEXTURE0);
    for (const auto& d : deferred_wind) {
      glBindVertexArray(d.vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.ebo);
      glBindTexture(GL_TEXTURE_2D, d.tex);
      for (size_t i = 0; i < d.counts.size(); i++) {
        glUniformMatrix4fv(loc_vp, 1, GL_FALSE, d.mats[i].data());
        glDrawElements(d.mode, d.counts[i], GL_UNSIGNED_INT, d.offsets[i]);
      }
    }
  }
  glBindVertexArray(saved_vao);
  deferred_static.clear();
  deferred_wind.clear();
  end_depth_pass();
}

void SunShadowMap::bind_disabled(unsigned int program) {
  if (!ready) {
    // cree les textures/FBO (jamais fait si le jeu demarre dans un interieur)
    float origin[3] = {0.f, 0.f, 0.f};
    update_matrix(origin);
  }
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(pc_uniform_loc(program, "sun_shadow_tex"), 14);
  glUniform1f(pc_uniform_loc(program, "sun_shadow_strength"), 0.f);
}

void SunShadowMap::bind_for_receive(unsigned int program) const {
  glActiveTexture(GL_TEXTURE14);
  // DOUBLE TAMPON : on lit la carte COMPLÈTE de la frame précédente (static_tex
  // après l'échange du début de frame) avec la matrice qui l'a rendue
  glBindTexture(GL_TEXTURE_2D, (pingpong_primed && !cache_mode) ? static_tex : tex);
  // 21/09 QA : `touch /tmp/dump_shadow_rx` -> dump de la carte telle que LUE par
  // les receveurs (complete, frame precedente) dans /tmp/shadowmap_rx.pgm
  {
    static int gate = 0;
    if ((gate++ & 63) == 0) {
      if (FILE* f = fopen("/tmp/dump_shadow_rx", "r")) {
        fclose(f);
        remove("/tmp/dump_shadow_rx");
        std::vector<float> buf(kSize * kSize);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, buf.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        if (FILE* out = fopen("/tmp/shadowmap_rx.pgm", "wb")) {
          fprintf(out, "P5\n%d %d\n255\n", kSize, kSize);
          for (int i = 0; i < kSize * kSize; i++) {
            unsigned char v = (unsigned char)(std::min(1.f, std::max(0.f, buf[i])) * 255.f);
            fwrite(&v, 1, 1, out);
          }
          fclose(out);
        }
        lg::warn("[QA] dump carte d'ombres (lecture) : strength={} primed={} vp15={} ancre=({:.0f},{:.0f},{:.0f}) dir=({:.2f},{:.2f},{:.2f}) vp14={:.4f} vp5={:.6f} vp12={:.4f} vp13={:.4f}",
                 g_sun_shadow_params[3], pingpong_primed, receive_vp[15], anchor_pos[0] / 4096.f,
                 anchor_pos[1] / 4096.f, anchor_pos[2] / 4096.f, g_sun_shadow_params[0],
                 g_sun_shadow_params[1], g_sun_shadow_params[2], receive_vp[14], receive_vp[5],
                 receive_vp[12], receive_vp[13]);
      }
    }
  }
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(pc_uniform_loc(program, "sun_shadow_tex"), 14);
  // direction du soleil pour le biais de pente (tfrag3.frag)
  glUniform3f(pc_uniform_loc(program, "sun_dir_w"), g_sun_shadow_params[0],
              g_sun_shadow_params[1], g_sun_shadow_params[2]);
  glUniformMatrix4fv(pc_uniform_loc(program, "sun_light_vp"), 1, GL_FALSE,
                     (pingpong_primed && !cache_mode) ? receive_vp : light_vp);
  glUniform1f(pc_uniform_loc(program, "sun_shadow_strength"),
              (ready && (!pingpong_primed || cache_mode || receive_vp[15] != 0.f))
                  ? g_sun_shadow_params[3]
                  : 0.f);
}

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/pipelines/opengl.h"

int pc_uniform_loc(unsigned int program, const char* literal_name) {
  // cle = programme + adresse du litteral (les litteraux sont stables)
  static std::unordered_map<u64, int> cache;
  const u64 key = ((u64)program << 48) ^ (u64)(uintptr_t)literal_name;
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }
  const int loc = glGetUniformLocation(program, literal_name);
  cache.emplace(key, loc);
  return loc;
}

DoubleDraw setup_opengl_from_draw_mode(DrawMode mode, u32 tex_unit, bool mipmap) {
  glActiveTexture(tex_unit);

  if (mode.get_zt_enable()) {
    glEnable(GL_DEPTH_TEST);
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        glDepthFunc(GL_NEVER);
        break;
      case GsTest::ZTest::ALWAYS:
        glDepthFunc(GL_ALWAYS);
        break;
      case GsTest::ZTest::GEQUAL:
        glDepthFunc(GL_GEQUAL);
        break;
      case GsTest::ZTest::GREATER:
        glDepthFunc(GL_GREATER);
        break;
      default:
        ASSERT(false);
    }
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  DoubleDraw double_draw;

  bool should_enable_blend = false;
  if (mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED) {
    should_enable_blend = true;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        should_enable_blend = false;
        // (SRC - SRC) * alpha + SRC = SRC, no blend.
        break;
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        // Cv = (Cs - Cd) * FIX + Cd
        // Cs * FIX * 0.5
        // Cd * FIX * 0.5
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO);
        glBlendColor(0.5, 0.5, 0.5, 0.5);
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        glBlendFunc(GL_DST_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        double_draw.color_mult = 0.5f;
        break;
      default:
        ASSERT(false);
    }
  } else {
    should_enable_blend = false;
  }

  if (should_enable_blend) {
    glEnable(GL_BLEND);
  } else {
    glDisable(GL_BLEND);
  }

  if (mode.get_clamp_s_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }

  if (mode.get_clamp_t_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }

  if (mode.get_filt_enable()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    // PC-REMAKE 25/08 (rochers lointains completement bugges) :
    // en mode « sans filtrage » (choix PS2 des artistes), la minification etait
    // en GL_NEAREST SANS mipmap — a 500 m un pixel d'ecran couvre des dizaines
    // de texels, le tirage point-a-point donnait un moire en croisillons et une
    // couleur delavee sur tout l'horizon. La MAGNIFICATION reste NEAREST (le
    // rendu net voulu de pres est intact) ; seule la minification lointaine
    // passe par les mipmaps.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }

  // for some reason, they set atest NEVER + FB_ONLY to disable depth writes
  bool alpha_hack_to_disable_z_write = false;

  float alpha_min = 0.;
  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_min = mode.get_aref() / 127.f;
        switch (mode.get_alpha_fail()) {
          case GsTest::AlphaFail::KEEP:
            // ok, no need for double draw
            break;
          case GsTest::AlphaFail::FB_ONLY:
            if (mode.get_depth_write_enable()) {
              // darn, we need to draw twice
              double_draw.kind = DoubleDrawKind::AFAIL_NO_DEPTH_WRITE;
              double_draw.aref_second = alpha_min;
            } else {
              alpha_min = 0.f;
            }
            break;
          default:
            ASSERT(false);
        }
        break;
      case DrawMode::AlphaTest::NEVER:
        if (mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
          alpha_hack_to_disable_z_write = true;
        } else {
          ASSERT(false);
        }
        break;
      default:
        ASSERT(false);
    }
  }

  // PC-REMAKE: smooth alpha-tested edges (foliage) by converting alpha into
  // MSAA coverage — this is what makes grass blades look clean instead of
  // stair-stepped. No-op when MSAA is off. Explicitly disabled otherwise so
  // the state never leaks into non-cutout draws.
  // SEULEMENT les vraies découpes (seuil ≥ 0.45, feuillage/herbe) : les draws à
  // seuil BAS utilisent leur alpha comme simple test — le convertir en
  // couverture rendait caisses, piliers et cheveux SEMI-TRANSPARENTS en MSAA 4
  // (enquête du 16/08 ; invisible sur le M2 qui ne
  // tournait pas en MSAA 4).
  if (alpha_min >= 0.45f && mode.get_depth_write_enable()) {
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  } else {
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  }

  if (mode.get_depth_write_enable() && !alpha_hack_to_disable_z_write) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }
  double_draw.aref_first = alpha_min;
  return double_draw;
}

DoubleDraw setup_tfrag_shader(SharedRenderState* render_state, DrawMode mode, ShaderId shader) {
  auto draw_settings = setup_opengl_from_draw_mode(mode, GL_TEXTURE0, true);
  auto sh_id = render_state->shaders[shader].id();
  if (auto u_id = pc_uniform_loc(sh_id, "alpha_min"); u_id != -1) {
    glUniform1f(u_id, draw_settings.aref_first);
  }
  if (auto u_id = pc_uniform_loc(sh_id, "alpha_max"); u_id != -1) {
    glUniform1f(u_id, 10.f);
  }
  if (auto u_id = pc_uniform_loc(sh_id, "force_opaque"); u_id != -1) {
    glUniform1f(u_id, g_pc_force_opaque);
  }
  return draw_settings;
}

std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z) {
  // renderers may eventually have tricks to do things in local coordinates - so use the convention
  // that the shader has already subtracted off the camera translation from the vertex position.
  // (I think this could help with accuracy too, since you aren't rotating and subtracting two large
  // vectors that are very close to each other)

  // this is the perspective x-scaling. This is used to map to a 256-pixel buffer.
  const float game_pxx = persp[0][0];
  // on PC, OpenGL uses normalized coordinates for drawing, so divide by the pixel width.
  // in the game, the perspective divide includes a multiplication by the fog constant, for PC,
  // just include this multiply here so we can let OpenGL do the perspective multiply.
  const float pc_pxx = fog_constant * game_pxx / 256.f;

  // this is the perspective y-scaling.
  const float game_pyy = persp[1][1];
  // same logic as y - there's a later SCISSOR scaling in the shader that expects this ratio.
  const float pc_pyy = -fog_constant * game_pyy / 128.f;

  // the depth is considered twice. Once, as the value to write into the depth buffer, which is
  // scaled for PC here:
  const float depth_scale = fog_constant * persp[2][2] / 8388608;

  // and once as the value used for perspective divide
  const float game_pzw = persp[2][3];
  const float game_depth_offset = persp[3][2];

  // set up PC scaling values
  math::Vector3f persp_scale(pc_pxx, pc_pyy, depth_scale);

  // it turns out that shifting the depth buffer to line up with OpenGL is equivalent to adding
  // transformed.w * (hvdf_z / 8388608.f - 1.f) to the depth value. We know that w is just depth *
  // pzw, so we can include the effect here:
  const float pc_z_offset = (hvdf_z / 8388608.f - 1.f);
  persp_scale.z() += pc_z_offset * game_pzw;

  std::array<math::Vector4f, 4> result;
  for (auto& x : result) {
    x.set_zero();
  }

  // fill out the upper 3x3 - simply scale the rotation matrix by the perspective scale.
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      result[row][col] = cam_T_w[row][col] * persp_scale[col];
    }
  }

  // fill out the right most column. This converts world-space points to depth for divide, scaled by
  // pzw. for now, copy the game.
  for (int row = 0; row < 3; row++) {
    result[row][3] = cam_T_w[row][2] * game_pzw;
  }

  // depth buffer offset - now needs to be scaled by the PC depth buffer scaling too
  result[3][2] = fog_constant * game_depth_offset / 8388608;

  return result;
}

void first_tfrag_draw_setup(const GoalBackgroundCameraData& settings,
                            SharedRenderState* render_state,
                            ShaderId shader) {
  const auto& sh = render_state->shaders[shader];
  sh.activate();
  auto id = sh.id();
  glUniform1i(pc_uniform_loc(id, "gfx_hack_no_tex"), Gfx::g_global_settings.hack_no_tex);
  glUniform1i(pc_uniform_loc(id, "decal"), false);
  glUniform1i(pc_uniform_loc(id, "tex_T0"), 0);
  glUniformMatrix4fv(pc_uniform_loc(id, "camera"), 1, GL_FALSE, settings.camera[0].data());

  auto newcam =
      make_new_cam_mat(settings.rot, settings.perspective, settings.fog.x(), settings.hvdf_off.z());

  /*
  fmt::print("camera:\n{}\n{}\n{}\n{}\n", settings.camera[0].to_string_aligned(),
             settings.camera[1].to_string_aligned(), settings.camera[2].to_string_aligned(),
             settings.camera[3].to_string_aligned());

  fmt::print("camera2:\n{}\n{}\n{}\n{}\n", newcam[0].to_string_aligned(),
             newcam[1].to_string_aligned(), newcam[2].to_string_aligned(),
             newcam[3].to_string_aligned());

  fmt::print("persp:\n{}\n{}\n{}\n{}\n", settings.perspective[0].to_string_aligned(),
             settings.perspective[1].to_string_aligned(),
             settings.perspective[2].to_string_aligned(),
             settings.perspective[3].to_string_aligned());
  fmt::print("rot:\n{}\n{}\n{}\n{}\n", settings.rot[0].to_string_aligned(),
             settings.rot[1].to_string_aligned(), settings.rot[2].to_string_aligned(),
             settings.rot[3].to_string_aligned());
  fmt::print("ctrans: {}\n", settings.trans.to_string_aligned());
  fmt::print("hvdf: {}\n", settings.hvdf_off.to_string_aligned());
  */

  glUniformMatrix4fv(pc_uniform_loc(id, "pc_camera"), 1, GL_FALSE, newcam[0].data());

  glUniform4f(pc_uniform_loc(id, "hvdf_offset"), settings.hvdf_off[0], settings.hvdf_off[1],
              settings.hvdf_off[2], settings.hvdf_off[3]);
  glUniform4f(pc_uniform_loc(id, "cam_trans"), settings.trans[0], settings.trans[1],
              settings.trans[2], settings.trans[3]);
  glUniform1f(pc_uniform_loc(id, "fog_constant"), settings.fog.x());
  glUniform1f(pc_uniform_loc(id, "fog_min"), settings.fog.y());
  glUniform1f(pc_uniform_loc(id, "fog_max"), settings.fog.z());
  glUniform4f(pc_uniform_loc(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
}

void interp_time_of_day_slow(const math::Vector<s32, 4> itimes[4],
                             const tfrag3::PackedTimeOfDay& in,
                             math::Vector<u8, 4>* out) {
  math::Vector<u16, 4> weights[8];
  for (int component = 0; component < 8; component++) {
    int quad_idx = component / 2;
    int word_off = (component % 2 * 2);
    for (int channel = 0; channel < 4; channel++) {
      int word = word_off + (channel / 2);
      int hw_off = channel % 2;

      u32 word_val = itimes[quad_idx][word];
      u32 hw_val = hw_off ? (word_val >> 16) : word_val;
      hw_val = hw_val & 0xff;
      weights[component][channel] = hw_val;
    }
  }

  math::Vector<u16, 4> temp[4];

  for (u32 color_quad = 0; color_quad < (in.color_count + 3) / 4; color_quad++) {
    for (auto& x : temp) {
      x.set_zero();
    }

    const u8* input_ptr = in.data.data() + color_quad * 128;
    for (u32 component = 0; component < 8; component++) {
      for (u32 color = 0; color < 4; color++) {
        for (u32 channel = 0; channel < 4; channel++) {
          temp[color][channel] += weights[component][channel] * (*input_ptr);
          input_ptr++;
        }
      }
    }

    for (u32 color = 0; color < 4; color++) {
      auto& o = out[color_quad * 4 + color];
      for (u32 channel = 0; channel < 3; channel++) {
        o[channel] = std::min(255, temp[color][channel] >> 6);
      }
      o[3] = std::min(128, temp[color][3] >> 6);
    }
  }
}

void interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                        const tfrag3::PackedTimeOfDay& packed_colors,
                        math::Vector<u8, 4>* out) {
  math::Vector<u16, 4> weights[8];
  for (int component = 0; component < 8; component++) {
    int quad_idx = component / 2;
    int word_off = (component % 2 * 2);
    for (int channel = 0; channel < 4; channel++) {
      int word = word_off + (channel / 2);
      int hw_off = channel % 2;

      u32 word_val = itimes[quad_idx][word];
      u32 hw_val = hw_off ? (word_val >> 16) : word_val;
      hw_val = hw_val & 0xff;
      weights[component][channel] = hw_val;
    }
  }

  // weight multipliers
  __m128i weights0 = _mm_setr_epi16(weights[0][0], weights[0][1], weights[0][2], weights[0][3],
                                    weights[0][0], weights[0][1], weights[0][2], weights[0][3]);
  __m128i weights1 = _mm_setr_epi16(weights[1][0], weights[1][1], weights[1][2], weights[1][3],
                                    weights[1][0], weights[1][1], weights[1][2], weights[1][3]);
  __m128i weights2 = _mm_setr_epi16(weights[2][0], weights[2][1], weights[2][2], weights[2][3],
                                    weights[2][0], weights[2][1], weights[2][2], weights[2][3]);
  __m128i weights3 = _mm_setr_epi16(weights[3][0], weights[3][1], weights[3][2], weights[3][3],
                                    weights[3][0], weights[3][1], weights[3][2], weights[3][3]);
  __m128i weights4 = _mm_setr_epi16(weights[4][0], weights[4][1], weights[4][2], weights[4][3],
                                    weights[4][0], weights[4][1], weights[4][2], weights[4][3]);
  __m128i weights5 = _mm_setr_epi16(weights[5][0], weights[5][1], weights[5][2], weights[5][3],
                                    weights[5][0], weights[5][1], weights[5][2], weights[5][3]);
  __m128i weights6 = _mm_setr_epi16(weights[6][0], weights[6][1], weights[6][2], weights[6][3],
                                    weights[6][0], weights[6][1], weights[6][2], weights[6][3]);
  __m128i weights7 = _mm_setr_epi16(weights[7][0], weights[7][1], weights[7][2], weights[7][3],
                                    weights[7][0], weights[7][1], weights[7][2], weights[7][3]);

  // saturation: note that alpha is saturated to 128 but the rest are 255.
  // TODO: maybe we should saturate to 255 for everybody (can do this using a single packus) and
  // change the shader to deal with this.
  __m128i sat = _mm_set_epi16(128, 255, 255, 255, 128, 255, 255, 255);

  for (u32 color_quad = 0; color_quad < packed_colors.color_count / 4; color_quad++) {
    // first, load colors. We put 16 bytes / register and don't touch the upper half because we
    // convert u8s to u16s.
    {
      const u8* base = packed_colors.data.data() + color_quad * 128;
      __m128i color0_p = _mm_loadu_si64((const __m128i*)(base + 0));
      __m128i color1_p = _mm_loadu_si64((const __m128i*)(base + 16));
      __m128i color2_p = _mm_loadu_si64((const __m128i*)(base + 32));
      __m128i color3_p = _mm_loadu_si64((const __m128i*)(base + 48));
      __m128i color4_p = _mm_loadu_si64((const __m128i*)(base + 64));
      __m128i color5_p = _mm_loadu_si64((const __m128i*)(base + 80));
      __m128i color6_p = _mm_loadu_si64((const __m128i*)(base + 96));
      __m128i color7_p = _mm_loadu_si64((const __m128i*)(base + 112));

      // unpack to 16-bits. each has 16x 16 bit colors.
      __m128i color0 = _mm_cvtepu8_epi16(color0_p);
      __m128i color1 = _mm_cvtepu8_epi16(color1_p);
      __m128i color2 = _mm_cvtepu8_epi16(color2_p);
      __m128i color3 = _mm_cvtepu8_epi16(color3_p);
      __m128i color4 = _mm_cvtepu8_epi16(color4_p);
      __m128i color5 = _mm_cvtepu8_epi16(color5_p);
      __m128i color6 = _mm_cvtepu8_epi16(color6_p);
      __m128i color7 = _mm_cvtepu8_epi16(color7_p);

      // multiply by weights
      color0 = _mm_mullo_epi16(color0, weights0);
      color1 = _mm_mullo_epi16(color1, weights1);
      color2 = _mm_mullo_epi16(color2, weights2);
      color3 = _mm_mullo_epi16(color3, weights3);
      color4 = _mm_mullo_epi16(color4, weights4);
      color5 = _mm_mullo_epi16(color5, weights5);
      color6 = _mm_mullo_epi16(color6, weights6);
      color7 = _mm_mullo_epi16(color7, weights7);

      // add. This order minimizes dependencies.
      color0 = _mm_adds_epi16(color0, color1);
      color2 = _mm_adds_epi16(color2, color3);
      color4 = _mm_adds_epi16(color4, color5);
      color6 = _mm_adds_epi16(color6, color7);

      color0 = _mm_adds_epi16(color0, color2);
      color4 = _mm_adds_epi16(color4, color6);

      color0 = _mm_adds_epi16(color0, color4);

      // divide, because we multiplied our weights by 2^7.
      color0 = _mm_srli_epi16(color0, 6);

      // saturate
      color0 = _mm_min_epu16(sat, color0);

      // back to u8s.
      auto result = _mm_packus_epi16(color0, color0);

      // store result
      _mm_storel_epi64((__m128i*)(&out[color_quad * 4]), result);
    }

    {
      const u8* base = packed_colors.data.data() + color_quad * 128 + 8;
      __m128i color0_p = _mm_loadu_si64((const __m128i*)(base + 0));
      __m128i color1_p = _mm_loadu_si64((const __m128i*)(base + 16));
      __m128i color2_p = _mm_loadu_si64((const __m128i*)(base + 32));
      __m128i color3_p = _mm_loadu_si64((const __m128i*)(base + 48));
      __m128i color4_p = _mm_loadu_si64((const __m128i*)(base + 64));
      __m128i color5_p = _mm_loadu_si64((const __m128i*)(base + 80));
      __m128i color6_p = _mm_loadu_si64((const __m128i*)(base + 96));
      __m128i color7_p = _mm_loadu_si64((const __m128i*)(base + 112));

      // unpack to 16-bits. each has 16x 16 bit colors.
      __m128i color0 = _mm_cvtepu8_epi16(color0_p);
      __m128i color1 = _mm_cvtepu8_epi16(color1_p);
      __m128i color2 = _mm_cvtepu8_epi16(color2_p);
      __m128i color3 = _mm_cvtepu8_epi16(color3_p);
      __m128i color4 = _mm_cvtepu8_epi16(color4_p);
      __m128i color5 = _mm_cvtepu8_epi16(color5_p);
      __m128i color6 = _mm_cvtepu8_epi16(color6_p);
      __m128i color7 = _mm_cvtepu8_epi16(color7_p);

      // multiply by weights
      color0 = _mm_mullo_epi16(color0, weights0);
      color1 = _mm_mullo_epi16(color1, weights1);
      color2 = _mm_mullo_epi16(color2, weights2);
      color3 = _mm_mullo_epi16(color3, weights3);
      color4 = _mm_mullo_epi16(color4, weights4);
      color5 = _mm_mullo_epi16(color5, weights5);
      color6 = _mm_mullo_epi16(color6, weights6);
      color7 = _mm_mullo_epi16(color7, weights7);

      // add. This order minimizes dependencies.
      color0 = _mm_adds_epi16(color0, color1);
      color2 = _mm_adds_epi16(color2, color3);
      color4 = _mm_adds_epi16(color4, color5);
      color6 = _mm_adds_epi16(color6, color7);

      color0 = _mm_adds_epi16(color0, color2);
      color4 = _mm_adds_epi16(color4, color6);

      color0 = _mm_adds_epi16(color0, color4);

      // divide, because we multiplied our weights by 2^7.
      color0 = _mm_srli_epi16(color0, 6);

      // saturate
      color0 = _mm_min_epu16(sat, color0);

      // back to u8s.
      auto result = _mm_packus_epi16(color0, color0);

      // store result
      _mm_storel_epi64((__m128i*)(&out[color_quad * 4 + 2]), result);
    }
  }
}

bool sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes) {
  math::Vector4f acc =
      planes[0] * sphere.x() + planes[1] * sphere.y() + planes[2] * sphere.z() - planes[3];

  return acc.x() > -sphere.w() && acc.y() > -sphere.w() && acc.z() > -sphere.w() &&
         acc.w() > -sphere.w();
}

void shadow_vis_from_light(const float* vp,
                           const std::vector<tfrag3::VisNode>& nodes,
                           std::vector<u8>& out) {
  out.resize(nodes.size());
  // rayon de la sphere exprime dans chaque axe de l'espace lumiere (borne sure)
  const float ku = std::fabs(vp[0]) + std::fabs(vp[4]) + std::fabs(vp[8]);
  const float kv = std::fabs(vp[1]) + std::fabs(vp[5]) + std::fabs(vp[9]);
  const float kd = std::fabs(vp[2]) + std::fabs(vp[6]) + std::fabs(vp[10]);
  for (size_t i = 0; i < nodes.size(); i++) {
    const auto& b = nodes[i].bsphere;
    const float x = b.x(), y = b.y(), z = b.z(), r = b.w();
    const float u = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    const float v = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    const float d = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
    out[i] = std::fabs(u) <= 1.f + r * ku && std::fabs(v) <= 1.f + r * kv &&
             std::fabs(d) <= 1.f + r * kd;
  }
}

u64 append_shadow_ranges(const std::vector<tfrag3::StripDraw>& draws,
                         size_t first,
                         size_t end,
                         const std::vector<u8>& vis,
                         std::vector<GLsizei>& counts,
                         std::vector<void*>& offsets) {
  u64 kept = 0;
  for (size_t i = first; i < end && i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    for (auto& grp : draw.vis_groups) {
      const bool v = grp.vis_idx_in_pc_bvh == UINT16_MAX ||
                     (grp.vis_idx_in_pc_bvh < vis.size() && vis[grp.vis_idx_in_pc_bvh]);
      if (v && grp.num_inds) {
        kept += grp.num_inds;
        const u64 prev_end =
            counts.empty() ? ~0ull : (u64)(size_t)offsets.back() / sizeof(u32) + counts.back();
        if (prev_end == iidx) {
          counts.back() += grp.num_inds;
        } else {
          counts.push_back(grp.num_inds);
          offsets.push_back((void*)(iidx * sizeof(u32)));
        }
      }
      iidx += grp.num_inds;
    }
  }
  return kept;
}

// this isn't super efficient, but we spend so little time here it's not worth it to go faster.
void cull_check_all_slow(const math::Vector4f* planes,
                         const std::vector<tfrag3::VisNode>& nodes,
                         const u8* level_occlusion_string,
                         u8* out) {
  if (level_occlusion_string) {
    for (size_t i = 0; i < nodes.size(); i++) {
      u16 my_id = nodes[i].my_id;
      bool not_occluded =
          my_id != 0xffff && level_occlusion_string[my_id / 8] & (1 << (7 - (my_id & 7)));
      out[i] = not_occluded && sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  } else {
    for (size_t i = 0; i < nodes.size(); i++) {
      out[i] = sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  }
}

void make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                 GLsizei* counts_out,
                                 void** index_offsets_out,
                                 const std::vector<tfrag3::ShrubDraw>& draws) {
  u64 md_idx = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.first_index_index;
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 1;
    counts_out[md_idx] = draw.num_indices;
    index_offsets_out[md_idx] = (void*)(iidx * sizeof(u32));
    md_idx++;
    draw_ptrs_out[i] = ds;
  }
}

u32 make_all_visible_multidraws(std::pair<int, int>* draw_ptrs_out,
                                GLsizei* counts_out,
                                void** index_offsets_out,
                                const std::vector<tfrag3::StripDraw>& draws) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 1;
    int num_inds = 0;
    for (auto& grp : draw.vis_groups) {
      num_tris += grp.num_tris;
      num_inds += grp.num_inds;
    }
    counts_out[md_idx] = num_inds;
    index_offsets_out[md_idx] = (void*)(iidx * sizeof(u32));
    draw_ptrs_out[i] = ds;
    md_idx++;
  }
  return num_tris;
}

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::ShrubDraw>& draws,
                                const u32* idx_in) {
  int idx_buffer_ptr = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    memcpy(&idx_out[idx_buffer_ptr], idx_in + draw.first_index_index,
           draw.num_indices * sizeof(u32));
    idx_buffer_ptr += draw.num_indices;
    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  return idx_buffer_ptr;
}

u32 make_multidraws_from_vis_string(std::pair<int, int>* draw_ptrs_out,
                                    GLsizei* counts_out,
                                    void** index_offsets_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  u32 sanity_check = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    ASSERT(sanity_check == iidx);
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 0;
    bool building_run = false;
    u64 run_start = 0;
    for (auto& grp : draw.vis_groups) {
      sanity_check += grp.num_inds;
      bool vis = grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (!vis) {
          building_run = false;
          counts_out[md_idx] = iidx - run_start;
          index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
          ds.second++;
          md_idx++;
        }
      } else {
        if (vis) {
          building_run = true;
          run_start = iidx;
        }
      }

      iidx += grp.num_inds;
    }

    if (building_run) {
      building_run = false;
      counts_out[md_idx] = iidx - run_start;
      index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
      ds.second++;
      md_idx++;
    }

    draw_ptrs_out[i] = ds;
  }
  return num_tris;
}

u32 make_multidraws_from_vis_and_proto_string(std::pair<int, int>* draw_ptrs_out,
                                              GLsizei* counts_out,
                                              void** index_offsets_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data) {
  u64 md_idx = 0;
  u32 num_tris = 0;
  u32 sanity_check = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u64 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    ASSERT(sanity_check == iidx);
    std::pair<int, int> ds;
    ds.first = md_idx;
    ds.second = 0;
    bool building_run = false;
    u64 run_start = 0;
    for (auto& grp : draw.vis_groups) {
      sanity_check += grp.num_inds;
      bool vis = (grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh]) &&
                 proto_vis_data[grp.tie_proto_idx];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (!vis) {
          building_run = false;
          counts_out[md_idx] = iidx - run_start;
          index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
          ds.second++;
          md_idx++;
        }
      } else {
        if (vis) {
          building_run = true;
          run_start = iidx;
        }
      }

      iidx += grp.num_inds;
    }

    if (building_run) {
      building_run = false;
      counts_out[md_idx] = iidx - run_start;
      index_offsets_out[md_idx] = (void*)(run_start * sizeof(u32));
      ds.second++;
      md_idx++;
    }

    draw_ptrs_out[i] = ds;
  }
  return num_tris;
}

u32 make_index_list_from_vis_string(std::pair<int, int>* group_out,
                                    u32* idx_out,
                                    const std::vector<tfrag3::StripDraw>& draws,
                                    const std::vector<u8>& vis_data,
                                    const u32* idx_in,
                                    u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    int vtx_idx = 0;
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    bool building_run = false;
    int run_start_out = 0;
    int run_start_in = 0;
    for (auto& grp : draw.vis_groups) {
      bool vis = grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (vis) {
          idx_buffer_ptr += grp.num_inds;
        } else {
          building_run = false;
          memcpy(&idx_out[run_start_out],
                 idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
                 (idx_buffer_ptr - run_start_out) * sizeof(u32));
        }
      } else {
        if (vis) {
          building_run = true;
          run_start_out = idx_buffer_ptr;
          run_start_in = vtx_idx;
          idx_buffer_ptr += grp.num_inds;
        }
      }
      vtx_idx += grp.num_inds;
    }

    if (building_run) {
      memcpy(&idx_out[run_start_out],
             idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
             (idx_buffer_ptr - run_start_out) * sizeof(u32));
    }

    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

u32 make_index_list_from_vis_and_proto_string(std::pair<int, int>* group_out,
                                              u32* idx_out,
                                              const std::vector<tfrag3::StripDraw>& draws,
                                              const std::vector<u8>& vis_data,
                                              const std::vector<u8>& proto_vis_data,
                                              const u32* idx_in,
                                              u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    int vtx_idx = 0;
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    bool building_run = false;
    int run_start_out = 0;
    int run_start_in = 0;
    for (auto& grp : draw.vis_groups) {
      bool vis = (grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh]) &&
                 proto_vis_data[grp.tie_proto_idx];
      if (vis) {
        num_tris += grp.num_tris;
      }

      if (building_run) {
        if (vis) {
          idx_buffer_ptr += grp.num_inds;
        } else {
          building_run = false;
          memcpy(&idx_out[run_start_out],
                 idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
                 (idx_buffer_ptr - run_start_out) * sizeof(u32));
        }
      } else {
        if (vis) {
          building_run = true;
          run_start_out = idx_buffer_ptr;
          run_start_in = vtx_idx;
          idx_buffer_ptr += grp.num_inds;
        }
      }
      vtx_idx += grp.num_inds;
    }

    if (building_run) {
      memcpy(&idx_out[run_start_out],
             idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer + run_start_in,
             (idx_buffer_ptr - run_start_out) * sizeof(u32));
    }

    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

u32 make_all_visible_index_list(std::pair<int, int>* group_out,
                                u32* idx_out,
                                const std::vector<tfrag3::StripDraw>& draws,
                                const u32* idx_in,
                                u32* num_tris_out) {
  int idx_buffer_ptr = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    std::pair<int, int> ds;
    ds.first = idx_buffer_ptr;
    u32 num_inds = 0;
    for (auto& grp : draw.vis_groups) {
      num_inds += grp.num_inds;
      num_tris += grp.num_tris;
    }
    memcpy(&idx_out[idx_buffer_ptr], idx_in + draw.unpacked.idx_of_first_idx_in_full_buffer,
           num_inds * sizeof(u32));
    idx_buffer_ptr += num_inds;
    ds.second = idx_buffer_ptr - ds.first;
    group_out[i] = ds;
  }
  *num_tris_out = num_tris;
  return idx_buffer_ptr;
}

// PC-REMAKE: inverse of the exact matrix the background vertex shaders use, so
// the post pass can turn (screen uv, depth) back into a world position. Built
// from the same `newcam` that first_tfrag_draw_setup uploads as `pc_camera`,
// including the two quirks of that shader: the whole thing is negated, the w of
// the translation column is dropped, and y is scaled by SCISSOR_ADJUST.
float g_world_from_clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
float g_clip_from_world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
bool g_world_from_clip_ok = false;

void update_render_state_from_pc_settings(SharedRenderState* state, const TfragPcPortData& data) {
  if (!state->has_pc_data) {
    for (int i = 0; i < 4; i++) {
      state->camera_planes[i] = data.camera.planes[i];
      state->camera_matrix[i] = data.camera.camera[i];
    }
    state->camera_pos = data.camera.trans;
    state->camera_hvdf_off = data.camera.hvdf_off;
    state->camera_fog = data.camera.fog;
    state->has_pc_data = true;

    auto newcam = make_new_cam_mat(data.camera.rot, data.camera.perspective, data.camera.fog.x(),
                                   data.camera.hvdf_off.z());
    // gl_Position = G * vec4(world - cam_trans, 1)
    float G[16];
    for (int c = 0; c < 3; c++) {
      for (int r = 0; r < 4; r++) {
        G[c * 4 + r] = -newcam[c][r];
      }
    }
    G[12] = -newcam[3][0];
    G[13] = -newcam[3][1];
    G[14] = -newcam[3][2];
    G[15] = 0.f;  // the shader forces transformed.w = 0 before the column adds
    const float y_scale = 512.f / 448.f;  // SCISSOR_ADJUST * HEIGHT_SCALE (Jak 1)
    for (int c = 0; c < 4; c++) {
      G[c * 4 + 1] *= y_scale;
    }
    for (int i = 0; i < 16; i++) {
      g_clip_from_world[i] = G[i];
    }
    g_world_from_clip_ok = mat_inverse_impl(G, g_world_from_clip);
  }
}
