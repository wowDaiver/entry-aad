# Maintenance et réglages

Tout se règle par de petits fichiers dans `/var/local/data/AA`, sur la partition de données : rien à remonter en écriture.
Un changement vaut au prochain démarrage d'Android Auto (menu *Arrêter* puis *Démarrer*). Les commandes se tapent dans une
session SSH sur l'unité ([installation](INSTALLATION.md#1-copier-larchive-dans-la-voiture)).

## Réglages

Le service ne lit aucun réglage dans son environnement : ses valeurs sont fixes, dans le code.

| Fichier | Effet |
|---|---|
| `vehicle` | Modèle et année annoncés au téléphone : `model=…` et `year=…`, une ligne chacun. Sans ce fichier : « iDrive », 2019 |
| `usb` | Présent : Android Auto par câble USB au lieu du sans-fil (case *Câble USB au lieu du sans-fil* du menu). Vide, ou le nom de la seule prise à surveiller : voir [USB](USB.md) |
| `autostart` | Présent : Android Auto démarre avec l'iDrive (case *Démarrage automatique* du menu) |
| `entry-direct` | Présent : la ligne *Android Auto* de l'iDrive ouvre directement Android Auto, au lieu de son menu |
| `kombi-off` | Présent : rien n'est envoyé au compteur ni à la tête haute |
| `gps-telephone` | Présent : le téléphone garde son propre GPS, celui de la voiture n'est pas transmis |
| `maintenance` | Présent : l'appli et le service gardent leurs journaux |

Exemple, couper le guidage sur le compteur, puis le rétablir :

```sh
touch /var/local/data/AA/kombi-off
rm /var/local/data/AA/kombi-off
```

## Journaux

Sans le fichier `maintenance`, rien n'est journalisé, ni sur la mémoire flash ni en mémoire vive. Pour chercher une
panne : activer les journaux, reproduire, les récupérer, puis les couper.

```sh
touch /var/local/data/AA/maintenance
```

| Journal | Emplacement | Taille |
|---|---|---|
| Appli de l'iDrive | `/var/local/data/AA/app.log`, et `app.old.log` après rotation | 512 Ko au plus |
| Service Android Auto | `/tmp/entry-aa-service.log` | en mémoire, perdu au redémarrage |

Depuis le PC :

```sh
scp root@169.254.1.0:/var/local/data/AA/app.log root@169.254.1.0:/tmp/entry-aa-service.log .
```

### Qualité du Wi-Fi

Journaux activés, le service écrit une ligne `[WifiLink]` quand le téléphone se connecte en sans-fil, puis une toutes
les 10 secondes jusqu'à la fin de la session :

```text
[WifiLink] 5180 MHz; signal -52 dBm (average -51); to the phone 150.0 Mbit/s; from the phone 135.0 Mbit/s;
in 10 s: 812 frames sent, 35 retried, 0 lost; on the Wi-Fi for 135 s; TCP round trip 4.2 ms, 0 segment(s) sent again
```

| Partie | Sens |
|---|---|
| `5180 MHz` | Fréquence du Wi-Fi de la voiture : vers 2400, la bande 2,4 GHz, partagée avec le Bluetooth ; vers 5000, la bande 5 GHz |
| `signal` | Niveau reçu du téléphone, et sa moyenne. Plus le nombre est bas, plus la liaison est fragile (-50 : bon, -80 : faible) |
| `to the phone`, `from the phone` | Débit radio choisi dans chaque sens |
| `frames sent, retried, lost` | Trames envoyées au téléphone depuis la ligne précédente, réémises, puis abandonnées |
| `on the Wi-Fi for` | Temps depuis que le téléphone a rejoint le Wi-Fi. S'il repart de zéro, le téléphone l'a quitté puis rejoint |
| `TCP round trip`, `sent again` | Temps d'aller-retour de la connexion Android Auto, et morceaux qu'il a fallu renvoyer |

Une partie que le Wi-Fi de la voiture ne donne pas est absente de la ligne. Ces chiffres sont lus dans le noyau, comme
le fait la commande `iw` : rien n'est réglé ni envoyé, et l'adresse du téléphone n'est pas écrite. Par câble USB, il n'y a
pas de ligne `[WifiLink]`.

### Effacer les journaux

Pour revenir à zéro journal :

```sh
rm -f /var/local/data/AA/maintenance /var/local/data/AA/app.log /var/local/data/AA/app.old.log /tmp/entry-aa-service.log
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
