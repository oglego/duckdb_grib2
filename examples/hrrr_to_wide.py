from datetime import date, datetime, timedelta
from time import perf_counter

import duckdb

start = date.fromisoformat(input("Start date (YYYY-MM-DD): "))
end = date.fromisoformat(input("End date (YYYY-MM-DD): "))
if end < start:
    raise ValueError("End date must be on or after start date")

search = r":(?:TMP:2 m above ground|DPT:2 m above ground|UGRD:10 m above ground|VGRD:10 m above ground|PRES:surface|GUST:surface|SNOD:surface|LAI:surface|VEG:surface|VGTYP:surface):"
bbox = [-93.4536, 37.0783, -93.1019, 37.2928]       # lon_min, lat_min, lon_max, lat_max

# --- Matching the Herbie script -------------------------------------------------------------------------------------
# Herbie's ds.where(inside_extent, drop=True) keeps every grid row and column that touches the extent. HRRR's grid is
# slightly rotated, so that rectangle holds a few cells just OUTSIDE the extent (4 of 88 for this extent).
KEEP_HERBIE_RECTANGLE = True      # True: 88 cells like Herbie. False: only the 84 cells inside the extent.
EXTRA_CELL_VALUES = "values"      # cells outside the extent: "values" keeps their real values, "nan" leaves them empty
INCLUDE_WIND_SPEED = False        # the derived wind speed is not a Herbie field; leave False to match its 10 variables
RECTANGLE_PAD_DEGREES = 0.05      # how far past the extent to read when looking for the rectangle's cells

value_columns = [
    "temp_2m_c", "dewpoint_2m_c", "u_10m_ms", "v_10m_ms",
    "pressure_hpa", "gust_ms", "snow_depth_m", "lai", "veg_pct", "veg_type",
]
if INCLUDE_WIND_SPEED:
    value_columns.insert(4, "wind_10m_ms")
if EXTRA_CELL_VALUES not in ("values", "nan"):
    raise ValueError('EXTRA_CELL_VALUES must be "values" or "nan"')

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-east-1';")
con.execute("LOAD \"../build/release/extension/grib2/grib2.duckdb_extension\"")


def tracked_memory_mb():
    return con.execute("SELECT coalesce(sum(memory_usage_bytes), 0) FROM duckdb_memory()").fetchone()[0] / 1e6


def grib_path(cycle):
    return f"s3://noaa-hrrr-bdp-pds/hrrr.{cycle:%Y%m%d}/conus/hrrr.t{cycle:%H}z.wrfsfcf00.grib2"


padded_bbox = [bbox[0] - RECTANGLE_PAD_DEGREES, bbox[1] - RECTANGLE_PAD_DEGREES,
               bbox[2] + RECTANGLE_PAD_DEGREES, bbox[3] + RECTANGLE_PAD_DEGREES]

# The cells to keep are the same for every cycle (same grid), so work them out once from the first cycle:
# `inside` are the cells in the extent; the rectangle is every grid row and column that has an inside cell.
first_cycle = datetime.combine(start, datetime.min.time())
keep_condition = (
    "cand.j IN (SELECT j FROM inside) AND cand.i IN (SELECT i FROM inside)" if KEEP_HERBIE_RECTANGLE
    else "inside.i IS NOT NULL"
)
sql_started = perf_counter()
con.execute(f"""
CREATE TABLE cell_rectangle AS
WITH cand AS (
  SELECT DISTINCT i, j, lat, lon
  FROM read_grib2('{grib_path(first_cycle)}', search := '{search}', bbox := {padded_bbox})
),
inside AS (
  SELECT i, j FROM cand
  WHERE lat BETWEEN {bbox[1]} AND {bbox[3]} AND lon BETWEEN {bbox[0]} AND {bbox[2]}
)
SELECT cand.i, cand.j, inside.i IS NOT NULL AS inside
FROM cand LEFT JOIN inside ON cand.i = inside.i AND cand.j = inside.j
WHERE {keep_condition}
""")
n_cells, n_i, n_j, n_inside = con.execute(
    "SELECT count(*), count(DISTINCT i), count(DISTINCT j), count(*) FILTER (WHERE inside) FROM cell_rectangle"
).fetchone()
if KEEP_HERBIE_RECTANGLE and n_cells != n_i * n_j:
    raise ValueError(f"The padded read did not cover the whole rectangle ({n_cells} of {n_i * n_j} cells); "
                     "raise RECTANGLE_PAD_DEGREES")
print(f"Keeping {n_cells} cells ({n_inside} inside the extent, {n_cells - n_inside} just outside)", flush=True)
cell_value = "r.value" if EXTRA_CELL_VALUES == "values" else "CASE WHEN c.inside THEN r.value END"


