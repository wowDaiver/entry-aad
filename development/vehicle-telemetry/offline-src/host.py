#!/usr/bin/env python3
"""Personal, self-contained offline vehicle-menu kit; Python standard library only."""

import argparse
import atexit
import contextlib
import fcntl
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

import capture

HERE = Path(__file__).resolve().parent
os.umask(0o077)
ID = "vehicle-telemetry-146-v1"
REMOTE_FILES = (
    "remote/common.sh",
    "remote/control.sh",
    "remote/worker.sh",
    "firmware.sha256",
    "payload.sha256",
    "REMOTE_SHA256SUMS",
    "payload/.vehicle-telemetry-owner",
    "payload/libVehicleTelemetry.so",
    "payload/VehicleTelemetry.xml",
    "payload/VehicleTelemetry.manifest",
    "payload/VehicleTelemetry_Texts.zip",
    "payload/VehicleTelemetry_Images.zip",
)
REQUIRED = set(REMOTE_FILES) | {
    "host.py",
    "capture.py",
    "ssh-askpass.sh",
    "hu-known_hosts",
    "hu-password.txt",
    "kit.json",
    "README-RU.md",
    "VALIDATION-RU.md",
    "CHECKS.log",
    "LICENSE",
    "NOTICE.txt",
    "00-local-check.sh",
    "01-check-and-capture.sh",
    "02-install.sh",
    "03-status.sh",
    "04-collect.sh",
    "05-disable.sh",
    "06-recover-lock.sh",
}


def write_json(path, data):
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def local_check():
    manifest = HERE / "SHA256SUMS"
    if manifest.is_symlink() or not manifest.is_file():
        raise RuntimeError("Нет SHA256SUMS: нужен полный комплект.")
    found = set()
    for line in manifest.read_text().splitlines():
        digest, relative = line.split("  ", 1)
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts or relative in found or not re.fullmatch("[0-9a-f]{64}", digest):
            raise RuntimeError("Некорректный SHA256SUMS.")
        found.add(relative)
        target = HERE / path
        if any(p.is_symlink() for p in (target, *target.parents) if p != HERE.parent) or not target.is_file():
            raise RuntimeError("Отсутствует файл или обнаружена ссылка: " + relative)
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise RuntimeError("Файл изменён или повреждён: " + relative)
    if found != REQUIRED:
        raise RuntimeError("Неверный состав комплекта: " + str(sorted(found ^ REQUIRED)))
    for command in ("python3", "ssh", "sh"):
        if not shutil.which(command):
            raise RuntimeError("Нет локальной команды " + command + "; проверить до поездки.")
    if not (HERE / "hu-password.txt").read_bytes().strip():
        raise RuntimeError("Не настроен пароль ГУ.")
    hosts = (HERE / "hu-known_hosts").read_text().splitlines()
    if not any(line.startswith("entrynav2-hu ") for line in hosts):
        raise RuntimeError("Не найден сохранённый ключ нашего ГУ.")
    for filename in ("hu-password.txt", "hu-known_hosts"):
        (HERE / filename).chmod(0o600)
    (HERE / "ssh-askpass.sh").chmod(0o700)
    for filename in sorted(p for p in REQUIRED if p.endswith(".sh")):
        subprocess.run(["sh", "-n", str(HERE / filename)], check=True)
    config = json.loads((HERE / "kit.json").read_text())
    if config["id"] != ID or config["vehicle_tested"] is not False:
        raise RuntimeError("Неверный профиль кандидата.")
    return config


