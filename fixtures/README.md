# GRIB2 fixtures

This directory contains compact, real NOAA messages used by the tests. The full regional GRIB2 files are much larger and
are not needed to run the test suite. They may be downloaded locally when you need to extract another message; files under
`fixtures/nbm/` are ignored by Git.

## Committed files

| File | Purpose |
|---|---|
| `nbm_msg.grib2` | NBM CONUS 2 m temperature; Lambert grid, packing template 5.3, alternating row scanning. |
| `nbm_complex_msg.grib2` | NBM CONUS categorical present weather; packing template 5.2. |
| `nbm_ak_msg.grib2` | NBM Alaska; north polar stereographic grid. |
| `nbm_hi_msg.grib2` | NBM Hawaii; Mercator grid. |
| `nbm_pr_msg.grib2` | NBM Puerto Rico; Mercator grid. |
| `nbm_gu_msg.grib2` | NBM Guam; Mercator grid. |
| `blend.t00z.core.f001.co.grib2.idx` | NOAA inventory for the complete CONUS NBM file; used to validate `.idx` parsing. |
| `hrrr_msg90_csnow_simple_const.grib2` | HRRR constant field using simple packing (5.0). |
| `hrrr_msg166_icec_spatial.grib2` | HRRR sea-ice cover using complex packing with spatial differencing (5.3), 2 bits per value. |
| `hrrr_msg70_snod_spatial.grib2` | HRRR snow depth using complex packing with spatial differencing (5.3), 17 bits per value. |

The tests also use synthetic GRIB2 files under `test/data/` for malformed inputs, bitmaps, missing values, and packing
edge cases. See [test/README.md](../test/README.md) for the test data inventory.

## NBM provenance

The NBM messages come from [NOAA NBM on AWS Open Data](https://registry.opendata.aws/noaa-nbm/), source file
`blend.20261023/00/core/blend.t00z.core.f001.co.grib2`.

`nbm_msg.grib2` is message 133, bytes 123034124–125133435 (2,099,312 bytes). Its grid is Lambert conformal,
2345 × 1597 cells, and its packing is `grid_complex_spatial_differencing` (template 5.3). The index fixture is the
source file's NOAA `.idx` inventory. NOAA retains only a limited window of recent runs, so these compact copies are the
stable test references.

`nbm_complex_msg.grib2` is message 52, bytes 55669237–56258264 (589,028 bytes). It uses template 5.2 and contains
categorical present weather. ecCodes reports its short name as `unknown`; the NOAA inventory names it `PWTHER`.

The regional fixtures are the first message of the corresponding f001 core files:

| Region | Grid | Dimensions | First message |
|---|---|---:|---|
| CONUS | Lambert conformal | 2345 × 1597 | 2 m temperature (message 133 in the captured source file) |
| Alaska | North polar stereographic | 1649 × 1105 | `aptmp`, 2 m |
| Hawaii | Mercator | 625 × 561 | `aptmp`, 2 m |
| Puerto Rico | Mercator | 339 × 225 | `aptmp`, 2 m |
| Guam | Mercator | 193 × 193 | `cape`, surface |

All five NBM regions use spherical earth definitions and alternating row scanning. The `.idx` inventory and full regional
files are not required to use the committed message fixtures.

If you already have a full regional file, extract its first message with:

```sh
python3 scripts/extract_message.py fixtures/nbm/blend.t00z.core.f001.ak.grib2 fixtures/nbm_ak_msg.grib2
```

To recreate the CONUS temperature sample while the captured NOAA run is still available:

```sh
curl -r 123034124-125133435 -o fixtures/nbm_msg.grib2 \
  https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.20261023/00/core/blend.t00z.core.f001.co.grib2
```

## HRRR provenance

The HRRR messages come from the [NOAA HRRR open data bucket](https://registry.opendata.aws/noaa-hrrr-pds/), source file
`hrrr.20260602/conus/hrrr.t00z.wrfsfcf00.grib2`. All three use the Lambert conformal grid (1799 × 1059), a spherical
earth with radius 6,371,229 m, and non-alternating row scanning.

| Fixture | Message | Bytes in source | Values |
|---|---:|---:|---|
| `hrrr_msg90_csnow_simple_const.grib2` | 90 | 52,114,409–52,114,596 (188 bytes) | Constant `CSNOW` surface field; template 5.0. |
| `hrrr_msg166_icec_spatial.grib2` | 166 | 138,576,038–138,576,294 (257 bytes) | `ICEC` surface; template 5.3, 2 bits per value. |
| `hrrr_msg70_snod_spatial.grib2` | 70 | 36,517,782–36,539,864 (22,083 bytes) | `SNOD` surface; template 5.3, 17 bits per value. |

[`scripts/fetch_hrrr_fixtures.sh`](../scripts/fetch_hrrr_fixtures.sh) downloads and verifies these messages. NOAA retains
only a limited window of HRRR runs, so committed copies provide stable test inputs.

The HRRR fixtures include a real constant field using template 5.0, but not a real varying field with that packing. The
synthetic `test/data/simple_*.grib2` files cover varying simple-packed values; they are re-encoded windows of the real
snow-depth field. Other HRRR products and domains have not been qualified.
