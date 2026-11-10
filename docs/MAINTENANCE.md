# Maintenance et réglages

Tout se règle par de petits fichiers dans `/var/local/data/AA`, sur la partition de données : rien à remonter en écriture.
Un changement vaut au prochain démarrage d'Android Auto (menu *Arrêter* puis *Démarrer*). Les commandes se tapent dans une
session SSH sur l'unité ([installation](INSTALLATION.md#1-copier-larchive-dans-la-voiture)).

## Réglages

Le service ne lit aucun réglage dans son environnement : ses valeurs sont fixes, dans le code.

| Fichier | Effet |
|---|---|
| `usb-phone` | Sélection USB ou automatique au démarrage ; absent : sans fil. Format et limites : [USB](USB.md) |
| `vehicle` | Modèle et année annoncés au téléphone : `model=…` et `year=…`, une ligne chacun. Sans ce fichier : « iDrive », 2019 |
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
