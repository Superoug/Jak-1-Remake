# Jak 1 Remake — mod OpenGOAL pour Jak and Daxter

🇬🇧 [English version](README.md)

Un remake visuel de **Jak and Daxter: The Precursor Legacy** sous forme de **mod pour le
lanceur officiel [OpenGOAL](https://opengoal.dev)** : éclairage moderne (soleil, ombres,
lampes, lueurs), herbe dense, eau retravaillée, textures HD fidèles.


> **Il faut posséder le jeu original.** Ce dépôt ne contient AUCUN fichier du jeu : ni
> niveaux, ni modèles, ni textures, ni sons. Tout est reconstruit sur l'ordinateur du joueur, à
> partir de SA copie du jeu (ISO PS2), par le lanceur OpenGOAL.

Tous les droits sur OpenGOAL sont réservés à leurs propriétaires respectifs.

## Installer (joueurs)

1. Installer le [lanceur OpenGOAL](https://opengoal.dev) (v2.4.10 ou plus récent) et faire
   l'installation normale de **Jak 1** avec votre ISO.
2. Dans le lanceur : **Réglages → Mods → Ajouter une source**, et coller l'adresse de la source
   de mods de ce projet.

   https://raw.githubusercontent.com/Superoug/Jak-1-Remake/main/mod-source/mods.json

   
4. Ouvrir la page des mods de Jak 1, choisir **Jak 1 Remake**, puis **Installer**. Le lanceur
   télécharge le mod, le reconstruit depuis votre jeu (quelques minutes) et l'ajoute à la liste.

Sans source de mods : télécharger l'archive de votre système sur la page des versions du
dépôt (`windows-….zip`, `linux-….tar.gz` ou `macos-intel-….tar.gz`), puis dans le lanceur :
page de **Jak 1 → Features → Mods → Ajouter depuis un fichier**, et choisir l'archive.

Plateformes : Windows, Linux (dont Steam Deck en mode bureau) et macOS (Intel, et Apple
Silicon via Rosetta).

Réglages en jeu : **Options → Graphismes → Remake** (nuit, lampes, soleil, ombres, rayons,
bloom, rendu remake…).

## Captures
Celles-ci sont prises directement en jeu. Aucune retouche. 

<img width="1920" height="1080" alt="01 - Village de Sandover (jour)" src="https://github.com/user-attachments/assets/0515f8f0-9946-44dc-814d-ccf4193a70f0" />

<img width="1604" height="906" alt="download" src="https://github.com/user-attachments/assets/2db9bec2-ad14-4e03-8eee-d7a9201e65ce" />

<img width="1800" height="1169" alt="Capture d’écran 2026-10-27 à 20 37 30" src="https://github.com/user-attachments/assets/c6f56095-4399-49cb-8623-462cbacd32dc" />

<img width="1920" height="1080" alt="09 - Cratere volcanique (jour)" src="https://github.com/user-attachments/assets/ce17ce8b-095c-4115-a3f8-bf737bebccd4" />

<img width="1920" height="1080" alt="13 - Plage au bord de l&#39;eau (jour)" src="https://github.com/user-attachments/assets/d5873977-e818-446c-8d34-b1ab04676493" />

<img width="1800" height="1169" alt="Capture d’écran 2026-10-30 à 10 32 29" src="https://github.com/user-attachments/assets/6edc7e7a-6f3a-4498-afb9-f4e9e199f292" />

<img width="1920" height="1080" alt="14 - Jak devant la cabane de Samos" src="https://github.com/user-attachments/assets/73b4fec6-60f8-4bd9-b910-d21ab0755aea" />


## Ce qui change par rapport au jeu d'origine

- Éclairage reconstruit : ombres dynamiques, lumières, reflets.
- Herbe dense, buissons qui ondulent, réagissent aux pas et au vent.
- Eau : reflets, rayon de soleil.
- Textures HD **fidèles**.
- Performances : ce projet vise les 60 images/s sur tous supports.

## Téléchargement

Ce mod est fini à environ 40%. Il reste énormément de travail et de bugs à corriger.

https://github.com/Superoug/Jak-1-Remake/releases

## Développeurs

Ce dépôt est basé sur [OG-Mod-Base](https://github.com/OpenGOAL-Mods/OG-Mod-Base) (lui-même
dérivé de [jak-project](https://github.com/open-goal/jak-project), licence ISC). Les
modifications du remake sont signalées dans le code par `PC-REMAKE`. Les versions sont
produites par l'action GitHub **Cut Mod Release** (binaires Windows, Linux, macOS Intel).

Le README d'origine d'OG-Mod-Base est dans [`docs/MOD-BASE-README.md`](docs/MOD-BASE-README.md).

Ce projet est rendu possible grâce à Claude. Merci de votre compréhension.
