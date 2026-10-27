# Fixtures

Real NOAA National Blend of Models (NBM) data used for validation.

| File | Description |
|---|---|
| `nbm_msg.grib2` | One GRIB2 message (2,099,312 bytes): CONUS 2 m temperature (`TMP:2 m above ground`, 1 hour forecast) |
| `blend.t00z.core.f001.co.grib2.idx` | The `.idx` inventory for the full source file (154 messages) |

## Provenance

- Dataset: [NOAA NBM on AWS Open Data](https://registry.opendata.aws/noaa-nbm/) (`s3://noaa-nbm-grib2-pds`). See that page for terms of use.
- Source file: `blend.20261023/00/core/blend.t00z.core.f001.co.grib2`
- Message: number 133 in the index, bytes 123034124 through 125133435 (the next message starts at 125133436).
- Grid: Lambert conformal, 2345 x 1597 = 3,744,965 points. Packing: `grid_complex_spatial_differencing`.

To recreate it:

```sh
curl -r 123034124-125133435 -o nbm_msg.grib2 \
  https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.20261023/00/core/blend.t00z.core.f001.co.grib2
```

Note: NOAA retains only a limited window of recent NBM runs, so the original file may no longer be on the bucket. The committed copies are the reference.

## What the index shows

The `.idx` file is useful for design work. In this CONUS file, many messages share one variable and level and differ only in a trailing qualifier: percentiles (`50% level`), probability thresholds (`prob >12.7`), or `ens std dev`. Some variables have no wgrib2 name (`var discipline=0 center=7 local_table=1 parmcat=0 parm=206`), and some levels are literally `reserved`.

## Other NBM regions

NBM publishes separate files per region, and they do not share a projection. First message of each f001 core file
(`blend.t00z.core.f001.<region>.grib2`), as reported by `grib2_message_info`:

| Region | Grid type | Ni x Nj | First message | Packing | `alternativeRowScanning` |
|---|---|---|---|---|---|
| `co` (CONUS) | `lambert` | 2345 x 1597 | see above (2 m temperature) | complex + spatial differencing | 1 |
| `ak` (Alaska) | `polar_stereographic` | 1649 x 1105 | `aptmp`, 2 m | complex + spatial differencing | 1 |
| `hi` (Hawaii) | `mercator` | 625 x 561 | `aptmp`, 2 m | complex + spatial differencing | 1 |
| `pr` (Puerto Rico) | `mercator` | 339 x 225 | `aptmp`, 2 m | complex + spatial differencing | 1 |
| `gu` (Guam) | `mercator` | 193 x 193 | `cape`, surface | complex + spatial differencing | 1 |

All regions use the same packing and the same stored-row order as CONUS, so no extra codecs are needed, but the grid
math must cover three projections.

Whole regional files are large (Alaska is about 70 MB), so only single messages are committed. Cut one out of a
downloaded file with:

```sh
python3 scripts/extract_message.py fixtures/nbm/blend.t00z.core.f001.ak.grib2 fixtures/nbm_ak_msg.grib2
```

`fixtures/nbm/` is git-ignored for the whole files.

## Template 5.2 message

`nbm_complex_msg.grib2` (589,028 bytes) is message 52 of the same CONUS file: `PWTHER:surface - reserved:1 hour fcst`,
bytes 55669237 through 56258264 (the next message starts at 56258265). It is the only message in that file that does not
use complex packing with spatial differencing: it uses template 5.2 (`grid_complex`), so it exercises a second packing
path. It is categorical present weather (values 0 to 76), has no bitmap, and ecCodes cannot name it (`shortName` is
`unknown`) because it is an NBM local parameter. The `.idx` name is the readable one.

```sh
curl -r 55669237-56258264 -o nbm_complex_msg.grib2 \
  https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.20261023/00/core/blend.t00z.core.f001.co.grib2
```

## NBM f001 packing survey

`grib2_index(..., details := true)` over the five regional f001 core files:

| File | Messages | Packing | Grid |
|---|---|---|---|
| CONUS | 153 + 1 | complex with spatial differencing, complex | Lambert |
| Alaska | 115 + 1 | same | polar stereographic |
| Guam | 36 + 1 | same | Mercator |
| Hawaii | 74 | complex with spatial differencing | Mercator |
| Puerto Rico | 49 | complex with spatial differencing | Mercator |

Only templates 5.3 and 5.2 occur, so no JPEG 2000 or CCSDS decoding is needed for this product. It is one forecast hour of
the core product; other products and hours may differ.
