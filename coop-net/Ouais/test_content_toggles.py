"""Unit and integration tests for Maps & Mods catalog toggles and 60 FPS Cap."""
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import server

class ContentTogglesTest(unittest.TestCase):
    def setUp(self):
        server.app.config["TESTING"] = True
        server.CONTENT_MANIFEST_URL = ""  # Prevent remote network requests during unit test
        self.client = server.app.test_client()
        with server._db_lock:
            conn = server.get_db()
            conn.execute("DELETE FROM local_sessions")
            conn.execute("DELETE FROM user_prefs")
            conn.execute(
                "INSERT INTO user_prefs (username_lower, username, exe_path, exe_md5, exe_verified, friendly_fire, voice_chat, mic_device, max_fps, local_ip, updated_at) "
                "VALUES ('testuser', 'TestUser', '', '', 0, 0, 1, -1, 60, '', '2026-11-26T00:00:00Z')"
            )
            conn.commit()
            conn.close()
        raw_token, csrf_token = server.issue_local_session("cloud_test_token", "testuser")
        self.csrf = csrf_token
        self.cookie = raw_token
        self.client.set_cookie("scct_session", raw_token)

    def test_catalog_contains_fps_cap(self):
        catalog = server._load_content_catalog()
        ids = [p["id"] for p in catalog]
        self.assertIn("fps-cap-60", ids)
        fps_pack = next(p for p in catalog if p["id"] == "fps-cap-60")
        self.assertEqual(fps_pack["name"], "60 FPS Cap")
        self.assertEqual(fps_pack["type"], "builtin")
        self.assertTrue(fps_pack.get("experimental"))

    def test_api_catalog_enabled_states(self):
        res = self.client.get(
            "/api/content/catalog"
        )
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertTrue(data["ok"])
        packs = data["packs"]
        fps_pack = next(p for p in packs if p["id"] == "fps-cap-60")
        self.assertTrue(fps_pack["installed"])
        self.assertTrue(fps_pack["enabled"])
        self.assertEqual(fps_pack["value"], 60)

    def test_toggle_fps_cap_off_and_on(self):
        # Toggle OFF
        res = self.client.post(
            "/api/content/toggle",
            headers={"X-CSRF-Token": self.csrf},
            json={"id": "fps-cap-60", "enabled": False}
        )
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertTrue(data["ok"])
        self.assertFalse(data["enabled"])
        self.assertEqual(data["value"], 0)

        # Check DB
        prefs = server.get_or_create_prefs("testuser")
        self.assertEqual(prefs["max_fps"], 0)

        # Check catalog reflects disabled state
        res2 = self.client.get(
            "/api/content/catalog"
        )
        fps_pack = next(p for p in res2.get_json()["packs"] if p["id"] == "fps-cap-60")
        self.assertFalse(fps_pack["enabled"])
        self.assertEqual(fps_pack["value"], 0)

        # Toggle back ON
        res3 = self.client.post(
            "/api/content/toggle",
            headers={"X-CSRF-Token": self.csrf},
            json={"id": "fps-cap-60", "enabled": True}
        )
        self.assertEqual(res3.status_code, 200)
        self.assertTrue(res3.get_json()["enabled"])
        self.assertEqual(res3.get_json()["value"], 60)
        prefs = server.get_or_create_prefs("testuser")
        self.assertEqual(prefs["max_fps"], 60)

    def test_toggle_widescreen_fix_and_scda_maps(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp_dir:
            # Setup fake game directory
            scripts_dir = os.path.join(tmp_dir, "System", "scripts")
            os.makedirs(scripts_dir, exist_ok=True)
            asi_path = os.path.join(scripts_dir, "SplinterCellChaosTheory.WidescreenFix.asi")
            with open(asi_path, "w") as f:
                f.write("asi content")

            maps_dir = os.path.join(tmp_dir, "Data", "Maps")
            os.makedirs(maps_dir, exist_ok=True)
            mf_path = os.path.join(maps_dir, "scda_pack_manifest.json")
            with open(mf_path, "w") as f:
                json.dump({"maps": ["scda_ice_a", "scda_ice_b"]}, f)

            rot_path = os.path.join(tmp_dir, "System", "MapRotation.ini")
            with open(rot_path, "w") as f:
                f.write("[Coop]\nMissionCoop[0]=01_Lighthouse\n")

            # Fake splintercell3.exe
            exe_path = os.path.join(tmp_dir, "System", "splintercell3.exe")
            with open(exe_path, "w") as f:
                f.write("exe")

            # Link game in user_prefs
            with server._db_lock:
                conn = server.get_db()
                conn.execute(
                    "UPDATE user_prefs SET exe_path=?, exe_md5='fake', exe_verified=1 WHERE username_lower='testuser'",
                    (exe_path,)
                )
                conn.commit()
                conn.close()

            # Create mock receipt for widescreen-fix
            rec_ws = server._receipt_path("widescreen-fix")
            with open(rec_ws, "w") as f:
                json.dump({"id": "widescreen-fix", "version": "1.0", "files": ["System/scripts/SplinterCellChaosTheory.WidescreenFix.asi"], "enabled": True}, f)

            # Create mock receipt for scda-coop-maps
            rec_scda = server._receipt_path("scda-coop-maps")
            with open(rec_scda, "w") as f:
                json.dump({"id": "scda-coop-maps", "version": "1.0", "files": [], "enabled": False}, f)

            try:
                # 1. Toggle widescreen-fix OFF
                res = self.client.post(
                    "/api/content/toggle",
                    headers={"X-CSRF-Token": self.csrf},
                    json={"id": "widescreen-fix", "enabled": False}
                )
                self.assertEqual(res.status_code, 200)
                self.assertFalse(os.path.isfile(asi_path))
                self.assertTrue(os.path.isfile(asi_path + ".disabled"))

                # 2. Toggle widescreen-fix ON
                res2 = self.client.post(
                    "/api/content/toggle",
                    headers={"X-CSRF-Token": self.csrf},
                    json={"id": "widescreen-fix", "enabled": True}
                )
                self.assertEqual(res2.status_code, 200)
                self.assertTrue(os.path.isfile(asi_path))
                self.assertFalse(os.path.isfile(asi_path + ".disabled"))

                # 3. Toggle scda-coop-maps ON (should merge rotation)
                res3 = self.client.post(
                    "/api/content/toggle",
                    headers={"X-CSRF-Token": self.csrf},
                    json={"id": "scda-coop-maps", "enabled": True}
                )
                self.assertEqual(res3.status_code, 200)
                with open(rot_path, "r") as f:
                    rot_content = f.read()
                self.assertIn("MissionCoop[7]", rot_content)

                # 4. Toggle scda-coop-maps OFF (should strip rotation)
                res4 = self.client.post(
                    "/api/content/toggle",
                    headers={"X-CSRF-Token": self.csrf},
                    json={"id": "scda-coop-maps", "enabled": False}
                )
                self.assertEqual(res4.status_code, 200)
                with open(rot_path, "r") as f:
                    rot_content_off = f.read()
                self.assertNotIn("MissionCoop[7]", rot_content_off)

            finally:
                if os.path.isfile(rec_ws):
                    os.remove(rec_ws)
                if os.path.isfile(rec_scda):
                    os.remove(rec_scda)

    def test_toggle_game_running_block(self):
        orig_running = server.any_game_running
        server.any_game_running = lambda: True
        try:
            res = self.client.post(
                "/api/content/toggle",
                headers={"X-CSRF-Token": self.csrf},
                json={"id": "widescreen-fix", "enabled": False}
            )
            # File mod toggles must be blocked with 409 when game is running
            self.assertEqual(res.status_code, 409)
            self.assertIn("Stop the game", res.get_json()["error"])
        finally:
            server.any_game_running = orig_running

if __name__ == "__main__":
    unittest.main()
