# AGENTS.md

Guidance for AI coding agents working in `mariadb-datalake` (the DuckDB + Apache Iceberg / Apache Paimon storage engine plugin for MariaDB).

## 1. Architecture & Two-Layer Build System

All engine code lives in namespace `myduck` (and `myduck::lakehouse` for lakehouse components), except `ha_duckdb` which is in global scope per MariaDB handler convention.

The build system (`CMakeLists.txt`) supports two modes:

### Layer A — Standalone Mode (No MariaDB SDK Required)
Enabled via `-DSTANDALONE_TESTS=ON` (or automatically when `MYSQL_SOURCE_DIR` is not defined and `find_package(mariadb-plugin)` is skipped). Builds:
- `libduckdb_bundle.a` (`cmake/duckdb.cmake` — DuckDB `third_parties/duckdb` + `parquet`, `avro`, `iceberg`, `httpfs`, `quack`)
- `libpaimon.a` (`cmake/paimon.cmake` — `third_parties/paimon-cpp` + bundled Arrow)
- `libduckdb_lakehouse.a` (`lakehouse/CMakeLists.txt`)
- `libquack_guard.a` (`quack_guard/CMakeLists.txt`)
- 8 standalone CTest executables: `quack_guard_test`, `mariadb_dialect_test`, `fiber_context_test`, `smoke_test`, `connection_test`, `paimon_scan_test`, `iceberg_catalog_test`, `lakehouse_e2e_test`

```bash
# Configure, build, and run all 8 Layer A test suites
cmake -S . -B build -DSTANDALONE_TESTS=ON -DDUCKDB_WERROR=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

> **Fast Out-of-Tree / Worktree Builds:** When `build/duckdb-build`, `build/duckdb-deps-install`, and `build/paimon-build` already exist in the main repository's `build/` directory, any alternate build directory (`build-asan`, `build-ubsan`, `/tmp/build-*`) or git worktree automatically reuses those prebuilt third-party archives (`cmake/duckdb.cmake`, `cmake/paimon.cmake`). Never delete `build/duckdb-build` or `build/paimon-build` unless upgrading submodules.

### Layer B — Full MariaDB Plugin Mode (`ha_duckdb.so`)
Builds `ha_duckdb.so` against an installed MariaDB plugin SDK (`share/cmake/mariadb-plugin/`):

```bash
# Build only (no install)
./build.sh -c -t Debug

# Build + install + start MariaDB, preserving existing datadir (-n)
./build.sh -n -S -t RelWithDebInfo

# Run MTR test suite against installed plugin
./run_mtr.sh -a
```

## 2. Module Map

| Directory / File | Responsibility |
|---|---|
| `ha_duckdb.cc`, `ha_duckdb.h` | MariaDB `handler` implementation (`create`, `open`, `delete_table`, `rename_table`, `write_row`, `update_row`, `delete_row`, `rnd_next`). Enforces `HA_ERR_TABLE_READONLY` on lakehouse tables (`is_lakehouse_table`) and rolls back orphaned DuckDB views if `create()` fails after view creation. |
| `ha_duckdb_pushdown.cc`, `ha_duckdb_pushdown.h` | `select_handler` / `unit_handler` query pushdown. Runs lexical preflight (`extract_source_query()`) and forwards SELECT queries via single-parse MariaDB PEG AST handoff (`duckdb_stream_mariadb_query()`). |
| `ha_duckdb_quack.cc`, `ha_duckdb_quack.h` | Lifecycle, audit logging, and alignment-safe `SHOW STATUS` callbacks for the Quack Flight SQL server. |
| `lakehouse/include/lakehouse/` | Public headers for URI parsing (`connection_string.h`), Paimon Arrow stream (`paimon_ffi_bridge.h`), `paimon_scan` registration (`paimon_table_function.h`), and Iceberg catalog registry (`lakehouse_catalog.h`). |
| `lakehouse/src/connection_string.cc` | `ParseLakehouseUri()`, `ValidateLakehousePath()`, `EscapeDuckDBSqlLiteral()`, and `ExtractLakehouseUri()`. Parses `paimon://`, `iceberg://`, and `iceberg+rest://` URIs and blocks null bytes, control chars, relative paths, and `..` traversal. |
| `lakehouse/src/paimon_ffi_bridge.cc` | `PaimonArrowArrayStream`: wraps `paimon::BatchReader` in an Arrow C Data Interface `ArrowArrayStream` (`get_schema`, `get_next`, `get_last_error`, `release`). |
| `lakehouse/src/paimon_table_function.cc` | Registers `paimon_scan(VARCHAR)` on `duckdb::DatabaseInstance`, translates `duckdb::TableFilterSet` to `paimon::PredicateBuilder`, pushes projection `column_ids`, and delegates execution to `duckdb::ArrowTableFunction::ArrowScanFunction`. |
| `lakehouse/src/lakehouse_catalog.cc` | Thread-safe (`std::shared_mutex`) singleton `LakehouseCatalog::Instance()` managing named catalog options and resolving `iceberg+rest://` table metadata via `libcurl` + `yyjson` (enforcing `https://` for non-loopback hosts). |
| `convertor/ddl_convertor.cc`, `convertor/dml_convertor.cc` | Translates MariaDB `TABLE` / `Alter_info` structures into DuckDB DDL/DML. Emits `CREATE OR REPLACE VIEW` (`paimon_scan` / `iceberg_scan`) and `DROP VIEW IF EXISTS` for lakehouse tables. |
| `runtime/duckdb_manager.cc` | Singleton owning `duckdb::DuckDB`, static extension loading (`duckdb_register_static_extensions`), `RegisterPaimonScan(*m_db->instance)`, and MariaDB PEG dialect warm-up. |
| `runtime/mariadb_dialect*.cc` | DuckDB `mariadb` PEG dialect grammar overrides (`runtime/mariadb_dialect_expression.cc`, `runtime/mariadb_dialect_select.cc`) and matcher factory (`MariadbMatcherFactory`). |
| `runtime/duckdb_mysql_compat.cc` | MySQL/MariaDB compatibility scalar functions (`ord`, `hex`, `oct`, `bin`, `conv`, date/time helpers) registered in DuckDB. |
| `runtime/fiber_context.cpp`, `runtime/fiber_scan.cc`, `runtime/cross_engine_scan.cc` | Cooperative stack-switching fiber runtime (`x86_64`, `i386`, `aarch64` inline asm + `ucontext` under ASan) and `_mdb_scan` table function for streaming InnoDB tables into DuckDB joins. |
| `common/mariadb_lexical_guard.cc`, `common/duckdb_types.cc` | Allocation-free SQL lexical preflight guard and MariaDB-to-DuckDB type/identifier conversions. |
| `quack_guard/`, `bridge/` | TLS Flight SQL listener, pre-bind/post-bind statement gate, catalog filter (`quack_guard/`), and MariaDB ACL authorizer bridge (`bridge/`). |
| `duckdb_udf.cc`, `duckdb_udf_wrappers.cc` | Administrative UDFs including `run_in_duckdb`, `duckdb_load_*`, `duckdb_export_*`, and `duckdb_refresh_catalog`. |