def day_query(day):
    """One day = 24 cycles = 24 read_grib2 calls, reduced to one row per cell and time."""
    cycles = [datetime.combine(day, datetime.min.time()).replace(hour=hour) for hour in range(24)]
    raw = "\nUNION ALL\n".join(f"""
SELECT r.valid_time, r.variable, r.level, r.lat, r.lon, {cell_value} AS value
FROM read_grib2('{grib_path(cycle)}', search := '{search}', bbox := {padded_bbox}) r
JOIN cell_rectangle c ON r.i = c.i AND r.j = c.j""" for cycle in cycles)
    return f"""
WITH raw AS ({raw})
SELECT
  valid_time, lat, lon,
  max(value) FILTER (WHERE variable='TMP'   AND level='2 m above ground')  - 273.15 AS temp_2m_c,
  max(value) FILTER (WHERE variable='DPT'   AND level='2 m above ground')  - 273.15 AS dewpoint_2m_c,
  max(value) FILTER (WHERE variable='UGRD'  AND level='10 m above ground')          AS u_10m_ms,
  max(value) FILTER (WHERE variable='VGRD'  AND level='10 m above ground')          AS v_10m_ms,
  sqrt(u_10m_ms ** 2 + v_10m_ms ** 2)                                               AS wind_10m_ms,
  max(value) FILTER (WHERE variable='PRES'  AND level='surface') / 100.0            AS pressure_hpa,
  max(value) FILTER (WHERE variable='GUST'  AND level='surface')                    AS gust_ms,
  max(value) FILTER (WHERE variable='SNOD'  AND level='surface')                    AS snow_depth_m,
  max(value) FILTER (WHERE variable='LAI'   AND level='surface')                    AS lai,
  max(value) FILTER (WHERE variable='VEG'   AND level='surface')                    AS veg_pct,
  max(value) FILTER (WHERE variable='VGTYP' AND level='surface')                    AS veg_type
FROM raw
GROUP BY valid_time, lat, lon
"""


# Read each day once, into a table. Every GRIB message is fetched and decoded exactly once, and no query ever has
# more than 24 read_grib2 calls in it.
days = [start + timedelta(d) for d in range((end - start).days + 1)]
for n, day in enumerate(days):
    con.execute(("CREATE TABLE grid_values AS " if n == 0 else "INSERT INTO grid_values ") + day_query(day))
    rows = con.execute("SELECT count(*) FROM grid_values").fetchone()[0]
    print(f"{day}: {rows} rows so far, DuckDB tracked memory {tracked_memory_mb():.0f} MB", flush=True)

# The wide table is built from the stored rows: no GRIB reads happen here.
#
# One pivot key per (variable, lat, lon), not "ON variable, lat, lon". With several ON columns DuckDB makes a column for
# every combination of their distinct values, not just the combinations that exist. On a curved grid like HRRR's every
# cell has its own latitude and its own longitude, so 11 variables x 84 latitudes x 84 longitudes is 77,616 columns
# (only 924 of them ever hold data), which runs out of memory.
#
# Column names look like d2m_37.092_266.891: the cfgrib short name, the latitude, and the longitude on a 0-360 scale,
# each coordinate rounded to 3 decimals with trailing zeros dropped (37.130 -> 37.13).
short_names = {
    "temp_2m_c": "t2m", "dewpoint_2m_c": "d2m", "u_10m_ms": "u10", "v_10m_ms": "v10", "wind_10m_ms": "si10",
    "pressure_hpa": "sp", "gust_ms": "gust", "snow_depth_m": "sde", "lai": "lai", "veg_pct": "veg", "veg_type": "vgtyp",
}


def coordinate(expression):
    return f"rtrim(rtrim(printf('%.3f', {expression}), '0'), '.')"


# Two different cells must not share a name, or max() would quietly merge them
distinct_cells, distinct_names = con.execute(
    "SELECT count(DISTINCT (lat, lon)), count(DISTINCT (round(lat, 3), round(lon, 3))) FROM grid_values"
).fetchone()
if distinct_cells != distinct_names:
    raise ValueError(f"Rounding to 3 decimals merges {distinct_cells - distinct_names} cells; use more decimals")

name_case = "CASE variable " + " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in short_names.items()) + " END"
unpivot_list = ", ".join(value_columns)
duckdb_df = con.execute(f"""
SELECT *
FROM (
  PIVOT (
    SELECT valid_time,
           {name_case} || '_' || {coordinate("lat")} || '_' || {coordinate("((lon % 360) + 360) % 360")} AS cell_column,
           value
    FROM grid_values UNPIVOT INCLUDE NULLS (value FOR variable IN ({unpivot_list}))
  )
  ON cell_column
  USING max(value)
  GROUP BY valid_time
)
ORDER BY valid_time
""").df()
print(f"SQL processing completed in {perf_counter() - sql_started:.2f} seconds")
print(f"{duckdb_df.shape[1]} columns including valid_time, {len(duckdb_df)} rows")
print(duckdb_df.iloc[:5, :6])
duckdb_df.to_csv(f"hrrr_{start:%Y%m%d}_{end:%Y%m%d}.csv", index=False)
