#!/usr/bin/env bash
# Download real HRRR messages as test fixtures for duckdb_grib2, by byte range, and check each one.
#
# What it fetches (the committed fixtures, under the same names, see fixtures/README.md "HRRR provenance"):
#   1. fixtures/hrrr_msg90_csnow_simple_const.grib2    message 90, CSNOW, template 5.0, a constant field
#   2. fixtures/hrrr_msg166_icec_spatial.grib2         message 166, ICEC, template 5.3, a constant field
#   3. fixtures/hrrr_msg70_snod_spatial.grib2          message 70, SNOD, template 5.3, real data
#   4. optionally, a simple-packing message with real data when one is small enough. This one is NOT committed: the
#      repo has no real varying field with template 5.0 (see fixtures/README.md).
# Each of 1 to 3 is looked up by message number AND variable AND packing template, so a run whose numbering differs
# stops with an explanation instead of saving some other message under the fixture's name. Each is then compared with
# the copy committed in git, so with the default DATE and CYCLE you should see "identical to the committed fixture".
#
# Usage, from anywhere inside the repo:
#   bash fetch_hrrr_fixtures.sh
#
# Optional environment:
#   DUCKDB=path/to/duckdb      the shell with the extension (default: searches build/{release,reldebug,debug}, then PATH)
#   GRIB2_EXT=path/to/grib2.duckdb_extension   only if the shell does not have the extension built in
#   DATE=20260602              HRRR date
#   CYCLE=00                   HRRR hourly cycle
#   MAX_REAL_BYTES=1500000     skip the 5.0 real-data fixture if the chosen message is bigger than this
#
# It overwrites the files above in the working tree. It stops at the first problem. Nothing is committed for you.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
if [ ! -d fixtures ] || [ ! -f CMakeLists.txt ]; then
  echo "error: $ROOT does not look like the duckdb_grib2 repo (no fixtures/ or CMakeLists.txt)" >&2
  exit 1
fi

DATE="${DATE:-20260602}"
CYCLE="${CYCLE:-00}"
FILE="hrrr.t${CYCLE}z.wrfsfcf00.grib2"
HTTPS_URL="https://noaa-hrrr-bdp-pds.s3.amazonaws.com/hrrr.${DATE}/conus/${FILE}"
S3_URL="s3://noaa-hrrr-bdp-pds/hrrr.${DATE}/conus/${FILE}"
MAX_REAL_BYTES="${MAX_REAL_BYTES:-1500000}"
OUT_DIR="fixtures"

# --- Find a DuckDB shell that has the grib2 extension -------------------------------------------------------------
# Tries $DUCKDB, then the usual build outputs under the repo, then `duckdb` on the PATH.
find_duckdb() {
  local c
  for c in "${DUCKDB:-}" "$ROOT/build/release/duckdb" "$ROOT/build/reldebug/duckdb" "$ROOT/build/debug/duckdb" \
           "$(command -v duckdb 2>/dev/null || true)"; do
    if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}

if ! DUCKDB="$(find_duckdb)"; then
  cat >&2 <<MSG
error: could not find a DuckDB shell. Looked for \$DUCKDB, $ROOT/build/{release,reldebug,debug}/duckdb, and duckdb on PATH.
Find yours (the one where grib2_index works) with:
    find "$ROOT" -name duckdb -type f -perm -u+x 2>/dev/null | grep -v '\.git/'
then run:
    DUCKDB=/full/path/to/duckdb bash $0
If you only have a plain duckdb plus a built extension file, also set GRIB2_EXT=/path/to/grib2.duckdb_extension
MSG
  exit 1
fi

# The extension file, in case the shell does not have it built in
if [ -z "${GRIB2_EXT:-}" ]; then
  for c in "$ROOT"/build/release/extension/grib2/grib2.duckdb_extension "$ROOT"/build/reldebug/extension/grib2/grib2.duckdb_extension \
           "$ROOT"/build/debug/extension/grib2/grib2.duckdb_extension; do
    if [ -f "$c" ]; then GRIB2_EXT="$c"; break; fi
  done
fi

USE_LOAD=0
if "$DUCKDB" -csv -noheader -c "SELECT grib2_eccodes_version()" >/dev/null 2>&1; then
  :
elif [ -n "${GRIB2_EXT:-}" ] && "$DUCKDB" -unsigned -cmd "LOAD '${GRIB2_EXT}'" -csv -noheader -c "SELECT grib2_eccodes_version()" >/dev/null 2>&1; then
  USE_LOAD=1
else
  echo "error: $DUCKDB does not have the grib2 extension (grib2_eccodes_version() failed)." >&2
  echo "       Set GRIB2_EXT=/path/to/grib2.duckdb_extension, or DUCKDB=/path/to/the/shell/you/use." >&2
  exit 1
fi
echo "using DuckDB: $DUCKDB$( [ "$USE_LOAD" = 1 ] && echo " (loading $GRIB2_EXT)" )"

# Run SQL through the DuckDB shell: no header, '|' separated
q() {
  local sql="$1"
  if [ "$USE_LOAD" = 1 ]; then
    "$DUCKDB" -unsigned -cmd "LOAD '${GRIB2_EXT}'" -csv -noheader -separator '|' -c "${sql}"
  else
    "$DUCKDB" -csv -noheader -separator '|' -c "${sql}"
  fi
}

