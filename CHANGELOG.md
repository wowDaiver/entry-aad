# Journal des versions

Tous les changements notables de ce projet. Format inspiré de [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/),
versions selon [SemVer](https://semver.org/lang/fr/).

Chaque version dit si elle a été **essayée en voiture** ou seulement **testée sur PC** (tests PC, et programmes ARM sous
QEMU avec les bibliothèques de la voiture).

## [Non publié]

### Ajouté

- Transport Android Auto USB/AOA facultatif, avec le même protocole TLS, décodeur, commandes, GPS et micro que le
  transport TCP. Le son reste sur le Bluetooth BMW. Sans fichier `usb-phone`, le démarrage reste sans fil.
- Sélection au démarrage par `/var/local/data/AA/usb-phone` : `auto PORT VID PID` choisit USB si le téléphone est
  présent sur ce port, sinon sans fil ; `PORT VID PID` impose USB. Aucun basculement pendant une session.
  [Configuration et limites](docs/USB.md).
- Requêtes de contrôle AOA 51/52/53 envoyées uniquement au téléphone choisi, puis interface 0 et paire bulk validées
  sur le même port après réénumération. Utilisation de la libusb déjà installée, sans détachement, reset, changement
  de configuration ni arrêt d'un service BMW. Aucun changement au point d'accès du téléphone.
- Tests simulés des transferts partiels, annulation, descripteurs, refus d'interface occupée, sélection au démarrage,
  arrêt et verrou du superviseur. Le relevé figé du téléphone est aussi rejoué sur USB, sans modifier ses octets.

### Validation et limites

- Essais courts en voiture sur BMW X3, EntryNav2 `002_146`, écran 1280×480, Samsung S24+ : image, molette et appui
  iDrive, son Bluetooth et micro confirmés. Le démarrage automatique USB depuis le menu iDrive a également fonctionné.
  Le test borné à 180 s inclut l'attente du branchement ; il représente environ 131 s de projection, pas 180 s.
- Après Arrêter, le processus BMW `vmostAuxInUSB` peut prendre l'interface AOA. Un débranchement/rebranchement avant
  Démarrer peut rester nécessaire ; le service ne lui reprend jamais l'interface.
- La stabilité sur 20 minutes, le débranchement/rebranchement en projection, le retour sans fil dans cette voiture et
  la caméra de recul ne sont pas validés par ces essais. La matrice ARM/QEMU complète reste à exécuter pour cette
  contribution. Les journaux et fichiers de la voiture ne font pas partie du dépôt.

## [0.11.0-rc.1] - 2026-11-10 · PC

Version candidate, **testée sur PC seulement** : à essayer en voiture avant la 0.11.0. Installée par-dessus la 0.10.0,
elle demande un redémarrage de l'unité.

### Corrigé

- Page vide de l'iDrive : ses délais (1, 3, 10 et 15 s) ne dépendent plus de l'heure de la voiture, que le GPS peut
  remettre à l'heure en roulant ([#2](https://github.com/Jiraiya43/entry-aad/issues/2)). Testé sur PC.
- Marche arrière : la page vide n'est plus redemandée par-dessus la caméra de recul quand celle-ci prend la main avant
  qu'Android Auto s'écarte ([#3](https://github.com/Jiraiya43/entry-aad/issues/3)). Testé sur PC.

### Modifié

- Sans fichier `vehicle`, le téléphone voit une BMW « iDrive » au lieu d'une « M340i », le modèle de la voiture d'essai
  ([#8](https://github.com/Jiraiya43/entry-aad/issues/8)). Le fichier `vehicle` donne le vrai modèle. Testé sur PC.
- Le superviseur `evo-trial` s'appelle maintenant `aa-supervisor` ([#7](https://github.com/Jiraiya43/entry-aad/issues/7)) :
  son programme est `/opt/aa/aa-supervisor`, et l'option `--trial-seconds` du service devient `--limit-seconds`.
  L'étape 3 de l'installation retire l'ancien programme. Testé sur PC.
- Le service annonce au téléphone la version du projet (fichier `VERSION`) comme version logicielle de l'unité, au lieu
  de la valeur fixe `1.3.33` ([#9](https://github.com/Jiraiya43/entry-aad/issues/9)). Testé sur PC.
- Origine des données venues d'ailleurs (certificat Android Auto, description du DHU de Google, images d'attente,
  relevés de la voiture) notée dans les fichiers et dans la licence. Les programmes sont inchangés.

## [0.10.0] - 2026-11-10 · voiture

Première version publique. Essayée sur une BMW G20 de 2019, logiciel iDrive `202111011321`.

### Ajouté

- **Android Auto sans fil** dans l'iDrive : le téléphone s'appaire en Bluetooth, puis rejoint le Wi-Fi de la voiture.
- **Appli iDrive** : ligne *Android Auto* du menu Media, avec Démarrer, Arrêter, démarrage automatique et aide. Android
  Auto lancé automatiquement reste à l'écran, molette sans effet sur l'iDrive.
- **Commandes d'origine** : molette, touches (NAV et MEDIA ouvrent les applis d'Android Auto), tactile, boutons du
  volant, bouton vocal vers l'Assistant, micro de la voiture.
- **GPS de la voiture** transmis au téléphone, jour et nuit, modes de conduite, caméra de recul, pastille du volume.
- **Guidage sur le compteur et la tête haute** : flèche, distance, rue, sortie de rond-point.
- **Contrôle de compatibilité** sans rien modifier (`evo-trial --compat`) et **procédure d'installation** par SSH
  ([docs/INSTALLATION.md](docs/INSTALLATION.md)), avec archive `entry-aad-X.Y.Z-armhf.tar` et empreintes.
- **Garde-fous** : l'appli se met en retrait si l'iDrive plante plusieurs fois après l'avoir chargée ; le Bluetooth est
  rendu à l'iDrive à l'arrêt.
- **Tests** : chaque octet envoyé au téléphone et au compteur est comparé à un relevé figé, sur PC et sous QEMU.

### Limites

- Essayée sur une seule voiture. La procédure d'installation a été répétée avec la BusyBox de la voiture sous QEMU, pas
  encore suivie sur une voiture.
- Voir les [limites connues](ROADMAP.md#limites-connues).

[Non publié]: https://github.com/Jiraiya43/entry-aad/compare/v0.11.0-rc.1...HEAD
[0.11.0-rc.1]: https://github.com/Jiraiya43/entry-aad/releases/tag/v0.11.0-rc.1
[0.10.0]: https://github.com/Jiraiya43/entry-aad/releases/tag/v0.10.0
