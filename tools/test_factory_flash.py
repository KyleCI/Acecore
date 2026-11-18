import json, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import factory as f
import factory_flash as ff

MAC = "F42DC9C090A8"
MAC2 = "AABBCCDDEEFF"


def tmpstate():
    d = tempfile.mkdtemp()
    return d, os.path.join(d, ff.JOURNAL), os.path.join(d, "units.csv"), os.path.join(d, "serial_counter.json")


class Models(unittest.TestCase):
    def test_mapping(self):
        self.assertEqual(ff.nvs_model_for("pc-dsp011-5kw"), "PC5K")
        self.assertEqual(ff.nvs_model_for("pc-stm01-5kw"), "PC5K")
        self.assertEqual(ff.nvs_model_for("pc-dsp011-35kw"), "PC3_5K")
        self.assertEqual(ff.nvs_model_for("pc-stm01-35kw"), "PC3_5K")
        self.assertEqual(ff.nvs_model_for("pc-dsp011-15kw"), "PC1_5K")
        self.assertEqual(ff.nvs_model_for("pc-stm01-15kw"), "PC1_5K")
        self.assertEqual(set(ff.NVS_MODEL), set(f.OTA_MODELS))
        for m in ff.NVS_MODEL.values():
            self.assertIn(m, f.MODELS)
            self.assertIsNone(f.model_hw_mismatch_warning(m, [k for k, v in ff.NVS_MODEL.items() if v == m][0]))
        with self.assertRaises(ValueError):
            ff.nvs_model_for("PC5K")

    def test_image_identity(self):
        img = b"\xff" * 64 + b"POWERCELL_FW|collector|pc-dsp011-5kw|2.5.36|END\x00" + b"POWERCELL_FW|collector|pc-dsp011-5kw|"
        self.assertEqual(ff.check_image(img, "pc-dsp011-5kw"), "2.5.36")
        with self.assertRaises(ff.FlashError):
            ff.check_image(img, "pc-dsp011-15kw")
        with self.assertRaises(ff.FlashError):
            ff.check_image(b"nothing", "pc-dsp011-5kw")
        with self.assertRaises(ff.FlashError):
            ff.check_image(img + b"POWERCELL_FW|collector|pc-stm01-5kw|2.5.36|END", "pc-dsp011-5kw")

    def test_find_image_does_not_confuse_5kw_and_15kw(self):
        d = tempfile.mkdtemp()
        for n in ("pc-dsp011-5kw_v2.5.36_merged.bin", "pc-dsp011-15kw_v2.5.36_merged.bin"):
            open(os.path.join(d, n), "wb").close()
        self.assertTrue(ff.find_image(d, "pc-dsp011-5kw").endswith("pc-dsp011-5kw_v2.5.36_merged.bin"))
        self.assertTrue(ff.find_image(d, "pc-dsp011-15kw").endswith("pc-dsp011-15kw_v2.5.36_merged.bin"))
        with self.assertRaises(ff.FlashError):
            ff.find_image(d, "pc-stm01-5kw")

    def test_mac_and_read_mac(self):
        self.assertEqual(ff.norm_mac("f4:2d:c9:c0:90:a8"), MAC)
        self.assertEqual(ff.parse_read_mac("Chip is ESP32\nMAC: f4:2d:c9:c0:90:a8\nHard resetting"), MAC)
        with self.assertRaises(ff.FlashError):
            ff.parse_read_mac("A fatal error occurred")

    def test_sums(self):
        d = tempfile.mkdtemp(); p = os.path.join(d, "SHA256SUMS")
        with open(p, "w") as fh:
            fh.write("ABCD  bins/x_merged.bin\nef01 *y.bin\n")
        self.assertEqual(ff.read_sums(p), {"x_merged.bin": "abcd", "y.bin": "ef01"})


