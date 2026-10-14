# Jak 1 Remake — mod OpenGOAL pour Jak and Daxter

Un remake visuel de **Jak and Daxter: The Precursor Legacy** sous forme de **mod pour le
lanceur officiel [OpenGOAL](https://opengoal.dev)** : éclairage moderne (soleil, ombres,
lampes, lueurs), herbe dense, eau et cascades retravaillées, textures HD fidèles, étalonnage
moderne… sans remodeler quoi que ce soit : le travail des artistes d'origine est conservé.

> **Il faut posséder le jeu original.** Ce dépôt ne contient AUCUN fichier du jeu : ni
> niveaux, ni modèles, ni textures, ni sons. Tout est reconstruit sur l'ordinateur du joueur, à
> partir de SA copie du jeu (ISO PS2), par le lanceur OpenGOAL.

Tous les droits sur OpenGOAL sont réservés à leurs propriétaires respectifs

## Installer (joueurs)

1. Installer le [lanceur OpenGOAL](https://opengoal.dev) (v2.4.10 ou plus récent) et faire
   l'installation normale de **Jak 1** avec votre ISO.
2. Dans le lanceur : **Réglages → Mods → Ajouter une source**, et coller l'adresse de la source
   de mods de ce projet (voir la page des versions du dépôt).
3. Ouvrir la page des mods de Jak 1, choisir **Jak 1 Remake**, puis **Installer**. Le lanceur
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

<img width="1920" height="1080" alt="04 - Plage des Sentinelles (nuit)" src="https://github.com/user-attachments/assets/e5722e9f-d5ea-4eef-b41f-a95e7c0ebc3d" />

<img width="1920" height="1080" alt="09 - Cratere volcanique (jour)" src="https://github.com/user-attachments/assets/ce17ce8b-095c-4115-a3f8-bf737bebccd4" />

<img width="1920" height="1080" alt="13 - Plage au bord de l&#39;eau (jour)" src="https://github.com/user-attachments/assets/d5873977-e818-446c-8d34-b1ab04676493" />

<img width="1920" height="1080" alt="14 - Jak devant la cabane de Samos" src="https://github.com/user-attachments/assets/73b4fec6-60f8-4bd9-b910-d21ab0755aea" />


## Ce qui change par rapport au jeu d'origine

- Éclairage reconstruit : Ombres dynamiques, lumières, reflet...
- Herbe dense, buissons qui ondulent, réagissent aux pas et au vent.
- Eau : reflets, brume sous-marine, cascades lisibles.
- Textures HD **fidèles**
- Performances : ce projet vise les 60 images/s sur tous support.

## Téléchargement

Ce mod est fini à environ 40%. Il reste énormément de travail et de bugs à corriger.
Une première bêta du mod va bientôt sortir.

## Développeurs

Ce dépôt est basé sur [OG-Mod-Base](https://github.com/OpenGOAL-Mods/OG-Mod-Base) (lui-même
dérivé de [jak-project](https://github.com/open-goal/jak-project), licence ISC). Les
modifications du remake sont signalées dans le code par `PC-REMAKE`. Les versions sont
produites par l'action GitHub **Cut Mod Release** (binaires Windows, Linux, macOS Intel).

Le README d'origine d'OG-Mod-Base est dans [`docs/MOD-BASE-README.md`](docs/MOD-BASE-README.md).

Ce projet est rendu possible grâce à Claude. Merci de votre compréhension.
