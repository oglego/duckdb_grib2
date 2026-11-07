"""pytest configuration for the DuckDB-vs-Herbie comparison suite.

    pytest test/herbie_compare                 # needs the built extension (see client.py for how it is found)
    pytest test/herbie_compare --runslow       # also compares every cell of the large CONUS / Alaska grids
    pytest test/herbie_compare -m network      # live NBM bucket tests (needs httpfs + internet)
    GRIB2_ORACLE=1 pytest test/herbie_compare  # self-check of the suite against a stand-in (see oracle.py)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from client import Grib2Client, Grib2Unavailable  # noqa: E402
from reference import GRIDS, RefCache  # noqa: E402


def pytest_addoption(parser):
    parser.addoption("--runslow", action="store_true", default=False, help="compare every cell of the large grids")


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: full-grid comparisons of the 3.7M-cell CONUS and 1.8M-cell Alaska grids")
    config.addinivalue_line("markers", "network: needs httpfs and access to the public NBM bucket")


def pytest_collection_modifyitems(config, items):
    if not config.getoption("--runslow"):
        skip = pytest.mark.skip(reason="needs --runslow")
        for item in items:
            if "slow" in item.keywords:
                item.add_marker(skip)
    if not os.environ.get("GRIB2_NETWORK"):
        skip = pytest.mark.skip(reason="set GRIB2_NETWORK=1 (and GRIB2_HTTPFS) to run network tests")
        for item in items:
            if "network" in item.keywords:
                item.add_marker(skip)


@pytest.fixture(scope="session")
def refs():
    cache = RefCache()
    yield cache
    cache.close()


@pytest.fixture(scope="session")
def rg(refs):
    """The client under test: the compiled extension, or the oracle stand-in when GRIB2_ORACLE is set."""
    if os.environ.get("GRIB2_ORACLE"):
        from oracle import OracleClient

        return OracleClient(refs)
    try:
        return Grib2Client.discover()
    except Grib2Unavailable as exc:
        pytest.skip(str(exc))


@pytest.fixture(scope="session")
def grid(refs):
    """grid('hi') -> (RefField, path) for one of the fixtures in reference.GRIDS."""

    def get(name):
        path, mno, _proj = GRIDS[name]
        return refs.get(path, mno), str(path)

    return get


GRID_NAMES = list(GRIDS)
SMALL_GRIDS = ["gu", "pr", "hi", "gu_masked"]
