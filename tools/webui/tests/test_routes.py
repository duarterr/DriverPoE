"""Route wiring smoke test -- catches a POST endpoint whose body model
doesn't resolve (FastAPI then silently treats `body` as a required query
param and every real request 422s). Run from tools/."""
from __future__ import annotations

import unittest

try:
    from webui.app import app
except Exception:  # pragma: no cover - fastapi not installed
    app = None


@unittest.skipIf(app is None, "webui deps not installed")
class TestRoutes(unittest.TestCase):
    def setUp(self):
        self.schema = app.openapi()  # forces resolution of every route model

    def test_json_post_endpoints_take_a_body(self):
        json_body_posts = {
            "/api/devices/{ip}/on", "/api/devices/{ip}/off", "/api/devices/{ip}/dim",
            "/api/devices/{ip}/identify", "/api/devices/{ip}/reboot",
            "/api/devices/{ip}/factory_reset", "/api/devices/{ip}/change_secret",
            "/api/devices/{ip}/dmx",
        }
        for path in json_body_posts:
            with self.subTest(path=path):
                post = self.schema["paths"][path]["post"]
                self.assertIn("requestBody", post,
                              f"{path} POST has no request body -- its model didn't resolve")

    def test_keys_upload_is_multipart(self):
        rb = self.schema["paths"]["/api/keys"]["post"]["requestBody"]
        self.assertIn("multipart/form-data", rb["content"])


if __name__ == "__main__":
    unittest.main()
