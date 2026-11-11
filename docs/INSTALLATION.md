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
- L'archive `entry-aad-<version>-armhf.tar` d'une [version publiée](https://github.com/Jiraiya43/entry-aad/releases),
  dont l'empreinte SHA-256 correspond au fichier `.sha256` publié à côté, ou fabriquée par `tools/package_release.py`
  ([compiler](../README.fr.md#compiler)).

Le numéro de version ne s'écrit que deux fois : dans la commande `scp` de l'étape 1 et sur la ligne `V=` de l'étape 2.
Y mettre celui de l'archive téléchargée. `169.254.1.0` est l'adresse de la voiture de référence sur le câble ENET : la
remplacer par celle de l'unité si elle diffère.

Cette page décrit la version en cours. Chaque archive contient sa propre `INSTALLATION.md` : pour une version plus
ancienne, suivre celle de l'archive (dans la 0.10.0, le superviseur s'appelle `evo-trial` et non `aa-supervisor`).

## 1. Copier l'archive dans la voiture

Depuis le PC, dans le dossier de l'archive :

```sh
scp entry-aad-0.12.0-rc.2-armhf.tar root@169.254.1.0:/tmp/
ssh root@169.254.1.0
```

Toutes les commandes suivantes se tapent dans cette session SSH, sur la voiture. `/tmp` est en mémoire : rien n'est
encore écrit sur la mémoire flash.

## 2. Contrôler la voiture

Rien n'est modifié pendant cette étape.

```sh
V=0.12.0-rc.2
cd /tmp && rm -rf entry-aad-$V && tar -xf entry-aad-$V-armhf.tar && cd entry-aad-$V
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

Ce résultat, avec la première ligne de `/etc/version`, fait un bon
[rapport de compatibilité](https://github.com/Jiraiya43/entry-aad/issues/new/choose) pour une voiture pas encore listée.

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
cd / && md5sum -c /tmp/entry-aad-$V/MD5SUMS
```

- `rm -f` avant la copie : l'iDrive garde l'ancienne appli en mémoire jusqu'à son redémarrage, et un fichier effacé puis
  recopié n'est jamais réécrit sous ses pieds.
- Le fichier `.installed-by-entryaad` marque chaque dossier comme ajouté par ce projet.
- Le dernier `md5sum -c` relit les fichiers installés : chacun doit répondre `OK`.
- Si `mount -o remount,ro /` échoue, le redémarrage qui suit remet le système en lecture seule.

Facultatif, le modèle et l'année que le téléphone affiche pour la voiture. Sans ce fichier, il affiche une BMW
« iDrive » de 2019 : l'unité ne connaît pas le nom du modèle.

```sh
mkdir -p /var/local/data/AA && printf 'model=320d\nyear=2020\n' > /var/local/data/AA/vehicle
```

Puis redémarrer l'unité :

```sh
rm -rf /tmp/entry-aad-$V /tmp/entry-aad-$V-armhf.tar
reboot
```

## 4. Premier lancement

Sur l'iDrive : *Media*, puis *Android Auto*, puis *Démarrer*. La première fois, appairer le téléphone à la voiture en
Bluetooth s'il ne l'est pas déjà, et accepter la voiture sur le téléphone : Android Auto rejoint ensuite le Wi-Fi de la
voiture tout seul. La case *Démarrage automatique* du menu lance Android Auto avec l'iDrive.

Android Auto peut aussi passer par un câble USB : cocher *Câble USB au lieu du sans-fil* dans le même menu, voir
[USB](USB.md).

## Mettre à jour

Refaire les étapes 1 à 3 avec la nouvelle archive, après avoir arrêté Android Auto depuis son menu. Les réglages de
`/var/local/data/AA` sont gardés. L'étape 3 vide `/opt/aa` avant la copie : un programme qui a changé de nom entre
deux versions (`evo-trial`, devenu `aa-supervisor` après la 0.10.0) ne reste pas sur l'unité.

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
| Android Auto ne démarre pas | Activer les journaux ([maintenance](MAINTENANCE.md)), reproduire, puis ouvrir un [ticket](https://github.com/Jiraiya43/entry-aad/issues/new/choose) avec les journaux. |
| Le système est resté en écriture | `mount -o remount,ro /`, ou redémarrer l'unité. |
| L'écran de l'iDrive ne répond plus | Maintenir le bouton marche/arrêt de l'iDrive environ 30 secondes pour redémarrer l'unité. |
