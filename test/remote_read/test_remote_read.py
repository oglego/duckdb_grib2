"""Remote reads: read_grib2 and grib2_index over HTTP and over S3, against a local server.

The SQL tests only read local files. These run the built DuckDB CLI with httpfs against test/remote_read/range_server.py,
which stands in for a web server and, in path style, for an S3 bucket. They check that a remote read returns exactly what
a local read does, that only the bytes a query needs are fetched when an .idx is present, that a missing .idx is handled
the way each kind of server reports it (404, and S3's 403 for a key the caller may not list), and that errors from a
remote file are reported rather than crashing.

Needs: the built CLI and extension (``make``), and httpfs, which is installed from DuckDB's extension repository on the
first run (or point GRIB2_HTTPFS at a httpfs.duckdb_extension). Without them the tests are skipped, unless
GRIB2_REQUIRE_EXTENSION is set (CI), when they fail instead.

    python3 -m unittest discover -s test/remote_read -v

Environment: GRIB2_DUCKDB_BIN, GRIB2_EXTENSION, GRIB2_HTTPFS (the same variables test/herbie_compare uses).
"""

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_server import RangeServer  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
CLI = Path(os.environ.get("GRIB2_DUCKDB_BIN", REPO / "build/release/duckdb"))
EXTENSION = Path(os.environ.get("GRIB2_EXTENSION", REPO / "build/release/extension/grib2/grib2.duckdb_extension"))
HTTPFS = os.environ.get("GRIB2_HTTPFS")
REQUIRE = bool(os.environ.get("GRIB2_REQUIRE_EXTENSION"))

# A point on the Puerto Rico grid and one in Missouri on the CONUS grid, both inside the fixtures
PR_POINT = "{'lat': 17.7425, 'lon': -66.4248}"
CONUS_POINT = "{'lat': 37.2090, 'lon': -93.2923}"


def q(text):
    return "'" + str(text).replace("'", "''") + "'"


class Sql:
    """Runs statements in a fresh CLI process and returns the rows of the last one (a SELECT)."""

    def __init__(self):
        self.setup = []
        if HTTPFS:
            self.setup.append("LOAD " + q(HTTPFS))
        else:
            self.setup += ["INSTALL httpfs", "LOAD httpfs"]
        self.setup.append("LOAD " + q(EXTENSION))

    def run(self, query, before=()):
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "out.csv"
            args = [str(CLI), "-unsigned"]
            for stmt in [*self.setup, *before, f"COPY ({query}) TO {q(out)} (FORMAT csv, HEADER true, NULLSTR 'NULL')"]:
                args += ["-c", stmt]
            proc = subprocess.run(args, capture_output=True, text=True, cwd=REPO, timeout=300)
            if proc.returncode != 0:
                if proc.returncode < 0 or "Segmentation" in proc.stderr:
                    raise AssertionError(
                        f"the DuckDB CLI crashed (rc={proc.returncode}) on: {query}\n{proc.stderr[-1500:]}"
                    )
                raise RuntimeError((proc.stderr or proc.stdout).strip())
            with open(out, newline="") as handle:
                return list(csv.reader(handle))


def setUpModule():
    missing = [str(p) for p in (CLI, EXTENSION) if not p.exists()]
    problem = f"not built: {', '.join(missing)}" if missing else None
    if problem is None:
        try:
            Sql().run("SELECT grib2_eccodes_version()")
        except Exception as exc:  # noqa: BLE001 - the reason is shown in the skip or failure
            problem = f"the CLI could not load httpfs and the extension: {exc}"
    if problem:
        if REQUIRE:
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)


