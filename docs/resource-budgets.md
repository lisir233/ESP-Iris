# Resource gate baseline (IRIS-T02)

The previous budget file used paths from another repository and was not run.
Several files already exceeded its limits at review baseline `b484e47`. This
change explicitly adopts the measured integration size, rounded upward to the
next 50 lines, while preserving existing limits where they remain sufficient.
It does not claim that larger modules are desirable or that line counts prove
latency, memory use, or maintainability. Further growth must change this reviewed
file; the checker fails on missing files, wrong path types and empty groups.

The executor is separately budgeted because it owns asynchronous service work.
Compatibility, reconciliation and request identity have separate host modules.
Workspace.tsx and the existing Gateway remain refactoring debt; their pre-existing
growth is not hidden as a performance improvement in this patch.

| Source | Review baseline lines | Integration lines | Previous limit | New limit |
| --- | ---: | ---: | ---: | ---: |
| `components/esp_iris/src/esp_iris.c` | 998 | 1089 | 1050 | 1100 |
| `components/esp_iris/src/esp_iris_services.c` | 1946 | 2002 | 1900 | 2050 |
| `components/esp_iris/src/esp_iris_files.c` | 1685 | 1685 | 1700 | 1700 |
| `components/esp_iris/tools/iris_gateway/gateway.py` | 1960 | 2021 | 1250 | 2050 |
| `components/esp_iris/tools/iris_gateway/session.py` | 1108 | 1189 | 1000 | 1200 |
| `components/esp_iris/tools/iris_gateway/files.py` | 509 | 509 | 550 | 550 |
| `components/esp_iris/tools/iris_gateway/file_routes.py` | 353 | 368 | 400 | 400 |
| `components/esp_iris/tools/frontend/src/Files.tsx` | 237 | 237 | 300 | 300 |
| `components/esp_iris/tools/frontend/src/Workspace.tsx` | 1525 | 1525 | 550 | 1550 |
| `components/esp_iris/src/esp_iris_executor.inc` | new | 369 | — | 400 |
| `components/esp_iris/tools/iris_gateway/compatibility.py` | new | 67 | — | 100 |
| `components/esp_iris/tools/iris_gateway/reconciliation.py` | new | 130 | — | 150 |
| `components/esp_iris/tools/iris_gateway/operation_identity.py` | new | 42 | — | 50 |

Firmware byte gates use the CI build directories. The existing services/coredump
limits are retained. The new inventory/update test binaries are limited to 1 MiB;
their 2 MiB flash profiles still rely on ESP-IDF's partition fit check. The
frontend remains limited to 400 KiB. Missing build artifacts fail the selected
gate; local source-only checks deliberately do not claim a firmware byte result.

CI runs the host matrix and separate C ASan/UBSan job. macOS uses the explicit
Intel runner label published in [GitHub's runner documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job),
so Python 3.8 does not depend on an ARM-only interpreter build. Configuring that
job is not evidence it has run: local acceptance reports list actual platforms.

Release indexing requires the declared application BIN/ELF/map, sdkconfig,
bootloader and partition table, and checks the IDF path against the build
description. It records hashes of tracked and untracked, non-ignored source
files as well as revision/dirty status. Indexing existing artifacts is not a
substitute for a successful build of that source snapshot or for hardware tests.
