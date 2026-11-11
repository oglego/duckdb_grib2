# Running the example

The loadable extension and the DuckDB Python package must have matching version identifiers. To run this example with
the pinned DuckDB `2.0.0` package, build the extension from the repository root with the release version override:

```sh
OVERRIDE_GIT_DESCRIBE=v2.0.0 make
```

Then, from this directory, sync the Python environment and run the example:

```sh
uv sync
uv run example.py
```

Without the override, a source build identifies itself as a development version (for example, `v2.0.0-dev88614`), which
cannot be loaded by the released `2.0.0` Python package.