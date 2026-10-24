#!/usr/bin/env python3
"""
details_sol.py : couches de DETAIL des sols (vues de pres), generees de zero (aucune image du jeu).

Les sols d'origine font 128 x 128 pixels etires sur des metres : de pres, meme agrandis, ils
sont flous. Chaque matiere recoit une couche de detail reel a l'echelle (1 metre = 1024 pixels,
~1 mm par pixel), en tuile, que le moteur pose par-dessus la texture d'origine SANS changer sa
couleur moyenne : seulement le grain et le relief fin.
  0 sable  : grains de 1 a 2 mm, quelques grains sombres, petites rides du vent (~8 cm)
  1 terre  : terre fine, gravillons et petits cailloux de 2 a 10 mm (chemins, terre battue)
  2 herbe  : brins fins couches et entremeles (sol herbeux, litiere)
  3 roche  : grain de la pierre, piqures et fines fissures (sols rocheux, grottes)
Sortie : <sortie>/sol_detail_<n>.png (RGBA : R = variation de clarte, 0.5 = rien ;
G, B = pente de la surface ; A = hauteur).

  details_sol.py --sortie <dossier> [--apercu fichier.png]
"""

import argparse
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

N = 1024
MM = 1.0  # taille d'un pixel en mm


def bruit(rng, kmin, kmax, pente=1.0):
    """Bruit tileable a bande limitee (cycles par tuile kmin..kmax), moyenne 0, ecart 1."""
    f = np.fft.fftfreq(N) * N
    kx, ky = np.meshgrid(f, f)
    k = np.sqrt(kx * kx + ky * ky)
    filt = ((k >= kmin) & (k <= kmax)).astype(np.float32) / np.maximum(k, 1.0) ** pente
    w = rng.standard_normal((N, N)).astype(np.float32)
    out = np.real(np.fft.ifft2(np.fft.fft2(w) * filt)).astype(np.float32)
    return out / (out.std() + 1e-8)


def tamponner(rng, densite, rayon_mm, forme="dome"):
    """Grains ou cailloux poses au hasard (en tuile) : hauteur et masque d'occupation."""
    h = np.zeros((N, N), np.float32)
    n = int(densite * N * N)
    xs = rng.uniform(0, N, n)
    ys = rng.uniform(0, N, n)
    rs = rng.uniform(rayon_mm[0], rayon_mm[1], n) / MM
    amps = rng.uniform(0.6, 1.0, n)
    # regroupe par rayon (noyaux precalcules), pose par addition sur une toile elargie
    m = int(np.ceil(rayon_mm[1] / MM)) + 3
    toile = np.zeros((N + 2 * m, N + 2 * m), np.float32)
    for x, y, r, a in zip(xs, ys, rs, amps):
        ri = int(np.ceil(r)) + 1
        yy, xx = np.mgrid[-ri:ri + 1, -ri:ri + 1].astype(np.float32)
        fx, fy = x - np.floor(x), y - np.floor(y)
        d = np.sqrt((xx - fx) ** 2 + ((yy - fy) * rng.uniform(0.75, 1.0)) ** 2) / r
        if forme == "dome":
            k = np.clip(1.0 - d * d, 0.0, 1.0) ** 0.6
        else:  # caillou : sommet plat, flancs raides
            k = np.clip((1.0 - d) * 3.0, 0.0, 1.0) ** 0.8
        k *= a * r
        cx, cy = int(np.floor(x)) + m, int(np.floor(y)) + m
        sl = toile[cy - ri:cy + ri + 1, cx - ri:cx + ri + 1]
        np.maximum(sl, k, out=sl)
    # repli des bords (tuile)
    toile[m:2 * m, :] = np.maximum(toile[m:2 * m, :], toile[N + m:N + 2 * m, :])
    toile[N:N + m, :] = np.maximum(toile[N:N + m, :], toile[0:m, :])
    toile[:, m:2 * m] = np.maximum(toile[:, m:2 * m], toile[:, N + m:N + 2 * m])
    toile[:, N:N + m] = np.maximum(toile[:, N:N + m], toile[:, 0:m])
    h = toile[m:N + m, m:N + m]
    return h


