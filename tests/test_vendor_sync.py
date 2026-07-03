"""The vendored HA copy must stay in sync with the xyaoled library.

Runs the exact checks of tools/check_vendor_sync.py, one pytest case per
check so a drift names the offending constant/builder directly.
"""
import pytest

from check_vendor_sync import build_checks

CHECKS = build_checks()


@pytest.mark.parametrize(
    "lib_value,vendored_value",
    [pytest.param(a, b, id=name) for name, a, b in CHECKS],
)
def test_vendored_matches_library(lib_value, vendored_value):
    assert lib_value == vendored_value