# grab OFFSET LENGTH OUTFILE  : fetch bytes [OFFSET, OFFSET+LENGTH-1] and verify the result is one whole GRIB2 message
grab() {
  local offset="$1" length="$2" out="$3"
  curl -fsS -r "${offset}-$((offset + length - 1))" -o "$out" "$HTTPS_URL"
  local size start end
  size="$(wc -c < "$out" | tr -d ' ')"
  start="$(head -c 4 "$out")"
  end="$(tail -c 4 "$out")"
  if [ "$size" != "$length" ] || [ "$start" != "GRIB" ] || [ "$end" != "7777" ]; then
    echo "error: $out is not a whole message (size=$size expected=$length start=$start end=$end)" >&2
    rm -f "$out"
    exit 1
  fi
  echo "ok   $out  ${size} bytes"
}

echo "== Looking for a 5.0 message with real data (this reads the .idx and message headers)"
# Prefer message 77 if it qualifies; otherwise the smallest 5.0 message that is not a constant field
REAL_ROW="$(q "SELECT message_number, variable, level, byte_offset, byte_length
               FROM grib2_index('${S3_URL}', details := true)
               WHERE packing_template = 0 AND byte_length > 1000 AND byte_length <= ${MAX_REAL_BYTES}
               ORDER BY (message_number = 77) DESC, byte_length
               LIMIT 1")"
echo "candidate (msg|variable|level|offset|length): ${REAL_ROW:-none}"
echo "message 77 as indexed:"
q "SELECT message_number, variable, level, packing_template, byte_offset, byte_length
   FROM grib2_index('${S3_URL}', details := true) WHERE message_number = 77"

mkdir -p "$OUT_DIR"
echo
echo "== Downloading"

# download_fixture MESSAGE VARIABLE PACKING OUTFILE
# Fetch one message, only if the inventory says message MESSAGE really is VARIABLE with packing template PACKING.
download_fixture() {
  local want_msg="$1" variable="$2" packing="$3" outfile="$4"
  local row offset length msg
  row="$(q "SELECT message_number, byte_offset, byte_length
            FROM grib2_index('${S3_URL}', details := true)
            WHERE message_number = ${want_msg} AND variable = '${variable}' AND packing_template = ${packing}")"
  if [ -z "$row" ]; then
    echo "error: message ${want_msg} of ${FILE} (${DATE}) is not ${variable} with packing template ${packing}." >&2
    echo "       The message numbering differs for this DATE/CYCLE. The inventory around it:" >&2
    q "SELECT message_number, variable, level, packing_template, byte_length
       FROM grib2_index('${S3_URL}', details := true)
       WHERE variable = '${variable}' OR message_number = ${want_msg}
       ORDER BY message_number" >&2
    exit 1
  fi
  IFS='|' read -r msg offset length <<EOF
$row
EOF
  echo "selected ${variable}: msg ${msg}, offset ${offset}, length ${length}"
  grab "$offset" "$length" "$outfile"
  # Compare with the copy committed in git, when there is one
  if git cat-file -e "HEAD:${outfile}" 2>/dev/null; then
    if git show "HEAD:${outfile}" | cmp -s - "$outfile"; then
      echo "     identical to the committed fixture"
    else
      echo "     DIFFERS from the committed fixture: the source data for ${DATE} ${CYCLE}z may have been replaced" >&2
    fi
  else
    echo "     (no committed copy to compare with)"
  fi
}

download_fixture 90 CSNOW 0 "${OUT_DIR}/hrrr_msg90_csnow_simple_const.grib2"
download_fixture 166 ICEC 3 "${OUT_DIR}/hrrr_msg166_icec_spatial.grib2"
download_fixture 70 SNOD 3 "${OUT_DIR}/hrrr_msg70_snod_spatial.grib2"

REAL_FILE=""
if [ -n "$REAL_ROW" ]; then
  IFS='|' read -r R_MSG R_VAR R_LEVEL R_OFFSET R_LENGTH <<EOF
$REAL_ROW
EOF
  REAL_FILE="${OUT_DIR}/hrrr_msg${R_MSG}_$(echo "$R_VAR" | tr 'A-Z' 'a-z')_simple.grib2"
  grab "$R_OFFSET" "$R_LENGTH" "$REAL_FILE"
else
  echo "note: no 5.0 message between 1000 B and ${MAX_REAL_BYTES} B; raise MAX_REAL_BYTES to try again"
fi

echo
echo "== Checking each file with the extension"
FILES="${OUT_DIR}/hrrr_msg90_csnow_simple_const.grib2 ${OUT_DIR}/hrrr_msg166_icec_spatial.grib2 ${OUT_DIR}/hrrr_msg70_snod_spatial.grib2"
[ -n "$REAL_FILE" ] && FILES="$FILES $REAL_FILE"
for f in $FILES; do
  echo "--- $f"
  echo "index  (variable|level|grid_template|packing_template|packing_type):"
  q "SELECT variable, level, grid_template, packing_template, packing_type FROM grib2_index('${f}', details := true)"
  echo "decode (cells|non-null|min|max):"
  q "SELECT count(*), count(value), min(value), max(value) FROM read_grib2('${f}', all_cells := true)"
done

echo
echo "== Done"
ls -l ${OUT_DIR}/hrrr_*.grib2
