# Feuille de route

Un point n'est coché qu'avec une preuve en voiture : un test sur PC ne suffit pas.

## 0.10 : première version publique

- [x] Contrôle de compatibilité sans rien modifier (`aa-supervisor --compat`)
- [x] Procédure d'installation manuelle, répétée avec la BusyBox de la voiture
- [ ] Procédure suivie de bout en bout sur la voiture de référence
- [x] Essai de la 0.10.0 en profil maintenance sur la voiture de référence (10 novembre 2026)
  - [ ] Le contrôle répond « Compatible », démarrage à blanc compris
  - [x] Android Auto venu tout seul (démarrage automatique, téléphone reconnecté) reste à l'écran, molette sans effet
    sur l'iDrive
  - [ ] Media, Android Auto, Ouvrir : l'iDrive reste sur sa page vide, y compris après un mode de conduite puis RETOUR
  - [ ] Volume : pastille avec le niveau, Android Auto opaque
  - [ ] Wi-Fi coupé : Android Auto se ferme en 5 secondes environ
  - [ ] Arrêter et Démarrer
  - [ ] Molette (un cran par pas), touches, tactile, modes de conduite, caméra, Assistant
  - [ ] GPS de la voiture dans Waze, flèches sur la tête haute
- [ ] Sans journaux (fichier `maintenance` absent) : tout marche pareil

## 0.12 : câble USB

- [x] Android Auto démarre par câble USB sur la voiture de référence, le sans-fil marche toujours (11 novembre 2026)
- [ ] Trajet de 20 à 30 minutes par câble, musique en Bluetooth
- [ ] Câble débranché puis rebranché en cours de route
- [ ] *Arrêter* puis *Démarrer* sans débrancher, puis retour au sans-fil
- [ ] Câble essayé sur une autre voiture

## 1.0 : une autre voiture

- [x] Rapport de compatibilité d'une deuxième voiture, sans rien installer : X3 G01 de 2020
  ([#17](https://github.com/Jiraiya43/entry-aad/issues/17))
- [ ] Première installation sur une autre voiture, puis désinstallation vérifiée
- [ ] Tableau des logiciels essayés rempli ([compatibilité](docs/COMPATIBILITE.md))
- [ ] Ancienne molette (contrôleurs C10/C20) essayée sur une vraie voiture

## Limites connues

| Limite | Cause | Piste |
|---|---|---|
| La touche MAP ouvre la carte BMW, pas la navigation d'Android Auto | choix actuel : MAP rend l'écran à l'iDrive | à décider |
| Le son passe par le Bluetooth de la voiture : qualité limitée, débuts de messages vocaux coupés | pas de son par le Wi-Fi | son par le Wi-Fi |
| Aide au stationnement ouverte sans marche arrière : Android Auto reste en retrait jusqu'à « Ouvrir » | l'écran affiché par l'iDrive n'est pas connu | lire l'état du PDC |
| Pendant un appel, la molette agit aussi sur l'écran d'appel de l'iDrive | idem | lire l'écran affiché par l'iDrive |
| Tête haute : la flèche ne revient pas après l'affichage d'une limitation de vitesse, et reste masquée au-delà de 1,6 km | comportement du compteur | à étudier |
| Le texte de sortie de rond-point du compteur est en français | texte fixe | suivre la langue de l'iDrive |
| Une seule identité de voiture pour tous : un téléphone utilisé dans deux voitures équipées les confond | `vehicleId` fixe | identifiant propre à chaque installation |

## Après 1.0

| Sujet | État |
|---|---|
| Son par le Wi-Fi au lieu du Bluetooth | en pause |
| Écran affiché par l'iDrive lu directement (alertes, PDC, appels) | à étudier |
| Boussole envoyée au téléphone (carte orientée à l'arrêt) | idée |
| Touche favori du tableau de bord qui ouvre Android Auto | idée |
| Pochette de l'album sur l'iDrive | idée |
| Autres tailles d'écran | il manque le rapport d'une voiture concernée |
| Autre structure de la bibliothèque Bluetooth | refusée pour l'instant, il manque le rapport et la bibliothèque |
| Message à l'écran si Android Auto ne démarre pas en production | idée |
| Mise à jour sans refaire tout le parcours | idée |

## Hors périmètre

- NBT Evo, ID7 et MGU : autres systèmes, ce programme ne peut pas y tourner.
- Marques autres que BMW.
