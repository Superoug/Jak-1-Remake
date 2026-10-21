#include "faithful_upscale.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace decompiler {

namespace {

constexpr double kPi = 3.14159265358979323846;

double sinc(double x) {
  if (x == 0.0) {
    return 1.0;
  }
  x *= kPi;
  return std::sin(x) / x;
}

double lanczos3(double x) {
  if (x > -3.0 && x < 3.0) {
    return sinc(x) * sinc(x / 3.0);
  }
  return 0.0;
}

int wrap(int i, int n) {
  i %= n;
  return i < 0 ? i + n : i;
}

u8 clip8(double v) {
  if (v <= 0.0) {
    return 0;
  }
  if (v >= 255.0) {
    return 255;
  }
  return (u8)std::lround(v);
}

struct Plane {
  int w = 0, h = 0;
  std::vector<double> v;
  double& at(int x, int y) { return v[(size_t)y * w + x]; }
  double at(int x, int y) const { return v[(size_t)y * w + x]; }
};

// Coefficients Lanczos-3 a la maniere de Pillow (precompute_coeffs) pour passer de
// in_size a out_size. `wrap_edges` : la texture est vue TUILEE (indices modulo), sinon
// les noyaux sont tronques au bord puis renormalises (Pillow sans tuilage).
struct Coeffs {
  std::vector<int> first;
  std::vector<int> count;
  std::vector<double> k;  // count[i] valeurs a partir de i * ksize
  int ksize = 0;
};

Coeffs make_coeffs(int in_size, int out_size, bool wrap_edges) {
  Coeffs c;
  const double scale = (double)in_size / out_size;
  const double filterscale = std::max(scale, 1.0);
  const double support = 3.0 * filterscale;
  c.ksize = (int)std::ceil(support) * 2 + 1;
  c.first.resize(out_size);
  c.count.resize(out_size);
  c.k.assign((size_t)out_size * c.ksize, 0.0);
  const double ss = 1.0 / filterscale;
  for (int xx = 0; xx < out_size; xx++) {
    const double center = (xx + 0.5) * scale;
    int xmin = (int)std::floor(center - support + 0.5);
    int xmax = (int)std::floor(center + support + 0.5);
    if (!wrap_edges) {
      xmin = std::max(xmin, 0);
      xmax = std::min(xmax, in_size);
    }
    const int n = std::min(xmax - xmin, c.ksize);
    double ww = 0.0;
    for (int x = 0; x < n; x++) {
      const double w = lanczos3((x + xmin - center + 0.5) * ss);
      c.k[(size_t)xx * c.ksize + x] = w;
      ww += w;
    }
    if (ww != 0.0) {
      for (int x = 0; x < n; x++) {
        c.k[(size_t)xx * c.ksize + x] /= ww;
      }
    }
    c.first[xx] = xmin;
    c.count[xx] = n;
  }
  return c;
}

// redimensionnement separable (horizontal puis vertical, arrondi 8 bits entre les
// passes comme Pillow)
Plane resize(const Plane& in, int out_w, int out_h, bool wrap_edges) {
  const Coeffs cx = make_coeffs(in.w, out_w, wrap_edges);
  const Coeffs cy = make_coeffs(in.h, out_h, wrap_edges);
  Plane tmp;
  tmp.w = out_w;
  tmp.h = in.h;
  tmp.v.resize((size_t)out_w * in.h);
  for (int y = 0; y < in.h; y++) {
    for (int x = 0; x < out_w; x++) {
      double acc = 0.0;
      for (int k = 0; k < cx.count[x]; k++) {
        const int sx = wrap_edges ? wrap(cx.first[x] + k, in.w) : cx.first[x] + k;
        acc += in.at(sx, y) * cx.k[(size_t)x * cx.ksize + k];
      }
      tmp.at(x, y) = clip8(acc);
    }
  }
  Plane out;
  out.w = out_w;
  out.h = out_h;
  out.v.resize((size_t)out_w * out_h);
  for (int y = 0; y < out_h; y++) {
    for (int x = 0; x < out_w; x++) {
      double acc = 0.0;
      for (int k = 0; k < cy.count[y]; k++) {
        const int sy = wrap_edges ? wrap(cy.first[y] + k, in.h) : cy.first[y] + k;
        acc += tmp.at(x, sy) * cy.k[(size_t)y * cy.ksize + k];
      }
      out.at(x, y) = clip8(acc);
    }
  }
  return out;
}

// masque flou (Pillow UnsharpMask rayon 1,2 / 48 % / seuil 2), flou gaussien en
// tuilage (l'image Python etait la texture tuilee 3x3)
void unsharp(Plane& p) {
  const double sigma = 1.2;
  const int r = 5;
  double kern[2 * r + 1];
  double ks = 0.0;
  for (int i = -r; i <= r; i++) {
    kern[i + r] = std::exp(-(i * i) / (2.0 * sigma * sigma));
    ks += kern[i + r];
  }
  for (auto& k : kern) {
    k /= ks;
  }
  Plane a = p, b = p;
  for (int y = 0; y < p.h; y++) {
    for (int x = 0; x < p.w; x++) {
      double acc = 0.0;
      for (int i = -r; i <= r; i++) {
        acc += p.at(wrap(x + i, p.w), y) * kern[i + r];
      }
      a.at(x, y) = acc;
    }
  }
  for (int y = 0; y < p.h; y++) {
    for (int x = 0; x < p.w; x++) {
      double acc = 0.0;
      for (int i = -r; i <= r; i++) {
        acc += a.at(x, wrap(y + i, p.h)) * kern[i + r];
      }
      b.at(x, y) = acc;
    }
  }
  for (size_t i = 0; i < p.v.size(); i++) {
    const double diff = p.v[i] - std::round(b.v[i]);
    if (std::abs(diff) >= 2.0) {
      p.v[i] = clip8(p.v[i] + diff * 0.48);
    }
  }
}

// borne chaque pixel au min/max du voisinage 3x3 (tuile) du texel d'origine
void clamp_to_source(Plane& big, const Plane& src, int factor) {
  for (int y = 0; y < big.h; y++) {
    for (int x = 0; x < big.w; x++) {
      const int sx = x / factor, sy = y / factor;
      double lo = 1e9, hi = -1e9;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          const double v = src.at(wrap(sx + dx, src.w), wrap(sy + dy, src.h));
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
      big.at(x, y) = std::clamp(big.at(x, y), lo, hi);
    }
  }
}

template <typename T>
std::vector<T> adv_mame2x(const std::vector<T>& a, int w, int h) {
  std::vector<T> out((size_t)w * h * 4);
  auto px = [&](int x, int y) { return a[(size_t)wrap(y, h) * w + wrap(x, w)]; };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const T B = px(x, y - 1), D = px(x - 1, y), E = px(x, y), F = px(x + 1, y), H = px(x, y + 1);
      const bool c1 = !(B == H) && !(D == F);
      const T e0 = (c1 && D == B) ? D : E;
      const T e1 = (c1 && B == F) ? F : E;
      const T e2 = (c1 && D == H) ? D : E;
      const T e3 = (c1 && H == F) ? F : E;
      const size_t ow = (size_t)w * 2;
      out[(size_t)(2 * y) * ow + 2 * x] = e0;
      out[(size_t)(2 * y) * ow + 2 * x + 1] = e1;
      out[(size_t)(2 * y + 1) * ow + 2 * x] = e2;
      out[(size_t)(2 * y + 1) * ow + 2 * x + 1] = e3;
    }
  }
  return out;
}

