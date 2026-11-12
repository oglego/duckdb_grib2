"""Why does the DuckDB wide table have a different number of columns than the Herbie one?

Reads ONE HRRR cycle both ways and compares which grid cells each method keeps:
  * Herbie "inside":     cells whose centre is inside the extent (what read_grib2 returns)
  * Herbie "rectangle":  every grid row and column that touches the extent (what ds.where(..., drop=True) keeps; the grid
                         is slightly rotated, so this includes a few cells just outside the extent)
  * DuckDB:              read_grib2 with bbox
Then it prints how many wide-table columns each choice would give, and which one reaches your target.
"""
from datetime import datetime

import numpy as np

# ---- edit these -----------------------------------------------------------------------------------------------------
CYCLE = datetime(2026, 11, 10, 0)  # any HRRR cycle that exists
TARGET_COLUMNS = 881  # the column count you expect, including the timestamp
bbox = [-93.4536, 37.0783, -93.1019, 37.2928]  # lon_min, lat_min, lon_max, lat_max
search = r":(?:TMP:2 m above ground|DPT:2 m above ground|UGRD:10 m above ground|VGRD:10 m above ground|PRES:surface|GUST:surface|SNOD:surface|LAI:surface|VEG:surface|VGTYP:surface):"
EXTENSION = "../build/release/extension/grib2/grib2.duckdb_extension"
HERBIE_VARIABLES = 10  # t2m, d2m, u10, v10, sp, gust, sde, lai, veg, vgtyp
DUCKDB_VARIABLES = 11  # the same ten plus the derived wind speed (si10)
# ----------------------------------------------------------------------------------------------------------------------


def coordinate_name(value):
    """3 decimals, trailing zeros dropped: 37.130 -> '37.13'."""
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return text or "0"


def cell_names(lat, lon):
    """One 'lat_lon' name per cell, longitude on a 0-360 scale."""
    lon360 = np.mod(np.asarray(lon, dtype=float), 360.0)
    return {f"{coordinate_name(a)}_{coordinate_name(b)}" for a, b in zip(np.ravel(lat), np.ravel(lon360))}


def herbie_selections(lat2d, lon2d, bbox):
    """Cells inside the extent, and the rectangle of grid rows/columns that touches it."""
    lon180 = ((lon2d + 180.0) % 360.0) - 180.0
    inside = (lat2d >= bbox[1]) & (lat2d <= bbox[3]) & (lon180 >= bbox[0]) & (lon180 <= bbox[2])
    rows = inside.any(axis=1)
    cols = inside.any(axis=0)
    rectangle = np.zeros_like(inside)
    rectangle[np.ix_(rows, cols)] = True
    return inside, rectangle


def nearest_distance(lat, lon, other_lat, other_lon):
    """Distance in degrees from each (lat, lon) to the closest of the other cells."""
    lat, lon = np.asarray(lat, float), np.mod(np.asarray(lon, float), 360.0)
    other_lat, other_lon = np.asarray(other_lat, float), np.mod(np.asarray(other_lon, float), 360.0)
    return np.array([np.min(np.hypot(other_lat - a, other_lon - b)) for a, b in zip(lat, lon)])


