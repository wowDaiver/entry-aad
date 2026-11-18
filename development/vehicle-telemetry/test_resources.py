import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from build_resources import build


class ResourcesTests(unittest.TestCase):
    def setUp(self):
        self.root = ET.parse(Path(__file__).with_name("VehicleTelemetry.xml")).getroot().find("pluginApp")

    def test_native_models_and_states_are_consistent(self):
        root = self.root
        models = [int(n.attrib["id"]) for n in root.find("models")]
        self.assertEqual(
            sorted(int(n.attrib["id"]) for n in root.findall("./models/raDataModel")),
            list(range(100, 115)) + list(range(130, 141)),
        )
        self.assertEqual(len(models), len(set(models)))
        self.assertEqual({int(n.attrib["id"]) for n in root.find("hmiStates")}, {10, 11, 12, 13, 14, 16, 17})
        labels = root.findall(".//label")
        self.assertEqual({int(n.attrib["model"]) for n in labels}, set(range(100, 115)) | set(range(130, 141)))
        self.assertEqual(len(labels), 26)
        for n in labels:
            self.assertIn(int(n.attrib["model"]), models)

    def test_all_rhmi_object_ids_are_globally_unique(self):
        # Native146 resolves actions/models in one object registry. In the VM,
        # action3/model3 and action4/model4 left the first two buttons inert.
        ids = [int(n.attrib["id"]) for n in self.root.iter() if "id" in n.attrib]
        self.assertEqual(len(ids), len(set(ids)))

    def test_entry_uses_own_vehicle_status_anchor(self):
        evidence = Path(__file__).resolve().parents[2] / "tests/fixtures/vehicle-telemetry/vehicle-menu-146.json"
        p = json.loads(evidence.read_text())["placement"]
        root = self.root
        self.assertEqual(root.attrib["applicationType"], "Custom")
        self.assertEqual(int(root.attrib["insertStateID"]), p["insert_state"])
        self.assertEqual(int(root.attrib["insertAfterFocusID"]), p["insert_after_focus"])
        self.assertEqual(int(root.attrib["entryButtonFocusID"]), p["entry_focus"])
        self.assertNotIn("replaceFocusID", root.attrib)
        self.assertEqual(len(root.findall("entryButton")), 1)

    def test_idrive_navigation_and_back_graph(self):
        root = self.root
        states = {int(n.attrib["id"]): n for n in root.find("hmiStates")}
        actions = {int(n.attrib["id"]): int(n.attrib["target"]) for n in root.find("actions")}
        entry = root.find("entryButton")
        self.assertEqual(actions[int(entry.attrib["action"])], 10)
        buttons = states[10].findall("./components/button")
        self.assertEqual({actions[int(b.attrib["action"])] for b in buttons}, {11, 12, 13, 14, 16})
        for sid, state in states.items():
            if sid != 10:
                self.assertEqual(int(state.attrib["parentId"]), 16 if sid == 17 else 10)
        self.assertNotIn("parentId", states[10].attrib)  # native entry owns external return
        self.assertTrue(all(len(s.findall("./components/label")) <= 5 for sid, s in states.items() if sid != 10))
        self.assertEqual(actions[int(states[16].find("./components/button").attrib["action"])], 17)

    def test_bundle_is_native_reproducible_and_has_russian_text(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            candidate = p / "test.so"
            candidate.write_bytes(b"\x7fELFtest-resource-packaging-only")
            first = build(candidate, p / "menu")
            digest = first["bundle_sha256"]
            second = build(candidate, p / "menu")
            self.assertEqual(second["bundle_sha256"], digest)
            self.assertFalse(second["installed"])
            self.assertFalse(any(name.endswith((".html", ".js")) for name in second["files"]))
            with zipfile.ZipFile(p / "menu/VehicleTelemetry_Texts.zip") as z:
                self.assertIn("Параметры автомобиля", z.read("ru.txt").decode("utf-8"))
            with zipfile.ZipFile(p / "menu/VehicleTelemetry_Images.zip") as z:
                self.assertEqual(z.read("1.png")[:8], b"\x89PNG\r\n\x1a\n")


if __name__ == "__main__":
    unittest.main()
