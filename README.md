# RetroChip

Instrument MIDI au son de six puces sonores de consoles et de cartouches, en Audio Unit et VST3, pour macOS sur Apple Silicon, et en VST3 pour Windows (voir [Autres logiciels et Windows](#autres-logiciels-et-windows)). En haut de la fenêtre, trois onglets choisissent la console : **NES**, **Genesis** et **SNES**. À côté de l'onglet, la NES propose sa puce, **2A03**, et deux puces de cartouches Famicom, **VRC6** et **N163** ; la Genesis ses deux puces, **FM** et **PSG** ; la SNES sa puce, **S-DSP**, ou **SoundFont**, qui joue des fichiers `.sf2` que tu fournis (voir [SoundFonts](#soundfonts)).

| Onglet et choix | Puce imitée | Son |
|---|---|---|
| **NES** > **2A03** | 2A03 | Pulse (4 rapports cycliques), triangle, bruit ou DPCM (échantillons), au choix par instance ; instruments FamiTracker `.fti` |
| **NES** > **VRC6** | VRC6 de Konami (cartouches Famicom) | Pulse (8 rapports cycliques) ou dent de scie ; instruments `.fti` |
| **NES** > **N163** | Namco 163 (cartouches Famicom) | Formes d'onde de 4 bits, 8 fournies ; instruments `.fti` avec leurs ondes |
| **Genesis** > **FM** | YM2612 | Synthèse FM à 4 opérateurs, 12 timbres, algorithme et rétroaction réglables, import de timbres `.tfi`, d'échantillons PCM 8 bits et de kits |
| **SNES** > **S-DSP** | S-DSP | Lecture d'échantillons compressés en BRR, avec enveloppe et écho |
| **Genesis** > **PSG** | SN76489, la deuxième puce de la Genesis | Ondes carrées ou bruit, 16 volumes |

L'infobulle de chaque bouton donne le nom de la puce imitée. Les noms des préréglages ne citent aucune console. **SoundFont** lit n'importe quel fichier `.sf2` ; il est rangé sous la SNES parce que les SoundFonts servent surtout ici à retrouver des sons de jeux SNES.

## Langue

L'interface est en anglais par défaut. Les boutons **EN** et **FR**, en bas à droite sous le clavier, la passent en français ou en anglais : le choix vaut pour toutes les instances et tous les projets, et il est gardé dans `~/Library/Application Support/SleeperMusic/RetroChip/Settings.xml` sur macOS, ou `%APPDATA%\SleeperMusic\RetroChip\Settings.xml` sous Windows. Les termes de synthé (Attack, Pulse, Gain, les noms des panneaux) restent les mêmes dans les deux langues. Ce README nomme les boutons par leur texte en français.

Les paramètres que Logic affiche pour l'automation portent des noms anglais dans les deux langues ; leurs valeurs propres à RetroChip (« Mon timbre TFI », « Infini ») suivent la langue choisie. Un message déjà affiché garde sa langue jusqu'au message suivant.

## Parcourir les sons au clavier

Clique dans la fenêtre du plugin, puis utilise les flèches pour passer d'un son à l'autre ; chaque son se joue tout de suite.

| Console | ↑ et ↓ | ← et → |
|---|---|---|
| **SNES** > **SoundFont** | instruments du fichier | fichiers du dossier |
| **Genesis** > **FM** | timbres (les tiens seulement s'ils ont un fichier importé) | fichiers `.tfi` du dossier |
| **SNES** > **S-DSP** | instruments | préréglages |
| **NES** (2A03, VRC6, N163) | préréglages de la puce | fichiers `.fti` du dossier |
| **Genesis** > **PSG** | préréglages | préréglages |

Arrivé au bout d'une liste, la flèche ne fait plus rien. Les autres touches, et les flèches avec Cmd, Option, Ctrl ou Maj, restent au logiciel hôte : la barre d'espace lance toujours la lecture dans Logic. Le clavier de l'écran ne prend jamais les touches du clavier de l'ordinateur.

## Modèle de jeu : un son par instance

Chaque instance du plugin joue **un seul son**. Avec les six puces, elle a 8 voix, qui servent aux accords, aux notes qui se chevauchent et aux relâchements. Si les 8 voix sont prises, la plus ancienne est remplacée (le choix **SoundFont** a ses propres voix, décrites plus bas). Sur la 2A03, l'instance joue un seul canal (pulse, triangle, bruit ou DPCM), comme la puce.

Pour un morceau complet, il faut donc une instance par son : une basse, une mélodie, un accompagnement, une percussion. Il y a 72 préréglages, soit 12 par puce, et le menu les groupe par puce.

## Utiliser dans Logic Pro

La compilation installe le plugin dans `~/Library/Audio/Plug-Ins`. Dans Logic Pro :

1. Crée une piste d'instrument logiciel.
2. Dans le logement Instrument, choisis **AU Instruments > SleeperMusic > RetroChip**.
3. Choisis la console avec le sélecteur, puis un préréglage.
4. Pour un deuxième son, crée une nouvelle piste d'instrument logiciel avec une nouvelle instance de RetroChip. Une piste égale un son : par exemple deux pistes **NES** (pulse), une piste **NES** (triangle) pour la basse, une piste **NES** (bruit) pour la batterie, une piste **Genesis** > **FM** pour une basse FM et une piste **SNES** > **S-DSP** pour les cordes.
5. Le sélecteur de console, le préréglage et les réglages sont enregistrés dans le projet, et leurs paramètres s'automatisent.

Si le plugin n'apparaît pas, ouvre **Logic Pro > Réglages > Gestionnaire de modules** et relance son analyse.

## Autres logiciels et Windows

Le plugin existe en deux formats : **Audio Unit** (Logic Pro, GarageBand, MainStage, sur macOS seulement) et **VST3** (Ableton Live, Reaper, Bitwig, Cubase, Studio One, FL Studio, sur macOS et sous Windows). Il n'existe pas en AAX (Pro Tools) ni en CLAP.

- **macOS** : la compilation installe le VST3 dans `~/Library/Audio/Plug-Ins/VST3`. Dans le logiciel, relance l'analyse des plugins si RetroChip n'apparaît pas.
- **Windows** : la compilation laisse le VST3 dans `build\RetroChip_artefacts\Release\VST3\RetroChip.vst3`, car le dossier des plugins demande les droits d'administrateur. Copie ce dossier `RetroChip.vst3` entier dans `C:\Program Files\Common Files\VST3`, puis relance l'analyse des plugins dans le logiciel. L'application autonome est dans `build\RetroChip_artefacts\Release\Standalone`.

Les dossiers de sons sont dans le dossier Musique de l'utilisateur sur les deux systèmes : `Musique\RetroChip\SoundFonts`, `Genesis` et `NES`.

Les flèches du clavier ne marchent que si le logiciel hôte laisse le clavier à la fenêtre du plugin. Logic, Live et Reaper le font après un clic dans la fenêtre ; certains logiciels gardent toujours les flèches pour eux.

## La NES : DPCM, instruments FamiTracker et puces de cartouches

**Le canal DPCM (2A03 > DPCM).** C'est le cinquième canal de la NES : il joue des échantillons sur 1 bit, que des jeux ont utilisés pour leur batterie, des voix ou des basses. Le bouton **Importer** du panneau Envelope accepte un WAV : il est converti à la vitesse la plus rapide de la puce (environ 33 kHz), et **limité à environ 1 s** (4081 octets, la limite de la puce).

- **Root Note** indique la touche qui joue le son tel quel. Les autres touches prennent la plus proche des 16 vitesses de la puce, comme dans les jeux : la hauteur avance par sauts, et aucune touche ne joue plus vite que la note de base.
- Sans **Loop**, le son joue jusqu'à sa fin, même une fois la touche relâchée. Avec **Loop**, il recommence tant que la touche est enfoncée.
- Le canal n'a pas de volume : l'enveloppe, la vélocité, le vibrato et le glide sont grisés.

**Les instruments FamiTracker (`.fti`).** Dans les jeux NES, un « instrument » est une suite de valeurs que le programme change 60 fois par seconde : volume, arpège, hauteur, rapport cyclique ou forme d'onde. FamiTracker les enregistre dans des fichiers `.fti`, que le bouton **Importer** accepte aussi.

- L'instrument va à sa puce (2A03, VRC6 ou N163), qui s'affiche, et la case **Instrument** s'active : il remplace alors les boutons de l'enveloppe. Décoche la case pour revenir à l'enveloppe. Chaque puce garde son propre instrument.
- Comme dans FamiTracker, la note s'arrête quand la touche est relâchée, sauf si la séquence de volume a un point de relâchement : elle joue alors sa fin.
- Un instrument 2A03 qui contient des échantillons DPCM devient un **kit** sur le canal DPCM : chaque échantillon joue sur sa touche, à sa vitesse. L'infobulle de l'état indique quelle touche joue quoi.
- Pris en charge : les fichiers 2.1 à 2.5 des puces 2A03, VRC6 et N163. Les instruments VRC7, FDS et 5B sont refusés avec un message.
- Les fichiers importés sont copiés dans le projet Logic.

**Le dossier d'instruments.** Sous le bouton **Importer**, un menu liste les fichiers `.fti` du dossier `~/Music/RetroChip/NES/`, avec une rubrique par sous-dossier, comme pour les SoundFonts. Choisir un fichier l'importe, comme s'il venait de la fenêtre d'import. Le bouton en forme de dossier ouvre ce dossier dans le Finder (et le crée s'il n'existe pas) ; le bouton en forme de flèche circulaire relit le dossier après un ajout, sans fermer le plugin. Le dossier est aussi relu chaque fois que la NES s'affiche. Les trois puces partagent ce dossier : chaque instrument va à sa puce.

**VRC6 et N163.** Ces puces étaient dans certaines cartouches Famicom japonaises, qui ajoutaient des canaux à la console.

- **VRC6** (Konami, par exemple *Akumajō Densetsu*) : une pulse à 8 rapports cycliques, ou une dent de scie plus forte, dont le volume change la forme comme sur la puce.
- **N163** (Namco) : des formes d'onde dessinées sur 32 pas de 4 bits. **Wave** en choisit une des 8 fournies, ou un instrument `.fti` N163 apporte les siennes. **Channels** imite le nombre de canaux que la puce fait tourner : plus il y en a, plus chacun est mis à jour lentement, ce qui donne le grain typique de la puce.

## La Genesis : deux puces

La Genesis a deux puces sonores, et ses musiques utilisent les deux. Pour un morceau, mets une instance par son : par exemple **Genesis** > **FM** pour la basse et la mélodie, **Genesis** > **FM** avec **Mon kit PCM** pour la batterie, et **Genesis** > **PSG** pour un arpège ou un charleston.

Avec **Genesis** > **PSG** (la puce SN76489) :

- **Tone** joue une onde carrée. La puce ne descend pas sous environ 109 Hz (la touche A1 du clavier du plugin) : plus bas, la hauteur reste à cette limite. Les notes aiguës sont un peu fausses, comme sur la console, car la puce divise son horloge par un nombre entier.
- **Noise** joue du bruit. **White** donne du bruit blanc, pour les percussions. **Periodic** donne un bourdonnement qui joue les notes et descend plus bas que Tone, ce que les jeux utilisent pour les basses.
- **Noise Rate** choisit l'une des trois vitesses de la puce (**High**, **Mid**, **Low**) ou **Note**, qui suit la touche jouée. À vitesse fixe, le bruit n'a pas de hauteur et Vibrato et Glide sont grisés.
- La puce n'a pas d'enveloppe : comme dans un pilote audio de jeu, le volume et la hauteur changent par pas, 60 fois par seconde, avec 16 volumes séparés de 2 dB. L'enveloppe et le vibrato peuvent donc sonner en marches d'escalier, et c'est voulu.

## Importer un WAV (SNES, S-DSP)

Avec **SNES** > **S-DSP**, le bouton **Importer** charge un fichier WAV choisi par toi, qui devient l'instrument « Mon échantillon ».

- Un seul échantillon par instance. Un nouvel import remplace le précédent.
- Un fichier stéréo est mixé en mono. Le fichier est converti à 32 kHz, puis compressé en BRR, hors du fil audio.
- **Limite de durée : environ 3,6 s** (64 Ko de BRR, soit 116 496 échantillons à 32 kHz). Un fichier plus long est coupé et l'interface l'indique.
- Un fichier illisible, vide ou non pris en charge laisse l'échantillon actuel en place, avec un message.
- La note de base indique à quelle note le fichier sonne tel quel. L'option de boucle reprend au début de l'échantillon.
- La longueur est ajustée au multiple de 16 échantillons le plus proche pour que les boucles se referment sans trou. La hauteur bouge d'autant, ce qui est imperceptible sauf sur un fichier très court.
- L'échantillon est enregistré dans l'état du plugin, donc dans le projet Logic : le fichier WAV d'origine n'est plus nécessaire à la réouverture.

## Importer un TFI ou un WAV (Genesis, FM)

Avec **Genesis** > **FM**, le bouton **Importer** accepte un fichier `.tfi`, un WAV ou plusieurs WAV. Chaque import remplace le précédent de sa sorte et laisse l'autre en place. Les deux sont enregistrés dans le projet Logic : les fichiers d'origine ne sont plus nécessaires à la réouverture. Un fichier inutilisable laisse le son actuel en place, avec un message.

**Le dossier de timbres.** Sous le bouton **Importer**, un menu liste les fichiers `.tfi` du dossier `~/Music/RetroChip/Genesis/`, avec une rubrique par sous-dossier. Choisir un fichier l'importe et le sélectionne comme timbre. Le bouton en forme de dossier ouvre ce dossier dans le Finder (et le crée s'il n'existe pas) ; le bouton en forme de flèche circulaire relit le dossier après un ajout, sans fermer le plugin. Le dossier est aussi relu chaque fois que **Genesis** > **FM** s'affiche.

**Timbre `.tfi` (« Mon timbre TFI »).** Un fichier TFI (TFM Music Maker) décrit un timbre FM de 42 octets et ne contient aucun son enregistré. Le plugin charge ses opérateurs, son SSG-EG, son algorithme et sa rétroaction. Ensuite, le timbre se joue comme les autres, avec les mêmes réglages.

**Échantillon WAV (« Mon échantillon PCM »).** Le fichier passe par le DAC de la puce, le canal PCM 8 bits de la Genesis.

- Un fichier stéréo est mixé en mono, puis converti à environ 13,3 kHz et en 8 bits, hors du fil audio.
- **Limite de durée : environ 19,7 s** (256 Ko). Un fichier plus long est coupé et l'interface l'indique.
- **Root Note** indique à quelle note le fichier sonne tel quel.
- **Loop** décide de ce que fait la touche. Sans boucle, l'échantillon joue jusqu'à sa fin, même une fois la touche relâchée, ce qui convient aux percussions. Avec la boucle, il reprend au début tant que la touche est enfoncée, puis s'éteint en 20 ms à 2 s selon le bouton **Release**.
- Les 8 voix sont additionnées puis écrêtées à 8 bits pour l'unique DAC, comme le ferait un pilote audio de console : un accord de notes fortes distorsionne.
- Feedback, Bright, Attack et Decay n'agissent pas sur un échantillon et sont grisés. Vibrato, Glide, Velocity et Gain restent actifs.

**Plusieurs WAV (« Mon kit PCM »).** Pour une batterie, comme dans les jeux Genesis, sélectionne plusieurs fichiers d'un coup dans la fenêtre d'import : chacun va sur sa touche et une seule instance joue tout le kit.

- Les fichiers sont placés par ordre de nom, en partant de la touche la plus grave du clavier affiché (C1), une touche par fichier. `1 kick.wav`, `2 snare.wav` et `3 hat.wav` vont donc sur C1, C#1 et D1. L'infobulle de l'état indique quelle touche joue quoi.
- Chaque son joue à sa hauteur d'origine et jusqu'à sa fin, même une fois la touche relâchée. Les touches sans son ne font rien.
- Seuls **Velocity** et **Gain** agissent sur un kit. Les autres réglages sont grisés.
- **Limites :** 61 sons, et 256 Ko au total (environ 19,7 s). Ce qui dépasse est laissé de côté ou coupé, avec un message. Un fichier illisible est ignoré sans laisser de trou.
- Un nouvel import remplace tout le kit. Si le kit est déjà le timbre choisi, un seul WAV donne un kit d'un son.

## SoundFonts

Le choix **SoundFont** de l'onglet SNES joue un fichier `.sf2` (SoundFont 2) que tu as toi-même téléchargé. RetroChip ne fournit aucun fichier SoundFont et n'en télécharge aucun : le choix de ce que tu télécharges et utilises t'appartient. Des SoundFonts tirés de jeux sont par exemple proposés par William Kage : <https://www.williamkage.com/snes_soundfonts/>.

**Dossier.** Place tes fichiers dans `~/Music/RetroChip/SoundFonts/`. Le bouton **Ouvrir le dossier** de l'interface le crée s'il n'existe pas. Chaque sous-dossier direct est une rubrique du menu, dont le nom s'affiche tel que tu l'as écrit. Les fichiers placés à la racine vont sous **Autres**. Par exemple :

```text
~/Music/RetroChip/SoundFonts/
├── SNES/
│   └── Mon jeu.sf2
├── Mes sons/
│   └── Cordes.sf2
└── Mon son.sf2          (apparaît sous « Autres »)
```

**Dézipper d'abord.** Les SoundFonts de ce site se téléchargent en `.zip`. RetroChip ne lit pas les ZIP : décompresse l'archive (double-clic dans le Finder), puis place le fichier `.sf2` obtenu dans un sous-dossier, comme `SNES/Mon jeu.sf2` ci-dessus.

**Dans Logic Pro.**

1. Crée une piste d'instrument logiciel avec **AU Instruments > SleeperMusic > RetroChip**.
2. Choisis l'onglet **SNES**, puis **SoundFont** à côté.
3. Si tu viens d'ajouter un fichier, clique sur **Actualiser** (la liste est aussi relue à l'ouverture de l'interface).
4. Choisis le fichier dans le premier menu, puis l'instrument dans le second.
5. Joue : chaque note utilise les zones, les boucles et les enveloppes de l'instrument choisi. L'écho, le gain et la vélocité se règlent dans l'interface.

**Limites.**

- **32 voix** au maximum par instance. Au-delà, une nouvelle note est ignorée. Une paire stéréo (une zone à gauche, une à droite) prend 2 voix : compte environ 16 notes pour un instrument stéréo.
- **Sortie mono**, comme les autres consoles. Les paires stéréo sont mixées en mono et le panoramique est ignoré ; pour qu'une paire ne sonne pas deux fois plus fort qu'une zone mono, chaque zone décalée vers un côté est atténuée (6 dB à fond d'un côté).
- **Ignoré dans les fichiers SF2 :** les modulateurs (donc la vélocité vers le filtre et le vibrato de la molette de modulation) et les envois de réverbération et de chorus.
- **Non pris en charge :** les fichiers ZIP, SFZ et SF3. Seuls les fichiers `.sf2` sont lus.
- Un fichier de plus de 1 Go est refusé.

**Le projet garde une référence, pas une copie.** Le projet Logic enregistre le chemin du fichier (relatif au dossier SoundFonts) et le numéro de l'instrument, mais pas les échantillons. Tant que le fichier reste à sa place dans le dossier, le projet rouvre le même son. Si tu le déplaces, le renommes ou le supprimes, l'instrument reste muet et un message donne le nom du fichier manquant. Garde donc les fichiers à leur place, ou remets-les-y avant d'ouvrir un ancien projet. Si tu remets le fichier pendant que le projet est ouvert, clique sur **Actualiser** : il est relu et l'instrument enregistré revient.

**Avant de vendre.** Le site de William Kage conseille de consulter un avocat avant de vendre quoi que ce soit qui utilise ces échantillons. RetroChip ne distribue aucun SoundFont ; les droits sur les fichiers que tu utilises sont ton affaire.

## Compiler

Il faut les Command Line Tools d'Apple et CMake. JUCE, ymfm et TinySoundFont doivent être clonés dans `external/`.

```bash
brew install cmake
```

JUCE 9.0.5 :

```bash
git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git external/JUCE
```

ymfm, épinglé au commit `81aec25ccbb98f4873a255f7551ac4dadac59b4a` (moteur du YM2612) :

```bash
git clone https://github.com/aaronsgiles/ymfm.git external/ymfm
git -C external/ymfm checkout 81aec25ccbb98f4873a255f7551ac4dadac59b4a
```

TinySoundFont, épinglé au commit `853a0a171759f1ddba0de1442133a75912bbeffa` (lecture des SoundFonts) :

```bash
git clone https://github.com/schellingb/TinySoundFont.git external/TinySoundFont
git -C external/TinySoundFont checkout 853a0a171759f1ddba0de1442133a75912bbeffa
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j8
```

**Sous Windows**, il faut Visual Studio 2022 (avec « Développement Desktop en C++ »), CMake et Git. Les commandes de clonage sont les mêmes ; ensuite, dans une invite de commandes :

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release
```

Cette compilation produit l'Audio Unit, le VST3 et l'application autonome, et copie l'AU et le VST3 dans `~/Library/Audio/Plug-Ins`. L'application autonome est dans `build/RetroChip_artefacts/Release/Standalone/RetroChip.app`.

## Tester

```bash
ctest --test-dir build -C Release --output-on-failure
```

```bash
auval -v aumu Rc2a Slpr
```

Il y a trois suites : celle du moteur, celle du plugin, et `host_tests`, qui charge le VST3 et l'AU compilés comme le fait un logiciel hôte. Elle vérifie le chargement, le son à 44,1, 48 et 96 kHz avec des blocs de tailles diverses, les préréglages, l'enregistrement et la réouverture d'un projet, l'interface et deux instances à la fois.

Le fichier `.github/workflows/build.yml` compile et teste le plugin sur macOS et sous Windows à chaque envoi sur GitHub, une fois le dépôt créé. Le VST3 Windows de chaque compilation se télécharge depuis la page de la compilation, pendant 14 jours.

Les tests du plugin écrivent des captures de l'interface dans `build/editor_*.png`.

Si `auval` ne voit pas la dernière version après une compilation, exécute d'abord `killall -9 AudioComponentRegistrar`.

## Structure

- `engine/` : moteurs sonores en C++ pur, sans JUCE.
  - `engine/` (racine) : moteur NES (2A03 avec DPCM, VRC6, N163), interface commune `ConsoleEngine` et rééchantillonneur.
  - `engine/nes/` : lecteur de fichiers FTI, séquences d'instrument, encodeur DPCM et formes d'onde du N163.
  - `engine/genesis/` : moteur YM2612 (deux puces ymfm, 12 timbres, lecture du PCM et des kits par le DAC), lecteur de fichiers TFI et moteur SN76489.
  - `engine/snes/` : moteur S-DSP (voix, enveloppe, écho, BRR, banque de 12 instruments).
  - `engine/soundfont/` : lecture des fichiers SoundFont avec TinySoundFont.
- `plugin/` : processeur, paramètres, préréglages, import WAV, TFI et FTI, dossiers de SoundFonts, de timbres et d'instruments, traduction française (`Localisation.cpp`) et interface JUCE.
- `tests/` : tests du moteur, du plugin et du chargement par un hôte (`host_tests.cpp`), avec les références de la 2A03 dans `tests/reference/`.
- `.github/workflows/` : compilation et tests sur macOS et Windows.
- `external/` : JUCE, ymfm et TinySoundFont, clonés à la main et ignorés par le dépôt.
- `LICENSE` : texte de la licence AGPLv3.
- `THIRD_PARTY_LICENSES.md` : licences de JUCE, de ymfm, de TinySoundFont et des composants inclus dans JUCE.

## Licence

RetroChip est un logiciel libre, © 2026 SleeperMusic, distribué sous la licence [GNU AGPLv3](LICENSE), **sans aucune garantie**. Toute personne qui distribue le plugin, modifié ou non, doit aussi fournir son code source sous cette licence.

Toute distribution doit inclure [`LICENSE`](LICENSE) et [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

- **JUCE :** utilisé sous AGPLv3. Il existe aussi sous licence commerciale, que ce projet n'utilise pas.
- **ymfm :** émulation des puces Yamaha FM par Aaron Giles, sous licence BSD 3 clauses.
- **TinySoundFont :** lecteur de SoundFonts par Bernhard Schelling, sous licence MIT.
- **SoundFonts :** aucun n'est fourni avec le plugin. Leurs droits restent ceux de leurs auteurs.
- **Tables des puces :** valeurs documentées du matériel, sans code d'émulateur tiers.

## Marques

RetroChip n'est ni affilié à Nintendo, Sega, Yamaha, Sony, Steinberg ou Apple, ni approuvé par ces sociétés. Leurs marques ne servent ici qu'à décrire le matériel et les formats concernés. VST est une marque de Steinberg Media Technologies GmbH.

## Diffusion

Les compilations de ce dépôt n'ont qu'une signature locale (ad hoc) et ne fonctionnent que sur Apple Silicon. Une version prête à installer demande un compte Apple Developer, une signature, une notarisation et un binaire universel pour les Mac Intel.

Les codes `Slpr` et `Rc2a` identifient le plugin dans les projets : ils doivent rester stables une fois le plugin diffusé.