def report(herbie_inside, herbie_rectangle, duck_names, duck_lat, duck_lon, lat2d, lon2d):
    n_inside, n_rectangle, n_duck = int(herbie_inside.sum()), int(herbie_rectangle.sum()), len(duck_names)
    print("\n=== Cells kept ===")
    print(f"Herbie, cells inside the extent        : {n_inside}")
    print(f"Herbie, rectangle that touches extent  : {n_rectangle}   (extra cells outside the extent: {n_rectangle - n_inside})")
    print(f"DuckDB read_grib2 with bbox            : {n_duck}")

    print("\n=== Wide-table columns (1 timestamp + cells x variables) ===")
    options = {
        f"Herbie rectangle, {HERBIE_VARIABLES} variables": 1 + n_rectangle * HERBIE_VARIABLES,
        f"Herbie inside,    {HERBIE_VARIABLES} variables": 1 + n_inside * HERBIE_VARIABLES,
        f"DuckDB,           {DUCKDB_VARIABLES} variables (with wind speed)": 1 + n_duck * DUCKDB_VARIABLES,
        f"DuckDB,           {HERBIE_VARIABLES} variables (wind speed removed)": 1 + n_duck * HERBIE_VARIABLES,
    }
    for label, columns in options.items():
        print(f"{label:62s}: {columns:6d}{'   <-- matches your target' if columns == TARGET_COLUMNS else ''}")
    if TARGET_COLUMNS not in options.values():
        print(f"None of these is {TARGET_COLUMNS}. If you also have {TARGET_COLUMNS - 1} = cells x variables, "
              f"check that for divisors (e.g. {TARGET_COLUMNS - 1} / {HERBIE_VARIABLES} = {(TARGET_COLUMNS - 1) / HERBIE_VARIABLES}).")

    print("\n=== Does the naming agree? (same cells, compared as names) ===")
    inside_names = cell_names(lat2d[herbie_inside], lon2d[herbie_inside])
    rectangle_only = herbie_rectangle & ~herbie_inside
    print(f"Herbie inside vs DuckDB : only in Herbie {len(inside_names - duck_names)}, only in DuckDB {len(duck_names - inside_names)}")
    if inside_names != duck_names:
        left_over = sorted(duck_names - inside_names)[:5]
        print("  e.g. only in DuckDB:", left_over, "| only in Herbie:", sorted(inside_names - duck_names)[:5])
        if duck_names - inside_names:
            keep = [n in (duck_names - inside_names) for n in [f"{coordinate_name(a)}_{coordinate_name(b)}"
                    for a, b in zip(duck_lat, np.mod(duck_lon, 360.0))]]
            distances = nearest_distance(np.array(duck_lat)[keep], np.array(duck_lon)[keep],
                                         lat2d[herbie_inside], lon2d[herbie_inside])
            print(f"  nearest Herbie cell to those DuckDB cells: max {distances.max():.6f} degrees "
                  f"({'a rounding/naming difference only' if distances.max() < 0.005 else 'genuinely different cells (neighbouring HRRR cells are about 0.027 degrees apart)'})")
    if rectangle_only.any():
        extra_lat, extra_lon = lat2d[rectangle_only], np.mod(lon2d[rectangle_only], 360.0)
        print(f"\nExtra cells in the Herbie rectangle (outside the extent), first 5 of {len(extra_lat)}:")
        for a, b in list(zip(extra_lat, extra_lon))[:5]:
            print(f"  lat {a:.4f}  lon360 {b:.4f}  (extent: lat {bbox[1]}..{bbox[3]}, lon360 {bbox[0] % 360:.4f}..{bbox[2] % 360:.4f})")


def main():
    import duckdb
    import xarray as xr
    from herbie import Herbie

    herbie = Herbie(CYCLE, model="hrrr", product="sfc", fxx=0, verbose=False)
    with xr.set_options(use_new_combine_kwarg_defaults=True):
        datasets = herbie.xarray(search=search, verbose=False)
    if isinstance(datasets, list):
        datasets = [d.drop_vars("heightAboveGround", errors="ignore") for d in datasets]
        datasets = xr.merge(datasets, join="exact", compat="no_conflicts")
    variables = sorted(set(datasets.data_vars))
    print(f"Herbie returned {len(variables)} variables: {variables}")
    lat2d, lon2d = datasets["latitude"].values, datasets["longitude"].values
    inside, rectangle = herbie_selections(lat2d, lon2d, bbox)

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-east-1';")
    con.execute(f'LOAD "{EXTENSION}"')
    rows = con.execute(f"""
        SELECT DISTINCT lat, lon FROM read_grib2(
          's3://noaa-hrrr-bdp-pds/hrrr.{CYCLE:%Y%m%d}/conus/hrrr.t{CYCLE:%H}z.wrfsfcf00.grib2',
          search := '{search}', bbox := {bbox})""").fetchall()
    duck_lat, duck_lon = [r[0] for r in rows], [r[1] for r in rows]
    duck_names = cell_names(duck_lat, duck_lon)
    report(inside, rectangle, duck_names, duck_lat, duck_lon, lat2d, lon2d)


if __name__ == "__main__":
    main()
