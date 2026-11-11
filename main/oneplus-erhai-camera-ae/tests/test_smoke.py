import unittest


class Smoke(unittest.TestCase):
    def test_package_imports(self):
        import camera_ae  # noqa: F401
