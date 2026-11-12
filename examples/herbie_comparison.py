from datetime import date, datetime, timedelta
from tempfile import TemporaryDirectory

import duckdb
import numpy as np
import pandas as pd
import xarray as xr
from herbie import Herbie

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
duckdb_df = con.execute(wide).df()

herbie_frames = []
with TemporaryDirectory(prefix="grib2-herbie-") as herbie_dir:
    for cycle in cycles:
        herbie = Herbie(
            cycle,
            model="hrrr",
            product="sfc",
            fxx=0,
            save_dir=herbie_dir,
            verbose=False,
        )
        inventory = herbie.inventory(search=search, verbose=False)
        forecasts = inventory["forecast_time"].dropna().unique()
        if len(forecasts) != 1:
            raise ValueError(f"Expected one forecast type for {cycle:%Y-%m-%d %HZ}, found {forecasts}")

        with xr.set_options(use_new_combine_kwarg_defaults=True):
            datasets = herbie.xarray(search=search, verbose=False)
        if isinstance(datasets, list):
            datasets = [dataset.drop_vars("heightAboveGround", errors="ignore") for dataset in datasets]
            datasets = xr.merge(datasets, join="exact", compat="no_conflicts")

        required_variables = {"t2m", "d2m", "u10", "v10", "sp", "gust", "sde", "lai", "veg", "vgtyp"}
        missing = required_variables.difference(datasets.data_vars)
        if missing:
            raise ValueError(f"Herbie did not return expected variables for {cycle:%Y-%m-%d %HZ}: {sorted(missing)}")

        datasets = datasets.assign_coords(
            i=("x", np.arange(datasets.sizes["x"])),
            j=("y", np.arange(datasets.sizes["y"])),
        )
        latitude = datasets["latitude"]
        longitude = ((datasets["longitude"] + 180) % 360) - 180
        inside_bbox = (
            (latitude >= bbox[1])
            & (latitude <= bbox[3])
            & (longitude >= bbox[0])
            & (longitude <= bbox[2])
        )
        cropped_latitude = latitude.where(inside_bbox, drop=True)
        cropped_longitude = longitude.where(inside_bbox, drop=True)
        cropped = datasets.where(inside_bbox, drop=True)
        herbie_df = cropped.to_dataframe().reset_index()
        herbie_df["latitude"] = cropped_latitude.values.reshape(-1)
        herbie_df["longitude"] = cropped_longitude.values.reshape(-1)
        herbie_df = herbie_df.loc[
            herbie_df["latitude"].between(bbox[1], bbox[3])
            & herbie_df["longitude"].between(bbox[0], bbox[2])
        ].copy()

        herbie_df["reference_time"] = pd.Timestamp(datasets["time"].values)
        herbie_df["forecast"] = forecasts[0]
        herbie_df["temp_2m_c"] = herbie_df["t2m"] - 273.15
        herbie_df["dewpoint_2m_c"] = herbie_df["d2m"] - 273.15
        herbie_df["u_10m_ms"] = herbie_df["u10"]
        herbie_df["v_10m_ms"] = herbie_df["v10"]
        herbie_df["wind_10m_ms"] = np.hypot(herbie_df["u10"], herbie_df["v10"])
        herbie_df["pressure_hpa"] = herbie_df["sp"] / 100.0
        herbie_df["gust_ms"] = herbie_df["gust"]
        herbie_df["snow_depth_m"] = herbie_df["sde"]
        herbie_df["lai"] = herbie_df["lai"]
        herbie_df["veg_pct"] = herbie_df["veg"]
        herbie_df["veg_type"] = herbie_df["vgtyp"]
        herbie_df = herbie_df.rename(columns={"latitude": "lat", "longitude": "lon"})
        herbie_frames.append(
            herbie_df[
                ["reference_time", "valid_time", "forecast", "i", "j", "lat", "lon", *value_columns]
            ]
        )
    herbie_df = pd.concat(herbie_frames, ignore_index=True)

key_columns = ["reference_time", "valid_time", "forecast", "i", "j"]
compare_columns = [*key_columns, "lat", "lon", *value_columns]
duckdb_result = duckdb_df[compare_columns].copy()
herbie_result = herbie_df[compare_columns].copy()
for result in (duckdb_result, herbie_result):
    for column in ["reference_time", "valid_time"]:
        result[column] = pd.to_datetime(result[column])
    for column in ["i", "j"]:
        result[column] = pd.to_numeric(result[column]).astype("int64")
    result["forecast"] = result["forecast"].astype(str)
duckdb_result = duckdb_result.sort_values(key_columns).reset_index(drop=True)
herbie_result = herbie_result.sort_values(key_columns).reset_index(drop=True)

key_comparison = duckdb_result[key_columns].merge(
    herbie_result[key_columns], how="outer", indicator=True
)
if duckdb_result.duplicated(key_columns).any() or herbie_result.duplicated(key_columns).any():
    raise AssertionError("DuckDB or Herbie returned duplicate cell keys")
if len(duckdb_result) != len(herbie_result) or not key_comparison["_merge"].eq("both").all():
    duckdb_only = key_comparison.loc[key_comparison["_merge"] == "left_only", key_columns].head()
    herbie_only = key_comparison.loc[key_comparison["_merge"] == "right_only", key_columns].head()
    raise AssertionError(
        "DuckDB and Herbie returned different cell keys "
        f"({len(duckdb_result)} DuckDB rows, {len(herbie_result)} Herbie rows).\n"
        f"DuckDB-only keys:\n{duckdb_only}\nHerbie-only keys:\n{herbie_only}"
    )

for column in ["lat", "lon", *value_columns]:
    duckdb_values = duckdb_result[column].to_numpy(dtype=float)
    herbie_values = herbie_result[column].to_numpy(dtype=float)
    matches = np.isclose(duckdb_values, herbie_values, rtol=1e-6, atol=1e-4, equal_nan=True)
    if not matches.all():
        max_difference = np.nanmax(np.abs(duckdb_values - herbie_values))
        raise AssertionError(
            f"{column} differs between DuckDB and Herbie in {(~matches).sum()} rows "
            f"(maximum absolute difference {max_difference})"
        )

print("DuckDB sample:")
print(duckdb_df.head(10))
print(
    f"\nBoth methods returned matching results for {len(duckdb_df)} rows "
    f"from {start} to {end} for bbox {bbox}."
)