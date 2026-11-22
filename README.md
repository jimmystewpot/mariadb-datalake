# DuckDB Storage Engine for MariaDB (`mariadb-datalake`)

A pluggable storage engine that embeds [DuckDB](https://github.com/duckdb/duckdb)'s columnar vectorized execution engine inside [MariaDB Server](https://github.com/MariaDB/server) with native C++ (zero-JVM) support for **Apache Iceberg** and **Apache Paimon** lakehouse tables.

Create a managed analytical table with `ENGINE=DuckDB` or attach an external Apache Paimon / Apache Iceberg table with `ENGINE=DuckDB CONNECTION='paimon://...'` / `CONNECTION='iceberg://...'` — analytical queries, aggregations, window functions, and cross-engine joins with InnoDB run in-process through one SQL interface and the standard `mariadb` client.

## Use Cases

- **HTAP (Hybrid Transactional/Analytical Processing)** — InnoDB handles OLTP, DuckDB handles analytics, both in the same MariaDB server with fiber-driven cross-engine joins.
- **Zero-JVM Data Lakehouse Queries** — Query Apache Paimon and Apache Iceberg tables directly from MariaDB in native C++ (`paimon-cpp` + Arrow C Data Interface and DuckDB `iceberg`/`avro`/`parquet`/`httpfs`), with projection and predicate pushdown.
- **Ad-hoc Analytical Queries** — Complex joins, aggregations, subqueries, and window functions over large datasets without ETL pipelines or a separate analytical cluster.

## Native Lakehouse Tables (Apache Paimon & Apache Iceberg)

External lakehouse tables are declared as read-only `ENGINE=DuckDB` tables using a `CONNECTION` URI (or `COMMENT='duckdb_connection=...'`):

```sql
-- Apache Paimon table on local or shared filesystem
CREATE TABLE orders_paimon (
  id BIGINT,
  region VARCHAR(32),
  amount DOUBLE
) ENGINE=DuckDB CONNECTION='paimon:///var/data/warehouse/sales.db/orders';

-- Apache Iceberg table via filesystem path
CREATE TABLE events_iceberg (
  event_id BIGINT,
  ts DATETIME,
  payload VARCHAR(255)
) ENGINE=DuckDB CONNECTION='iceberg:///var/data/iceberg_warehouse/analytics/events';

-- Apache Iceberg table via REST Catalog (HTTPS enforced for remote hosts)
CREATE TABLE rest_metrics (
  id BIGINT,
  val DOUBLE
) ENGINE=DuckDB CONNECTION='iceberg+rest://catalog.example.com/v1/namespaces/analytics/tables/metrics';
```

### How Lakehouse Integration Works

1. **View-Backed DDL Translation (`convertor/ddl_convertor.cc`, `ha_duckdb.cc`)**:
   - When `CREATE TABLE ... ENGINE=DuckDB CONNECTION='...'` executes, `ParseLakehouseUri()` (`lakehouse/src/connection_string.cc`) validates the URI scheme (`paimon://`, `iceberg://`, `iceberg+rest://`), rejects null bytes, control characters, relative paths, and `..` path traversal segments, and emits:
     - `CREATE OR REPLACE VIEW "db"."table" AS SELECT * FROM paimon_scan('<escaped_path>')`
     - `CREATE OR REPLACE VIEW "db"."table" AS SELECT * FROM iceberg_scan('<resolved_metadata_location>')`
   - If MariaDB fails after the DuckDB view is created during `ha_duckdb::create()`, the engine issues a compensating `DROP VIEW IF EXISTS` rollback.
   - `DROP TABLE` and `RENAME TABLE` emit `DROP VIEW IF EXISTS` and recreated view definitions for lakehouse tables.
   - `INSERT`, `UPDATE`, `DELETE`, and `TRUNCATE` on lakehouse tables are rejected at the handler boundary with `HA_ERR_TABLE_READONLY`.
2. **Apache Paimon C++ Reader (`lakehouse/src/paimon_table_function.cc`, `lakehouse/src/paimon_ffi_bridge.cc`)**:
   - Registers the `paimon_scan(path)` DuckDB `TableFunction` backed by `paimon-cpp` (`third_parties/paimon-cpp`).
   - Streams Apache Arrow record batches through a zero-copy `ArrowArrayStream` (`PaimonArrowArrayStream`) directly into DuckDB's `ArrowTableFunction::ArrowScanFunction`.
   - Pushes column projections (`column_ids`) and DuckDB `TableFilterSet` predicates (`=`, `!=`, `<`, `<=`, `>`, `>=`, `IS NULL`, `IS NOT NULL`, `IN`, `AND`, `OR`) down to `paimon::PredicateBuilder` and `paimon::ReadContextBuilder`.
3. **Apache Iceberg & REST Catalog Resolver (`lakehouse/src/lakehouse_catalog.cc`, `duckdb_udf_wrappers.cc`)**:
   - Statically links DuckDB's `iceberg`, `avro`, `parquet`, and `httpfs` extensions.
   - `LakehouseCatalog::Instance()` provides a thread-safe (`std::shared_mutex`) catalog registry that resolves `iceberg+rest://` table endpoints via `libcurl` and parses `metadata-location` with `yyjson`. Remote REST catalogs require `https://` (`http://` is permitted only for `localhost` / `127.0.0.1` / `[::1]`).
   - Provides the `SELECT duckdb_refresh_catalog('catalog_name')` UDF (`duckdb_udf_wrappers.cc`) to invalidate cached REST metadata snapshots.

## Roadmap

- **Analytical GIS** — deliver geospatial analytics via DuckDB's Spatial extension.
- **Faster HTAP over InnoDB-only data** — run queries against InnoDB-only data through DuckDB's vectorized execution.
- **Partial query pushdown** — implement a derived handler to push parts of complex queries down into DuckDB.
- **Write path for Lakehouse formats** — support Iceberg/Paimon append and compaction from MariaDB DML.

## Performance

TPC-H Scale Factor 10 (~11 GB raw data, 86.6M rows total, ~60M in `lineitem`).

**Hardware / environment:** Intel Core i7-13700H (14 cores / 20 threads), 64 GB RAM, NVMe SSD (ext4). MariaDB 11.4.14 with embedded DuckDB v2.0.0, default `duckdb_memory_limit`, `threads=20`. Single warm run (`tpch/run_all.sh`); numbers stable to ~±15%.

| Metric | Result |
|---|---|
| Data loading (Parquet via `read_parquet()`) | 33 seconds |
| All 22 TPC-H queries | **3.75 seconds** total |

### Per-query latency (seconds, warm)

| Query | Time | Query | Time | Query | Time |
|---|---:|---|---:|---|---:|
| q01 | 0.208 | q09 | 0.353 | q17 | 0.108 |
| q02 | 0.066 | q10 | 0.305 | q18 | 0.335 |
| q03 | 0.153 | q11 | 0.059 | q19 | 0.142 |
| q04 | 0.140 | q12 | 0.138 | q20 | 0.131 |
| q05 | 0.130 | q13 | 0.462 | q21 | 0.227 |
| q06 | 0.064 | q14 | 0.109 | q22 | 0.095 |
| q07 | 0.129 | q15 | 0.089 | **Total** | **3.75** |
| q08 | 0.135 | q16 | 0.170 | | |

Each query also pays a small fixed cost (~40 ms) for client connect and pushdown setup — noticeable on the cheapest queries, negligible on the heavy analytical ones. See [`docs/tpch_sf10_duckdb_v2_benchmark.md`](docs/tpch_sf10_duckdb_v2_benchmark.md) for the comparison with DuckDB v1.5.2 (4.30 s total), and [`docs/tpch_sf10_query_benchmark.md`](docs/tpch_sf10_query_benchmark.md) and [`docs/tpch_sf10_ingestion_benchmark.md`](docs/tpch_sf10_ingestion_benchmark.md) for the v1.5.2 methodology.

## Tutorials

Step-by-step guides for loading and analyzing open datasets with the DuckDB storage engine:

- [NYC Taxi Trips](docs/tutorials/nyc-taxi-trips.md) — loading Parquet via `run_in_duckdb()`, then cross-engine joins between `ENGINE=DuckDB` and `ENGINE=InnoDB` tables. Follows the ClickHouse taxi tutorial.
- [OWID CO₂ Emissions](docs/tutorials/owid-co2-emissions.md) — loading a CSV dataset and running aggregations and window functions.

## How It Works

DuckDB is an in-process analytical database. Its performance rests on three pillars:

- **Columnar storage** — minimizes unnecessary data reads.
- **Vectorized execution** — processes data in batches for maximum CPU cache efficiency.
- **Parallelism** — leverages all available processor cores.

Tables created with `ENGINE=DuckDB` store data in DuckDB's native format. InnoDB and DuckDB tables coexist in the same database.

MariaDB still parses, resolves, and authorizes the client statement first. SELECT/source pushdown then uses DuckDB's `mariadb` PEG dialect and an AST handoff, not SQL text rewriting. "One parse" means one DuckDB parse of the forwarded source, not removal of MariaDB parsing or of generated internal SQL parsing. The select/unit factories in `ha_duckdb_pushdown.cc` guard the original source via `extract_source_query()`; `ha_duckdb_select_handler::init_scan()` then calls `duckdb_stream_mariadb_query()` in `runtime/duckdb_query.cc`. Its `parse_mariadb_statement()` helper (`runtime/mariadb_dialect.cc`) scopes `CurrentDialectSetting::SetLocal()` / `ResetLocal()`, calls `ExtractStatements()` once, and requires exactly one SELECT. The base dialect is restored before `Submit(AST)`: no top-level rewrites, requoting, or text retries. Dialect selection uses no SQL `SET`/`RESET` or per-query grammar compilation. The dialect is registered and warmed by `DuckdbManager::Initialize()` (`runtime/duckdb_manager.cc`).

```text
Original SELECT/source → lexical guard → DuckDB mariadb PEG → SELECT AST
                                         base restored → Submit(AST) → streamed rows
Factory refusal → normal MariaDB row interface (success is not guaranteed)
Late parse/bind/execute error → error, no fallback
```

This is a fixed-profile dialect, not full MariaDB `sql_mode` semantics. Direct UPDATE/DELETE still use guarded, requoted text; generated DDL/DML and `run_in_duckdb()` remain on the base DuckDB dialect. See [`docs/security-model.md`](docs/security-model.md) for the forwarding boundary and remaining external-access gap.

## Building

The engine is built outside the MariaDB server tree against an installed MariaDB plugin SDK (`find_package(mariadb-plugin)`, MDEV-40608). DuckDB is built from the `third_parties/duckdb` submodule via `ExternalProject_Add`, currently pinned to `5406f514e6aa` (post-v2.0.0 tag).

### Prerequisites

A MariaDB installation that provides both the server and the Development component (`share/cmake/mariadb-plugin/`), of the same version, under one prefix. Either:

- the `MariaDB-devel` (RPM) or `libmariadb-dev` (DEB) package of a MariaDB release that includes MDEV-40608, or
- a server built from source and installed together with the Development component, e.g. `cmake --install <server-build> --component Development`.

The plugin is ABI-bound to the server it was built against: `build.sh` refuses to install it into a server whose version differs from the SDK's.

Remote file access (`https://`, `http://`, `s3://`) comes from DuckDB's `httpfs` extension, which is fetched at build time and linked statically. It needs the OpenSSL and libcurl development packages (`openssl-devel libcurl-devel` / `libssl-dev libcurl4-openssl-dev`, installed by `build.sh -D`); without them, or with `-DWITH_DUCKDB_HTTPFS=OFF`, the engine is built without it.

### Build and install (Layer B — Full MariaDB Plugin)

```bash
git clone --recurse-submodules https://github.com/jimmystewpot/mariadb-datalake.git
cd mariadb-datalake

# Install build dependencies (requires root)
./build.sh -D

# Build only (into ./build)
./build.sh -t RelWithDebInfo -c

# Build, install into the server, keep the existing datadir and start the server
./build.sh -t RelWithDebInfo -n -S
```

Without `-n`, `build.sh` deletes and re-initializes the datadir (`/var/lib/mysql` by default).

`cmake --install` puts `ha_duckdb.so` into the server plugin directory, `duckdb.cnf` into the server's additional config directory and the MTR suite into `<mariadb-test>/plugin/duckdb`. If the server does not read that config directory, load the engine with `INSTALL SONAME 'ha_duckdb';`.

Paths and layout:

| Option | Default | Meaning |
|---|---|---|
| `--mariadb-prefix <dir>` | `/usr` | MariaDB installation with the Development component; used as `CMAKE_PREFIX_PATH` and as the server prefix |
| `--build-dir <dir>` | `./build` | Plugin build directory |
| `--datadir <dir>` | `/var/lib/mysql` | Server data directory |
| `--layout DEB\|RPM\|STANDALONE` | `RPM` for `-p rpm`, `STANDALONE` for `-p tgz`, otherwise `DEB` | Install layout of the server; must match it, otherwise the plugin lands where the server does not look for it |

Run `./build.sh -h` for the full list of options.

### Build packages

```bash
./build.sh -t RelWithDebInfo -p rpm   # Rocky/Fedora/Amazon Linux
./build.sh -t RelWithDebInfo -p deb   # Debian/Ubuntu
./build.sh -t RelWithDebInfo -p tgz
```

### Tests (MTR)

`run_mtr.sh` runs the suite with the installed MTR (`<prefix>/share/mariadb/mariadb-test`, override with `--mtr-dir`) against the installed engine, so run `build.sh` without `-c` first. Source tests are copied into the installed suite before each run, and `-r` copies recorded results back to `mysql-test/duckdb/r/`.

```bash
./run_mtr.sh create_table_column      # one test
./run_mtr.sh -r create_table_column   # run and record
./run_mtr.sh -a                       # all tests
./run_mtr.sh -a -e                    # all tests against a running server
```

### Standalone Layer A Build & Unit Tests (No MariaDB Server Required)

Layer A builds `libduckdb_lakehouse.a`, `libquack_guard.a`, DuckDB (with `parquet`, `avro`, `iceberg`, `httpfs`, and `quack`), `paimon-cpp`, and all 8 standalone CTest suites without requiring an installed MariaDB server or plugin SDK:

```bash
cmake -S . -B build -DSTANDALONE_TESTS=ON -DDUCKDB_WERROR=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

The 8 Layer A test suites are:
- `quack_guard_test` — Quack server pre-bind/post-bind gates, catalog filtering, and TLS session security.
- `mariadb_dialect_test` — MariaDB lexical preflight (including tight ASan heap buffers at misaligned offsets `0..15`), PEG dialect grammar, and single-parse AST handoff (`--benchmark` for warmed parser p50/p95/p99).
- `fiber_context_test` — Cooperative fiber stack switching (`x86_64`, `i386`, `aarch64` inline assembly and `ucontext`), active-state tracking, and 16-byte stack frame alignment across misaligned stack sizes.
- `smoke_test` — Static extension loader verification for `iceberg`, `parquet`, `avro`, and `paimon-cpp` symbols.
- `connection_test` — `paimon://`, `iceberg://`, and `iceberg+rest://` URI parsing, path-traversal/null-byte rejection, and tight-buffer misaligned-offset tests.
- `paimon_scan_test` — `paimon_scan()` Arrow C Data Interface bridge, projection pushdown, filter pushdown, multi-chunk slicing (`65`, `2049`, `5000` rows), and corrupted-table error propagation.
- `iceberg_catalog_test` — `LakehouseCatalog` thread-safe registry, mock REST catalog HTTP resolution, `iceberg_scan()` Parquet reading, and HTTPS enforcement for remote hosts.
- `lakehouse_e2e_test` — End-to-end view-based DDL lifecycle (`CREATE OR REPLACE VIEW`, `DROP VIEW IF EXISTS`), Paimon + Iceberg + native DuckDB cross-format joins, aggregations, and read-only enforcement.

### Sanitizer & Strict Warning Verification (ASan + UBSan)

All targets configured via `duckdb_setup_target` (`cmake/duckdb_target_setup.cmake`) compile under `-Wall -Wextra -Wcast-qual -Wcast-align(=strict) -Wconversion -Wsign-conversion -pedantic -Wshadow -Werror` and support `-DWITH_ASAN=ON` and `-DWITH_UBSAN=ON` across GCC and Clang:

```bash
# ASan + UBSan (instruments libraries and test binaries)
cmake -S . -B build-asan -DSTANDALONE_TESTS=ON -DDUCKDB_WERROR=ON -DWITH_ASAN=ON -DWITH_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure

# Standalone UBSan (exercises native x86_64/ARM64 fiber assembly without ASan ucontext fallback)
cmake -S . -B build-ubsan -DSTANDALONE_TESTS=ON -DDUCKDB_WERROR=ON -DWITH_ASAN=OFF -DWITH_UBSAN=ON
cmake --build build-ubsan -j$(nproc)
ctest --test-dir build-ubsan --output-on-failure
```

Alternate build directories (`build-asan`, `build-ubsan`, etc.) automatically reuse prebuilt `build/duckdb-build` and `build/paimon-build` artifacts when `build/` has already been built.

## Cross-Engine Queries

The engine supports cross-engine joins — a single `SELECT` can combine DuckDB tables with tables from other engines (e.g. InnoDB). When the query planner detects a mix of engines, the `select_handler` does the following:

1. Opens the non-DuckDB tables via MariaDB's handler API and registers them in a thread-local table registry.
2. Derives a per-table predicate from the query's `WHERE` (via `make_cond_for_table` in `ha_duckdb_select_handler::init_scan()`, `ha_duckdb_pushdown.cc`) and registers it alongside each external table.
3. Pushes the **entire query** — including all `WHERE`, `JOIN`, `GROUP BY`, and `ORDER BY` clauses — down to DuckDB through the MariaDB PEG SELECT AST handoff.
4. DuckDB's replacement scan callback transparently redirects references to non-DuckDB tables to the `_mdb_scan` table function.

The `_mdb_scan` table function does **not** loop over the handler directly. On its first call it lazily spawns a **cooperative fiber** (stack-switching runtime borrowed from `libmariadb`) running on a dedicated background `THD`. Inside the fiber it builds a synthetic, projection- and predicate-pushed
`SELECT <needed columns> FROM <db>.<table> [WHERE <pushed predicate>]`
and executes it through the **full MariaDB pipeline** (`lex_start` → `parse_sql` → `mysql_execute_command`, called in `fiber_scan_func()`, `runtime/fiber_scan.cc`). The predicate is printed with `COND::print()` in `ha_duckdb_select_handler::init_scan()` (`ha_duckdb_pushdown.cc`); `build_synthetic_select()` (`runtime/fiber_scan.cc`) builds the synthetic SQL. This SELECT is parsed by MariaDB, not by the DuckDB MariaDB dialect. A custom `select_result_interceptor` (`select_to_duckdb`) converts each result row's `Item`s into a DuckDB `DataChunk`; when a chunk fills (`STANDARD_VECTOR_SIZE`) the fiber **yields** back to DuckDB, which consumes the chunk and resumes the fiber for the next batch.

Because the external scan runs through `mysql_execute_command`, it uses the **MariaDB optimizer and all available access paths** (index range/ref access, etc.), and the pushed `WHERE` predicate is evaluated by MariaDB *before* rows reach DuckDB. On the DuckDB side, the full query AST lets DuckDB's optimizer reorder joins, build hash tables, and run all aggregation/sorting over the streamed rows. A direct `ha_rnd_next` loop is retained only as a fallback when no fiber is used.

This means queries like the following just work:

```sql
SELECT d.id, d.amount, i.name
  FROM analytics.orders d          -- ENGINE=DuckDB
  JOIN inventory.products i        -- ENGINE=InnoDB
    ON d.product_id = i.id
 WHERE d.amount > 1000;
```

DuckDB handles the join, aggregation, and sorting; InnoDB rows are produced on demand by a fiber-driven MariaDB query. No data copying or ETL is required.

## Current Limitations

- **DECIMAL precision > 38** — DuckDB supports up to 38 digits; wider MariaDB DECIMALs will fail on DDL conversion.
- **Some MariaDB functions are yet not pushdown-compatible** — `GROUP_CONCAT()`, `DATE_FORMAT()`, `JSON_CONTAINS()`, `FOUND_ROWS()`, `LAST_INSERT_ID()`, and a few others have no DuckDB equivalent or differ in syntax. Factory refusal leaves execution to MariaDB's normal row interface, which is not guaranteed to succeed. Once pushdown is accepted, a late parse, bind, or execution error is returned to the caller; incompatible functions do not trigger an error-time fallback.
- **Strict GROUP BY** — DuckDB rejects `SELECT` columns not in `GROUP BY` and not aggregated, even when MariaDB's `sql_mode` allows it.
- **XA transactions** — `XA PREPARE` is not supported by the engine.
- **Table partitioning** — `CREATE TABLE ... PARTITION BY` and converting a DuckDB table with `ALTER TABLE ... PARTITION BY` are not supported; the engine declares `HTON_NO_PARTITION`.
- **Collations** — MariaDB UCA-based collation rules are approximated via DuckDB's built-in `NOCASE`/`NOACCENT` collations for UTF-8 charsets; non-UTF8 charsets fall back to binary comparison. See [`docs/collation-mapping.md`](docs/collation-mapping.md) for the full mapping and known gaps.
- **Cross-engine scan is yet single-threaded** — each external (non-DuckDB) table is produced by a single fiber-driven MariaDB query (`_mdb_scan` reports `MaxThreads() == 1`); only the DuckDB side of the query is parallelized.
- **ALTER COLUMN DROP DEFAULT** — not propagated to DuckDB catalog.
- **Tables without a primary key** (allowed only with `duckdb_require_primary_key=OFF`) — row-by-row `UPDATE`/`DELETE` (used e.g. with triggers or on a replica) silently skips rows containing NULL values and changes all copies of duplicate rows. See [`docs/architecture.md`](docs/architecture.md#path-3-row-by-row-dml).
- **Row-based replication into DuckDB tables requires `binlog_row_image=FULL` on the master** (the default). A replica rejects `UPDATE` events with a partial after-image (`MINIMAL`, `NOBLOB`, `FULL_NODUP`) instead of filling the missing columns with defaults. The replica does not detect missing or conflicting rows (`slave_exec_mode=STRICT` is not enforced) and is not crash-safe. See [`docs/architecture.md`](docs/architecture.md#path-3a-row-based-replication-apply).
- **Timezone propagation** — `TIMESTAMP` columns are stored as `TIMESTAMPTZ`; timezone must be set consistently between MariaDB and DuckDB contexts to avoid shifts.

See [`docs/mariadb-duckdb-incompatibilities.md`](docs/mariadb-duckdb-incompatibilities.md) for a detailed compatibility matrix.

## License

This project is licensed under the GNU General Public License v2. See [COPYING](COPYING) for details.

DuckDB itself is licensed under the MIT License.

## Acknowledgments

**Alibaba and the AliSQL Project** — A special thank you to Alibaba and their engineering team for open-sourcing [AliSQL](https://github.com/alibaba/AliSQL). AliSQL is a MySQL branch developed at Alibaba Group and extensively used in their production infrastructure. The December 2025 open-source release of AliSQL 8.0 included integration of DuckDB as a native storage engine, providing a valuable reference implementation and validating the viability of embedding DuckDB inside a MySQL-compatible server. The DuckDB Engine for MariaDB draws heavily on this experience.

**The DuckDB Project** — None of this would be possible without the remarkable work of the [DuckDB team](https://github.com/duckdb/duckdb). DuckDB has grown from an academic research project into one of the most impressive analytical engines available today — fast, embeddable, dependency-free, and released under the permissive MIT License. Its clean C++ codebase and well-defined embedding API are what make integrations like this one feasible.
