# Maintenance et réglages

Tout se règle par de petits fichiers dans `/var/local/data/AA`, sur la partition de données : rien à remonter en écriture.
Un changement vaut au prochain démarrage d'Android Auto (menu *Arrêter* puis *Démarrer*). Les commandes se tapent dans une
session SSH sur l'unité ([installation](INSTALLATION.md#1-copier-larchive-dans-la-voiture)).

## Réglages

Le service ne lit aucun réglage dans son environnement : ses valeurs sont fixes, dans le code.

| Fichier | Effet |
|---|---|
| `vehicle` | La voiture annoncée au téléphone, une ligne par réglage : `model=…`, `year=…`, `side=left` ou `side=right` (côté du conducteur : Android Auto dispose son écran pour ce côté), `fuel=petrol` ou `fuel=diesel`. Sans ce fichier ou sans une ligne : la série codée dans la voiture (« G20 », « F56 » ; « iDrive » si le codage ne la dit pas), 2019, essence, et le côté codé dans la voiture (conduite à gauche s'il est illisible) ; une ligne `side=` l'emporte sur le codage. La marque (BMW ou MINI) vient du codage de l'iDrive |
| `usb` | Présent : Android Auto par câble USB au lieu du sans-fil (case *Câble USB au lieu du sans-fil* du menu). Vide, ou le nom de la seule prise à surveiller : voir [USB](USB.md) |
| `autostart` | Présent : Android Auto démarre avec l'iDrive (case *Démarrage automatique* du menu) |
| `entry-direct` | Présent : la ligne *Android Auto* de l'iDrive ouvre directement Android Auto, au lieu de son menu |
| `kombi-off` | Présent : rien n'est envoyé au compteur ni à la tête haute (Ethernet des Série G ou MOST des Série F) |
| `kombi-single-maneuver` | Présent au démarrage : graphe limité à la prochaine manœuvre, pour un compteur qui refuse le trajet complet (voir ci-dessous). Sans ce fichier : graphe historique |
| `gps-telephone` | Présent : le téléphone garde son propre GPS, celui de la voiture n'est pas transmis |
| `maintenance` | Présent : l'appli et le service gardent leurs journaux (case *Garder les journaux* du menu, voir [Journaux](#journaux)) |
| `update-report.txt` | Écrit par la mise à jour par clé USB ([installation](INSTALLATION.md#par-clé-usb-sans-pc)) : ce que sa dernière vérification ou installation a trouvé et fait. Copié avec les journaux |
| `update-pending` | Présent pendant les quelques secondes où une mise à jour par clé USB met ses fichiers en place, ou après une coupure à ce moment-là : la mise à jour suivante le lit pour terminer. Ne pas y toucher, sauf après une réinstallation par SSH, qui le rend inutile |

Exemple, couper le guidage sur le compteur, puis le rétablir :

```sh
touch /var/local/data/AA/kombi-off
rm /var/local/data/AA/kombi-off
```

### Graphe limité à la prochaine manœuvre

Si le compteur refuse le graphe à plusieurs routes avec `0x26`, arrêter Android Auto dans son menu,
puis sélectionner le graphe limité à la prochaine manœuvre :

```sh
mkdir -p /var/local/data/AA && touch /var/local/data/AA/kombi-single-maneuver
```

Redémarrer Android Auto depuis son menu pour appliquer ce choix. Pour revenir au graphe historique,
arrêter Android Auto, retirer le fichier, puis redémarrer Android Auto :

```sh
rm -f /var/local/data/AA/kombi-single-maneuver
```

Aucun redémarrage de l'iDrive ni changement de codage n'est nécessaire. Résultat et limites :
[compatibilité](COMPATIBILITE.md#flèches-sur-le-compteur-g01--logiciel-146).

## Revenir à Android Auto avec MEDIA

Quand le téléphone est connecté et que l'iDrive est à l'écran, appuyer deux fois brièvement sur MEDIA : chaque
appui dure au plus 500 ms, et les deux relâchements sont espacés de 500 ms au plus. Aucun fichier de réglage à créer.
Un appui simple conserve son action BMW ; lorsque l'image Android Auto est visible, MEDIA garde son action habituelle.

Le raccourci ne lance aucune session. Il est ignoré pendant un appel, en marche arrière, pendant l'arrêt ou si la
session s'est terminée. Les maintiens, le simple toucher et les copies répétées d'un même état ne sont pas deux appuis.
Le service transmet une demande à usage unique à l'appli de l'iDrive ; elle vérifie l'état à nouveau avant de montrer
l'image. Une demande vieille de plus de deux secondes ou issue d'un autre processus est supprimée.

Le journal du service, en profil maintenance, écrit `double MEDIA: guarded iDrive recall request written` ;
l'appli indique `double MEDIA` dans son journal quand elle traite la demande. Le geste est testé sur PC et sous QEMU,
pas encore en voiture. NAV et le bouton natif CarLife ne font pas partie de ce raccourci.

## Journaux

Sans le fichier `maintenance`, rien n'est journalisé, ni sur la mémoire flash ni en mémoire vive. Pour chercher une
panne : activer les journaux, reproduire, les récupérer, puis les couper.

### Sans PC, depuis le menu Android Auto de l'iDrive

1. Cocher *Garder les journaux*, qui crée le fichier `maintenance`. Le journal de l'appli commence aussitôt, celui du
   service au prochain *Démarrer* (ou au prochain démarrage de l'iDrive).
2. Reproduire la panne. Les journaux restent après le sommeil de la voiture.
3. Brancher une clé USB (FAT32, exFAT ou NTFS), puis choisir *Copier les journaux sur clé USB*. La ligne dit ce qui
   s'est passé : *Copie des journaux...*, puis *Journaux copiés sur la clé USB*, *Aucune clé USB trouvée*, *Aucun
   journal : cochez Garder* ou *Copie impossible* (clé pleine, protégée ou retirée). Les journaux sont dans le dossier
   `EntryAA-logs/<date>` de la clé, avec la version du logiciel de l'unité, le rapport de la dernière mise à jour par
   clé USB s'il y en a eu une ([installation](INSTALLATION.md#par-clé-usb-sans-pc)) et un `info.txt` qui les liste.
   Rien n'est effacé ni changé sur l'unité.
4. Décocher *Garder les journaux*. Les journaux déjà écrits restent sur l'unité, pour une copie plus tard.

Les journaux ne contiennent ni mot de passe ni adresse Wi-Fi ou Bluetooth ; ils peuvent contenir le nom Bluetooth du
téléphone.

### En SSH

```sh
touch /var/local/data/AA/maintenance
```

| Journal | Emplacement | Taille |
|---|---|---|
| Appli de l'iDrive | `/var/local/data/AA/app.log`, et `app.old.log` après rotation | 512 Ko au plus chacun |
| Service Android Auto | `/var/local/data/AA/service.log`, et `service.old.log` ; `/tmp/entry-aa-service.log` est un lien vers lui | 512 Ko au plus chacun, gardé après le sommeil de la voiture |

Le journal du service est sur la partition de données, comme celui de l'appli : le superviseur le surveille toutes les
5 s, et au-delà de 512 Ko il le recopie dans `service.old.log` puis le vide. Si la partition refuse de l'ouvrir, il
va en mémoire vive dans `/tmp/entry-aa-service.log`, comme avant (perdu au sommeil de la voiture).

Depuis le PC :

```sh
scp 'root@169.254.1.0:/var/local/data/AA/*.log' .
```

### Qualité du Wi-Fi

Journaux activés, le service écrit une ligne `[WifiLink]` quand le téléphone se connecte en sans-fil, puis une toutes
les 10 secondes jusqu'à la fin de la session :

```text
[WifiLink] 5180 MHz; signal -52 dBm (average -51); from the phone 135.0 Mbit/s;
in 10 s: 812 frames sent, 35 retried, 0 lost; on the Wi-Fi for 135 s; TCP round trip 4.2 ms, 0 segment(s) sent again;
BMW Apps Bluetooth link idle
```

| Partie | Sens |
|---|---|
| `5180 MHz` | Fréquence du Wi-Fi de la voiture : vers 2400, la bande 2,4 GHz, partagée avec le Bluetooth ; vers 5000, la bande 5 GHz |
| `signal` | Niveau reçu du téléphone, et sa moyenne. Plus le nombre est bas, plus la liaison est fragile (-50 : bon, -80 : faible). Quand la puce n'a pas mesuré la dernière trame, seule la moyenne est écrite (`average signal`) |
| `from the phone` | Débit radio choisi par le téléphone. Le débit vers le téléphone n'est pas écrit : la puce Wi-Fi le choisit seule et le pilote ne le connaît pas |
| `frames sent, retried, lost` | Trames envoyées au téléphone depuis la ligne précédente, réémises, puis abandonnées |
| `on the Wi-Fi for` | Temps depuis que le téléphone a rejoint le Wi-Fi. S'il repart de zéro, le téléphone l'a quitté puis rejoint |
| `TCP round trip`, `sent again` | Temps d'aller-retour de la connexion Android Auto, et morceaux qu'il a fallu renvoyer |
| `BMW Apps Bluetooth link` | Lien Bluetooth des BMW Apps (My BMW, AAIdrive, option *Apps* du téléphone dans les réglages de la voiture) : `connected` quand un téléphone le tient, `idle` sinon ; un autre état est écrit en chiffres (`slot state`, `link state`). Il passe par la même puce que le Wi-Fi |

Une partie que le Wi-Fi de la voiture ne donne pas est absente de la ligne. Ces chiffres sont lus dans le noyau, comme
le fait la commande `iw` : rien n'est réglé ni envoyé, et l'adresse du téléphone n'est pas écrite. L'état du lien BMW
Apps est lu dans la mémoire de la pile Bluetooth BMW, comme au démarrage, sans rien y changer ; il manque quand Android
Auto occupe lui-même l'emplacement des BMW Apps (`takes SPP 0` dans le journal du superviseur). Par câble USB, il n'y a
pas de ligne `[WifiLink]`.

Avant chaque échange Wi-Fi avec le téléphone, le service demande au point d'accès de la voiture de ne plus proposer
U-APSD, une économie d'énergie du téléphone, et écrit une ligne `car access point:` (journal du service, toujours) :

| Fin de la ligne | Sens |
|---|---|
| `beacon rebuilt` | Fait : le téléphone rejoint le Wi-Fi sans U-APSD |
| `beacon already rebuilt for this hostapd start` | Déjà fait depuis le dernier démarrage du point d'accès : le réglage seul, sans reconstruire la balise une seconde fois |
| `a station is associated` | Un appareil était déjà sur le Wi-Fi de la voiture (téléphone revenu, iPhone en CarPlay) : personne n'est déconnecté, et cet appareil garde U-APSD jusqu'à sa prochaine connexion |
| `RELOAD refused` | hostapd a pris le réglage mais pas la reconstruction de sa balise : le téléphone peut encore demander U-APSD |
| `left on` | hostapd a refusé le réglage ou n'a pas répondu : rien n'a changé |

Le réglage est fait dans la mémoire de hostapd, jamais dans un fichier de la voiture : un redémarrage de l'unité, ou
de hostapd par le système, remet celui de BMW.

### Silences du téléphone et espacement des images

Journaux activés, le service écrit aussi une ligne `[Session]` toutes les 10 secondes d'une session, sans fil ou USB :

```text
[Session] in 10 s: the phone's longest silence 120 ms; video: 298 frames, 41 within 25 ms of the previous one,
38 held (longest 23 ms)
```

| Partie | Sens |
|---|---|
| `longest silence` | Plus long moment sans rien recevoir du téléphone. Plusieurs secondes : la liaison s'est bloquée |
| `frames` | Images reçues du téléphone |
| `within 25 ms of the previous one` | Images arrivées moins de 25 ms après la précédente : des images par paires, dont l'écran ne dessine que la seconde |
| `held`, `longest` | Sans fil seulement : images retenues jusqu'à 25 ms après la précédente, et la plus longue attente. Toujours 0 en USB |

### Effacer les journaux

Pour revenir à zéro journal :

```sh
cd /var/local/data/AA && rm -f maintenance app.log app.old.log service.log service.old.log /tmp/entry-aa-service.log
```

## Garde-fou

Si l'iDrive redémarre trois fois de suite moins de 2 minutes après avoir chargé l'appli, celle-ci ne se lance plus et
l'iDrive repart sans elle : le fichier `/var/local/data/AA/app-guard` compte ces démarrages, et s'efface tout seul après
2 minutes sans incident. Le fichier `app.disable`, posé à la main, garde aussi l'appli éteinte. Pour la réactiver, après
avoir trouvé la cause :

```sh
rm -f /var/local/data/AA/app.disable /var/local/data/AA/app-guard
reboot
```

## État de l'installation

```sh
ls -la /opt/aa /opt/hmi/data/ro/Plugins/EntryAad /var/local/data/AA
grep -c libEntryAad /proc/$(pidof HmiMain3D)/maps   # plus de 0 : l'iDrive a chargé l'appli
pidof aa-supervisor entry-aad                           # Android Auto en marche
```
