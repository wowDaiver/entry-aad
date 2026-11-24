# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check what git would publish: no password, no signing key, no compiled program, nothing large, no local-only folder.

Every file of every commit is read from git itself, not from the working tree: what is checked is what leaves the PC.
  python tools/verify_repo.py             the history of HEAD and the staged files (CI, by hand)
  python tools/verify_repo.py --pre-push  the commits being pushed, as git lists them on the hook's input
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MAX_BYTES = 1024 * 1024
FORBIDDEN_PREFIXES = ("artifacts/", ".secrets/", "rootfs/")
NO_COMMIT = "0" * 40


def git(*args, data=None):
    return subprocess.run(["git", *args], cwd=ROOT, input=data, check=True, capture_output=True).stdout


def pushed_revisions(hook_input):
    """The local commits named on the pre-push hook's input; a deleted branch sends none."""
    revisions = []
    for line in hook_input.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[1] != NO_COMMIT:
            revisions.append(fields[1])
    return revisions


def files_of(revisions, staged):
    """(path, blob id) of every file in the history of the revisions, plus the index when asked."""
    found = {}
    if revisions:
        listing = git("rev-list", "--objects", *revisions).decode("utf-8", "replace").splitlines()
        candidates = [line.split(" ", 1) for line in listing if " " in line]
        query = "".join(f"{sha}\n" for sha, _ in candidates).encode()
        types = git("cat-file", "--batch-check=%(objecttype)", data=query).decode().split()
        for (sha, path), kind in zip(candidates, types):
            if kind == "blob":
                found[(path, sha)] = None
    if staged:
        for line in git("ls-files", "-s", "-z").split(b"\0"):
            if line:
                meta, path = line.decode("utf-8", "replace").split("\t", 1)
                found[(path, meta.split()[1])] = None
    return list(found)


def problems_in(path, data, secret, key_lines=()):
    if path.startswith(FORBIDDEN_PREFIXES):
        yield "this folder stays local"
    if len(data) > MAX_BYTES:
        yield f"{len(data)} bytes"
    if data.startswith((b"\x7fELF", b"MZ")):
        yield "compiled program"
    if secret and secret in data:
        yield "contains the car's password"
    if any(line in data for line in key_lines):
        yield "contains part of the update signing key"


def signing_key_lines():
    """The base64 lines of the private key that signs the updates, when this PC holds it: none may leave it."""
    key = ROOT / ".secrets/update-signing-key.pem"
    if not key.exists():
        return ()
    lines = key.read_bytes().split(b"\n")
    return tuple(line.strip() for line in lines if len(line.strip()) >= 40 and not line.startswith(b"-----"))


def main():
    pre_push = "--pre-push" in sys.argv[1:]
    revisions = pushed_revisions(sys.stdin.read()) if pre_push else ["HEAD"]
    if (
        not pre_push
        and subprocess.run(["git", "rev-parse", "-q", "--verify", "HEAD"], cwd=ROOT, capture_output=True).returncode
    ):
        revisions = []  # no commit yet: only the index
    secret_file = ROOT / ".secrets/hu-password.txt"
    secret = secret_file.read_text(encoding="utf-8").strip().encode() if secret_file.exists() else None
    key_lines = signing_key_lines()
    files = files_of(revisions, staged=not pre_push)
    problems = []
    for path, sha in files:
        data = git("cat-file", "blob", sha)
        problems.extend(f"{path} ({sha[:8]}): {problem}" for problem in problems_in(path, data, secret, key_lines))
    if problems:
        print("\n".join(f"FAIL {problem}" for problem in sorted(set(problems))))
        return 1
    state = "password searched" if secret else "no local password to search for"
    state += ", signing key searched" if key_lines else ""
    print(f"PASS: {len(files)} file versions, nothing private or compiled ({state})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
