# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual isolated worker against an injected native bus, never a car."""

import os
import select
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent


class WorkerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / "build/host-most-worker"
        cls.library = ROOT / "build/host-fake-vmost.so"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="tm-", dir="/tmp")
        self.base = Path(self.tmp.name)
        self.path = str(self.base / "private/most.sock")
        self.bus = str(self.base / "bus")
        self.log = self.base / "requests"
        self.log.touch()
        self.state = self.base / "state"
        self.state.mkdir(mode=0o700)
        env = dict(
            os.environ,
            TELEMETRY_TEST_SOCKET=self.path,
            TELEMETRY_TEST_LIBRARY=str(self.library),
            TELEMETRY_TEST_STATE=str(self.state),
            TELEMETRY_FAKE_BUS=self.bus,
            TELEMETRY_FAKE_LOG=str(self.log),
        )
        self.output = (self.base / "worker.log").open("w+")
        self.proc = subprocess.Popen([self.binary], env=env, stdout=self.output, stderr=subprocess.STDOUT)
        self.wait(lambda: Path(self.path).exists())
        self.sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.clients = []
        self.at = 0

    def tearDown(self):
        for s in self.clients:
            s.close()
        self.sender.close()
        self.proc.terminate()
        self.proc.wait(timeout=3)
        self.output.seek(0)
        output = self.output.read()
        self.output.close()
        self.tmp.cleanup()
        self.assertEqual(self.proc.returncode, 0, output)
        self.assertNotIn("Sanitizer", output)

    def wait(self, predicate, timeout=2):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return
            time.sleep(0.01)
        self.fail("condition timed out")

    def client(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(self.path)
        self.clients.append(s)
        return s

    @staticmethod
    def request(ecu=0x12, did=0xF150):
        uds = b"\x01\x04" if did == 0x0104 else b"\x22" + struct.pack(">H", did)
        return struct.pack(">IHBB", len(uds) + 2, 1, 0xF4, ecu) + uds

    def forwarded(self, ecu=0x12, did=0xF150):
        self.wait(lambda: self.log.stat().st_size > self.at)
        data = self.log.read_bytes()
        size = data[self.at]
        raw = data[self.at + 1 : self.at + 1 + size]
        self.at += size + 1
        uds = self.request(ecu, did)[8:]
        self.assertEqual(raw, b"\x63\xf0\x31\x01\x0f\x0b" + bytes([ecu]) + struct.pack(">H", len(uds)) + uds)

    def answer(self, ecu=0x12, did=0xF150, data=b"\x0f\x1f\xf0", *, function=0x50, operation=0xC, uds=None):
        if uds is None:
            uds = (b"\x41\x04" if did == 0x0104 else b"\x62" + struct.pack(">H", did)) + data
        p = b"\xf0\x63\x71\x01\x0f\x0b" + bytes([ecu]) + struct.pack(">H", len(uds)) + uds
        self.sender.sendto(struct.pack("<BBHIH", 6, 0xF0, function, operation, len(p)) + p, self.bus)
        return struct.pack(">IHBB", len(uds) + 2, 1, ecu, 0xF4) + uds

    def read(self, s, expected):
        data = b""
        while len(data) < len(expected):
            part = s.recv(32)
            self.assertTrue(part)
            data += part
        self.assertEqual(data, expected)

    def identify(self, s, ecu=0x12):
        s.sendall(self.request(ecu))
        self.forwarded(ecu)
        self.read(s, self.answer(ecu, data=b"\x0f\x1f\xf0" if ecu == 0x12 else b"\x0f\x21\xd0"))

    def test_idle_gate_fragmentation_and_all_records(self):
        time.sleep(0.1)
        self.assertFalse(Path(self.bus).exists(), "idle worker opened vehicle bus")
        s = self.client()
        raw = self.request()
        for byte in raw[:-1]:
            s.sendall(bytes([byte]))
            time.sleep(0.002)
        self.assertEqual(self.log.stat().st_size, 0)
        s.sendall(raw[-1:])
        self.forwarded()
        self.read(s, self.answer())
        self.identify(s, 0x18)
        records = [
            (0x12, 0x42B9, 2),
            (0x12, 0x43C0, 2),
            (0x12, 0x5868, 1),
            (0x12, 0x5866, 2),
            (0x12, 0x0104, 1),
            (0x18, 0x6000, 1),
            (0x18, 0xDA2E, 2),
            (0x18, 0xDA22, 2),
            (0x18, 0xDA2A, 4),
            (0x18, 0x6009, 1),
            (0x18, 0x600A, 1),
            (0x18, 0x600C, 1),
            (0x12, 0x43C2, 1),
            (0x12, 0x48A5, 2),
            (0x18, 0xDA27, 2),
            (0x12, 0x40C0, 16),
            (0x18, 0xDA31, 2),
        ]
        for ecu, did, size in records:
            s.sendall(self.request(ecu, did))
            self.forwarded(ecu, did)
            self.read(s, self.answer(ecu, did, bytes(range(size))))
        s.close()
        self.wait(lambda: not Path(self.bus).exists())
        s = self.client()
        self.identify(s)  # New UI must identify again.

    def test_reject_writes_unknown_reads_pipeline_and_ungated_measurements(self):
        requests = [
            self.request(did=0x42B9),
            self.request(ecu=0x10),
            self.request(did=0xDEAD),
            self.request().replace(b"\x22\xf1\x50", b"\x2e\xf1\x50"),
            self.request() + self.request(),
            b"\0\0\0\xff\0\1",
        ]
        for request in requests:
            s = self.client()
            s.sendall(request)
            self.assertEqual(s.recv(1), b"")
            s.close()
        self.assertEqual(self.log.stat().st_size, 0)

    def test_identity_mismatch_does_not_unlock(self):
        s = self.client()
        s.sendall(self.request())
        self.forwarded()
        self.read(s, self.answer(data=b"\x0f\x1f\x00"))
        s.sendall(self.request(did=0x42B9))
        self.assertEqual(s.recv(1), b"")
        self.assertEqual(self.at, self.log.stat().st_size)

    def test_ignore_foreign_responses_and_pending_then_relay_negative(self):
        s = self.client()
        self.identify(s)
        s.sendall(self.request(did=0x42B9))
        self.forwarded(did=0x42B9)
        self.answer(did=0x43C0, data=b"\0\1")
        self.answer(ecu=0x18, did=0x42B9, data=b"\0\1")
        self.answer(did=0x42B9, data=b"\0\1", function=0x51)
        self.answer(did=0x42B9, data=b"\0\1", operation=0)
        self.answer(uds=b"\x7f\x22\x78")
        self.assertFalse(select.select([s], [], [], 0.1)[0])
        self.read(s, self.answer(uds=b"\x7f\x22\x31"))
        s.sendall(self.request(did=0x42B9))
        self.forwarded(did=0x42B9)
        self.read(s, self.answer(did=0x42B9, data=b"\x01\x02"))

    def test_retired_answer_never_reaches_reopened_dashboard(self):
        s = self.client()
        self.identify(s)
        s.sendall(self.request(did=0x42B9))
        self.forwarded(did=0x42B9)
        s.close()
        time.sleep(0.1)
        new = self.client()
        self.assertEqual(new.recv(1), b"")
        new.close()
        self.answer(did=0x42B9, data=b"\x01\x02")
        self.wait(lambda: not Path(self.bus).exists())
        s = self.client()
        self.identify(s)

    def test_timeout_quarantine_then_recovery(self):
        s = self.client()
        self.identify(s)
        s.sendall(self.request(did=0x42B9))
        self.forwarded(did=0x42B9)
        s.settimeout(7)
        self.assertEqual(s.recv(1), b"")
        self.wait(lambda: not Path(self.bus).exists())
        s = self.client()
        self.assertEqual(s.recv(1), b"")
        time.sleep(30.2)
        s = self.client()
        self.identify(s)


if __name__ == "__main__":
    unittest.main()
