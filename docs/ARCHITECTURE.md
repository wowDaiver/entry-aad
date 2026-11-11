# Architecture

## Vue d'ensemble

Trois programmes tournent sur l'unité, une seule machine ARMv7 sous Linux (noyau 3.12, glibc 2.15) :

| Programme | Emplacement | Rôle |
|---|---|---|
| `libEntryAad.so` | `/opt/hmi/data/ro/Plugins/EntryAad` | Appli chargée par `HmiMain3D` : ligne du menu Media, menu, focus, garde-fou |
| `aa-supervisor` | `/opt/aa` | Superviseur : prépare le Bluetooth et le Wi-Fi, lance le service, rend le Bluetooth à la fin. `--compat` répète le démarrage à blanc pour le contrôle de compatibilité |
| `entry-aad` | `/opt/aa` | Service Android Auto |

Le dossier `/var/local` interdit l'exécution : les programmes doivent aller sur le système, en lecture seule le reste du
temps. Les réglages restent dans `/var/local/data/AA`.

## Une connexion

1. L'appli iDrive démarre le superviseur, sur demande ou automatiquement.
2. Le service annonce le profil Bluetooth d'Android Auto et transmet au téléphone les identifiants du Wi-Fi de la voiture.
3. Le téléphone rejoint ce Wi-Fi et ouvre la connexion TCP. La session Android Auto est chiffrée en TLS.
4. Le service décrit la voiture au téléphone, reçoit la vidéo H.264 et l'affiche dans une couche du LayerManager.
5. Entrées, GPS, jour et nuit, micro et guidage circulent ensuite chacun sur son canal.
6. Si le téléphone se tait 5 secondes, la session se ferme et le service attend une nouvelle connexion.

## `core/` : Android Auto

Le protocole Android Auto côté voiture, sans rien de propre à BMW.

| Fichier | Rôle |
|---|---|
| `aa_session` | Session Android Auto : cycle de vie, fils, délais ; aiguille chaque message vers son canal |
| `aa_channels.cpp` | Un gestionnaire par canal : vidéo, audio, entrées, capteurs, micro, navigation |
| `aa_protocol.hpp` | Canaux, types de messages et drapeaux du protocole, nommés |
| `aa_frame` | Découpage et réassemblage des trames, borné contre les longueurs hostiles |
| `aa_messages` | Construction des messages envoyés au téléphone |
| `aa_tls` | Chiffrement TLS de la session (OpenSSL), objets possédés en RAII |
| `aa_input.hpp`, `car_inbox` | Événements tactiles et touches de la voiture, file d'attente vers la session |
| `pb.hpp` | Écriture des messages protobuf, lecture bornée de ce que le téléphone envoie |
| `tcp_server` | Attente de la connexion du téléphone sur le Wi-Fi de la voiture |
| `wireless_setup` | Échange Bluetooth qui transmet le Wi-Fi au téléphone |
| `sdr_builder` | Description de la voiture envoyée au téléphone ; la plateforme lui donne le fichier `vehicle` et y retire les capteurs absents |
| `car_location.hpp` | Position de la voiture vers le capteur Android Auto |
| `crash_handler` | Trace d'un plantage, puis le nettoyage enregistré par la plateforme |
| `include/` | Modèle de description de la voiture, certificat de l'unité |

## `platform/` : l'EntryNav2

| Fichier | Rôle |
|---|---|
| `main.cpp` | Ligne de commande du service et assemblage des modules |
| `service_app` | Le service en marche : réactions à la session, à la vidéo, aux entrées et aux commandes de l'appli |
| `peek_timing` | Quand effacer l'image d'Android Auto : volume, fenêtres de l'iDrive, mode de conduite, touches (testé sur PC) |
| `wireless_loop` | Boucle d'attente du téléphone en Bluetooth, puis de sa connexion Wi-Fi |
| `usb_loop`, `usb_accessory` | Mode USB : repère un téléphone Android sur les prises, le passe en mode accessoire |
| `usb_bridge`, `usb_devfs` | Mode USB : pont entre le téléphone (usbfs du noyau) et la session, par un socket local |
| `service_contract.hpp` | Chemins et port partagés par l'appli iDrive, le superviseur et le service |
| `evo_video`, `evo_ilm`, `omap_dmabuf` | Décodage et affichage de la vidéo, gestion de sa couche |
| `evo_pill`, `volume_pill.hpp` | Pastille du volume au-dessus d'Android Auto : client Wayland, dessin |
| `evo_knob`, `evo_touch`, `input_mode.hpp` | Entrées : molette, touches, tactile |
| `vmost_client` | Accès au bus vMOST de la voiture (molette, touches, Wi-Fi), chargé à l'exécution |
| `evo_ccs.hpp`, `evo_touch_frame.hpp` | Messages de la molette BMW (deux générations de contrôleur) et trames du tactile |
| `evo_gps` | Lecture passive du GPS de la voiture |
| `evo_mic` | Micro de la voiture pour l'Assistant |
| `evo_wlan`, `evo_ap` | Wi-Fi de la voiture, allumé comme le fait CarPlay |
| `wifi_link` | Qualité de la liaison Wi-Fi avec le téléphone, lue dans le noyau pour les journaux (testé sur PC) |
| `alps_spp`, `evo_spp_snapshot` | Pile Bluetooth de la voiture : commandes, et lecture de son état en mémoire |
| `evo_elf.hpp` | Symboles exportés par une bibliothèque de la voiture, lus dans son fichier |
| `evo_control` | Commandes de l'appli vers le service |
| `kombi_nav` | Service de guidage vers le compteur et la tête haute : fils, réglage `kombi-off`, annulation d'urgence |
| `kombi_phone` | Lecture des messages de navigation d'Android Auto |
| `kombi_guide` | Machine de guidage : manœuvre, distance, voies, rue, approche |
| `kombi_display`, `kombi_graph` | Codes et tables du compteur, dessin des manœuvres et des voies |
| `kombi_client`, `kombi_someip` | Liaison SOME/IP avec le compteur (service `0xB006`) |
| `evo_bmw_guidance` | Détection de la navigation BMW en cours, qui garde la priorité |