class Connection:
    def __init__(self, host):
        self.target = "root@" + host
        self.temp = tempfile.TemporaryDirectory(prefix="vt146-", dir="/tmp")
        self.options = [
            "-F",
            "/dev/null",
            "-o",
            "ControlPath=" + self.temp.name + "/c",
            "-o",
            "ConnectTimeout=8",
            "-o",
            "ServerAliveInterval=3",
            "-o",
            "ServerAliveCountMax=2",
            "-o",
            "ProxyCommand=none",
            "-o",
            "ProxyJump=none",
            "-o",
            "CanonicalizeHostname=no",
            "-o",
            "UserKnownHostsFile=" + str(HERE / "hu-known_hosts"),
            "-o",
            "GlobalKnownHostsFile=/dev/null",
            "-o",
            "HostKeyAlias=entrynav2-hu",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "CheckHostIP=no",
            "-o",
            "PubkeyAuthentication=no",
            "-o",
            "KbdInteractiveAuthentication=no",
            "-o",
            "NumberOfPasswordPrompts=1",
        ]
        env = {
            **os.environ,
            "SSH_ASKPASS": str(HERE / "ssh-askpass.sh"),
            "SSH_ASKPASS_REQUIRE": "force",
            "DISPLAY": "vehicle-telemetry-offline:0",
        }
        try:
            result = subprocess.run(
                ["ssh", *self.options, "-o", "PreferredAuthentications=password", "-MNf", self.target],
                env=env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=25,
                start_new_session=True,
            )
            if result.returncode:
                raise RuntimeError("SSH не подключился:\n" + result.stderr.decode(errors="replace"))
        except BaseException:
            self.close()
            raise
        atexit.register(self.close)

    def run(self, command, *, data=None, output=None, timeout=40):
        return subprocess.run(
            ["ssh", *self.options, "-o", "BatchMode=yes", self.target, command],
            input=data,
            stdin=None if data is not None else subprocess.DEVNULL,
            stdout=output if output is not None else subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )

    def close(self):
        if self.temp is not None:
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(
                    ["ssh", *self.options, "-O", "exit", self.target],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5,
                )
            self.temp.cleanup()
            self.temp = None


def record(result, directory, name):
    body = (result.stdout or b"") + b"\n" + result.stderr
    (directory / (name + ".txt")).write_bytes(body)
    print(body.decode(errors="replace"), end="", flush=True)
    if result.returncode:
        raise RuntimeError("Команда завершилась с кодом " + str(result.returncode) + "; см. " + str(directory))


def stage(conn, directory):
    remote = "/tmp/" + ID + "-" + os.urandom(6).hex()
    write_json(
        directory / "connection.json",
        {
            "stage": remote,
            "kit": ID,
            "manual_hu_restart_only": True,
            "started_utc": datetime.now(timezone.utc).isoformat(),
        },
    )
    # Check tmpfs BEFORE transferring. Only this unique directory is created in RAM.
    script = (
        'awk -v p=/tmp \'($2=="/" || p==$2 || index(p,$2"/")==1) && length($2)>='
        'n {n=length($2);f=$3} END{exit !(f=="tmpfs" || f=="ramfs")}\' /proc/mou'
        "nts"
    )
    record(conn.run(script), directory, "ram-check")
    body = io.BytesIO()
    with tarfile.open(fileobj=body, mode="w") as archive:
        for filename in REMOTE_FILES:
            archive.add(HERE / filename, arcname=filename, recursive=False)
    command = "umask 077; mkdir " + shlex.quote(remote) + " && tar -xf - -C " + shlex.quote(remote)
    record(conn.run(command, data=body.getvalue()), directory, "transfer")
    return remote


