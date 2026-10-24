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
