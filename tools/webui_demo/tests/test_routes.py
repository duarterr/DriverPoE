"""Route wiring smoke test -- catches a POST endpoint whose body model
doesn't resolve (FastAPI then silently treats `body` as a required query
param and every real request 422s). Run from tools/."""
from __future__ import annotations

import unittest

try:
    from webui_demo.app import app
except Exception:  # pragma: no cover - fastapi/numpy not installed
    app = None


@unittest.skipIf(app is None, "webui_demo deps not installed")
class TestRoutes(unittest.TestCase):
    def setUp(self):
        self.schema = app.openapi()  # forces resolution of every route model

    def test_json_post_endpoints_take_a_body(self):
        # every POST that the frontend calls with a JSON body must declare one
        json_body_posts = {
            "/api/scan", "/api/group", "/api/spectrum",
            "/api/identify", "/api/effects/start",
        }
        for path in json_body_posts:
            with self.subTest(path=path):
                self.assertIn("requestBody", self.schema["paths"][path]["post"],
                              f"{path} POST has no request body -- its model didn't resolve")

    def test_spectrum_body_is_the_spectrum_model(self):
        ref = self.schema["paths"]["/api/spectrum"]["post"]["requestBody"] \
            ["content"]["application/json"]["schema"]["$ref"]
        self.assertTrue(ref.endswith("/SpectrumRequest"), ref)


if __name__ == "__main__":
    unittest.main()