def sable(rng):
    # rides du vent : ~8 cm, asymetriques, sinueuses
    y = np.mgrid[0:N, 0:N][0].astype(np.float32)
    deform = bruit(rng, 1, 4) * 18.0
    ph = (y + deform) / (N / 12.0)
    s = ph - np.floor(ph)
    ride = np.where(s < 0.7, s / 0.7, (1.0 - s) / 0.3)
    ride = ride * (0.55 + 0.45 * (bruit(rng, 1, 3) * 0.5 + 0.5).clip(0, 1))
    h = ride * 2.5
    grains = tamponner(rng, 0.11, (0.5, 1.1), "dome")
    h += grains * 0.9 + bruit(rng, 120, 400) * 0.12
    alb = 1.0 + bruit(rng, 60, 400) * 0.035 + (ride - 0.5) * 0.05
    # quelques grains sombres et clairs (mineraux, coquillages broyes)
    sombres = tamponner(rng, 0.004, (0.5, 0.9), "dome")
    clairs = tamponner(rng, 0.003, (0.5, 0.9), "dome")
    alb += -0.35 * (sombres > 0.2) + 0.25 * (clairs > 0.2)
    return h, alb


def cailloux(rng, n, rmin, rmax):
    """Petits cailloux bombes, ovales et irreguliers, tournes au hasard, a moitie enfouis."""
    m = int(np.ceil(rmax)) + 4
    toile = np.zeros((N + 2 * m, N + 2 * m), np.float32)
    alb = np.zeros_like(toile)
    for _ in range(n):
        x, y = rng.uniform(0, N, 2)
        r = rng.uniform(rmin, rmax)
        asp = rng.uniform(0.55, 0.95)
        rot = rng.uniform(0, np.pi)
        ri = int(np.ceil(r)) + 2
        yy, xx = np.mgrid[-ri:ri + 1, -ri:ri + 1].astype(np.float32)
        xx -= x - np.floor(x)
        yy -= y - np.floor(y)
        u = xx * np.cos(rot) + yy * np.sin(rot)
        v = (-xx * np.sin(rot) + yy * np.cos(rot)) / asp
        ang = np.arctan2(v, u)
        bord = 1.0 + 0.12 * np.sin(3 * ang + rng.uniform(0, 6.28)) + 0.07 * np.sin(5 * ang + rng.uniform(0, 6.28))
        d = np.sqrt(u * u + v * v) / (r * bord)
        k = np.sqrt(np.clip(1.0 - d * d, 0.0, 1.0)) * r * asp * 0.8 - r * 0.15
        k = np.clip(k, 0, None)
        cx, cy = int(np.floor(x)) + m, int(np.floor(y)) + m
        sl = toile[cy - ri:cy + ri + 1, cx - ri:cx + ri + 1]
        sa = alb[cy - ri:cy + ri + 1, cx - ri:cx + ri + 1]
        prend = k > sl
        sl[prend] = k[prend]
        sa[prend] = rng.normal(0.0, 0.14)
    for t in (toile, alb):
        t[m:2 * m, :] += t[N + m:N + 2 * m, :] * (t[m:2 * m, :] == 0)
        t[N:N + m, :] += t[0:m, :] * (t[N:N + m, :] == 0)
        t[:, m:2 * m] += t[:, N + m:N + 2 * m] * (t[:, m:2 * m] == 0)
        t[:, N:N + m] += t[:, 0:m] * (t[:, N:N + m] == 0)
    return toile[m:N + m, m:N + m], alb[m:N + m, m:N + m]


def terre(rng):
    h = bruit(rng, 6, 60, 0.6) * 0.9 + bruit(rng, 60, 300) * 0.25
    alb = 1.0 + bruit(rng, 4, 40, 0.5) * 0.06
    c1, a1 = cailloux(rng, 420, 2.5, 8.0)
    c2, a2 = cailloux(rng, 2600, 0.8, 2.4)
    c = np.maximum(c1, c2)
    ac = np.where(c1 >= c2, a1, a2)
    occ = (c > 0.05).astype(np.float32)
    alb = alb * (1 - occ) + occ * (1.0 + ac)
    pied = cv2.GaussianBlur(c, (0, 0), 2.5)
    alb -= np.clip(pied - c, 0, None) * 0.06
    h = h + c * 0.9
    return h, alb


