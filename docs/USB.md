# Android Auto par câble USB

**Expérimental.** Android Auto reste sans fil par défaut. Le câble est une solution pour un téléphone qui ne sait pas
faire d'Android Auto sans fil, ou quand le Wi-Fi de la voiture rame. L'état des essais est dans le
[journal des versions](../CHANGELOG.md).

Avec le câble, l'image, les commandes, le micro, le GPS et le guidage passent par l'USB. **Le son reste sur le
Bluetooth de la voiture**, comme en sans fil : le téléphone doit donc rester appairé à la voiture.

## Choisir le câble ou le sans-fil

Dans le menu Android Auto de l'iDrive, cocher **Câble USB au lieu du sans-fil**, puis brancher le téléphone sur une
prise USB de la voiture avec un câble de données. La première fois, le téléphone demande d'autoriser Android Auto.
Décocher la case ramène au sans-fil.

- Si Android Auto tourne quand la case change, il s'arrête puis redémarre tout seul dans le nouveau mode, en une
  dizaine de secondes.
- **Rien n'est deviné :** brancher ou débrancher un câble ne change jamais de mode. Un téléphone mis en charge par un
  passager ne prend donc pas la place du vôtre tant que la case n'est pas cochée.
- En mode USB, le service ne touche ni au Bluetooth ni au Wi-Fi de la voiture.

La case pose ou retire le fichier `/var/local/data/AA/usb`. Par SSH sur l'unité
([installation](INSTALLATION.md#1-copier-larchive-dans-la-voiture)), Android Auto arrêté depuis son menu, c'est la même
chose que :

```sh
touch /var/local/data/AA/usb   # câble USB
rm /var/local/data/AA/usb      # sans-fil
```

Le fichier est lu à chaque démarrage d'Android Auto et vaut jusqu'à son arrêt.

## Ce que fait le service

1. Il lit dans le système (`/sys/bus/usb/devices`) ce qui est branché, sans rien envoyer.
2. Il ne s'adresse qu'à un appareil qui ressemble à un téléphone Android (transfert de photos ou interface du
   constructeur). Les clés USB, les claviers, les concentrateurs, les appareils Apple et ce qui est soudé dans l'unité
   ne reçoivent jamais rien.
3. Il lui demande de passer en mode accessoire, le mode qu'Android Auto utilise sur un câble
   ([protocole AOA](https://source.android.com/docs/core/interaction/accessories/aoa) : requêtes 51, 52 et 53). Un
   appareil qui ne répond pas à la première n'est plus sollicité.
4. Le téléphone revient en accessoire sur la même prise : le service prend son interface et y fait passer la session
   Android Auto, la même qu'en sans fil.

Le service ne prend jamais une interface USB à un pilote ou à un programme BMW. Quand le téléphone est en mode
accessoire sans que le service puisse s'en servir (resté ainsi après un *Arrêter*, ou tenu par un programme de la
voiture), il réinitialise le téléphone, lui seul : cela revient à le débrancher et le rebrancher. Il le sollicite
ensuite à nouveau, trois fois au plus, puis le laisse tranquille jusqu'au prochain branchement. À l'*Arrêter*, le
téléphone est sorti du mode accessoire de la même façon.

## Limiter à une prise

Si un autre appareil de la voiture devait être sollicité à tort, écrire le nom de la seule prise à surveiller dans le
fichier. Le nom se lit téléphone branché :

```sh
grep -l . /sys/bus/usb/devices/*/product 2>/dev/null   # une ligne par appareil : .../3-1/product
cat /sys/bus/usb/devices/3-1/product                    # le nom du téléphone
echo 3-1 > /var/local/data/AA/usb
```

Un fichier qui contient autre chose qu'un nom de prise empêche le démarrage : le service ne devine pas. Décocher la
case du menu retire le fichier, nom de prise compris : il est à réécrire ensuite.

## En cas de souci

| Symptôme | Que faire |
|---|---|
| Rien ne se passe au branchement | Câble de données et non de charge seule. Sur le téléphone, choisir « Transfert de fichiers » dans la notification USB, puis rebrancher. |
| Le téléphone affiche Android Auto mais pas l'iDrive | Débrancher, attendre 10 secondes, rebrancher. |
| Pas de son | Le son passe par le Bluetooth : vérifier que le téléphone est connecté à la voiture. |
| Toujours rien | Activer les journaux ([maintenance](MAINTENANCE.md)), reproduire, et joindre `/tmp/entry-aa-service.log` à un ticket : les lignes `[UsbLoop]` et `[UsbBridge]` disent ce qui a été vu sur les prises. |