class RemoteReadTest(unittest.TestCase):
    def setUp(self):
        self.sql = Sql()
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    # -- files ----------------------------------------------------------------------------------------------------------
    def copy(self, source, name=None):
        target = self.tmp / (name or Path(source).name)
        shutil.copy(REPO / source, target)
        return target

    def make_big_file(self):
        """Three NBM messages (about 2.2 MB) in one file with an .idx: CONUS, Puerto Rico and Hawaii."""
        parts = ["fixtures/nbm_msg.grib2", "fixtures/nbm_pr_msg.grib2", "fixtures/nbm_hi_msg.grib2"]
        names = ["TMP", "APTMP", "DPT"]
        data, offsets, lines = b"", [], []
        for number, part in enumerate(parts, start=1):
            offsets.append(len(data))
            lines.append(f"{number}:{len(data)}:d=2026102300:{names[number - 1]}:2 m above ground:1 hour fcst:")
            data += (REPO / part).read_bytes()
        path = self.tmp / "big.grib2"
        path.write_bytes(data)
        Path(str(path) + ".idx").write_text("\n".join(lines) + "\n")
        return path, offsets, len(data)

    # -- queries --------------------------------------------------------------------------------------------------------
    @staticmethod
    def values_query(path, args=""):
        return (
            "SELECT message_number, variable, forecast, strftime(valid_time, '%Y-%m-%d %H:%M') AS valid_time, "
            f"round(value, 3) AS value FROM read_grib2({q(path)}{args}) ORDER BY message_number"
        )

    def s3(self, port):
        return [
            "CREATE SECRET remote_test (TYPE S3, KEY_ID 'test', SECRET 'test', REGION 'us-east-1', "
            f"ENDPOINT '127.0.0.1:{port}', URL_STYLE 'path', USE_SSL false)"
        ]

    # -- tests ----------------------------------------------------------------------------------------------------------
    def test_http_file_without_sidecar_matches_a_local_read(self):
        """A server that answers 404 for the missing .idx: the file is walked message by message."""
        local = self.copy("test/data/valid_times.grib2", "plain.grib2")
        args = f", points := [{{'lat': 38.5, 'lon': -97.5}}]"
        expected = self.sql.run(self.values_query(local, args))
        self.assertEqual(len(expected), 9, "eight messages and a header")
        with RangeServer(self.tmp, missing_status=404) as server:
            got = self.sql.run(self.values_query(server.url + "/plain.grib2", args))
            self.assertEqual(got, expected)
            self.assertTrue(any(r[1] == "plain.grib2.idx" and r[3] == 404 for r in server.log), "the .idx was probed")

    def test_http_sidecar_403_is_treated_as_no_sidecar(self):
        """S3 answers 403 for a missing key when the caller may not list the bucket; the file is still readable."""
        local = self.copy("test/data/valid_times.grib2", "plain.grib2")
        args = ", points := [{'lat': 38.5, 'lon': -97.5}]"
        expected = self.sql.run(self.values_query(local, args))
        with RangeServer(self.tmp, missing_status=403) as server:
            got = self.sql.run(self.values_query(server.url + "/plain.grib2", args))
            self.assertEqual(got, expected)

    def test_http_sidecar_401_is_reported(self):
        """An authentication failure on the .idx is not "no .idx": the query must fail, not quietly walk the file."""
        self.copy("test/data/valid_times.grib2", "plain.grib2")
        with RangeServer(self.tmp, missing_status=401) as server:
            with self.assertRaises(RuntimeError) as caught:
                self.sql.run(
                    self.values_query(server.url + "/plain.grib2", f", message := 1, points := [{CONUS_POINT}]")
                )
            self.assertIn("401", str(caught.exception))

    def test_s3_file_without_sidecar_when_the_bucket_cannot_be_listed(self):
        """The real httpfs S3 code path, against a server that answers 403 for the missing .idx like a private bucket."""
        local = self.copy("test/data/valid_times.grib2", "plain.grib2")
        args = ", points := [{'lat': 38.5, 'lon': -97.5}]"
        expected = self.sql.run(self.values_query(local, args))
        with RangeServer(self.tmp, prefix="/bucket", missing_status=403) as server:
            got = self.sql.run(self.values_query("s3://bucket/plain.grib2", args), before=self.s3(server.port))
            self.assertEqual(got, expected)

    def test_s3_file_with_sidecar(self):
        path, _, _ = self.make_big_file()
        args = f", message := 2, points := [{PR_POINT}]"
        expected = self.sql.run(self.values_query(path, args))
        self.assertEqual(len(expected), 2)
        with RangeServer(self.tmp, prefix="/bucket") as server:
            got = self.sql.run(self.values_query("s3://bucket/big.grib2", args), before=self.s3(server.port))
            self.assertEqual(got, expected)

    def test_only_the_selected_message_is_fetched_when_there_is_an_idx(self):
        path, offsets, size = self.make_big_file()
        pr_length = offsets[2] - offsets[1]
        args = f", message := 2, points := [{PR_POINT}]"
        expected = self.sql.run(self.values_query(path, args))
        with RangeServer(self.tmp) as server:
            got = self.sql.run(self.values_query(server.url + "/big.grib2", args))
            self.assertEqual(got, expected)
            served = server.data_bytes_served("big.grib2")
            detail = "\n".join(map(str, server.requests_for("big.grib2")))
            self.assertGreaterEqual(served, pr_length, f"the message itself must have been fetched\n{detail}")
            self.assertLess(served, 0.9 * size, f"fetched {served} of {size} bytes for one message\n{detail}")

    def test_remote_index_matches_a_local_index(self):
        path, _, _ = self.make_big_file()
        query = lambda target: (  # noqa: E731
            "SELECT message_number, byte_offset, byte_length, variable, forecast, step_hours "
            f"FROM grib2_index({q(target)}) ORDER BY message_number"
        )
        expected = self.sql.run(query(path))
        self.assertEqual(len(expected), 4)
        with RangeServer(self.tmp) as server:
            self.assertEqual(self.sql.run(query(server.url + "/big.grib2")), expected)

    def test_background_prefetch_gives_the_same_rows(self):
        """grib2_io_threads > 1 fetches messages on other threads; the rows must match a single-threaded local read."""
        path, _, _ = self.make_big_file()
        args = f", points := [{PR_POINT}, {CONUS_POINT}, {{'lat': 22.005, 'lon': -154.4903}}]"
        expected = self.sql.run(self.values_query(path, args))
        self.assertGreater(len(expected), 3)
        with RangeServer(self.tmp) as server:
            got = self.sql.run(self.values_query(server.url + "/big.grib2", args), before=["SET grib2_io_threads = 4"])
            self.assertEqual(got, expected)

    def test_a_truncated_remote_file_is_an_error_not_a_crash(self):
        data = (REPO / "fixtures/nbm_pr_msg.grib2").read_bytes()
        (self.tmp / "cut.grib2").write_bytes(data[: len(data) // 2])
        with RangeServer(self.tmp) as server:
            with self.assertRaises(RuntimeError) as caught:
                self.sql.run(self.values_query(server.url + "/cut.grib2", f", message := 1, points := [{CONUS_POINT}]"))
            self.assertIn("truncat", str(caught.exception).lower())

    def test_a_missing_remote_file_is_an_error(self):
        with RangeServer(self.tmp) as server:
            with self.assertRaises(RuntimeError):
                self.sql.run(self.values_query(server.url + "/not_there.grib2", ", message := 1"))


if __name__ == "__main__":
    unittest.main()
