from datetime import date, datetime, timedelta
from tempfile import TemporaryDirectory
from time import perf_counter

import duckdb
import numpy as np
import pandas as pd

start = date.fromisoformat(input("Start date (YYYY-MM-DD): "))
end = date.fromisoformat(input("End date (YYYY-MM-DD): "))
if end < start:
    raise ValueError("End date must be on or after start date")

search = r":(?:TMP:2 m above ground|DPT:2 m above ground|UGRD:10 m above ground|VGRD:10 m above ground|PRES:surface|GUST:surface|SNOD:surface|LAI:surface|VEG:surface|VGTYP:surface):"
bbox = [-93.4536, 37.0783, -93.1019, 37.2928]       # lon_min, lat_min, lon_max, lat_max
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-east-1';")
con.execute("LOAD \"../build/release/extension/grib2/grib2.duckdb_extension\"")

cycles = [
    datetime.combine(start + timedelta(days=day_offset), datetime.min.time()).replace(hour=hour)
    for day_offset in range((end - start).days + 1)
    for hour in range(24)
]

raw = "\nUNION ALL\n".join(f"""
SELECT reference_time, valid_time, variable, level, forecast, lat, lon, i, j, value
FROM read_grib2(
  's3://noaa-hrrr-bdp-pds/hrrr.{cycle:%Y%m%d}/conus/hrrr.t{cycle:%H}z.wrfsfcf00.grib2',
  search := '{search}', bbox := {bbox})""" for cycle in cycles)

query = f"""
WITH raw AS ({raw}),
grid_values AS (
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
),
hourly AS (
  SELECT
    valid_time,
    list(struct_pack(
      lat := lat,
      lon := lon,
      temp_2m_c := temp_2m_c,
      dewpoint_2m_c := dewpoint_2m_c,
      u_10m_ms := u_10m_ms,
      v_10m_ms := v_10m_ms,
      wind_10m_ms := wind_10m_ms,
      pressure_hpa := pressure_hpa,
      gust_ms := gust_ms,
      snow_depth_m := snow_depth_m,
      lai := lai,
      veg_pct := veg_pct,
      veg_type := veg_type
    ) ORDER BY lat, lon) AS grid
  FROM grid_values
  GROUP BY valid_time
)
SELECT valid_time, grid
FROM hourly
ORDER BY valid_time
"""
query_started = perf_counter()
duckdb_df = con.execute(query).df()
duckdb_elapsed = perf_counter() - query_started
print(f"DuckDB query completed in {duckdb_elapsed:.2f} seconds, retrieved {len(duckdb_df)} rows")
print(duckdb_df.head())
duckdb_df.to_csv("duckdb_grib2.csv", index=False)