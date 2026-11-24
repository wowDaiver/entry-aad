#!/usr/bin/env python3
# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the iDrive app's text and image databases.

EntryAad_ALL_Texts.zip holds <lang>.txt ("id=text" lines, same format as BMW's PluginsCoded apps) and
EntryAad_ALL_Images.zip holds <imageId>.png, from hmiapp/res. Both are reproducible: same input, same bytes. The Help
page's title carries the project's version (VERSION), so a photo of it tells which version a car runs.

Usage: make_db.py <res dir> <output dir>
"""

import struct
import sys
import zipfile
import zlib
from pathlib import Path

LANGS = {
    "de": "Deutsch",
    "en-UK": "English (UK)",
    "en-US": "English (US)",
    "es-ES": "Espanol",
    "es-US": "Espanol (US)",
    "fr-FR": "Francais",
    "fr-CA": "Francais (CA)",
    "it": "Italiano",
    "pt": "Portugues",
    "ru": "Russian",
}
TEXTS = {
    1: "Android Auto",
    2: "Android Auto: Connecting...",
    10: "Start Android Auto",
    11: "Stop Android Auto",
    13: "Connecting to the phone...",
    14: "Open Android Auto",
    15: "Start automatically",
    16: "Help",
    17: "USB cable instead of wireless",
    18: "Keep logs",
    40: "Copy logs to USB stick",
    41: "Copying logs...",
    42: "Logs copied to the USB stick",
    43: "No USB stick found",
    44: "No logs: tick Keep logs",
    45: "Copy failed",
    46: "Update from USB stick",
    47: "Reading the USB stick...",
    48: "Install the update",
    49: "Install the older version",
    50: "Reinstall this version",
    51: "Finish the update",
    52: "Already up to date",
    53: "No update on the USB stick",
    54: "Update not valid",
    55: "Update not for this car",
    56: "Stopping Android Auto...",
    57: "Installing: keep ignition on",
    58: "Installed: iDrive restarting",
    59: "Installed: restart the iDrive",
    60: "Update failed: nothing changed",
    61: "Incomplete: install it again",
    62: "Busy: try again shortly",
    63: "Installation not recognised",
    64: "Not during a call or in reverse",
    20: "NAV, MAP: Android Auto navigation",
    21: "MEDIA: Android Auto media",
    22: "COM: Android Auto phone",
    23: "MENU: iDrive - BACK: back",
    24: "Phone paired, sound over Bluetooth",
    26: "Free: github.com/Jiraiya43",
    30: " ",
}  # blank title of the page held under Android Auto (no "Android Auto" tab during the volume)
LANG_TEXTS = {  # ASCII only
    "fr-FR": {
        2: "Android Auto : connexion...",
        10: "Demarrer Android Auto",
        11: "Arreter Android Auto",
        13: "Connexion au telephone...",
        14: "Ouvrir Android Auto",
        15: "Demarrage automatique",
        16: "Aide",
        17: "Cable USB au lieu du sans-fil",
        18: "Garder les journaux",
        40: "Copier les journaux sur cle USB",
        41: "Copie des journaux...",
        42: "Journaux copies sur la cle USB",
        43: "Aucune cle USB trouvee",
        44: "Aucun journal : cochez Garder",
        45: "Copie impossible",
        46: "Mettre a jour par cle USB",
        47: "Lecture de la cle USB...",
        48: "Installer la mise a jour",
        49: "Installer la version anterieure",
        50: "Reinstaller cette version",
        51: "Terminer la mise a jour",
        52: "Deja a jour",
        53: "Aucune mise a jour sur la cle",
        54: "Mise a jour non valide",
        55: "Mise a jour incompatible",
        56: "Arret d'Android Auto...",
        57: "Installation, laisser le contact",
        58: "Installee : redemarrage iDrive",
        59: "Installee : redemarrer l'iDrive",
        60: "Echec : rien n'a ete modifie",
        61: "Incomplete : la reinstaller",
        62: "Occupe : reessayer plus tard",
        63: "Installation non reconnue",
        64: "Pas pendant un appel ni en marche AR",
        20: "NAV, MAP : navigation Android Auto",
        21: "MEDIA : musique Android Auto",
        22: "COM : telephone Android Auto",
        23: "MENU : iDrive - BACK : retour",
        24: "Telephone appaire, son en Bluetooth",
        26: "Gratuit : github.com/Jiraiya43",
    },
    "de": {
        2: "Android Auto: Verbinden...",
        10: "Android Auto starten",
        11: "Android Auto beenden",
        13: "Verbindung zum Telefon...",
        14: "Android Auto oeffnen",
        15: "Automatisch starten",
        16: "Hilfe",
        17: "USB-Kabel statt kabellos",
        18: "Protokolle speichern",
        40: "Protokolle auf USB-Stick kopieren",
        41: "Protokolle werden kopiert...",
        42: "Protokolle auf USB-Stick kopiert",
        43: "Kein USB-Stick gefunden",
        44: "Keine Protokolle: Speichern an",
        45: "Kopieren fehlgeschlagen",
        46: "Update vom USB-Stick",
        47: "USB-Stick wird gelesen...",
        48: "Update installieren",
        49: "Aeltere Version installieren",
        50: "Version neu installieren",
        51: "Update abschliessen",
        52: "Bereits aktuell",
        53: "Kein Update auf dem USB-Stick",
        54: "Update ungueltig",
        55: "Update passt nicht zum Auto",
        56: "Android Auto wird beendet...",
        57: "Installation: Zuendung anlassen",
        58: "Installiert: iDrive startet neu",
        59: "Installiert: iDrive neu starten",
        60: "Fehler: nichts geaendert",
        61: "Unvollstaendig: neu installieren",
        62: "Belegt: spaeter erneut",
        63: "Installation nicht erkannt",
        64: "Nicht bei Anruf oder Rueckwaerts",
        20: "NAV, MAP: Android Auto Navigation",
        21: "MEDIA: Android Auto Medien",
        22: "COM: Android Auto Telefon",
        23: "MENU: iDrive - BACK: zurueck",
        24: "Telefon gekoppelt, Ton per Bluetooth",
        26: "Kostenlos: github.com/Jiraiya43",
    },
}
LANG_TEXTS["fr-CA"] = LANG_TEXTS["fr-FR"]
HELP_TITLE = 27  # "<Help> - <version>": title of the Help page (its six lines are full, the page does not scroll)
FIXED_DATE = (2026, 11, 2, 0, 0, 0)  # the same date in every archive: reproducible files
VERSION_FILE = Path(__file__).resolve().parents[1] / "VERSION"


def add(archive, name, data):
    """Store one member, compressed, with the fixed date."""
    info = zipfile.ZipInfo(name, FIXED_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data)


def project_version():
    """The project's version, as VERSION gives it: one line of ASCII letters, digits, dots and dashes."""
    version = VERSION_FILE.read_text().strip()
    if not version or not all(c.isascii() and (c.isalnum() or c in ".-") for c in version):
        raise SystemExit(f"FAIL: {VERSION_FILE} does not hold a version: {version!r}")
    return version


def write_texts(path, version):
    """One <lang>.txt per language: English, overridden by the language's own texts."""
    with zipfile.ZipFile(path, "w") as archive:
        for lang, langname in LANGS.items():
            texts = dict(TEXTS)
            texts.update(LANG_TEXTS.get(lang, {}))
            texts[HELP_TITLE] = f"{texts[16]} - {version}"
            body = f"100000={langname}\n" + "".join(f"{key}={value}\n" for key, value in sorted(texts.items()))
            if not body.isascii():
                raise SystemExit(f"FAIL: {lang} texts must stay ASCII")
            add(archive, lang + ".txt", body.encode("utf-8"))


def clear_png(w, h):
    # Fully transparent RGBA PNG (no image library): hides the big Android Auto logo of the blank state held under
    # Android Auto (seen while it is dimmed for the volume bar). An opaque black image looked like a window frame.
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    raw = b"".join(bytes(1 + 4 * w) for _ in range(h))  # filter byte 0 + transparent pixels, per row
    return (
        bytes([0x89])
        + b"PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def write_images(path, res):
    """The entry icon, the preview, the clear page background and the 8 frames of the connecting animation."""
    with zipfile.ZipFile(path, "w") as archive:
        add(archive, "55001.png", (res / "aa_row_icon_63x54.png").read_bytes())
        add(archive, "55002.png", (res / "aa_content_preview_549x484.png").read_bytes())
        add(archive, "55003.png", clear_png(549, 484))
        for frame in range(8):
            add(archive, f"{55010 + frame}.png", (res / "spinner" / f"spin_{frame}.png").read_bytes())


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: make_db.py <res dir> <output dir>")
    res, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    write_texts(out / "EntryAad_ALL_Texts.zip", project_version())
    write_images(out / "EntryAad_ALL_Images.zip", res)
    print("wrote EntryAad_ALL_Texts.zip, EntryAad_ALL_Images.zip in", out)


if __name__ == "__main__":
    main()