class Plan(unittest.TestCase):
    def test_fresh_state_issues_ace_000004_first(self):
        # Caleb 18 Nov 2026 17:03 WAT: highest issued ID is ACE-000003.
        p = ff.plan_serial(MAC, [], [], None)
        self.assertEqual((p["serial"], p["mode"], p["next_counter"]["next"]), ("ACE-000004", "new", 5))
        p = ff.plan_serial(MAC, [], [], None, start_at=4)
        self.assertEqual((p["serial"], p["next_counter"]["next"]), ("ACE-000004", 5))
        p = ff.plan_serial(MAC, [], [], None, start_at=20)
        self.assertEqual(p["serial"], "ACE-000020")

    def test_start_at_below_4_refused(self):
        for n in (0, 1, 2, 3):
            with self.assertRaises(ff.FlashError):
                ff.plan_serial(MAC, [], [], None, start_at=n)
            with self.assertRaises(ff.FlashError):
                ff.plan_serial(MAC, [], [], {"prefix": "ACE-", "next": 1}, start_at=n)
        import contextlib, io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = ff.main(["--model", "pc-dsp011-5kw", "--port", "COM5", "--start-at", "3",
                          "--state", tempfile.mkdtemp(), "--bins", tempfile.mkdtemp()])
        self.assertEqual(rc, 1)
        self.assertIn("--start-at 3 refused", out.getvalue())

    def test_serials_1_to_3_never_issued(self):
        # A real but low counter, units.csv without them, a journal without them: still ACE-000004.
        for c in ({"prefix": "ACE-", "next": 1}, {"prefix": "ACE-", "next": 2}, {"prefix": "ACE-", "next": 3}, None):
            self.assertEqual(ff.plan_serial(MAC, [], [], c)["serial"], "ACE-000004")
        units = [{"serial": "ACE-000004", "device_id": "111111111111"}]
        self.assertEqual(ff.plan_serial(MAC, units, [], {"prefix": "ACE-", "next": 1})["serial"], "ACE-000005")
        # A real counter above 4 is honoured.
        self.assertEqual(ff.plan_serial(MAC, [], [], {"prefix": "ACE-", "next": 12})["serial"], "ACE-000012")
        # factory.py's own counter path skips them too.
        d = tempfile.mkdtemp(); cp = os.path.join(d, "serial_counter.json"); csvp = os.path.join(d, "units.csv")
        self.assertEqual(f.load_counter(cp)["next"], 4)
        for n in (1, 2, 3):
            c = {"prefix": "ACE-", "next": n}
            self.assertEqual(f.next_new_serial(c, csvp), "ACE-000004"); self.assertEqual(c["next"], 4)
        # Pre-issued journal rows have no MAC, so no board reuses them through the journal.
        j = ff.pre_issued_events([])
        self.assertEqual([r["serial"] for r in j], list(f.PRE_ISSUED_SERIALS))
        self.assertTrue(all(r["state"] == "pre_issued" and r["mac"] is None for r in j))
        self.assertEqual(ff.pre_issued_events(j), [])
        p = ff.plan_serial(MAC, [], j, None)
        self.assertEqual((p["serial"], p["mode"]), ("ACE-000004", "new"))

    def test_pre_issued_board_reprovisions_from_units_csv(self):
        units = [{"serial": "ACE-000002", "device_id": "f4:2d:c9:c0:90:a8"}]
        p = ff.plan_serial(MAC, units, ff.pre_issued_events([]), None)
        self.assertEqual((p["serial"], p["mode"], p["next_counter"]), ("ACE-000002", "reuse", None))

    def test_start_at_cannot_go_backwards(self):
        with self.assertRaises(ff.FlashError):
            ff.plan_serial(MAC, [], [], {"prefix": "ACE-", "next": 7}, start_at=3)
        self.assertEqual(ff.plan_serial(MAC, [], [], {"prefix": "ACE-", "next": 7}, start_at=10)["serial"], "ACE-000010")

    def test_next_skips_used_and_burned(self):
        units = [{"serial": "ACE-000004", "device_id": "111111111111"}]
        j = [{"serial": "ACE-000005", "mac": "222222222222", "state": "burned"},
             {"serial": "ACE-000006", "mac": "333333333333", "state": "failed"}]
        p = ff.plan_serial(MAC, units, j, {"prefix": "ACE-", "next": 4})
        self.assertEqual((p["serial"], p["next_counter"]["next"]), ("ACE-000007", 8))

    def test_same_mac_in_units_csv_reuses(self):
        units = [{"serial": "ACE-000001", "device_id": "f4:2d:c9:c0:90:a8"}]
        p = ff.plan_serial(MAC, units, [], {"prefix": "ACE-", "next": 9})
        self.assertEqual((p["serial"], p["mode"], p["next_counter"]), ("ACE-000001", "reuse", None))

    def test_same_mac_in_journal_reuses_unless_burned(self):
        j = [{"serial": "ACE-000008", "mac": MAC, "state": "reserved"},
             {"serial": "ACE-000008", "mac": MAC, "state": "failed"}]
        p = ff.plan_serial(MAC, [], j, {"prefix": "ACE-", "next": 9})
        self.assertEqual((p["serial"], p["mode"]), ("ACE-000008", "reuse"))
        j.append({"serial": "ACE-000008", "mac": MAC, "state": "burned"})
        p = ff.plan_serial(MAC, [], j, {"prefix": "ACE-", "next": 9})
        self.assertEqual((p["serial"], p["mode"]), ("ACE-000009", "new"))

    def test_other_board_never_gets_a_failed_serial(self):
        j = [{"serial": "ACE-000008", "mac": MAC, "state": "failed"}]
        p = ff.plan_serial(MAC2, [], j, {"prefix": "ACE-", "next": 8})
        self.assertEqual(p["serial"], "ACE-000009")