Plane channel(const u32* rgba, int w, int h, int c) {
  Plane p;
  p.w = w;
  p.h = h;
  p.v.resize((size_t)w * h);
  for (size_t i = 0; i < p.v.size(); i++) {
    p.v[i] = (rgba[i] >> (8 * c)) & 0xff;
  }
  return p;
}

}  // namespace

// PC-REMAKE 17/10 : IRIS DES YEUX. Les iris sont des dessins de 16 px (bam-iris-16x16)
// agrandis enormement a l'ecran. AdvMAME2x n'arrondit un contour que si les couleurs
// voisines sont EXACTEMENT egales : sur les degrades d'un iris il ne lissait rien et
// rendait des blocs 4x4 (yeux pixelises). Ici : la silhouette de l'iris (ce qui n'est
// pas le blanc de l'oeil) est agrandie puis lissee avant d'etre redecoupee net, et les
// couleurs d'origine, prolongees sous le contour, sont agrandies en Lanczos. Le dessin
// (couleurs, liseré sombre, reflet) est celui d'origine ; seuls les escaliers disparaissent.
bool faithful_iris_texture(const std::string& tex_name) {
  // (les portes en iris des precurseurs, irisdoor, ne sont pas des yeux)
  return tex_name.find("iris") != std::string::npos &&
         tex_name.find("door") == std::string::npos;
}

