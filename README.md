# Jak 1 Remake — OpenGOAL mod for Jak and Daxter

🇫🇷 [Version française](README_FR.md)

A visual remake of **Jak and Daxter: The Precursor Legacy** as a **mod for the official
[OpenGOAL](https://opengoal.dev) launcher**: modern lighting (sun, shadows, lamps, glows),
dense grass, reworked water, faithful HD textures.


> **You must own the original game.** This repository contains NO game files: no levels,
> no models, no textures, no sounds. Everything is rebuilt on the player's computer, from
> THEIR copy of the game (PS2 ISO), by the OpenGOAL launcher.

All rights to OpenGOAL are reserved to their respective owners.

## Installation (players)

1. Install the [OpenGOAL launcher](https://opengoal.dev) (v2.4.10 or newer) and do the
   regular **Jak 1** installation with your ISO.
2. In the launcher: **Settings → Mods → Add**, and paste this project's mod source address

https://github.com/Superoug/Jak-1-Remake/blob/main/mod-source/mods.json

3. Open the Jak 1 mods page, select **Jak 1 Remake**, then **Install**. The launcher
   downloads the mod, rebuilds it from your game (a few minutes) and adds it to the list.

Without a mod source: download the archive for your system from the repository's releases
page (`windows-….zip`, `linux-….tar.gz` or `macos-intel-….tar.gz`), then in the launcher:
**Jak 1 → Features → Mods → Add from File**, and pick the archive.

Platforms: Windows, Linux (including Steam Deck in desktop mode) and macOS (Intel, and Apple
Silicon through Rosetta).

In-game settings: **Options → Graphics → Remake** (night intensity, lamps, sun, shadows, sun
rays, bloom, remake look…).

## Screenshots
These are taken directly in-game. No retouching.

<img width="1920" height="1080" alt="01 - Sandover Village (day)" src="https://github.com/user-attachments/assets/0515f8f0-9946-44dc-814d-ccf4193a70f0" />

<img width="1920" height="1080" alt="04 - Sentinel Beach (night)" src="https://github.com/user-attachments/assets/e5722e9f-d5ea-4eef-b41f-a95e7c0ebc3d" />

<img width="1920" height="1080" alt="09 - Volcanic Crater (day)" src="https://github.com/user-attachments/assets/ce17ce8b-095c-4115-a3f8-bf737bebccd4" />

<img width="960" height="624" alt="Capture d’écran 2026-10-21 à 13 36 21" src="https://github.com/user-attachments/assets/84256de8-73e3-4730-9111-54988a44e965" />

<img width="1800" height="1169" alt="Capture d’écran 2026-10-21 à 13 30 23" src="https://github.com/user-attachments/assets/e35f5959-d541-44f6-9add-fb76dc5ec72a" />

<img width="640" height="416" alt="Capture d’écran 2026-10-21 à 13 40 43" src="https://github.com/user-attachments/assets/09e6d9b9-b67d-46b2-96ae-b3e98595054a" />



## What changes compared to the original game

- Rebuilt lighting: dynamic shadows, lights, reflections.
- Dense grass, swaying bushes that react to footsteps and wind.
- Water: reflections, sun rays.
- **Faithful** HD textures.
- Performance: this project aims for 60 frames per second on every platform.

## Download

This mod is about 40% complete. There is still a lot of work to do and many bugs to fix.

https://github.com/Superoug/Jak-1-Remake/releases

## Developers

This repository is based on [OG-Mod-Base](https://github.com/OpenGOAL-Mods/OG-Mod-Base)
(itself derived from [jak-project](https://github.com/open-goal/jak-project), ISC license).
The remake's changes are marked in the code with `PC-REMAKE`. Releases are produced by the
**Cut Mod Release** GitHub Action (Windows, Linux and macOS Intel binaries).

The original OG-Mod-Base README is in [`docs/MOD-BASE-README.md`](docs/MOD-BASE-README.md).

This project is made possible thanks to Claude. Thank you for your understanding.