class Journal(unittest.TestCase):
    def test_states_and_view(self):
        d, jp, _, _ = tmpstate()
        for s in ("reserved", "flashed", "provisioned", "registered", "verified"):
            ff.append_journal(jp, {"serial": "ACE-000002", "mac": MAC, "model": "pc-dsp011-5kw", "state": s})
        with self.assertRaises(ValueError):
            ff.append_journal(jp, {"serial": "ACE-000002", "state": "done"})
        v = ff.journal_view(ff.read_journal(jp))["ACE-000002"]
        self.assertEqual(v["state"], "verified")
        self.assertEqual(v["history"], ["reserved", "flashed", "provisioned", "registered", "verified"])
        for line in open(jp, encoding="utf-8"):
            self.assertIn("ts", json.loads(line))

    def test_corrupt_journal_stops(self):
        d, jp, _, _ = tmpstate()
        with open(jp, "w") as fh:
            fh.write("{not json\n")
        with self.assertRaises(ff.FlashError):
            ff.read_journal(jp)

    def test_failure_mid_flash_keeps_serial_tied_to_mac(self):
        d, jp, up, cp = tmpstate()
        f.save_counter(cp, {"prefix": "ACE-", "next": 4})
        p = ff.plan_serial(MAC, [], ff.read_journal(jp), f.load_counter(cp))
        ev = {"serial": p["serial"], "mac": MAC, "model": "pc-dsp011-5kw"}
        ff.append_journal(jp, dict(ev, state="reserved")); f.save_counter(cp, p["next_counter"])
        ff.append_journal(jp, dict(ev, state="failed", note="write_flash: timeout"))
        # Same board again: same serial, counter untouched.
        p2 = ff.plan_serial(MAC, [], ff.read_journal(jp), f.load_counter(cp))
        self.assertEqual((p2["serial"], p2["mode"]), ("ACE-000004", "reuse"))
        self.assertEqual(f.load_counter(cp)["next"], 5)
        # A different board gets the next number, never ACE-000004.
        self.assertEqual(ff.plan_serial(MAC2, [], ff.read_journal(jp), f.load_counter(cp))["serial"], "ACE-000005")

    def test_abandon(self):
        d, jp, up, _ = tmpstate()
        ff.append_journal(jp, {"serial": "ACE-000004", "mac": MAC, "model": "pc-dsp011-5kw", "state": "failed"})
        self.assertTrue(ff.abandon(jp, [], "ACE-000004", "dead board"))
        self.assertFalse(ff.abandon(jp, [], "ACE-000004", ""))
        self.assertEqual(ff.journal_view(ff.read_journal(jp))["ACE-000004"]["state"], "burned")
        self.assertEqual(ff.plan_serial(MAC2, [], ff.read_journal(jp), {"prefix": "ACE-", "next": 4})["serial"], "ACE-000005")
        with self.assertRaises(ff.FlashError):
            ff.abandon(jp, [{"serial": "ACE-000001", "device_id": MAC2}], "ACE-000001", "")
        with self.assertRaises(ff.FlashError):
            ff.abandon(jp, [], "ACE-000099", "")

    def test_lock_is_exclusive(self):
        d, _, _, _ = tmpstate()
        with ff.StateLock(d):
            with self.assertRaises(ff.FlashError):
                with ff.StateLock(d):
                    pass
        with ff.StateLock(d):
            pass
        self.assertFalse(os.path.exists(os.path.join(d, ff.LOCKFILE)))


