# Android Auto par USB

Contribution expérimentale : elle ajoute AOA au service existant et conserve le mode sans fil par défaut.
Les anciennes archives publiées ne contiennent pas ce transport. Il faut compiler les programmes de cette branche ;
aucune bibliothèque ni aucun fichier BMW ne doit être ajouté au dépôt ou remplacé dans la voiture.

## Choisir au démarrage

Le superviseur lit `/var/local/data/AA/usb-phone` à chaque Démarrer, y compris au démarrage automatique de l'iDrive.
Ce fichier régulier doit appartenir à root et être inaccessible en écriture au groupe et aux autres (mode `600`).
Il contient une seule ligne, sans commentaire :

| Contenu | Comportement |
|---|---|
| Fichier absent | Sans fil, comme avant |
| `auto PORT VID PID` | Téléphone présent au port choisi : USB ; absent ou autre périphérique : sans fil |
| `PORT VID PID` | USB seulement, attente du téléphone choisi |

`PORT` est le nom physique sous `/sys/bus/usb/devices`, par exemple `3-1`. `VID` et `PID` sont les identifiants
hexadécimaux du téléphone **avant AOA**, lus dans `idVendor` et `idProduct` sur ce port. Ils peuvent changer avec le
mode USB du téléphone : ne pas recopier les valeurs d'une autre installation. Ne jamais choisir un hub ou un
périphérique interne BMW. Un câble de données et l'autorisation Android Auto du téléphone sont nécessaires.

Une fois ces valeurs identifiées et Android Auto arrêté, créer ce fichier dans le dossier de réglages existant,
puis appliquer `chmod 600 /var/local/data/AA/usb-phone`. Retirer ce seul fichier remet le prochain démarrage en mode
sans fil. Aucun changement ne demande de remonter le système en écriture.

Le choix est fixé pour toute la durée du service : brancher ou retirer un câble ne transforme pas une session sans
fil en session USB. Utiliser Arrêter, brancher ou débrancher, puis Démarrer. Un fichier mal formé ou un état USB
illisible bloque le démarrage ; cela n'est pas interprété comme une absence du téléphone. Un échec de connexion USB
ne déclenche pas de repli silencieux vers le sans-fil.

## Ce que le mode USB change

La projection TLS passe par AOA (requêtes de contrôle 51, 52 et 53, puis transferts bulk), au lieu de TCP. Le service
vérifie le téléphone avant la négociation, sa réénumération sur le même port, les descripteurs et la paire bulk de
l'interface 0, alternate 0. L'interface ADB n'est pas utilisée. La libusb `libusb-1.0.so.0` installée sur l'unité est
chargée dynamiquement ; son absence fait échouer la connexion USB, sans empêcher le transport sans fil.

Image, commandes, GPS, micro et guidage gardent les mêmes canaux Android Auto. Le son reste sur le Bluetooth BMW.
Le mode USB ne demande ni point d'accès BMW, ni annonce Bluetooth WPP, ni serveur TCP. Le point d'accès du téléphone
n'est pas modifié ; il n'est pas nécessaire à la projection USB.

Le mode sans fil conserve son démarrage et sa restauration Bluetooth habituels. Ni le superviseur ni le service ne
réinitialisent un périphérique USB, ne détachent un pilote, ne changent sa configuration et n'arrêtent de service BMW.

## Limite de possession de l'interface

Le processus BMW `vmostAuxInUSB` peut réclamer l'interface AOA, notamment après Arrêter. Si elle est occupée, la
connexion est refusée ; aucun propriétaire n'est délogé. Débrancher puis rebrancher le téléphone **avant** Démarrer
peut être nécessaire. Les tentatives pour une même adresse USB sont bornées. Le choix automatique autorise un
accessoire AOA déjà présent uniquement sur le port explicitement configuré, sans garantie que l'interface soit libre.

## Développement et preuve

`make check` inclut les tests simulés USB et de sélection. `make test-arm-all` construit leurs variantes ARM et
`tests/run_qemu.py` les exécute avec les autres tests. Le même relevé `session_phone.golden` est comparé sur TCP et
USB, sans régénération. Ces tests ne remplacent ni une bibliothèque libusb réelle ni un essai en voiture.

Les essais courts et leurs limites sont décrits dans [CHANGELOG.md](../CHANGELOG.md#non-publié). Ne pas en déduire
une validation de toutes les fonctions ou de toutes les unités EntryNav2.