std::optional<FaithfulUpscale> faithful_iris_upscale(const u32* rgba, int w, int h) {
  if (w <= 0 || h <= 0 || std::max(w, h) > 32) {
    return faithful_upscale(rgba, w, h);  // pas un petit iris : agrandissement normal
  }
  const int factor = 128 / std::max(w, h);
  const int ow = w * factor;
  const int oh = h * factor;
  const size_t n = (size_t)w * h;
  auto lum_of = [](u32 p) {
    return 0.299 * (p & 0xff) + 0.587 * ((p >> 8) & 0xff) + 0.114 * ((p >> 16) & 0xff);
  };
  // le blanc de l'oeil = le texel le plus clair
  size_t white_i = 0;
  for (size_t i = 1; i < n; i++) {
    if (lum_of(rgba[i]) > lum_of(rgba[white_i])) {
      white_i = i;
    }
  }
  const double ref = lum_of(rgba[white_i]);
  // couverture de l'iris (0 sur le blanc, 1 dans l'iris, partielle sur les texels de bord)
  std::vector<double> cov(n);
  std::vector<bool> solid(n);
  Plane col[3];
  for (int c = 0; c < 3; c++) {
    col[c] = channel(rgba, w, h, c);
  }
  for (size_t i = 0; i < n; i++) {
    cov[i] = std::clamp((ref - 0.05 * 255.0 - lum_of(rgba[i])) / (0.30 * 255.0), 0.0, 1.0);
    solid[i] = cov[i] > 0.85;
  }
  // couleurs de l'iris prolongees dans le blanc (pas de melange blanc au contour)
  for (int it = 0; it < 8; it++) {
    std::vector<bool> next = solid;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const size_t i = (size_t)y * w + x;
        if (solid[i]) {
          continue;
        }
        double acc[3] = {0, 0, 0};
        int cnt = 0;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            const int sx = x + dx, sy = y + dy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h || !solid[(size_t)sy * w + sx]) {
              continue;
            }
            for (int c = 0; c < 3; c++) {
              acc[c] += col[c].at(sx, sy);
            }
            cnt++;
          }
        }
        if (cnt > 0) {
          for (int c = 0; c < 3; c++) {
            col[c].at(x, y) = acc[c] / cnt;
          }
          next[i] = true;
        }
      }
    }
    solid = next;
  }
  Plane big[3];
  for (int c = 0; c < 3; c++) {
    big[c] = resize(col[c], ow, oh, false);
  }
  // silhouette : agrandissement bilineaire puis flou gaussien (sigma 0,9 texel d'origine)
  std::vector<double> cu((size_t)ow * oh);
  for (int y = 0; y < oh; y++) {
    for (int x = 0; x < ow; x++) {
      const double fx = std::clamp((x + 0.5) / factor - 0.5, 0.0, w - 1.0);
      const double fy = std::clamp((y + 0.5) / factor - 0.5, 0.0, h - 1.0);
      const int x0 = (int)fx, y0 = (int)fy;
      const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
      const double ax = fx - x0, ay = fy - y0;
      const double top = cov[(size_t)y0 * w + x0] * (1 - ax) + cov[(size_t)y0 * w + x1] * ax;
      const double bot = cov[(size_t)y1 * w + x0] * (1 - ax) + cov[(size_t)y1 * w + x1] * ax;
      cu[(size_t)y * ow + x] = top * (1 - ay) + bot * ay;
    }
  }
  // flou gaussien separable (bords prolonges)
  auto gauss = [ow, oh](std::vector<double>& img, double sigma) {
    const int r = (int)(3 * sigma) + 1;
    std::vector<double> g(2 * r + 1);
    double sum = 0;
    for (int k = -r; k <= r; k++) {
      g[k + r] = std::exp(-(k * k) / (2 * sigma * sigma));
      sum += g[k + r];
    }
    for (auto& v : g) {
      v /= sum;
    }
    std::vector<double> tmp(img.size());
    for (int y = 0; y < oh; y++) {
      for (int x = 0; x < ow; x++) {
        double a = 0;
        for (int k = -r; k <= r; k++) {
          a += g[k + r] * img[(size_t)y * ow + std::clamp(x + k, 0, ow - 1)];
        }
        tmp[(size_t)y * ow + x] = a;
      }
    }
    for (int y = 0; y < oh; y++) {
      for (int x = 0; x < ow; x++) {
        double a = 0;
        for (int k = -r; k <= r; k++) {
          a += g[k + r] * tmp[(size_t)std::clamp(y + k, 0, oh - 1) * ow + x];
        }
        img[(size_t)y * ow + x] = a;
      }
    }
  };
  // silhouette arrondie (0,9 texel d'origine) ; couleurs adoucies (0,5 texel) : le liseré
  // d'origine alterne texels sombres et clairs, qui faisaient des dents sur le contour
  gauss(cu, 0.9 * factor);
  for (int c = 0; c < 3; c++) {
    gauss(big[c].v, 0.5 * factor);
  }
  FaithfulUpscale res;
  res.w = ow;
  res.h = oh;
  res.rgba.resize((size_t)ow * oh);
  const u32 white = rgba[white_i];
  for (int y = 0; y < oh; y++) {
    for (int x = 0; x < ow; x++) {
      const size_t i = (size_t)y * ow + x;
      double t = std::clamp((cu[i] - 0.5) / 0.08 + 0.5, 0.0, 1.0);
      t = t * t * (3 - 2 * t);
      u32 v = 0;
      for (int c = 0; c < 3; c++) {
        const double wc = (white >> (8 * c)) & 0xff;
        v |= (u32)clip8(big[c].at(x, y) * t + wc * (1 - t)) << (8 * c);
      }
      // alpha : celui du texel d'origine (constant sur un iris)
      const int sx = std::min(x / factor, w - 1), sy = std::min(y / factor, h - 1);
      v |= rgba[(size_t)sy * w + sx] & 0xff000000u;
      res.rgba[i] = v;
    }
  }
  return res;
}

