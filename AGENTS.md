# Repository Guidelines

## Project Structure & Module Organization

`components/esp_iris/` contains the ESP-IDF component. Public headers live in `include/`, implementation files in `src/`, and the wire contract and test vectors in `protocol/`. Host tooling is under `components/esp_iris/tools/`: `iris_gateway/` is the Python gateway, `tests/` contains pytest coverage, and `frontend/` is the React/Vite workbench. Hardware-oriented fixture projects live in `components/esp_iris/test_apps/`. Use `examples/esp_iris_minimal/` for normal integration and smoke builds. Treat `build*/`, `managed_components/`, `node_modules/`, caches, and generated `sdkconfig` files as generated artifacts; do not hand-edit them.

## Build, Test, and Development Commands

Run ESP-IDF commands from an initialized ESP-IDF shell:

```bash
cd examples/esp_iris_minimal && idf.py build
idf.py -p /dev/serial/by-id/<device> flash
```

Use a stable serial path and verify the target before flashing. Build fixture firmware by entering the desired directory under `components/esp_iris/test_apps/` and running `idf.py build`.

```bash
python3 -m pip install -r components/esp_iris/tools/requirements-dev.txt
cd components/esp_iris/tools && python3 -m pytest
python3 -m ruff check iris_gateway tests
cd frontend && npm ci && npm run test:unit && npm run build
```

Use `npm run dev` for the local workbench and `npm run test:e2e` for Playwright tests.

## Coding Style & Naming Conventions

Use four spaces in C and Python, and two spaces in TypeScript/TSX. Follow existing C layout: `snake_case` functions and variables, `esp_iris_` for public APIs, `iris_` for internal helpers, and uppercase `ESP_IRIS_*` constants. Python modules and tests use `snake_case`; React components use `PascalCase`. Keep protocol changes synchronized across C, Python, `protocol/spec.md`, and `golden_vectors.json`. Run Ruff before submitting Python changes; preserve the frontend's TypeScript strictness and double-quote style.

## Testing Guidelines

Name Python tests `test_*.py`, Vitest files `*.test.ts`, and Playwright specs `*.spec.ts`. Add regression tests beside the affected layer. No numeric coverage threshold is defined; changes must pass relevant pytest, Vitest/build, and ESP-IDF fixture builds. Hardware changes should record the board, target, transport, and observed result.

## Commit & Pull Request Guidelines

Git history is not included in this snapshot, so no repository-specific prefix can be verified. Use concise, imperative subjects such as `Fix USB reconnect backoff`, and keep each commit focused. Pull requests should explain behavior and protocol/configuration impact, list commands run, link the issue, and include workbench screenshots for visible UI changes. Never commit Wi-Fi credentials, pairing tokens, TLS keys, or developer passwords; keep secrets in ignored `sdkconfig`, environment variables, or private files.
