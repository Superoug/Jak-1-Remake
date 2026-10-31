#include "shore_map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "third-party/json.hpp"

// CARTES DU RIVAGE

namespace decompiler {
namespace {

constexpr float kPas = 0.5f;
constexpr float kBorne = 32.f;
constexpr float kMer = 0.f;
constexpr float kChenal = 6.f;  // demi-largeur minimale (m) d'un passage vers le large
constexpr float kInterieur = 40.f;  // eau interieure (rivieres) : hors de +/- kBorne
const char* const kNiveaux[] = {"training", "village1", "beach",    "jungle",
                                "misty",    "village2", "swamp",    "sunken"};
const char* const kCote[] = {"rock", "cliff", "wall", "stone", "boulder",
                             "island", "shore", "bank", "beach", "sand"};
// SURPLOMBS. Ce qui passe au-dessus de l'eau sans la toucher
constexpr float kSurplomb = 1.0f;
constexpr float kSurplombMax = 30.f;  // m d'eau gagnee au plus sous un surplomb
const char* const kNiveauxSurplomb[] = {"village2", "training"};
// SURPLOMBS EN MER
const char* const kNiveauxSurplombMer[] = {"village1"};
constexpr float kFond = 0.3f;

struct Tri {
  float p[3][3];
  // decor qui n'est pas de la cote (poteaux, murs, objets)
  bool autre = false;
};

std::vector<Tri> lire_niveau(const fs::path& dir, const std::string& lv) {
  std::vector<Tri> out;
  auto jp = dir / (lv + ".json");
  auto bp = dir / (lv + ".bin");
  if (!fs::exists(jp) || !fs::exists(bp)) {
    return out;
  }
  nlohmann::json j;
  try {
    j = parse_commented_json(file_util::read_text_file(jp), jp.string());
  } catch (...) {
    return out;
  }
  auto bin = file_util::read_binary_file(bp);
  auto floats = [&](size_t off, size_t n) -> const float* {
    if (off + n * sizeof(float) > bin.size()) {
      return nullptr;
    }
    return reinterpret_cast<const float*>(bin.data() + off);
  };
  auto ajouter = [&](const float* p, size_t ntri, const float* m, bool autre = false) {
    for (size_t t = 0; t < ntri; t++) {
      Tri tr;
      tr.autre = autre;
      for (int k = 0; k < 3; k++) {
        const float* v = p + (t * 3 + k) * 3;
        if (m) {
          // 3 colonnes + translation (metres)
          for (int r = 0; r < 3; r++) {
            tr.p[k][r] = m[r] * v[0] + m[3 + r] * v[1] + m[6 + r] * v[2] + m[9 + r];
          }
        } else {
          tr.p[k][0] = v[0];
          tr.p[k][1] = v[1];
          tr.p[k][2] = v[2];
        }
      }
      out.push_back(tr);
    }
  };
  for (const auto& q : j.value("terrain", nlohmann::json::array())) {
    size_t n = q.value("tris", 0);
    const float* p = floats(q.value("pos", (size_t)0), n * 9);
    if (n && p) {
      ajouter(p, n, nullptr);
    }
  }
  const auto& protos = j.value("protos", nlohmann::json::array());
  const auto& ins = j.value("instances", nlohmann::json::object());
  size_t count = ins.value("count", (size_t)0);
  const float* mats = floats(ins.value("mat", (size_t)0), count * 12);
  std::vector<int> pidx;
  if (ins.contains("proto")) {
    pidx = ins["proto"].get<std::vector<int>>();
  }
  if (!mats || pidx.size() != count) {
    return out;
  }
  for (size_t i = 0; i < protos.size(); i++) {
    const auto& p = protos[i];
    if (p.value("kind", "") != "tie") {
      continue;
    }
    std::string nom = p.value("name", "");
    std::transform(nom.begin(), nom.end(), nom.begin(), ::tolower);
    bool cote = false;
    for (auto* k : kCote) {
      cote |= nom.find(k) != std::string::npos;
    }
    bool surplomb = false;
    for (auto* k : kNiveauxSurplomb) {
      surplomb |= lv == k;
    }
    if (!cote && !surplomb) {
      continue;
    }
    for (size_t k = 0; k < count; k++) {
      if (pidx[k] != (int)i) {
        continue;
      }
      for (const auto& q : p.value("parts", nlohmann::json::array())) {
        size_t n = q.value("tris", 0);
        const float* pp = floats(q.value("pos", (size_t)0), n * 9);
        if (n && pp) {
          ajouter(pp, n, mats + k * 12, !cote);
        }
      }
    }
  }
  return out;
}

float percentile(std::vector<float> v, float q) {
  if (v.empty()) {
    return 0.f;
  }
  size_t k = std::min(v.size() - 1, (size_t)(q * (v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

// remplissage d'un polygone convexe (au plus 4 sommets) sur la grille
template <typename F>
void remplir_avec(int w, int h, const float (*poly)[2], int n, F&& cellule) {
  float ymin = 1e30f, ymax = -1e30f;
  for (int i = 0; i < n; i++) {
    ymin = std::min(ymin, poly[i][1]);
    ymax = std::max(ymax, poly[i][1]);
  }
  int y0 = std::max(0, (int)std::ceil(ymin - 0.5f));
  int y1 = std::min(h - 1, (int)std::floor(ymax - 0.5f));
  for (int y = y0; y <= y1; y++) {
    float yc = y + 0.5f;
    float xa = 1e30f, xb = -1e30f;
    for (int i = 0; i < n; i++) {
      const float* a = poly[i];
      const float* b = poly[(i + 1) % n];
      if ((a[1] <= yc && b[1] > yc) || (b[1] <= yc && a[1] > yc)) {
        float x = a[0] + (yc - a[1]) / (b[1] - a[1]) * (b[0] - a[0]);
        xa = std::min(xa, x);
        xb = std::max(xb, x);
      }
    }
    if (xa > xb) {
      continue;
    }
    int x0 = std::max(0, (int)std::ceil(xa - 0.5f));
    int x1 = std::min(w - 1, (int)std::floor(xb - 0.5f));
    for (int x = x0; x <= x1; x++) {
      cellule(x, y);
    }
  }
}

// partie d'un triangle au-dessus (dessus = true) ou au-dessous du plan y = niveau
int couper(const Tri& t, float niveau, bool dessus, float x0, float z0, float (*poly)[2]) {
  int n = 0;
  for (int a = 0; a < 3 && n < 4; a++) {
    const float* pa = t.p[a];
    const float* pb = t.p[(a + 1) % 3];
    bool ia = dessus ? pa[1] > niveau : pa[1] < niveau;
    bool ib = dessus ? pb[1] > niveau : pb[1] < niveau;
    if (ia) {
      poly[n][0] = (pa[0] - x0) / kPas;
      poly[n][1] = (pa[2] - z0) / kPas;
      n++;
    }
    if (ia != ib && n < 4) {
      float s = (niveau - pa[1]) / (pb[1] - pa[1]);
      poly[n][0] = (pa[0] + (pb[0] - pa[0]) * s - x0) / kPas;
      poly[n][1] = (pa[2] + (pb[2] - pa[2]) * s - z0) / kPas;
      n++;
    }
  }
  return n;
}

// trait d'un segment (coordonnees de grille) sans trou
void tracer(std::vector<uint8_t>& g, int w, int h, float ax, float ay, float bx, float by) {
  int n = 1 + (int)(std::max(std::abs(bx - ax), std::abs(by - ay)) * 4.f);
  int px = -1, py = -1;
  for (int s = 0; s <= n; s++) {
    float f = (float)s / n;
    int x = (int)std::floor(ax + (bx - ax) * f), y = (int)std::floor(ay + (by - ay) * f);
    if (x < 0 || y < 0 || x >= w || y >= h) {
      px = -1;
      continue;
    }
    g[(size_t)y * w + x] = 1;
    if (px >= 0 && px != x && py != y) {
      g[(size_t)py * w + x] = 1;
    }
    px = x;
    py = y;
  }
}

// hauteur du triangle au centre de la cellule (x, y) de la grille
float hauteur_a(const Tri& t, float x0, float z0, int x, int y) {
  float ax = (t.p[0][0] - x0) / kPas, az = (t.p[0][2] - z0) / kPas;
  float bx = (t.p[1][0] - x0) / kPas, bz = (t.p[1][2] - z0) / kPas;
  float cx = (t.p[2][0] - x0) / kPas, cz = (t.p[2][2] - z0) / kPas;
  float ymin = std::min({t.p[0][1], t.p[1][1], t.p[2][1]});
  float ymax = std::max({t.p[0][1], t.p[1][1], t.p[2][1]});
  float det = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
  if (std::abs(det) < 0.05f) {
    return ymin;
  }
  float px = x + 0.5f, pz = y + 0.5f;
  float u = ((px - ax) * (cz - az) - (cx - ax) * (pz - az)) / det;
  float v = ((bx - ax) * (pz - az) - (px - ax) * (bz - az)) / det;
  float hy = t.p[0][1] + u * (t.p[1][1] - t.p[0][1]) + v * (t.p[2][1] - t.p[0][1]);
  return std::clamp(hy, ymin, ymax);
}

// transformee de distance euclidienne exacte (Felzenszwalb), au carre, en cellules
void edt_1d(const float* f, float* d, int n, std::vector<int>& v, std::vector<float>& z) {
  int k = 0;
  v[0] = 0;
  z[0] = -1e30f;
  z[1] = 1e30f;
  for (int q = 1; q < n; q++) {
    float s;
    while (true) {
      s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.f * q - 2.f * v[k]);
      if (s <= z[k] && k > 0) {
        k--;
      } else {
        break;
      }
    }
    if (s <= z[k]) {
      v[0] = q;
      z[0] = -1e30f;
      z[1] = 1e30f;
      k = 0;
      continue;
    }
    k++;
    v[k] = q;
    z[k] = s;
    z[k + 1] = 1e30f;
  }
  k = 0;
  for (int q = 0; q < n; q++) {
    while (z[k + 1] < q) {
      k++;
    }
    d[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
  }
}

// distance (cellules) de chaque cellule a la plus proche cellule `cible`
std::vector<float> distance(const std::vector<uint8_t>& cible, int w, int h) {
  const float inf = 1e20f;
  std::vector<float> g((size_t)w * h);
  for (size_t i = 0; i < g.size(); i++) {
    g[i] = cible[i] ? 0.f : inf;
  }
  int n = std::max(w, h);
  std::vector<float> f(n), d(n), z(n + 1);
  std::vector<int> v(n);
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) {
      f[y] = g[(size_t)y * w + x];
    }
    edt_1d(f.data(), d.data(), h, v, z);
    for (int y = 0; y < h; y++) {
      g[(size_t)y * w + x] = d[y];
    }
  }
  for (int y = 0; y < h; y++) {
    edt_1d(&g[(size_t)y * w], d.data(), w, v, z);
    for (int x = 0; x < w; x++) {
      g[(size_t)y * w + x] = std::sqrt(d[x]);
    }
  }
  return g;
}

uint16_t vers_half(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
  uint32_t m = x & 0x7fffff;
  if (e <= 0) {
    return (uint16_t)sign;
  }
  if (e >= 31) {
    return (uint16_t)(sign | 0x7bff);
  }
  return (uint16_t)(sign | (e << 10) | (m >> 13));
}

void carte(const std::string& lv,
           const std::vector<Tri>& siens,
           const std::vector<Tri>& tous,
           size_t siens_debut,
           const fs::path& out_dir) {
  // zone cotiere jouable
  std::vector<float> xs, zs;
  for (const auto& t : siens) {
    if (t.autre) {
      continue;  // la zone reste celle de la cote (meme grille qu'avant)
    }
    float lo = std::min({t.p[0][1], t.p[1][1], t.p[2][1]});
    float hi = std::max({t.p[0][1], t.p[1][1], t.p[2][1]});
    if (lo < kMer + 3.f && hi > kMer - 3.f) {
      xs.push_back((t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3.f);
      zs.push_back((t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3.f);
    }
  }
  if (xs.size() < 100) {
    return;
  }
  float x0 = percentile(xs, 0.01f), x1 = percentile(xs, 0.99f);
  float z0 = percentile(zs, 0.01f), z1 = percentile(zs, 0.99f);
  float mx = percentile(xs, 0.5f), mz = percentile(zs, 0.5f);
  x0 = std::max(x0, mx - 350.f) - 64.f;
  x1 = std::min(x1, mx + 350.f) + 64.f;
  z0 = std::max(z0, mz - 350.f) - 64.f;
  z1 = std::min(z1, mz + 350.f) + 64.f;
  int w = (int)std::ceil((x1 - x0) / kPas), h = (int)std::ceil((z1 - z0) / kPas);
  if (w <= 2 || h <= 2) {
    return;
  }
  // terre : partie de chaque triangle au-dessus de la mer, en projection
  std::vector<uint8_t> sol((size_t)w * h, 0);
  std::vector<float> bas((size_t)w * h, 1e30f);
  std::vector<uint8_t> ligne((size_t)w * h, 0);  // ou le decor coupe la surface de la mer
  std::vector<uint8_t> fond((size_t)w * h, 0);  // du decor sous la mer (kFond)
  bool surplomb_mer = false;
  for (auto* k : kNiveauxSurplombMer) {
    surplomb_mer |= lv == k;
  }
  for (size_t ti = 0; ti < tous.size(); ti++) {
    const auto& t = tous[ti];
    // surplombs : seulement le decor du niveau lui-meme
    bool sien = ti >= siens_debut && ti < siens_debut + siens.size();
    float ymin = std::min({t.p[0][1], t.p[1][1], t.p[2][1]});
    float ymax = std::max({t.p[0][1], t.p[1][1], t.p[2][1]});
    float bx0 = std::min({t.p[0][0], t.p[1][0], t.p[2][0]});
    float bx1 = std::max({t.p[0][0], t.p[1][0], t.p[2][0]});
    float bz0 = std::min({t.p[0][2], t.p[1][2], t.p[2][2]});
    float bz1 = std::max({t.p[0][2], t.p[1][2], t.p[2][2]});
    if (bx1 < x0 || bx0 > x1 || bz1 < z0 || bz0 > z1) {
      continue;
    }
    float poly[4][2];
    if (surplomb_mer && !t.autre && ymin < kMer - kFond) {
      int nf = couper(t, kMer - kFond, false, x0, z0, poly);
      if (nf >= 3) {
        remplir_avec(w, h, poly, nf, [&](int x, int y) { fond[(size_t)y * w + x] = 1; });
      }
    }
    if (ymax <= kMer) {
      continue;
    }
    if (ymin < kMer) {
      float seg[2][2];
      int m = 0;
      for (int a = 0; a < 3 && m < 2; a++) {
        const float* pa = t.p[a];
        const float* pb = t.p[(a + 1) % 3];
        if ((pa[1] > kMer) != (pb[1] > kMer)) {
          float s = (kMer - pa[1]) / (pb[1] - pa[1]);
          seg[m][0] = (pa[0] + (pb[0] - pa[0]) * s - x0) / kPas;
          seg[m][1] = (pa[2] + (pb[2] - pa[2]) * s - z0) / kPas;
          m++;
        }
      }
      if (m == 2) {
        tracer(ligne, w, h, seg[0][0], seg[0][1], seg[1][0], seg[1][1]);
      }
    }
    if (t.autre) {
      continue;  // decor hors cote : le trait seulement
    }
    int n = couper(t, kMer, true, x0, z0, poly);
    if (n >= 3) {
      remplir_avec(w, h, poly, n, [&](int x, int y) {
        size_t i = (size_t)y * w + x;
        sol[i] = 1;
        bas[i] = sien ? std::min(bas[i], std::max(hauteur_a(t, x0, z0, x, y), kMer)) : kMer;
      });
    }
  }
  {
    bool appliquer = false;
    for (auto* k : kNiveauxSurplomb) {
      appliquer |= lv == k;
    }
    // le trait ou le decor coupe la mer reste de la terre (sans trou)
    std::vector<uint8_t> sol0 = sol;
    if (appliquer) {
      for (size_t i = 0; i < sol.size(); i++) {
        sol0[i] |= ligne[i];
      }
    }
    std::vector<uint8_t> eau0((size_t)w * h, 0);
    std::vector<int> file;
    auto semer = [&](int x, int y) {
      size_t i = (size_t)y * w + x;
      if (!sol0[i] && !eau0[i]) {
        eau0[i] = 1;
        file.push_back((int)i);
      }
    };
    for (int x = 0; x < w; x++) {
      semer(x, 0);
      semer(x, h - 1);
    }
    for (int y = 0; y < h; y++) {
      semer(0, y);
      semer(w - 1, y);
    }
    for (size_t k = 0; k < file.size(); k++) {
      int i = file[k], x = i % w, y = i / w;
      if (x > 0) semer(x - 1, y);
      if (x < w - 1) semer(x + 1, y);
      if (y > 0) semer(x, y - 1);
      if (y < h - 1) semer(x, y + 1);
    }
    // largeur d'eau gagnee sous les surplombs, couche par couche depuis la mer d'origine
    std::vector<uint8_t> sous((size_t)w * h, 0);
    std::vector<int> couche = file, suivante;
    size_t n_surplomb = 0;
    for (int d = 0; d < (int)(kSurplombMax / kPas) && !couche.empty(); d++) {
      suivante.clear();
      for (int i : couche) {
        int x = i % w, y = i / w;
        int vois[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& v : vois) {
          if (v[0] < 0 || v[1] < 0 || v[0] >= w || v[1] >= h) {
            continue;
          }
          size_t j = (size_t)v[1] * w + v[0];
          if (sol0[j] && !ligne[j] && !sous[j] && bas[j] > kMer + kSurplomb) {
            sous[j] = 1;
            n_surplomb++;
            suivante.push_back((int)j);
          }
        }
      }
      std::swap(couche, suivante);
    }
    if (appliquer) {
      for (size_t i = 0; i < sol.size(); i++) {
        sol[i] = sol0[i] && !sous[i];
      }
    }
    lg::info("PC-REMAKE rivage : {} : {} cellules sous un surplomb ({})", lv, n_surplomb,
             appliquer ? "eau" : "laissees en terre");
  }
  // mer = eau reliee au bord de la grille
  std::vector<uint8_t> mer((size_t)w * h, 0);
  std::vector<int> pile;
  auto pousser = [&](int x, int y) {
    size_t i = (size_t)y * w + x;
    if (!sol[i] && !mer[i]) {
      mer[i] = 1;
      pile.push_back((int)i);
    }
  };
  for (int x = 0; x < w; x++) {
    pousser(x, 0);
    pousser(x, h - 1);
  }
  for (int y = 0; y < h; y++) {
    pousser(0, y);
    pousser(w - 1, y);
  }
  while (!pile.empty()) {
    int i = pile.back();
    pile.pop_back();
    int x = i % w, y = i / w;
    if (x > 0) pousser(x - 1, y);
    if (x < w - 1) pousser(x + 1, y);
    if (y > 0) pousser(x, y - 1);
    if (y < h - 1) pousser(x, y + 1);
  }
  // les rivieres et les chenaux etroits ne sont pas la mer (pas de vagues ni d'ecume)
  std::vector<uint8_t> mer0 = mer;
  {
    auto d_sol = distance(sol, w, h);
    std::vector<uint8_t> large((size_t)w * h, 0);
    for (size_t i = 0; i < large.size(); i++) {
      large[i] = mer[i] && d_sol[i] * kPas > kChenal;
    }
    std::vector<uint8_t> coeur((size_t)w * h, 0);
    auto pousser2 = [&](int x, int y) {
      size_t i = (size_t)y * w + x;
      if (large[i] && !coeur[i]) {
        coeur[i] = 1;
        pile.push_back((int)i);
      }
    };
    for (int x = 0; x < w; x++) {
      pousser2(x, 0);
      pousser2(x, h - 1);
    }
    for (int y = 0; y < h; y++) {
      pousser2(0, y);
      pousser2(w - 1, y);
    }
    while (!pile.empty()) {
      int i = pile.back();
      pile.pop_back();
      int x = i % w, y = i / w;
      if (x > 0) pousser2(x - 1, y);
      if (x < w - 1) pousser2(x + 1, y);
      if (y > 0) pousser2(x, y - 1);
      if (y < h - 1) pousser2(x, y + 1);
    }
    auto d_coeur = distance(coeur, w, h);
    for (size_t i = 0; i < mer.size(); i++) {
      mer[i] = mer[i] && d_coeur[i] * kPas <= kChenal + 0.5f * kPas;
    }
  }
  // CRIQUES. Une petite crique reliee a la mer par une entree etroite passait pour une riviere
  if (lv != "jungle" && lv != "swamp") {
    constexpr float kCrique = 30.f;  // m
    constexpr float kCriqueAire = 2500.f;  // m2
    auto d_vers_mer = distance(mer, w, h);
    std::vector<int> comp((size_t)w * h, -1);
    std::vector<int> pile2, cellules;
    size_t n_criques = 0;
    for (size_t i0 = 0; i0 < mer.size(); i0++) {
      if (!mer0[i0] || mer[i0] || comp[i0] >= 0) {
        continue;
      }
      cellules.clear();
      pile2.assign(1, (int)i0);
      comp[i0] = (int)i0;
      float dmax = 0.f;
      while (!pile2.empty()) {
        int i = pile2.back();
        pile2.pop_back();
        cellules.push_back(i);
        dmax = std::max(dmax, d_vers_mer[i] * kPas);
        int x = i % w, y = i / w;
        int vois[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& v : vois) {
          if (v[0] < 0 || v[1] < 0 || v[0] >= w || v[1] >= h) {
            continue;
          }
          size_t j = (size_t)v[1] * w + v[0];
          if (mer0[j] && !mer[j] && comp[j] < 0) {
            comp[j] = (int)i0;
            pile2.push_back((int)j);
          }
        }
      }
      if (dmax <= kCrique && cellules.size() * kPas * kPas <= kCriqueAire) {
        for (int i : cellules) {
          mer[i] = 1;
        }
        n_criques++;
      }
    }
    lg::info("PC-REMAKE rivage : {} : {} criques rendues a la mer", lv, n_criques);
  }
  // surplombs en mer (voir kNiveauxSurplombMer), depuis la mer seule
  if (surplomb_mer) {
    std::vector<int> couche, suivante;
    for (size_t i = 0; i < mer.size(); i++) {
      if (mer[i]) {
        couche.push_back((int)i);
      }
    }
    size_t n_mer = 0;
    for (int d = 0; d < (int)(kSurplombMax / kPas) && !couche.empty(); d++) {
      suivante.clear();
      for (int i : couche) {
        int x = i % w, y = i / w;
        int vois[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& v : vois) {
          if (v[0] < 0 || v[1] < 0 || v[0] >= w || v[1] >= h) {
            continue;
          }
          size_t j = (size_t)v[1] * w + v[0];
          if (sol[j] && !mer[j] && !ligne[j] && fond[j] && bas[j] > kMer + kSurplomb) {
            mer[j] = 1;
            mer0[j] = 1;
            n_mer++;
            suivante.push_back((int)j);
          }
        }
      }
      std::swap(couche, suivante);
    }
    lg::info("PC-REMAKE rivage : {} : {} cellules de mer sous un surplomb", lv, n_mer);
  }
  // distance a la terre des cellules de mer
  std::vector<uint8_t> terre(mer.size());
  for (size_t i = 0; i < mer.size(); i++) {
    terre[i] = !mer0[i];
  }
  auto d_ter = distance(terre, w, h);  // distance a la terre (cellules de mer)
  auto d_mer = distance(mer, w, h);  // distance a la mer (cellules de terre)
  std::vector<uint16_t> out((size_t)w * h);
  std::vector<float> sdv((size_t)w * h);
  for (size_t i = 0; i < out.size(); i++) {
    float sd = mer[i] ? (d_ter[i] - 0.5f) * kPas : -(d_mer[i] - 0.5f) * kPas;
    sd = std::clamp(sd, -kBorne, kBorne);
    sdv[i] = sd;
  }
  // TRAIT DE COTE LISSE
  {
    const int R = 6;
    const float sig = 2.f;  // cases
    float k[2 * R + 1];
    float ks = 0.f;
    for (int i = -R; i <= R; i++) {
      k[i + R] = std::exp(-0.5f * i * i / (sig * sig));
      ks += k[i + R];
    }
    std::vector<float> tmp(sdv.size());
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float acc = 0.f;
        for (int i = -R; i <= R; i++) {
          acc += k[i + R] * sdv[(size_t)y * w + std::clamp(x + i, 0, w - 1)];
        }
        tmp[(size_t)y * w + x] = acc / ks;
      }
    }
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float acc = 0.f;
        for (int i = -R; i <= R; i++) {
          acc += k[i + R] * tmp[(size_t)std::clamp(y + i, 0, h - 1) * w + x];
        }
        sdv[(size_t)y * w + x] = acc / ks;
      }
    }
  }
  for (size_t i = 0; i < out.size(); i++) {
    float sd = sdv[i];
    if (mer0[i] && !mer[i]) {
      sd = kInterieur;
    }
    out[i] = vers_half(sd);
  }
  file_util::create_dir_if_needed(out_dir);
  std::vector<uint8_t> data(28 + out.size() * 2);
  memcpy(data.data(), "RIV1", 4);
  float hdr_f[3] = {x0, z0, kPas};
  memcpy(data.data() + 4, hdr_f, 12);
  uint32_t hdr_u[2] = {(uint32_t)w, (uint32_t)h};
  memcpy(data.data() + 16, hdr_u, 8);
  float mer_y = kMer;
  memcpy(data.data() + 24, &mer_y, 4);
  memcpy(data.data() + 28, out.data(), out.size() * 2);
  file_util::write_binary_file(out_dir / (lv + ".riv"), data.data(), data.size());
  lg::info("PC-REMAKE rivage : carte de {} ({}x{})", lv, w, h);
}

}  // namespace

void build_shore_maps(const fs::path& editor_dir, const fs::path& out_dir) {
  std::vector<std::vector<Tri>> par_niveau;
  std::vector<Tri> tous;
  std::vector<size_t> debut;
  for (auto* lv : kNiveaux) {
    debut.push_back(tous.size());
    par_niveau.push_back(lire_niveau(editor_dir, lv));
    tous.insert(tous.end(), par_niveau.back().begin(), par_niveau.back().end());
  }
  for (size_t i = 0; i < par_niveau.size(); i++) {
    if (!par_niveau[i].empty()) {
      carte(kNiveaux[i], par_niveau[i], tous, debut[i], out_dir);
    }
  }
}

}  // namespace decompiler