class Glue(unittest.TestCase):
    def test_factory_args(self):
        a = ff.factory_args("COM5", "ACE-000004", "PC5K", "st", "st/factory.env", True, True)
        self.assertEqual(a, ["COM5", "--serial", "ACE-000004", "--reprovision", "--model", "PC5K",
                             "--state", "st", "--env", "st/factory.env", "--register", "--lock"])
        self.assertNotIn("--register", ff.factory_args("COM5", "ACE-000004", "PC5K", "s", "e", False, False))

    def test_readback(self):
        info = f.parse_info("device_id=%s serial=ACE-000004 secret=set\napi_key=set locked=0\n"
                            "model=PC5K rated_kva=5.0\nhardware_model=pc-dsp011-5kw\n" % MAC)
        self.assertEqual(ff.check_readback(info, "ACE-000004", "pc-dsp011-5kw", "PC5K"), [])
        bad = dict(info, serial="unset", api_key="MISSING", model="unset")
        self.assertEqual(len(ff.check_readback(bad, "ACE-000004", "pc-dsp011-5kw", "PC5K")), 3)

    def test_env_check_never_needs_values_shown(self):
        d = tempfile.mkdtemp(); p = os.path.join(d, "factory.env")
        with self.assertRaises(ff.FlashError):
            ff.check_env(p, False)
        with open(p, "w") as fh:
            fh.write("API_KEY=\nADMIN_KEY=\n")
        with self.assertRaises(ff.FlashError) as e:
            ff.check_env(p, False)
        with open(p, "w") as fh:
            fh.write("API_KEY=k123\nADMIN_KEY=\n")
        ff.check_env(p, False)
        with self.assertRaises(ff.FlashError) as e:
            ff.check_env(p, True)
        self.assertNotIn("k123", str(e.exception))

    def test_dry_run_writes_nothing(self):
        d, jp, up, cp = tmpstate()
        with open(os.path.join(d, "factory.env"), "w") as fh:
            fh.write("API_KEY=k\n")
        bins = tempfile.mkdtemp(); img = os.path.join(bins, "pc-dsp011-5kw_v2.5.36_merged.bin")
        with open(img, "wb") as fh:
            fh.write(b"\x00POWERCELL_FW|collector|pc-dsp011-5kw|2.5.36|END\x00")
        with open(os.path.join(bins, "SHA256SUMS"), "w") as fh:
            fh.write("%s  %s\n" % (ff.sha256_file(img), os.path.basename(img)))
        f.save_counter(cp, {"prefix": "ACE-", "next": 4})
        fake = os.path.join(d, "fake_esptool.py")
        with open(fake, "w") as fh:
            fh.write("import sys\nassert 'read_mac' in sys.argv, sys.argv\nprint('MAC: f4:2d:c9:c0:90:a8')\n")
        rc = ff.main(["--model", "pc-dsp011-5kw", "--port", "COM5", "--state", d, "--bins", bins,
                      "--dry-run", "--esptool", "%s %s" % (sys.executable, fake)])
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(jp))
        self.assertEqual(f.load_counter(cp)["next"], 4)

    def test_fresh_state_journals_pre_issued_then_ace_000004(self):
        d, jp, up, cp = tmpstate()
        with open(os.path.join(d, "factory.env"), "w") as fh:
            fh.write("API_KEY=k\n")
        bins = tempfile.mkdtemp(); img = os.path.join(bins, "pc-dsp011-5kw_v2.5.37_merged.bin")
        with open(img, "wb") as fh:
            fh.write(b"\x00POWERCELL_FW|collector|pc-dsp011-5kw|2.5.37|END\x00")
        with open(os.path.join(bins, "SHA256SUMS"), "w") as fh:
            fh.write("%s  %s\n" % (ff.sha256_file(img), os.path.basename(img)))
        fake = os.path.join(d, "fake_esptool.py")
        with open(fake, "w") as fh:  # read_mac works, erase_flash fails: nothing reaches a board
            fh.write("import sys\nif 'read_mac' in sys.argv:\n    print('MAC: f4:2d:c9:c0:90:a8')\nelse:\n    sys.exit(2)\n")
        argv = ["--model", "pc-dsp011-5kw", "--port", "COM5", "--state", d, "--bins", bins,
                "--esptool", "%s %s" % (sys.executable, fake)]
        self.assertEqual(ff.main(argv), 1)
        view = ff.journal_view(ff.read_journal(jp))
        for s in f.PRE_ISSUED_SERIALS:
            self.assertEqual((view[s]["state"], view[s]["mac"]), ("pre_issued", None))
        self.assertEqual((view["ACE-000004"]["mac"], view["ACE-000004"]["history"]), (MAC, ["reserved", "failed"]))
        self.assertEqual(f.load_counter(cp)["next"], 5)
        # Rerun: same board keeps ACE-000004, pre_issued rows are not duplicated.
        self.assertEqual(ff.main(argv), 1)
        rows = ff.read_journal(jp)
        self.assertEqual(sum(1 for r in rows if r["state"] == "pre_issued"), 3)
        self.assertEqual(f.load_counter(cp)["next"], 5)
        self.assertEqual(ff.journal_view(rows)["ACE-000004"]["history"], ["reserved", "failed", "reserved", "failed"])

    def test_sha_mismatch_refuses_before_board(self):
        d, jp, _, _ = tmpstate()
        with open(os.path.join(d, "factory.env"), "w") as fh:
            fh.write("API_KEY=k\n")
        bins = tempfile.mkdtemp(); img = os.path.join(bins, "pc-dsp011-5kw_v2.5.36_merged.bin")
        with open(img, "wb") as fh:
            fh.write(b"POWERCELL_FW|collector|pc-dsp011-5kw|2.5.36|END")
        with open(os.path.join(bins, "SHA256SUMS"), "w") as fh:
            fh.write("00  %s\n" % os.path.basename(img))
        rc = ff.main(["--model", "pc-dsp011-5kw", "--port", "COM5", "--state", d, "--bins", bins,
                      "--esptool", "false-esptool-should-not-run"])
        self.assertEqual(rc, 1)
        self.assertFalse(os.path.exists(jp))


if __name__ == "__main__":
    unittest.main()
