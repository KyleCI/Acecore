import csv, os, sys, tempfile, unittest
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
        with open(c, newline="", encoding="utf-8") as fh:
            self.assertEqual(next(csv.reader(fh)), f.CSV_FIELDS)

    def test_csv_adds_hardware_model_column(self):
        d = tempfile.mkdtemp(); c = os.path.join(d, "u.csv")
        with open(c, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=["serial", "device_id"])
            w.writeheader(); w.writerow({"serial": "ACE-000001", "device_id": "AABBCCDDEEFF"})
        f.append_csv(c, {"serial": "ACE-000002", "device_id": "001122334455", "hardware_model": "pc-stm01-5kw"})
        with open(c, newline="", encoding="utf-8") as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual(rows[0]["serial"], "ACE-000001")
        self.assertEqual(rows[0]["hardware_model"], "")
        self.assertEqual(rows[1]["hardware_model"], "pc-stm01-5kw")

    def test_blank_urls_use_firmware_default(self):
        default = "https://api-dev.acecore.tech"
        self.assertEqual(f.DEFAULT_OTA, default)
        self.assertEqual(f.DEFAULT_API, default)
        for env in ({}, {"OTA_URL": "", "API_URL": ""}, {"OTA_URL": "  ", "API_URL": "\t"}):
            self.assertEqual(f.resolve_factory_urls(env), (default, default))
        self.assertEqual(f.resolve_factory_urls({"OTA_URL": "https://ota.example", "API_URL": ""}),
                         ("https://ota.example", "https://ota.example"))
        self.assertEqual(f.resolve_factory_urls({"OTA_URL": "", "API_URL": "https://api.example"}),
                         (default, "https://api.example"))
        cmds = f.nvs_sets("factory-key", f.resolve_factory_urls({"OTA_URL": "", "API_URL": ""})[0])
        self.assertEqual(cmds, [("api_key", "factory-key"), ("ota_url", default)])
        self.assertEqual([k for k, _ in f.nvs_sets("factory-key", "  ")], ["api_key"])
        for key, val in f.nvs_sets("", "") + f.nvs_sets("factory-key", " "):
            self.assertTrue(val.strip())
            self.assertGreater(len(("SET %s %s" % (key, val)).split()), 2)

    def test_model(self):
        for v in ("PC5K", "pc5k", "5K", "5kva", " PC5KVA "): self.assertEqual(f.normalize_model(v), "PC5K")
        self.assertEqual(f.normalize_model("10k"), "PC10K"); self.assertEqual(f.normalize_model("PC3K"), "PC3K")
        for v in ("PC3_5K", "pc3_5k", "3.5K", "3.5kva", "PC3.5K", "35K", "PC35K", " pc35k "):
            self.assertEqual(f.normalize_model(v), "PC3_5K")
        for v in ("PC7K", "", "5"):
            with self.assertRaises(ValueError): f.normalize_model(v)
        self.assertIn("PC5K", f.model_hw_mismatch_warning("PC5K", "pc-dsp011-35kw"))
        self.assertIn("PC3_5K", f.model_hw_mismatch_warning("PC3_5K", "pc-dsp011-5kw"))
        self.assertIsNone(f.model_hw_mismatch_warning("PC3_5K", "pc-dsp011-35kw"))
        self.assertIsNone(f.model_hw_mismatch_warning("PC5K", "pc-dsp011-5kw"))
        self.assertIsNone(f.model_hw_mismatch_warning("PC5K", ""))
        # 2.5.32 product names: 3.5 kVA is Prime Plus, 5 kVA is Ultra (STM and DSP).
        for hw in ("pc-dsp011-35kw", "pc-stm01-35kw"):
            w = f.model_hw_mismatch_warning("PC5K", hw)
            self.assertIn("Powercell Prime Plus 3.5 kVA", w)
            self.assertNotIn("Powercell Prime 3.5", w)
            self.assertNotIn("Ultra", w)
        for hw in ("pc-dsp011-5kw", "pc-stm01-5kw"):
            w = f.model_hw_mismatch_warning("PC3_5K", hw)
            self.assertIn("Powercell Ultra 5 kVA", w)
            self.assertNotIn("Prime Plus", w)

    def test_ota_model_from_info(self):
        text = ("device_id=F42DC9C090A8 mac=F4:2D:C9:C0:90:A8 gateway=ACE-000001 fw=2.5.32\n"
                "model=PC5K rated_kva=5.0 usable_kw=4.5\n"
                "hardware_model=pc-dsp011-5kw\n")
        st = f.parse_info(text)
        self.assertEqual(st["model"], "PC5K")
        self.assertEqual(f.require_ota_model(st), "pc-dsp011-5kw")
        for m in f.OTA_MODELS:
            self.assertEqual(f.require_ota_model({"hardware_model": m}), m)
        for bad in ("", "esp32dev", "PC5K", "pc-dsp011-5kW", "hw-v0.1.1-5kw"):
            with self.assertRaises(ValueError):
                f.require_ota_model({"hardware_model": bad})
        with self.assertRaises(ValueError):
            f.require_ota_model({})
        body = f.register_payload("F42DC9C090A8", "ab" * 32, "F4:2D:C9:C0:90:A8",
                                  "ACE-000001", "2.5.32", "pc-dsp011-5kw")
        self.assertEqual(body["onboarding_version"], 2)
        self.assertIs(body["passcode_is_default"], True)
        self.assertNotIn("passcode_fingerprint", body)
        self.assertEqual(body, {
            "device_id": "F42DC9C090A8",
            "device_secret": "ab" * 32,
            "mac": "F4:2D:C9:C0:90:A8",
            "gateway_id": "ACE-000001",
            "firmware_version": "2.5.32",
            "device_kind": "collector",
            "model": "pc-dsp011-5kw",
            "onboarding_version": 2,
            "passcode_is_default": True,
        })
        with self.assertRaises(ValueError):
            f.register_payload("F42DC9C090A8", "secret", "", "ACE-000001", "2.5.32", "PC5K")
        label = f.label_file_text("PWC2;S=Powercell-000001;P=abc;D=ACE-000001",
                                  ["LABEL ap_pass=abc", "LABEL_QR WIFI:T:WPA;S:x;P:y;;"],
                                  "pc-stm01-35kw")
        self.assertEqual(label, "PWC2;S=Powercell-000001;P=abc;D=ACE-000001\n"
                                "hardware_model=pc-stm01-35kw\n"
                                "LABEL_QR WIFI:T:WPA;S:x;P:y;;\n")

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

