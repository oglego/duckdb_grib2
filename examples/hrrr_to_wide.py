from datetime import date, datetime, timedelta

import duckdb

start = date.fromisoformat(input("Start date (YYYY-MM-DD): "))
end = date.fromisoformat(input("End date (YYYY-MM-DD): "))
if end < start:
    raise ValueError("End date must be on or after start date")

search = r":(?:TMP:2 m above ground|DPT:2 m above ground|UGRD:10 m above ground|VGRD:10 m above ground|PRES:surface|GUST:surface|SNOD:surface|LAI:surface|VEG:surface|VGTYP:surface):"
bbox = [-93.4536, 37.0783, -93.1019, 37.2928]       # lon_min, lat_min, lon_max, lat_max
value_columns = [
    "temp_2m_c", "dewpoint_2m_c", "u_10m_ms", "v_10m_ms", "wind_10m_ms",
    "pressure_hpa", "gust_ms", "snow_depth_m", "lai", "veg_pct", "veg_type",
]

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-east-1';")
con.execute("LOAD \"../build/release/extension/grib2/grib2.duckdb_extension\"")


def tracked_memory_mb():
    return con.execute("SELECT coalesce(sum(memory_usage_bytes), 0) FROM duckdb_memory()").fetchone()[0] / 1e6


def day_query(day):
    """One day = 24 cycles = 24 read_grib2 calls, reduced to one row per cell and time."""
    cycles = [datetime.combine(day, datetime.min.time()).replace(hour=hour) for hour in range(24)]
    raw = "\nUNION ALL\n".join(f"""
SELECT valid_time, variable, level, lat, lon, value
FROM read_grib2(
  's3://noaa-hrrr-bdp-pds/hrrr.{cycle:%Y%m%d}/conus/hrrr.t{cycle:%H}z.wrfsfcf00.grib2',
  search := '{search}', bbox := {bbox})""" for cycle in cycles)
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
unpivot_list = ", ".join(value_columns)
duckdb_df = con.execute(f"""
SELECT *
FROM (
  PIVOT (
    SELECT valid_time, printf('%s_%.4f_%.4f', variable, lat, lon) AS cell_column, value
    FROM grid_values UNPIVOT (value FOR variable IN ({unpivot_list}))
  )
  ON cell_column
  USING max(value)
  GROUP BY valid_time
)
ORDER BY valid_time
""").df()
print(duckdb_df.shape)
print(duckdb_df.iloc[:5, :6])