## 3. Coding, Sanitizer, & Strict Warning Standards

Every target configured through `duckdb_setup_target` (`cmake/duckdb_target_setup.cmake`) is compiled with:
- `-Wall -Wextra -Wcast-qual -Wconversion -Wsign-conversion -pedantic -Wshadow`
- `-Wcast-align=strict` (GCC) or `-Wcast-align` (Clang)
- `-Werror` (when `-DDUCKDB_WERROR=ON`)
- `-fsanitize=address -fno-omit-frame-pointer` (when `-DWITH_ASAN=ON`)
- `-fsanitize=undefined,bounds,object-size -fno-sanitize-recover=all -fno-omit-frame-pointer` (when `-DWITH_UBSAN=ON`)

When modifying C++ code, preserve these invariants:
1. **No Unaligned Pointer Dereferences:** Never write to or read from `void *` / `char *` buffers via `*(longlong *)buff` or `*(__m128i *)ptr`. Always use `std::memcpy` for unaligned buffer packing and `uintptr_t` for address alignment arithmetic.
2. **No Signed-Char or Narrowing Cast Warnings:** Always cast `char` to `static_cast<unsigned char>(ch)` before `<cctype>` calls (`std::toupper`, `std::isdigit`, etc.) or byte classification tables so code compiles identically on signed-`char` (`x86_64`) and unsigned-`char` (`aarch64`) targets.
3. **No Out-of-Bounds Pointer Arithmetic:** Never decrement a pointer before the start of an array (`ptr--` past `base` to `base - 1` is C++17 `[expr.add]/4` undefined behavior even without dereferencing). Use `size_t` indices for backward scans.
4. **Tight ASan Buffer Testing:** Any string/URI/SQL scanner accepting `std::string_view` must never assume NUL termination or read past `sv.data() + sv.size()`. Test scanners with exact-sized `new char[offset + len]` allocations across misaligned offsets `0..15` and payload sizes `> 256` bytes (above 64-byte SIMD loop thresholds).
5. **Test Both `ASan+UBSan` and `UBSan-standalone`:** Under ASan, `runtime/fiber_context.h` selects `FIBER_USE_UCONTEXT`. Under standalone UBSan (`-DWITH_ASAN=OFF -DWITH_UBSAN=ON`), the native `x86_64` / `aarch64` assembly backends run. Verify changes against both configurations.

## 4. Git Workflow Requirements

- Every commit MUST be GPG-signed (`-S`) and DCO-signed-off (`-s` / `--signoff`):
  ```bash
  git commit -S -s -m "<type>(<scope>): <summary>"
  ```
- Never commit or force-add (`git add -f`) any files under `docs/superpowers/` or `.superpowers/`.
- Run `ctest --test-dir build --output-on-failure` and verify a clean working tree (`git status`) before pushing.