bool faithful_upscale_frozen(const std::string& tex_name) {
  return tex_name.find("sky") != std::string::npos || tex_name.find("cloud") != std::string::npos ||
         tex_name.find("eye") != std::string::npos;
}

std::optional<FaithfulUpscale> faithful_upscale(const u32* rgba, int w, int h, int max_side) {
  if (w <= 0 || h <= 0) {
    return std::nullopt;
  }
  const int side = std::max(w, h);
  int factor = side <= 128 ? 4 : 2;
  while (factor > 1 && side * factor > max_side) {
    factor /= 2;
  }
  if (factor == 1) {
    return std::nullopt;
  }
  const size_t n = (size_t)w * h;
  FaithfulUpscale res;
  res.w = w * factor;
  res.h = h * factor;

  // 1. peu de couleurs ou toute petite texture : AdvMAME2x (couleurs existantes)
  std::set<u32> colors;
  std::set<u8> alphas;
  for (size_t i = 0; i < n; i++) {
    colors.insert(rgba[i] & 0xffffff);
    alphas.insert(rgba[i] >> 24);
  }
  if (colors.size() <= 32 || side <= 16) {
    std::vector<u32> out(rgba, rgba + n);
    int cw = w, ch = h;
    for (int f = 1; f < factor; f *= 2) {
      out = adv_mame2x(out, cw, ch);
      cw *= 2;
      ch *= 2;
    }
    res.rgba = std::move(out);
    return res;
  }

  // 2. Lanczos tuile + masque flou + bornes du voisinage, par canal
  Plane big[4];
  for (int c = 0; c < 3; c++) {
    const Plane src = channel(rgba, w, h, c);
    big[c] = resize(src, res.w, res.h, true);
    unsharp(big[c]);
    clamp_to_source(big[c], src, factor);
  }
  // alpha
  if (alphas.size() <= 2) {
    std::vector<u8> a(n);
    for (size_t i = 0; i < n; i++) {
      a[i] = rgba[i] >> 24;
    }
    int cw = w, ch = h;
    for (int f = 1; f < factor; f *= 2) {
      a = adv_mame2x(a, cw, ch);
      cw *= 2;
      ch *= 2;
    }
    big[3].w = res.w;
    big[3].h = res.h;
    big[3].v.assign(a.begin(), a.end());
  } else {
    const Plane src = channel(rgba, w, h, 3);
    big[3] = resize(src, res.w, res.h, true);
    clamp_to_source(big[3], src, factor);
  }
  res.rgba.resize((size_t)res.w * res.h);
  for (size_t i = 0; i < res.rgba.size(); i++) {
    u32 v = 0;
    for (int c = 0; c < 4; c++) {
      v |= (u32)clip8(big[c].v[i]) << (8 * c);
    }
    res.rgba[i] = v;
  }

  // 3. controle : re-reduite, la texture doit retomber sur l'original (texels visibles)
  double err = 0.0;
  size_t nvis = 0;
  {
    Plane back[3];
    for (int c = 0; c < 3; c++) {
      back[c] = resize(channel(res.rgba.data(), res.w, res.h, c), w, h, false);
    }
    for (size_t i = 0; i < n; i++) {
      if ((rgba[i] >> 24) <= 8) {
        continue;
      }
      double d = 0.0;
      for (int c = 0; c < 3; c++) {
        d += std::abs(back[c].v[i] - (double)((rgba[i] >> (8 * c)) & 0xff));
      }
      err += d / 3.0;
      nvis++;
    }
  }
  if (nvis > 0 && err / nvis > 6.0) {
    // repli : Lanczos simple, borne a la plage d'origine de chaque canal
    for (int c = 0; c < 4; c++) {
      const Plane src = channel(rgba, w, h, c);
      double lo = 255, hi = 0;
      for (double v : src.v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      big[c] = resize(src, res.w, res.h, false);
      for (auto& v : big[c].v) {
        v = std::clamp(v, lo, hi);
      }
    }
    for (size_t i = 0; i < res.rgba.size(); i++) {
      u32 v = 0;
      for (int c = 0; c < 4; c++) {
        v |= (u32)clip8(big[c].v[i]) << (8 * c);
      }
      res.rgba[i] = v;
    }
  }
  return res;
}

}  // namespace decompiler
