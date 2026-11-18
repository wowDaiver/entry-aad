import hashlib
import io
import tarfile
import unittest

from collect146 import KNOWN, PROVIDER, SERVICE, validate


def packed(files):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode="w:") as archive:
        for path, body in files:
            info = tarfile.TarInfo(path)
            info.size = len(body)
            archive.addfile(info, io.BytesIO(body))
    result.seek(0)
    return result


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.files = [(p, b"\x7fELFfixture " + p.encode()) for p in (*KNOWN, PROVIDER[0], SERVICE)]
        self.expected = {p: hashlib.sha256(body).hexdigest() for p, body in self.files if p in KNOWN}

    def test_verified_set(self):
        self.assertEqual(len(validate(packed(self.files), self.expected)), 5)

    def test_foreign_firmware(self):
        self.files[0] = (KNOWN[0], b"\x7fELFforeign")
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            validate(packed(self.files), self.expected)

    def test_missing_provider(self):
        with self.assertRaisesRegex(ValueError, "incomplete"):
            validate(packed(self.files[:3]), self.expected)

    def test_traversal_and_duplicate_entries(self):
        for extra in [("../escape", b"\x7fELF"), self.files[0]]:
            with self.subTest(extra=extra[0]), self.assertRaisesRegex(ValueError, "unexpected archive entry"):
                validate(packed(self.files + [extra]), self.expected)


if __name__ == "__main__":
    unittest.main()