class Prime15(unittest.TestCase):
    """2.5.33: Powercell Prime 1.5 kVA (pc-dsp011-15kw / pc-stm01-15kw, NVS PC1_5K). Provisional ratings."""
    def test_ota_models(self):
        self.assertEqual(len(f.OTA_MODELS), 6)
        for m in ("pc-dsp011-15kw", "pc-stm01-15kw"):
            self.assertIn(m, f.OTA_MODELS)
            self.assertEqual(f.require_ota_model({"hardware_model": m}), m)
            body = f.register_payload("F42DC9C090A8", "ab" * 32, "", "ACE-000001", "2.5.33", m)
            self.assertEqual(body["model"], m)
        for bad in ("pc-dsp011-1.5kw", "pc-dsp011-15kW", "pc-dsp011-1_5kw", "pc-stm01-15k", "PC1_5K"):
            with self.assertRaises(ValueError):
                f.require_ota_model({"hardware_model": bad})
        sys.path.insert(0, os.path.dirname(__file__))
        import ota_upload
        self.assertEqual(set(ota_upload.MODELS), set(f.OTA_MODELS))

    def test_nvs_model(self):
        self.assertIn("PC1_5K", f.MODELS)
        self.assertIn("PC3K", f.MODELS)  # legacy 3 kVA kept, not reused for 1.5 kVA
        for v in ("PC1_5K", "pc1_5k", "1.5K", "1.5kva", "PC1.5K", "15K", "PC15K", " pc15k ", "PC1.5KVA"):
            self.assertEqual(f.normalize_model(v), "PC1_5K")
        self.assertEqual(f.normalize_model("PC3K"), "PC3K")
        self.assertEqual(f.normalize_model("35K"), "PC3_5K")

    def test_mismatch_warnings(self):
        for hw in ("pc-dsp011-15kw", "pc-stm01-15kw"):
            self.assertIsNone(f.model_hw_mismatch_warning("PC1_5K", hw))
            for m in ("PC5K", "PC3_5K", "PC3K", "PC10K"):
                w = f.model_hw_mismatch_warning(m, hw)
                self.assertIsNotNone(w, (m, hw))
                self.assertIn(m, w)
            self.assertIn("Powercell Prime 1.5 kVA", f.model_hw_mismatch_warning("PC3_5K", hw))
        for hw in ("pc-dsp011-35kw", "pc-stm01-35kw"):
            self.assertIn("Powercell Prime Plus 3.5 kVA", f.model_hw_mismatch_warning("PC1_5K", hw))
        for hw in ("pc-dsp011-5kw", "pc-stm01-5kw"):
            self.assertIn("Powercell Ultra 5 kVA", f.model_hw_mismatch_warning("PC1_5K", hw))
        # Existing pairs unchanged: a 15kw id is not mistaken for a 5kw id.
        self.assertIsNone(f.model_hw_mismatch_warning("PC5K", "pc-dsp011-5kw"))
        self.assertIsNone(f.model_hw_mismatch_warning("PC3_5K", "pc-stm01-35kw"))

if __name__ == "__main__": unittest.main()