def capture_firmware(conn, directory, config):
    with tempfile.TemporaryFile() as stream:
        result = conn.run(capture.REMOTE, output=stream, timeout=65)
        record(result, directory, "capture-command")
        if stream.tell() > capture.MAX_BYTES:
            raise RuntimeError("Архив библиотек превышает лимит.")
        stream.seek(0)
        files = capture.validate(stream, config["firmware_sha256"])
    destination = directory / "firmware"
    destination.mkdir()
    rows = []
    for name, body in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
        rows.append({"path": name, "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()})
    write_json(
        destination / "capture.json",
        {
            "identity_146_verified": True,
            "source": "physical HU over pinned ENET SSH",
            "hu_writes": False,
            "telemetry_values_read": 0,
            "files": rows,
        },
    )
    print("CAPTURE_VERIFIED_146:", destination, flush=True)


def worker(conn, remote, operation, directory):
    inner = 'sh "$1/remote/worker.sh" "$2"; rc=$?; printf "%s\\n" "$rc" >"$1/exit.tmp"; mv "$1/exit.tmp" "$1/exit"'
    command = "nohup sh -c " + shlex.quote(inner) + " vt " + shlex.quote(remote) + " " + operation
    command += " >" + shlex.quote(remote + "/worker.log") + " 2>&1 </dev/null &"
    record(conn.run(command), directory, "launch")
    print("Worker запущен. При обрыве связи сначала 03-status.sh, не повторная установка.", flush=True)
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        result = conn.run(
            "if test -f "
            + shlex.quote(remote + "/exit")
            + "; then cat "
            + shlex.quote(remote + "/exit")
            + "; else echo RUNNING; fi"
        )
        if result.returncode:
            record(result, directory, "poll-error")
        if result.stdout.strip() != b"RUNNING":
            log = conn.run("cat " + shlex.quote(remote + "/worker.log"))
            record(log, directory, "worker")
            code = result.stdout.strip()
            if code != b"0":
                raise RuntimeError(
                    "Worker завершился с ошибкой "
                    + code.decode(errors="replace")
                    + ". Запусти 03-status.sh; порядок восстановления в README."
                )
            return
        time.sleep(2)
    raise RuntimeError("Результат ещё не получен; worker может работать. Запусти 03-status.sh, не повторяй установку.")


def previous_workers(conn, directory):
    # Reconnect to known stages from our own reports; do not scan processes or execute arbitrary paths.
    for ref in sorted((HERE / "reports").glob("*/connection.json"))[-8:]:
        try:
            remote = json.loads(ref.read_text())["stage"]
        except (ValueError, KeyError):
            continue
        if not re.fullmatch("/tmp/" + ID + "-[0-9a-f]{12}", remote):
            continue
        command = "if test -d " + shlex.quote(remote) + "; then echo STAGE=" + shlex.quote(remote) + "; "
        command += (
            "for name in exit worker.log; do if test -f "
            + shlex.quote(remote)
            + '/"$name"; then echo "$name"; tail -n 100 '
            + shlex.quote(remote)
            + '/"$name"; fi; done; fi'
        )
        record(conn.run(command), directory, "prior-" + ref.parent.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("local-check", "check", "install", "status", "collect", "disable", "unlock"))
    parser.add_argument("--host", default="169.254.1.0")
    args = parser.parse_args()
    if not re.fullmatch(r"169\.254\.(?:[0-9]{1,3})\.(?:[0-9]{1,3})", args.host) or any(
        int(v) > 255 for v in args.host.split(".")
    ):
        parser.error("Ожидается link-local IPv4 ГУ 169.254.x.x, без URL/порта.")
    config = local_check()
    if args.action == "local-check":
        print("LOCAL_OFFLINE_KIT_PASS; сетевых соединений не было")
        return
    # A stale local file never blocks work; flock is released by the OS on process exit.
    with (HERE / ".host.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Другой скрипт этого комплекта ещё работает.") from None
        name = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-") + os.urandom(3).hex() + "-" + args.action
        directory = HERE / "reports" / name
        directory.mkdir(parents=True)
        print("Отчёт:", directory, flush=True)
        conn = Connection(args.host)
        try:
            remote = stage(conn, directory)
            # Preflight must not repeat the bulk backup that previously made the
            # HU unstable. Fingerprints and the loader are checked on-device.
            if args.action == "collect":
                capture_firmware(conn, directory, config)
            control = "sh " + shlex.quote(remote + "/remote/control.sh")
            if args.action in ("install", "disable"):
                # Install always rechecks SHA/layout/loader; it never trusts an old local ready flag.
                if args.action == "install":
                    record(conn.run(control + " check"), directory, "preflight")
                worker(conn, remote, args.action, directory)
                record(conn.run(control + " status"), directory, "status-after")
            else:
                record(conn.run(control + " " + args.action), directory, args.action)
                if args.action in ("status", "collect"):
                    previous_workers(conn, directory)
            write_json(
                directory / "result.json",
                {
                    "action": args.action,
                    "script_passed": True,
                    "vehicle_ui_verified": False,
                    "live_values_verified": False,
                },
            )
        except BaseException as exc:
            write_json(directory / "result.json", {"action": args.action, "script_passed": False, "error": str(exc)})
            raise
        finally:
            conn.close()


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print("STOP:", error, file=sys.stderr)
        sys.exit(1)
