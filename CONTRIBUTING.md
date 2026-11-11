# Contribuer

*Contributions in English are welcome too. The project documentation is in French; code, comments and commit
scopes are in English.*

Merci de votre intérêt ! Le plus utile, par ordre :

1. **Un rapport de compatibilité** d'une autre voiture : le contrôle de l'[installation](docs/INSTALLATION.md) ne
   modifie rien. Ouvrez un ticket *Compatibility report* avec ce qu'il affiche.
2. **Un signalement de bug**, avec la version, le logiciel de la voiture et, si possible, les journaux du profil
   maintenance.
3. **Une demande de fusion**, idéalement après en avoir parlé dans un ticket.

## Ce qui ne va jamais dans git

- **Le mot de passe de la voiture.** Rangé dans `.secrets/hu-password.txt` (ignoré par git), `tools/verify_repo.py` le
  cherche dans chaque commit envoyé pour vérifier qu'il n'a fuité nulle part.
- **Les fichiers BMW** : la copie du système de la voiture, ses bibliothèques, journaux et captures. Les tests qui en ont
  besoin les lisent dans `~/entrynav2/rootfs`, hors du dépôt. Seule exception : les quelques relevés courts de
  `tests/fixtures/`, dont l'origine est notée dans [tests/fixtures/README.md](tests/fixtures/README.md).
- **Les données personnelles** : numéro de série de la voiture (VIN), adresses Bluetooth et Wi-Fi, positions GPS.
- **Les programmes compilés.**

`.gitignore` n'autorise que ce qu'il liste. `tools/verify_repo.py` relit dans git chaque fichier de chaque commit envoyé.
Après un clone, activer ce contrôle à chaque envoi :

```sh
git config core.hooksPath .githooks
```

## Sécurité de la voiture

- Sans fil par défaut ; par câble USB seulement avec le fichier `usb`, que pose la case du menu : le mode n'est
  jamais déduit de ce qui est branché. Le Bluetooth est toujours rendu à l'iDrive à l'arrêt.
- USB : seul un téléphone Android reçoit des requêtes ou une réinitialisation. Jamais d'interface prise à un pilote
  ou à un programme BMW.
- Aucun service BMW redémarré tout seul, aucun fichier BMW modifié. Deux dossiers ajoutés, toujours retirables.
- Toute nouvelle écriture sur le réseau de la voiture est annoncée dans le CHANGELOG et dans la procédure d'installation.
- Le contrôle de compatibilité n'écrit rien sur la mémoire flash : seulement dans `/tmp`.
- Ne pas changer `vehicleId` dans `core/sdr_builder.cpp` : le téléphone y range ses réglages pour la voiture.
  `tests/test_sdr_g20.cpp` fige les octets envoyés au téléphone.
- Ne jamais régénérer un relevé figé de `tests/fixtures/` pour faire passer un test : `test_session_phone`,
  `test_wireless_phone` et `test_kombi_golden` comparent chaque octet envoyé au téléphone et au compteur. Un relevé ne
  change qu'avec un changement de comportement voulu, expliqué dans le commit et le CHANGELOG.
- Pas de réglage par variable d'environnement : des constantes, des arguments de ligne de commande, quelques fichiers
  interrupteurs dans `/var/local/data/AA`.

## Dire ce qui est prouvé

Distinguer toujours « essayé en voiture » de « testé sur PC ». Rien n'est validé tant qu'une voiture ne l'a pas montré.
Le CHANGELOG et la ROADMAP suivent cette règle, les demandes de fusion aussi.

Toute commande destinée à la voiture se teste d'abord avec la BusyBox 1.21 de la voiture sous QEMU : pas de
`ip neigh`, pas de `netstat -p`, pas de classes de caractères dans `tr`, et les processus d'une session SSH meurent à sa
fermeture.

## Tests

| Niveau | Commande | Il faut |
|---|---|---|
| PC, comme la CI | `make check` | `g++`, `libssl-dev`, `libdbus-1-dev`, `protobuf-compiler`, `python3-protobuf`, clang-format 18.1.8 et ruff 0.6.9 (`pip install -r requirements-dev.txt`) |
| ARM | `bash toolchain/tc.sh make -j4 all hmiapp test-arm-all` | Docker, la copie du système de la voiture |
| ARM sous QEMU | `python3 -I tests/run_qemu.py` | `qemu-user`, la copie du système |
| Programmes validés | `make verify_binary verify_wireless` | idem |
| Procédure d'installation | les commandes de [docs/INSTALLATION.md](docs/INSTALLATION.md), avec la BusyBox de la voiture sous QEMU | idem |

## Style

| Langage | Règles | Outil |
|---|---|---|
| C et C++ | GCC 4.7 en `-std=gnu++0x` (pas de C++14), `.clang-format` | `python tools/format_sources.py` (clang-format 18.1.8) |
| Python | 3.9 et plus, une fonction `main()`, `pathlib`, pas d'`assert` pour les contrôles | `python tools/format_sources.py`, `ruff check .` (0.6.9) |
| Shell | POSIX `sh` pour BusyBox 1.21, ASCII, indentation 4 | `shellcheck`, `shfmt -i 4` |

Commentaires et journaux en anglais, une étiquette par module (`[Video]`, `[KombiNav]`…) ; textes affichés par
l'iDrive en français. Chaque fichier source commence par :

```cpp
// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Une ligne qui dit ce que fait le fichier.
```

## Commits et branches

- Une branche par sujet, partie de `main` : `fix/…`, `feat/…`, `docs/…`.
- Messages au format [Conventional Commits](https://www.conventionalcommits.org/fr/) : `type(portée): description` en
  français, à l'impératif ou au constat, 72 caractères au plus. Types : `feat`, `fix`, `refactor`, `perf`, `test`,
  `docs`, `build`, `ci`, `chore`. Le corps explique pourquoi.
- Un commit = un changement cohérent qui compile et passe `make check`.
- Les versions suivent [SemVer](https://semver.org/lang/fr/) et sont marquées par une étiquette `vX.Y.Z`.

## Publier une version

1. `make check`, la compilation ARM, `tests/run_qemu.py`, `make verify_binary verify_wireless`.
2. Mettre à jour `VERSION` et `CHANGELOG.md` (section de la version, date, « PC » ou « voiture »).
3. `python3 tools/package_release.py` : `artifacts/entry-aad-X.Y.Z-armhf.tar` et son `.sha256`.
4. Demande de fusion `chore(version): X.Y.Z`, puis étiquette `vX.Y.Z` sur `main` et publication sur GitHub de l'archive
   et de son `.sha256`, avec l'extrait du CHANGELOG. Une version jamais essayée en voiture est marquée *pre-release*.