## `hmiapp/` : l'appli de l'iDrive

`libEntryAad.so`, chargée par `HmiMain3D` : la ligne *Android Auto* du menu Media et son menu.

| Fichier | Rôle |
|---|---|
| `entry_aa_app.cpp` | L'appli elle-même : création, menu, bouton d'entrée, relevé d'état chaque seconde |
| `bmw_framework` | Déclarations de l'interface BMW des applis, accroches de la table virtuelle |
| `hold_machine` | Quand demander la page vide de l'iDrive sous Android Auto (classe pure, testée sur PC) |
| `pending_restart.hpp` | Redémarrage d'Android Auto quand la case du câble USB change en marche (testé sur PC) |
| `service_link` | Lancement du superviseur, commandes UDP au service, arrêt |
| `car_signals` | Marche arrière, appels, jour et nuit, mode de conduite, lus dans l'iDrive |
| `app_guards` | Interrupteur, garde-fou contre les plantages, démarrage automatique |
| `app_log`, `log_profile.hpp` | Journal de l'appli, profil maintenance |
| `popup_peek.hpp`, `cds_parse.hpp` | Fenêtres de l'iDrive par-dessus Android Auto, lecture des données de l'iDrive |
| `mono_clock.hpp` | Temps écoulé depuis le démarrage de l'unité, pour les délais |
| `EntryAad.xml`, `EntryAad.manifest`, `res/`, `make_db.py` | Écrans, textes et images de l'appli |

## `tools/` : supervision et outils

| Fichier | Rôle |
|---|---|
| `aa_supervisor.cpp` | Le superviseur `aa-supervisor` ; `--compat` répète le démarrage à blanc pour le contrôle de compatibilité |
| `package_release.py` | L'archive d'une version, à copier dans la voiture ([installation](INSTALLATION.md)) |
| `verify_*.py`, `format_sources.py` | Contrôles du dépôt, des programmes ARM et du style |

## Tests

| Test | Vérifie |
|---|---|
| `make test-host` | Logique, sur le PC |
| `tests/run_qemu.py` | Programmes ARM avec les bibliothèques de la voiture, sous QEMU |
| `tests/test_sdr_g20.cpp` | Octets de la description de la voiture envoyée au téléphone, figés |
| `tests/test_session_phone.cpp` | Session complète face à un faux téléphone : chaque message envoyé, comparé à un relevé figé |
| `tests/test_wireless_phone.cpp` | Échange Bluetooth du Wi-Fi, octet par octet, comparé à un relevé figé |
| `tests/test_kombi_golden.cpp` | Chaque trame envoyée au compteur, octet par octet, comparée à un relevé figé |
| `tests/test_peek_timing.cpp`, `tests/test_wireless_lifecycle.cpp` | Effacement de l'image, cycle de vie Bluetooth puis Wi-Fi du service |
| `tests/test_wifi_link.cpp` | Qualité du Wi-Fi : messages du noyau, ligne de journal, connexion rendue en fin de session |
| `tests/test_pending_restart.cpp` | Redémarrage d'Android Auto quand la case du câble USB change en marche |
| `tests/test_hold_machine.cpp` | Page vide de l'iDrive : ouverture, démarrage automatique, fenêtres, modes, appels, touches |
| `tests/test_aa_frame.cpp`, `tests/test_aa_messages.cpp`, `tests/test_tcp_server.cpp` | Trames hostiles, messages construits, écoute TCP |
| `make verify_binary verify_wireless` | Programmes ARM : bibliothèques résolues sur la voiture, empreintes des programmes validés |
| `tools/verify_repo.py`, `tools/verify_core.py` | Contenu de git : ni mot de passe, ni programme compilé, ni fichier BMW ; licence |
