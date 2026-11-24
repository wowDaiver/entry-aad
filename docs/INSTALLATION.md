# Installation

L'installation se fait à la main, par SSH, en quelques commandes. Rien n'est remplacé : deux dossiers sont ajoutés au
système de l'unité, qui ne passe en écriture que le temps de la copie. Chaque étape a été répétée avec la BusyBox de la
voiture.

> [!CAUTION]
> Voiture à l'arrêt, contact mis, pendant toute l'opération. Ne pas couper le contact pendant que le système est en
> écriture (étape 3). En cas de doute, un redémarrage de l'unité remet le système en lecture seule.

## Il faut

- Une unité **EntryNav2** compatible ([compatibilité](COMPATIBILITE.md)).
- Un PC avec un client SSH : Windows 10 ou 11 (client OpenSSH intégré), Linux ou macOS.
- Un **câble ENET** entre la prise OBD de la voiture et le PC. La carte réseau du PC reste en adressage automatique.
- Le **mot de passe root** de l'unité. Il n'est pas fourni avec ce projet.
- L'archive `entrynav2-android-auto-<version>-armhf.tar` d'une [version publiée](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases),
  dont l'empreinte SHA-256 correspond au fichier `.sha256` publié à côté, ou fabriquée par `tools/package_release.py`
  ([compiler](../README.fr.md#compiler)). Jusqu'à la 0.12.0-rc.6, l'archive et son dossier s'appelaient
  `entry-aad-<version>` : pour ces versions, remplacer `entrynav2-android-auto` par `entry-aad` dans les commandes.

Le numéro de version ne s'écrit que deux fois : dans la commande `scp` de l'étape 1 et sur la ligne `V=` de l'étape 2.
Y mettre celui de l'archive téléchargée. `169.254.1.0` est l'adresse de la voiture de référence sur le câble ENET : la
remplacer par celle de l'unité si elle diffère.

Cette page décrit la version en cours. Chaque archive contient sa propre `INSTALLATION.md` : pour une version plus
ancienne, suivre celle de l'archive (dans la 0.10.0, le superviseur s'appelle `evo-trial` et non `aa-supervisor`).

## 1. Copier l'archive dans la voiture

Depuis le PC, dans le dossier de l'archive :

```sh
scp entrynav2-android-auto-0.13.0-rc.10-armhf.tar root@169.254.1.0:/tmp/
ssh root@169.254.1.0
```

Toutes les commandes suivantes se tapent dans cette session SSH, sur la voiture. `/tmp` est en mémoire : rien n'est
encore écrit sur la mémoire flash.

## 2. Contrôler la voiture

Rien n'est modifié pendant cette étape.

```sh
V=0.13.0-rc.10
cd /tmp && rm -rf entrynav2-android-auto-$V && tar -xf entrynav2-android-auto-$V-armhf.tar && cd entrynav2-android-auto-$V
md5sum -c MD5SUMS
head -n 1 /etc/version
df -k /
```

La ligne `V=` vaut pour toute la session SSH, étape 3 comprise : après une reconnexion, la retaper.

Chaque fichier doit répondre `OK`, et il faut au moins 16 Mo libres (colonne `Available`, en Ko). Ensuite, le chargeur de
l'unité vérifie que chaque bibliothèque et chaque fonction utilisée existe, sans rien exécuter, puis `aa-supervisor --compat`
répète à blanc le démarrage d'Android Auto :

```sh
L=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib
for f in opt/aa/entry-aad opt/aa/aa-supervisor opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so; do
    LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes LD_LIBRARY_PATH=$L /lib/ld-linux-armhf.so.3 ./$f 2>&1 |
        grep -E 'not found|undefined symbol|not defined in file'
done
LD_LIBRARY_PATH=$L XDG_RUNTIME_DIR=/run/system/0 WAYLAND_DISPLAY=wayland-0 HOME=/tmp \
    busybox timeout -t 60 ./opt/aa/aa-supervisor --compat
```

- La boucle ne doit **rien** afficher : chaque ligne montre une fonction ou une bibliothèque absente de cette voiture.
- `aa-supervisor --compat` répond une ligne par point : `@ok|…` réussi, `@warn|…` douteux, `@block|…` **bloquant**. Une seule
  ligne `@block` : ne pas installer. Le sens de chaque point est dans [compatibilité](COMPATIBILITE.md).
- Exception : un `@block` sur `alps`, `uuid` ou `spp` (détail `wireless only`) n'empêche que le sans-fil. L'installation
  reste possible en mode [USB](USB.md), à choisir avant le premier *Démarrer*.

Ce résultat, avec la première ligne de `/etc/version`, fait un bon
[rapport de compatibilité](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/new/choose) pour une voiture pas encore listée.

## 3. Installer

Si une version précédente tourne, l'arrêter d'abord depuis son menu de l'iDrive (*Arrêter*). Noter aussi si
`/opt/hmi/data/ro/Plugins` existe déjà (`ls -d /opt/hmi/data/ro/Plugins`) : sinon l'installation le crée, et seule la
désinstallation d'une unité où il n'existait pas avant pourra le retirer.

```sh
mount -o remount,rw /
mkdir -p /opt/hmi/data/ro/Plugins/EntryAad /opt/aa
rm -f /opt/hmi/data/ro/Plugins/EntryAad/* /opt/aa/*
cp opt/hmi/data/ro/Plugins/EntryAad/* /opt/hmi/data/ro/Plugins/EntryAad/
cp opt/aa/* /opt/aa/
chmod 755 /opt/aa/entry-aad /opt/aa/aa-supervisor /opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so
touch /opt/aa/.installed-by-entryaad /opt/hmi/data/ro/Plugins/EntryAad/.installed-by-entryaad
sync
mount -o remount,ro /
cd / && md5sum -c /tmp/entrynav2-android-auto-$V/MD5SUMS
```

- `rm -f` avant la copie : l'iDrive garde l'ancienne appli en mémoire jusqu'à son redémarrage, et un fichier effacé puis
  recopié n'est jamais réécrit sous ses pieds.
- Le fichier `.installed-by-entryaad` marque chaque dossier comme ajouté par ce projet.
- Le dernier `md5sum -c` relit les fichiers installés : chacun doit répondre `OK`.
- Si `mount -o remount,ro /` échoue, le redémarrage qui suit remet le système en lecture seule.

Facultatif, la voiture annoncée au téléphone : le modèle et l'année qu'il affiche, et le carburant. Sans ce fichier,
il affiche la série codée dans la voiture (par exemple « G20 » ou « F56 », « iDrive » si le codage ne la dit pas), 2019
et essence : l'unité ne connaît pas le nom du modèle. La marque, BMW ou MINI, vient du codage de l'iDrive et ne se
règle pas ici. Le côté du conducteur, pour lequel Android Auto dispose son écran, vient
aussi du codage de la voiture (voir plus bas pour l'imposer).

Remplacer `MODELE` et `ANNEE` par ceux de la voiture (par exemple `320d` et `2020`, ou `Cooper S` et `2019`) ;
24 caractères au plus pour le modèle : lettres sans accent, chiffres, espaces, `-`, `.` et `_`. Remplacer `CARBURANT`
par `petrol` (essence) ou `diesel` ; une autre valeur garde l'essence.

```sh
mkdir -p /var/local/data/AA && printf 'model=MODELE\nyear=ANNEE\nfuel=CARBURANT\n' > /var/local/data/AA/vehicle
```

Si le journal du service dit `driver side not read` (codage illisible) ou annonce le mauvais côté, une ligne
`side=left` (volant à gauche) ou `side=right` (volant à droite) ajoutée à ce fichier l'emporte sur le codage.

Puis redémarrer l'unité :

```sh
rm -rf /tmp/entrynav2-android-auto-$V /tmp/entrynav2-android-auto-$V-armhf.tar
reboot
```

## 4. Premier lancement

Sur l'iDrive : *Media*, puis *Android Auto*, puis *Démarrer*. La première fois, appairer le téléphone à la voiture en
Bluetooth s'il ne l'est pas déjà, et accepter la voiture sur le téléphone : Android Auto rejoint ensuite le Wi-Fi de la
voiture tout seul. La case *Démarrage automatique* du menu lance Android Auto avec l'iDrive.

Android Auto peut aussi passer par un câble USB : cocher *Câble USB au lieu du sans-fil* dans le même menu, voir
[USB](USB.md).

## Mettre à jour

### Par clé USB, sans PC

Une installation dont le menu Android Auto a la ligne *Mettre à jour par clé USB* (depuis la 0.13.0-rc.10) se met
à jour depuis l'archive d'une version publiée, sans PC ni SSH. Voiture à l'arrêt, contact mis.

1. Télécharger l'archive `entrynav2-android-auto-<version>-armhf.tar` d'une
   [version publiée](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases) et la copier **telle quelle**, sans
   l'extraire, à la racine d'une clé USB (FAT32, exFAT ou NTFS). Le nom peut changer un peu (« (1) » ajouté par le
   navigateur), mais doit commencer par `entrynav2-android-auto-` et finir par `.tar`. S'il y a plusieurs archives sur
   la clé, la plus récente qui est valide est prise.
2. Brancher la clé, puis *Media*, *Android Auto*, et dans le menu *Mettre à jour par clé USB*. La ligne affiche
   *Lecture de la clé USB...* et vérifie tout sans rien installer (seul son rapport est écrit, voir plus bas) : la
   signature de l'archive, l'empreinte de chaque fichier, que chaque programme est bien fait pour cette unité (ARM,
   chargeur de la voiture, fichier entier), et, par le chargeur de l'unité comme à l'étape 2, que chaque bibliothèque
   et chaque fonction des nouveaux programmes existe sur cette voiture.
3. La ligne dit alors ce qu'un second appui installe : *Installer la mise à jour*, *Installer la version antérieure*
   (plus ancienne que celle installée) ou *Réinstaller cette version* (les fichiers de la voiture ne sont plus ceux de
   cette version). Appuyer de nouveau dans les 30 secondes : c'est exactement l'archive vérifiée qui est installée,
   même si une autre est ajoutée sur la clé entre-temps. Jamais pendant un appel ni en marche arrière : le redémarrage
   couperait l'appel ou la caméra.
4. Android Auto s'arrête s'il tourne. La ligne affiche *Installation, laisser le contact* pendant quelques secondes,
   puis *Installée : redémarrage iDrive* : l'unité redémarre d'elle-même et revient avec la nouvelle version. Les
   réglages de `/var/local/data/AA` sont gardés. Si l'installation est refusée sans rien avoir changé, Android Auto,
   arrêté pour elle, repart.

| La ligne dit | Sens |
|---|---|
| *Déjà à jour* | La voiture a exactement les fichiers de l'archive. |
| *Aucune clé USB trouvée*, *Aucune mise à jour sur la clé* | Pas de clé, ou pas d'archive `entrynav2-android-auto-…-armhf.tar` à sa racine. |
| *Mise à jour non valide* | Archive abîmée, plus ancienne que la mise à jour par clé USB, ou pas signée par le projet (voir plus bas). |
| *Mise à jour incompatible* | Un nouveau programme n'est pas fait pour cette unité, ou le chargeur de l'unité ne trouve pas une bibliothèque ou une fonction dont il a besoin : rien n'est installé. |
| *Échec : rien n'a été modifié* | Refus avant le premier fichier remplacé : manque de place, système qui refuse l'écriture, archive vérifiée retirée de la clé avant le second appui… |
| *Pas pendant un appel ni en marche AR* | Le redémarrage couperait l'appel ou la caméra : recommencer après. |
| *Terminer la mise à jour* | Une mise à jour a été coupée pendant l'échange des fichiers et peut se terminer : appuyer de nouveau, la clé n'est pas nécessaire. |
| *Incomplète : la réinstaller* | Une partie seulement est peut-être en place (coupure, installation interrompue) : refaire la mise à jour avec la clé. Android Auto reste arrêté jusque-là. |
| *Occupé : réessayer plus tard* | Android Auto ne s'est pas arrêté en 45 s, ou une autre mise à jour tourne. |
| *Installation non reconnue* | Les dossiers n'ont pas le fichier `.installed-by-entryaad` de l'étape 3 : rien n'est touché. |
| *Installée : redémarrer l'iDrive* | Installée, mais l'unité n'a pas redémarré d'elle-même au bout d'une minute : maintenir le bouton marche/arrêt de l'iDrive environ 30 secondes. |

Chaque passage écrit ce qu'il a trouvé et fait dans `EntryAA-update.txt`, à la racine de la clé quand elle accepte
l'écriture, et dans `/var/local/data/AA/update-report.txt`, que *Copier les journaux sur clé USB* copie aussi : à
joindre à un ticket.

Ce qui protège la voiture :

- Seule une archive signée par le projet est installée : la liste `UPDATE` de l'archive donne l'empreinte SHA-256 de
  chaque fichier installé, et `UPDATE.sig` la signe (RSA 3072 bits). Une archive fabriquée soi-même
  (`tools/package_release.py --unsigned`) s'installe par SSH seulement.
- Rien n'est écrit avant la fin des vérifications, refaites sur l'archive relue au second appui. Les nouveaux fichiers
  sont d'abord écrits à côté des anciens, relus depuis la mémoire flash et vérifiés ; puis chacun remplace l'ancien
  d'un coup, par un renommage : chaque fichier est toujours l'ancien ou le nouveau, jamais un mélange des deux.
- Juste avant les remplacements, la liste signée est gardée sur la partition de données. Si le courant est coupé
  pendant les remplacements, certains fichiers sont nouveaux et d'autres encore anciens : la mise à jour suivante
  termine les remplacements, avec ou sans la clé. Coupé avant, l'ancienne version reste entière.
- Le système passe en écriture au début de l'écriture des fichiers. Il ne peut revenir en lecture seule qu'une fois
  libérés les fichiers remplacés que l'iDrive garde ouverts (son appli) : il reste donc en écriture jusqu'au
  redémarrage qui suit, quelques secondes plus tard, et qui le remet en lecture seule.
- Seuls `/opt/aa` et `/opt/hmi/data/ro/Plugins/EntryAad` sont touchés. Les fichiers que la nouvelle version n'a plus y
  sont retirés, comme le fait l'étape 3.

### Par SSH

Refaire les étapes 1 à 3 avec la nouvelle archive, après avoir arrêté Android Auto depuis son menu. Les réglages de
`/var/local/data/AA` sont gardés. L'étape 3 vide `/opt/aa` avant la copie : un programme qui a changé de nom entre
deux versions (`evo-trial`, devenu `aa-supervisor` après la 0.10.0) ne reste pas sur l'unité. C'est le chemin à suivre
une fois pour passer d'une version sans la ligne *Mettre à jour par clé USB* à une version qui l'a.

Si une mise à jour par clé USB avait été coupée avant, son journal est devenu inutile : le retirer pendant l'étape 3,
`rm -f /var/local/data/AA/update-pending`.

## Désinstaller

Arrêter Android Auto depuis son menu, puis, par SSH :

```sh
mount -o remount,rw /
rm -rf /opt/hmi/data/ro/Plugins/EntryAad /opt/aa
sync
mount -o remount,ro /
rm -rf /var/local/data/AA
reboot
```

Si `/opt/hmi/data/ro/Plugins` n'existait pas avant l'installation et qu'il est vide, `rmdir /opt/hmi/data/ro/Plugins`
le retire aussi (avant `sync`). Garder `/var/local/data/AA` conserve les réglages pour une prochaine installation.

## En cas de souci

| Symptôme | Que faire |
|---|---|
| Pas de ligne Android Auto dans *Media* | Vérifier l'étape 3 (`md5sum -c`), puis redémarrer l'unité : l'iDrive ne lit ses applis qu'au démarrage. |
| L'iDrive redémarre plusieurs fois juste après l'avoir chargée | L'appli ne se lance plus au troisième essai et l'iDrive repart sans elle. Pour la réactiver après correction : `rm -f /var/local/data/AA/app.disable /var/local/data/AA/app-guard`, puis redémarrer. |
| Sans fil : le téléphone, appairé à la voiture, ne lance jamais Android Auto | Réappairer le téléphone pendant qu'Android Auto tourne (*Démarrer* d'abord). Un téléphone appairé quand le service ne tournait pas garde une liste des services de la voiture sans Android Auto ([#72](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/72)). |
| Android Auto ne démarre pas | Activer les journaux ([maintenance](MAINTENANCE.md)), reproduire, puis ouvrir un [ticket](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/new/choose) avec les journaux. |
| Le système est resté en écriture | `mount -o remount,ro /`, ou redémarrer l'unité. |
| L'écran de l'iDrive ne répond plus | Maintenir le bouton marche/arrêt de l'iDrive environ 30 secondes pour redémarrer l'unité. |
