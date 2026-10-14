# Jak Remake — construire et jouer sur Windows et Steam Deck

Le remake est un **fork d'OpenGOAL** : tout ce qui a été ajouté (rendu C++/GLSL 4.1, code GOAL,
textures HD) est multiplateforme par construction. OpenGOAL lui-même tourne officiellement sur
Windows, Linux (dont Steam Deck) et macOS, **x86-64 uniquement**. Le Mac ne peut pas produire
d'exécutable Windows ou Linux : il faut compiler sur la plateforme cible, ou laisser GitHub
Actions le faire (recommandé, voir §3).

Ce que contient une installation, quelle que soit la plateforme :

| Élément | D'où il vient | Distribué ? |
|---|---|---|
| `gk` / `gk.exe` (le jeu), `goalc`, `extractor` | compilation du dépôt (CMake) | oui (binaires) |
| `custom_assets/jak1/texture_replacements/` (≈ 460 Mo, 3 800 PNG) | ce dépôt (dossier ignoré par git : à zipper à part) | oui (« pack de textures ») |
| `decompiler_out/`, `out/` (niveaux `.fr3`, DGO, textes) | générés localement par `extractor` depuis **l'ISO du joueur** | non (contenu du jeu) |
| `iso_data/` | l'ISO PS2 (USA ou PAL) du joueur | non |

## 1. Windows 10/11 (x86-64)

Prérequis : [Visual Studio 2022](https://visualstudio.microsoft.com/) avec « Développement Desktop
en C++ » + **Clang** (composant *C++ Clang tools for Windows*), CMake ≥ 3.25, Ninja, Git,
Python 3, [NASM](https://www.nasm.us/). Tous sont dans l'installeur VS sauf NASM.

```powershell
git clone <adresse du dépôt du remake> jak-project
cd jak-project
cmake --preset Release-windows-clang
cmake --build --preset Release-windows-clang -j 8
```

Les exécutables sortent dans `build\Release-windows-clang\bin\` (sinon `build\...\game\`).
Ensuite, avec l'ISO :

```powershell
mkdir iso_data\jak1 ; copy "Jak and Daxter.iso" iso_data\jak1\
.\extractor.exe --extract --decompile --compile --play iso_data\jak1\<nom>.iso
```

(`--extract` déballe l'ISO, `--decompile` produit `decompiler_out`, `--compile` construit les
DGO + niveaux `.fr3` — **c'est à cette étape que le pack `texture_replacements` est cuit
dans les niveaux** : le dossier doit être en place AVANT ; `--play` lance le jeu.)

Lancement normal : `gk.exe --game jak1 -- -boot -fakeiso` (sans `-debug`).

## 2. Steam Deck (SteamOS / Linux x86-64)

Deux voies :

**A. Sans compiler sur le Deck** : récupérer les binaires Linux produits par GitHub Actions
(§3) ou par n'importe quel PC Linux x86-64, les copier dans un dossier
(`~/JakRemake/`), y mettre `custom_assets/jak1/texture_replacements/`, l'ISO dans
`iso_data/jak1/`, puis en mode Bureau :

```bash
cd ~/JakRemake
./extractor --extract --decompile --compile iso_data/jak1/*.iso
./gk --game jak1 -- -boot -fakeiso
```

Ajouter `gk` comme « jeu non-Steam » avec les arguments `--game jak1 -- -boot -fakeiso`
pour le lancer depuis le mode Gaming (manette reconnue via SDL, comme sur Mac).

**B. Compiler sur le Deck** : le système est en lecture seule ; utiliser
[distrobox](https://distrobox.it/) avec une image Arch ou Ubuntu :

```bash
distrobox create -n jak -i archlinux:latest && distrobox enter jak
sudo pacman -S base-devel cmake ninja clang lld nasm python git
cmake --preset Release-linux-clang && cmake --build --preset Release-linux-clang -j 8
```

Les binaires sont dans `build/Release-linux-clang/bin/`. OpenGL 4.1 est fourni par Mesa/RADV
(le Deck dépasse largement : GL 4.6).

## 3. Voie recommandée : GitHub Actions construit tout

Le dépôt contient déjà les workflows d'OpenGOAL (`.github/workflows/build-matrix.yaml` :
Windows clang + msvc, Linux clang + gcc, macOS). Il suffit de :

1. créer un dépôt GitHub (fork de `open-goal/jak-project` ou dépôt neuf), y pousser la
   branche du remake ;
2. GitHub compile automatiquement à chaque push ; les artefacts (zip avec `gk.exe`,
   `goalc.exe`, `extractor.exe` pour Windows, idem Linux) se téléchargent depuis l'onglet
   *Actions* du dépôt ;
3. distribuer à côté le pack `texture_replacements` (zip, ≈ 460 Mo).

Le joueur fournit son ISO et lance `extractor` une fois (§1/§2) — exactement le flux
d'OpenGOAL, dont le lanceur officiel n'est pas nécessaire.

## 4. Points de vigilance vérifiés (22/09)

- Aucun appel POSIX-seulement dans le code ajouté (l'`#include <unistd.h>` de
  `background_common.cpp` a été retiré) ; les fichiers-drapeaux de débogage (`/tmp/...`)
  sont simplement ignorés quand ils n'existent pas.
- GLSL : tout est en `#version 410 core` sans extension ; les pilotes Mesa (Deck) et
  NVIDIA/AMD Windows sont plus stricts qu'Apple sur les samplers — le seul cas connu
  (sampler d'ombres sur l'unité 0) a été corrigé le 21/09.
- Les réglages (`pc-settings.gc`) et sauvegardes vivent dans le dossier utilisateur
  d'OpenGOAL (`%APPDATA%\OpenGOAL\jak1` sous Windows, `~/.config/OpenGOAL/jak1` sous Linux).
- Non testé faute de machine : la compilation Windows/Linux de CE fork. Le premier build
  CI dira s'il reste un détail (avertissement traité en erreur, etc.).
