import duckdb
from datetime import date, timedelta
from time import perf_counter

start = date.fromisoformat(input("Start date (YYYY-MM-DD): "))
end = date.fromisoformat(input("End date (YYYY-MM-DD): "))
search = r":(?:TMP:2 m above ground|DPT:2 m above ground|UGRD:10 m above ground|VGRD:10 m above ground|PRES:surface|GUST:surface|SNOD:surface|LAI:surface|VEG:surface|VGTYP:surface):"
bbox = [-93.4536, 37.0783, -93.1019, 37.2928]       # lon_min, lat_min, lon_max, lat_max

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-east-1';")
con.execute("LOAD \"../build/release/extension/grib2/grib2.duckdb_extension\"")

days = [start + timedelta(d) for d in range((end - start).days + 1)]
raw = "\nUNION ALL\n".join(f"""
SELECT reference_time, valid_time, variable, level, forecast, lat, lon, i, j, value
FROM read_grib2(
  's3://noaa-hrrr-bdp-pds/hrrr.{d:%Y%m%d}/conus/hrrr.t00z.wrfsfcf00.grib2',
  search := '{search}', bbox := {bbox})""" for d in days)

wide = f"""
WITH raw AS ({raw})
SELECT
  reference_time, valid_time, forecast, i, j, lat, lon,
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
GROUP BY reference_time, valid_time, forecast, i, j, lat, lon
ORDER BY valid_time, i, j
"""
query_started = perf_counter()
df = con.execute(wide).df()
elapsed = perf_counter() - query_started
print("Data Sample:\n\n")
print(df.head(10))
print(f"\n\nRetrieved {len(df)} rows from {start} to {end} for bbox {bbox} in {elapsed:.2f} seconds")