import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import factory as f

class T(unittest.TestCase):
    def test_serial(self):
        self.assertEqual(f.format_serial("ACE-", 7), "ACE-000007")
        with self.assertRaises(ValueError): f.format_serial("PC-", 7)
    def test_parse(self):
        st = f.parse_info("device_id=F42DC9C090A8 mac=F4:2D gateway=ACE-000001 fw=2.5.0\n"
                          "serial=unset ssid=x secret=set\napi_key=MISSING wifi_ssid= locked=0\n"
                          "LABEL ap_pass=abc admin_token=set\nLABEL PWC2;S=Powercell-000001;P=abc;D=ACE-000001\n")
        self.assertEqual((st["serial"], st["api_key"], st["locked"]), ("unset", "MISSING", "0"))
        self.assertEqual(f.pick_label(st["labels"]), "PWC2;S=Powercell-000001;P=abc;D=ACE-000001")
    def test_counter_csv(self):
        d = tempfile.mkdtemp(); p = os.path.join(d, "c.json"); c = os.path.join(d, "u.csv")
        cnt = f.load_counter(p); self.assertEqual(cnt["next"], 1); cnt["next"] = 5; f.save_counter(p, cnt)
        self.assertEqual(f.load_counter(p)["next"], 5)
        f.append_csv(c, {"serial": "ACE-000005"}); self.assertTrue(f.serial_in_log(c, "ACE-000005"))

    def test_model(self):
        for v in ("PC5K", "pc5k", "5K", "5kva", " PC5KVA "): self.assertEqual(f.normalize_model(v), "PC5K")
        self.assertEqual(f.normalize_model("10k"), "PC10K"); self.assertEqual(f.normalize_model("PC3K"), "PC3K")
        for v in ("PC7K", "", "5"):
            with self.assertRaises(ValueError): f.normalize_model(v)

class R(unittest.TestCase):
    def test_reprovision(self):
        d = tempfile.mkdtemp(); c = os.path.join(d, "u.csv")
        f.append_csv(c, {"serial": "ACE-000002", "device_id": "F42DC9C181A8"})
        self.assertTrue(f.check_reprovision(c, "ACE-000002", "F42DC9C181A8", "unset")[0])
        self.assertTrue(f.check_reprovision(c, "ACE-000002", "f4:2d:c9:c1:81:a8", "ACE-000002")[0])
        self.assertFalse(f.check_reprovision(c, "ACE-000002", "AABBCCDDEEFF", "unset")[0])
        self.assertFalse(f.check_reprovision(c, "ACE-000002", "F42DC9C181A8", "ACE-000009")[0])
        self.assertFalse(f.check_reprovision(c, "ACE-2", "F42DC9C181A8", "unset")[0])
        ok, msg = f.check_reprovision(c, "ACE-000005", "F42DC9C181A8", "unset")
        self.assertTrue(ok); self.assertIn("WARNING", msg)

if __name__ == "__main__": unittest.main()