def herbe(rng):
    h = np.zeros((N, N), np.float32)
    alb = np.zeros((N, N), np.float32)
    ang0 = rng.uniform(0, np.pi)
    n = 11000
    for i in range(n):
        x, y = rng.uniform(0, N, 2)
        a = ang0 + rng.normal(0, 1.1)
        L = rng.uniform(16, 42)
        courbe = rng.normal(0, 0.035)
        w0 = rng.uniform(1.6, 3.0)
        haut = 0.4 + 0.6 * i / n  # les derniers poses sont au-dessus
        teinte = rng.normal(0, 1)
        pts = []
        for j in range(7):
            t = j / 6.0
            aa = a + courbe * L * t
            pts.append((x + np.cos(aa) * L * t, y + np.sin(aa) * L * t))
        for ox in (-N, 0, N):
            for oy in (-N, 0, N):
                for j in range(6):
                    w = max(1, int(round(w0 * (1 - j / 6.0) + 0.6)))
                    p0 = (int((pts[j][0] + ox) * 4), int((pts[j][1] + oy) * 4))
                    p1 = (int((pts[j + 1][0] + ox) * 4), int((pts[j + 1][1] + oy) * 4))
                    if -200 < p0[0] < (N + 50) * 4 and -200 < p0[1] < (N + 50) * 4:
                        cv2.line(h, p0, p1, float(haut), w, cv2.LINE_AA, shift=2)
                        cv2.line(alb, p0, p1, float(teinte), w, cv2.LINE_AA, shift=2)
    h = cv2.GaussianBlur(h, (0, 0), 0.6)
    creux = np.clip(1.0 - h / 0.6, 0, 1)
    albedo = 1.0 + np.clip(alb, -2, 2) * 0.07 - creux * 0.22 + bruit(rng, 4, 30) * 0.05
    return h * 2.2, albedo


def roche(rng):
    h = bruit(rng, 8, 80, 0.7) * 0.8 + bruit(rng, 80, 400) * 0.22
    # fines fissures : lignes de crete d'un bruit etire, interrompues par un masque
    y, x = np.mgrid[0:N, 0:N].astype(np.float32)
    r = np.abs(bruit(rng, 3, 14, 0.8) + 0.3 * bruit(rng, 14, 60))
    fiss = np.clip(1.0 - r / 0.05, 0, 1) ** 2
    masque = np.clip(bruit(rng, 2, 8) * 1.2 - 0.3, 0, 1)
    fiss *= masque
    h -= fiss * 0.9
    pits = tamponner(rng, 0.0015, (0.6, 1.6), "dome")
    h -= pits * 0.8
    alb = 1.0 + bruit(rng, 30, 300) * 0.045 - fiss * 0.15 - (pits > 0.3) * 0.08
    return h, alb


def encoder(h, alb, pente_mm):
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 / MM
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 / MM
    gx, gy = gx * pente_mm, gy * pente_mm
    n = np.dstack([-gx, -gy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    r = np.clip(0.5 + (alb - 1.0), 0, 1)
    hh = (h - h.min()) / (h.max() - h.min() + 1e-6)
    out = np.dstack([r, n[..., 0] * 0.5 + 0.5, n[..., 1] * 0.5 + 0.5, hh])
    return np.clip(np.round(out * 255), 0, 255).astype(np.uint8)


def apercu(rgba, base):
    """Rendu eclaire (soleil rasant) d'un morceau de 25 cm, pour juger a l'oeil."""
    r = rgba[:256, :256].astype(np.float32) / 255.0
    nx, ny = r[..., 1] * 2 - 1, r[..., 2] * 2 - 1
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    L = np.array([0.6, -0.35, 0.72])
    L /= np.linalg.norm(L)
    d = np.clip(nx * L[0] + ny * L[1] + nz * L[2], 0, 1)
    col = np.array(base, np.float32)[None, None, :] * (0.5 + r[..., :1] * 1.0) * (0.35 + 0.8 * d[..., None])
    return np.clip(col * 255, 0, 255).astype(np.uint8)


MATIERES = [("sable", sable, 1.0, (0.85, 0.74, 0.52)), ("terre", terre, 0.9, (0.55, 0.43, 0.30)),
            ("herbe", herbe, 0.8, (0.35, 0.55, 0.18)), ("roche", roche, 0.9, (0.52, 0.50, 0.48))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sortie", required=True)
    ap.add_argument("--apercu", default="")
    a = ap.parse_args()
    out = Path(a.sortie)
    out.mkdir(parents=True, exist_ok=True)
    vues = []
    for i, (nom, f, pente, base) in enumerate(MATIERES):
        rng = np.random.default_rng(1000 + i)
        h, alb = f(rng)
        rgba = encoder(h, alb, pente)
        Image.fromarray(rgba, "RGBA").save(out / f"sol_detail_{i}.png")
        print(nom, "ok", flush=True)
        vues.append(apercu(rgba, base))
    if a.apercu:
        Image.fromarray(np.hstack(vues)).resize((1024 * 2, 512), Image.NEAREST).save(a.apercu)


if __name__ == "__main__":
    main()
