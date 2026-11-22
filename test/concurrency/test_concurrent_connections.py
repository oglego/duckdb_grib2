"""Threads reading through the extension at the same time, in one process.

The SQL logic tests run statements one after another, even on separate connections, so they cannot show a race. These
tests run several DuckDB connections on separate threads against one database, all calling read_grib2, grib2_index,
grib2_message_info and grib2_point_value together, and compare every result with a single-threaded run. A race in the
extension, or in ecCodes when it is not thread-safe and the locking guard is missing, shows up as a wrong result, an
error or a crash.

Needs the Python ``duckdb`` package at the same version the extension is built for (``pip install duckdb==2.0.0``) and
the built extension (``make``). Without them the tests are skipped, unless GRIB2_REQUIRE_EXTENSION is set (CI), when
they fail instead.

    python3 -m unittest discover -s test/concurrency -v

Environment: GRIB2_EXTENSION (path to grib2.duckdb_extension), GRIB2_CONCURRENCY_THREADS, GRIB2_CONCURRENCY_ROUNDS.
"""

import os
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXTENSION = Path(os.environ.get("GRIB2_EXTENSION", REPO / "build/release/extension/grib2/grib2.duckdb_extension"))
REQUIRE = bool(os.environ.get("GRIB2_REQUIRE_EXTENSION"))
THREADS = int(os.environ.get("GRIB2_CONCURRENCY_THREADS", "8"))
ROUNDS = int(os.environ.get("GRIB2_CONCURRENCY_ROUNDS", "12"))

# name -> SQL. Each touches ecCodes differently: header-only indexing, full decodes on several grids, the scalar functions
# (which DuckDB itself runs on several threads), and read_grib2 with more than one message.
WORKLOADS = {
    "point_conus": (
        "SELECT round(value, 3), i, j FROM read_grib2('fixtures/nbm_msg.grib2', "
        "points := [{'lat': 37.2090, 'lon': -93.2923}])"
    ),
    "bbox_guam": "SELECT count(*), round(sum(value), 1) FROM read_grib2('fixtures/nbm_gu_msg.grib2', bbox := [-180, -90, 180, 90])",
    "bbox_alaska": (
        "SELECT count(*), round(sum(value), 1) FROM read_grib2('fixtures/nbm_ak_msg.grib2', bbox := [-150, 60, -148, 61])"
    ),
    "multi_grid_points": (
        "SELECT message_number, round(value, 3) FROM read_grib2('test/data/multi.grib2', "
        "points := [{'lat': 17.7425, 'lon': -66.4248}, {'lat': 22.005, 'lon': -154.4903}]) ORDER BY message_number"
    ),
    "index_with_details": (
        "SELECT message_number, byte_offset, byte_length, grid_template, packing_template "
        "FROM grib2_index('test/data/multi.grib2', details := true) ORDER BY message_number"
    ),
    "scalar_info": ("SELECT grib2_message_info(content) FROM read_blob('fixtures/nbm_msg.grib2')"),
    "scalar_point_value": (
        "SELECT round(grib2_point_value(content, 37.2090, -93.2923), 3) FROM read_blob('fixtures/nbm_msg.grib2')"
    ),
    "complex_packing": (
        "SELECT count(*), round(sum(value), 1) FROM read_grib2('fixtures/nbm_complex_msg.grib2', "
        "bbox := [-100, 35, -99, 36])"
    ),
}


def run_concurrently(make_cursor, workloads, expected, threads, rounds):
    """Run every workload ``rounds`` times on each of ``threads`` threads (each with its own cursor), starting together.

    Returns a list of problems: a wrong result or an exception, with the thread and workload it happened on.
    """
    problems = []
    lock = threading.Lock()
    barrier = threading.Barrier(threads)
    names = list(workloads)

    def worker(index):
        cursor = make_cursor()
        barrier.wait()  # all threads start at once
        for round_number in range(rounds):
            # a different starting point per thread, so different workloads overlap
            for step in range(len(names)):
                name = names[(index + round_number + step) % len(names)]
                try:
                    got = cursor.execute(workloads[name]).fetchall()
                except Exception as exc:  # noqa: BLE001 - reported with the workload, not hidden
                    with lock:
                        problems.append(f"thread {index} round {round_number} {name}: {type(exc).__name__}: {exc}")
                    continue
                if got != expected[name]:
                    with lock:
                        problems.append(
                            f"thread {index} round {round_number} {name}: got {got!r}, expected {expected[name]!r}"
                        )

    with ThreadPoolExecutor(max_workers=threads) as pool:
        for future in [pool.submit(worker, i) for i in range(threads)]:
            future.result()
    return problems


def setUpModule():
    global CON, EXPECTED
    problem = None
    try:
        import duckdb
    except ImportError:
        problem = (
            "the python duckdb package is not installed (pip install duckdb==2.0.0, the version the extension targets)"
        )
    if problem is None and not EXTENSION.exists():
        problem = f"extension not built: {EXTENSION}"
    if problem is None:
        try:
            CON = duckdb.connect(config={"allow_unsigned_extensions": "true"})
            CON.execute(f"LOAD '{EXTENSION}'")
            CON.execute("SET threads = 4")
        except Exception as exc:  # noqa: BLE001 - a version mismatch is the usual cause
            problem = f"could not load {EXTENSION} into python duckdb {duckdb.__version__}: {exc}"
    if problem:
        if REQUIRE:
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)
    os.chdir(REPO)
    EXPECTED = {name: CON.cursor().execute(sql).fetchall() for name, sql in WORKLOADS.items()}


class ConcurrentConnectionsTest(unittest.TestCase):
    def test_expected_results_are_sane(self):
        """A guard on the test itself: every workload returns something, so a comparison cannot pass on empty results."""
        for name, rows in EXPECTED.items():
            self.assertTrue(rows, f"{name} returned no rows")
        self.assertEqual(EXPECTED["bbox_guam"][0][0], 37249)
        for name in ("bbox_alaska", "complex_packing"):
            self.assertGreater(EXPECTED[name][0][0], 0, f"{name} selected no cells, so it would test nothing")

    def test_many_connections_running_every_workload_get_the_single_threaded_answers(self):
        problems = run_concurrently(CON.cursor, WORKLOADS, EXPECTED, THREADS, ROUNDS)
        self.assertEqual(problems, [], "\n".join(problems[:10]))

    def test_the_same_workload_on_many_connections_at_once(self):
        for name in ("point_conus", "complex_packing", "index_with_details"):
            problems = run_concurrently(CON.cursor, {name: WORKLOADS[name]}, EXPECTED, THREADS, ROUNDS)
            self.assertEqual(problems, [], f"{name}\n" + "\n".join(problems[:10]))

    def test_a_failing_query_on_one_thread_does_not_disturb_the_others(self):
        workloads = dict(WORKLOADS)
        workloads["bad_file"] = "SELECT * FROM read_grib2('test/data/truncated.grib2', all_cells := true)"
        expected = dict(EXPECTED)
        problems = run_concurrently(CON.cursor, workloads, {**expected, "bad_file": None}, THREADS, 4)
        # the bad query must fail on every thread and every round; nothing else may go wrong
        others = [p for p in problems if " bad_file:" not in p]
        self.assertEqual(others, [], "\n".join(others[:10]))
        failed = [p for p in problems if " bad_file:" in p]
        self.assertEqual(len(failed), THREADS * 4, "the truncated file must be an error every time")
        self.assertTrue(all("Error" in p for p in failed), failed[:2])


if __name__ == "__main__":
    unittest.main()
