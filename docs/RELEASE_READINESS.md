# Production and community-extension release checklist

**This project is not yet ready to be presented as an official DuckDB community extension for general production use.**
Do not describe the extension as production-ready or publish it for general use until every release gate below is
complete and its evidence is reviewed.

- [ ] **Define and verify the supported meteorological data matrix.** Document supported sources, GRIB2 grid templates,
  Earth shapes, scanning modes, packing templates, and forecast/product types. The repository has real fixture and SQL coverage for NBM
  forecast-hour-1 core products and for the HRRR CONUS surface file (three real messages, and the HRRR Lambert grid in the
  projection oracle). Remaining: a real HRRR message packed as 5.0 with varying values (the 5.0 fixture is a constant
  field), and any other HRRR product or domain, which stays out of the supported matrix until tested. Keep unsupported
  combinations out of production claims.
- [x] **Validate bounding boxes and bound search matching.** Bounding boxes now reject non-finite coordinates and
  longitudes outside -360 to 360 before grid selection. `search` now uses DuckDB's RE2 engine, avoiding
  backtracking-time blowups; regression tests cover invalid boxes and unsupported regex syntax.
- [x] **Recognise cells with no data without relying on 9999.** ecCodes fills them with 9999 however the message marks
  them. The extension used to treat 9999 as missing only when a bitmap was present, which returned cells flagged by
  complex packing (5.2, 5.3, no bitmap) as real 9999s, and would have masked genuine values of exactly 9999. It now has
  ecCodes decode them as NaN. Fixtures and tests cover both cases (`test/cpp/missing_value_check.cpp`, SQL tests). Not
  yet exercised against the ecCodes the extension vendors (2.47.0); the CI check uses Ubuntu's ecCodes.
- [ ] **Test query resource limits under adversarial inputs.** Exercise documented size, memory, cancellation, and
  concurrency limits with hostile or malformed inputs.
- [ ] **Qualify decoding of untrusted data.** The local `.idx` and section parsers have sanitizer/fuzz coverage, and
  only packing templates 5.0, 5.2 and 5.3 are allowed through to ecCodes (all others are refused up front). ecCodes'
  full-message decoder is still only mutation-tested against the distribution's ecCodes rather than the vendored
  version, and may crash on malformed input. Fuzzing the vendored build is deferred (it needs ecCodes built from source
  in CI). Establish a supported ecCodes
  version/update policy, exercise malformed-message cases, and document an agreed risk/containment policy before
  recommending use with data from untrusted sources.
- [ ] **Finish operational release documentation.** Document the stable function/API compatibility policy, supported
  DuckDB versions, upgrade/release process, and maintainer contact/reporting path; keep the install instructions and
  limitations aligned with the published binary.
