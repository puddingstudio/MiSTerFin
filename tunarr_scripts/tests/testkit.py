"""Tiny shared counter/diff-printer for test_*.py files — not a framework,
just avoids retyping the same ~10 lines in every test file. Mirrors the
CHECK()-macro/t()-helper pattern used in ../../tests/test_*.c."""


class Checker:
    def __init__(self):
        self.checks = 0
        self.failures = 0

    def check(self, cond: bool, label: str, got=None, want=None) -> None:
        self.checks += 1
        if not cond:
            self.failures += 1
            if got is not None or want is not None:
                print(f"  FAIL {label}\n    got  {got!r}\n    want {want!r}")
            else:
                print(f"  FAIL {label}")

    def eq(self, got, want, label: str) -> None:
        self.check(got == want, label, got, want)
