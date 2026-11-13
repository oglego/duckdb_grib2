# Production and community-extension release checklist

**This project is not yet ready to be presented as an official DuckDB community extension for general production use.**
Do not describe the extension as production-ready or publish it for general use until every release gate below is
complete and its evidence is reviewed.

- [ ] **Define and verify the supported meteorological data matrix.** Document supported sources, GRIB2 grid templates,
  Earth shapes, scanning modes, packing templates, and forecast/product types. Expand independent-oracle tests beyond
  the current NBM forecast-hour-1 fixtures for every combination claimed as supported; explicitly keep unsupported
  combinations out of production claims until tested.
- [x] **Validate bounding boxes and bound search matching.** Bounding boxes now reject non-finite coordinates and
  longitudes outside -360 to 360 before grid selection. `search` now uses DuckDB's RE2 engine, avoiding
  backtracking-time blowups; regression tests cover invalid boxes and unsupported regex syntax.
- [ ] **Test query resource limits under adversarial inputs.** Exercise documented size, memory, cancellation, and
  concurrency limits with hostile or malformed inputs.
- [ ] **Qualify decoding of untrusted data.** The local `.idx` and section parsers have sanitizer/fuzz coverage, but
  ecCodes' full-message decoder is not fuzzed here and may crash on malformed input. Establish a supported ecCodes
  version/update policy, exercise malformed-message cases, and document an agreed risk/containment policy before
  recommending use with data from untrusted sources.
- [ ] **Verify release builds and end-to-end behavior on every advertised platform.** Require the distribution
  pipeline and SQL tests to pass for the supported DuckDB versions and architectures, and add repeatable coverage for
  remote `httpfs` reads, missing/stale sidecars, and failure/cancellation behavior. Publish the exact tested
  DuckDB/ecCodes versions and platform exclusions.
- [ ] **Finish operational release documentation.** Document the stable function/API compatibility policy, supported
  DuckDB versions, upgrade/release process, and maintainer contact/reporting path; keep the install instructions and
  limitations aligned with the published binary.
