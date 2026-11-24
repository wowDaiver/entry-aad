# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""The release archive as tools/package_release.py writes it, read by the car's update code.

A key made for the test signs an archive built by the packager's own functions; the C++ reader
(tests/test-update-package --verify) must take it. Also: the signature is the same every time (the archive stays
reproducible), a key the programs do not trust is refused at packaging, and the project's key in
platform/update_key.hpp reads as a DER key.

Usage: test_update_release.py <tests/test-update-package>
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package_release  # noqa: E402

VERSION = "0.14.0-rc.1"


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True, capture_output=True)


def files() -> dict:
    elf = b"\x7fELF"
    plugin = package_release.PLUGIN
    return {
        "opt/aa/aa-supervisor": elf + b"supervisor",
        "opt/aa/entry-aad": elf + b"service" * 500,
        f"{plugin}/libEntryAad.so": elf + b"app",
        f"{plugin}/EntryAad.manifest": b"manifest",
        f"{plugin}/EntryAad.xml": b"<pluginApps/>",
        f"{plugin}/EntryAad_ALL_Texts.zip": b"texts",
        f"{plugin}/EntryAad_ALL_Images.zip": b"images",
        "MD5SUMS": b"sums\n",
        "LICENSE.md": b"licence\n",
    }


def main() -> int:
    reader = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        key, public = work / "key.pem", work / "key.der"
        openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(key))
        openssl("pkey", "-in", str(key), "-pubout", "-outform", "DER", "-out", str(public))
        trusted = [public.read_bytes()]

        content = files()
        listing = package_release.update_list(VERSION, content)
        assert listing.startswith(b"entrynav2-android-auto update 1\nversion 0.14.0-rc.1\nfile 755 ")
        assert b"MD5SUMS" not in listing and b"LICENSE" not in listing, "only the installed files are listed"
        content["UPDATE"] = listing
        content["UPDATE.sig"] = package_release.sign(listing, key, trusted)
        assert package_release.sign(listing, key, trusted) == content["UPDATE.sig"], "the signature must not vary"

        archive = work / f"entrynav2-android-auto-{VERSION}-armhf.tar"
        package_release.write_archive(archive, f"entrynav2-android-auto-{VERSION}", content, 1763856000)
        run = subprocess.run([str(reader), "--verify", str(archive), str(public)], capture_output=True, text=True)
        assert run.returncode == 0 and f"version {VERSION}, 7 files" in run.stdout, run.stdout + run.stderr

        # The same archive under a key the reader was not given: refused.
        other = work / "other.der"
        openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:1024", "-out", str(work / "other.pem"))
        openssl("pkey", "-in", str(work / "other.pem"), "-pubout", "-outform", "DER", "-out", str(other))
        run = subprocess.run([str(reader), "--verify", str(archive), str(other)], capture_output=True, text=True)
        assert run.returncode == 1 and "not the project's" in run.stdout, run.stdout

        # Packaging with a key the programs do not trust stops before any archive is written.
        try:
            package_release.sign(listing, key)
        except SystemExit as refusal:
            assert "not one the programs trust" in str(refusal)
        else:
            raise AssertionError("a key the programs do not trust signed a release")

    keys = package_release.project_keys()
    assert keys and all(k[:1] == b"\x30" and len(k) > 400 for k in keys), "platform/update_key.hpp holds no DER key"
    print("PASS update release: packaged, signed, read by the car's update code")
    return 0


if __name__ == "__main__":
    sys.exit(main())
