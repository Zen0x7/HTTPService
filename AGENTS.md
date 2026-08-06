# AGENTS.md

Guía de mantenimiento para agentes y colaboradores de este repositorio.

## Proyecto

`HTTPService` es un proyecto CMake base (C++23) que compila un único binario
`httpservice`. Usa Boost 1.92.0_b1, GoogleTest y Google Benchmark desde la imagen
`ghcr.io/zen0x7/bcompiler`.

- `CMakeLists.txt` — lib `httpservice_core` + binario `httpservice` + tests + benchmarks.
- `CMakePresets.json` — presets `debug`, `release`, `asan`.
- `include/httpservice/version.hpp` — API pública mínima.
- `src/version.cpp`, `src/main.cpp` — implementación y binario.
- `tests/tests_version.cpp` — pruebas con GTest.
- `benchmarks/version_benchmark.cpp` — micro benchmark.
- `.github/workflows/ci.yml` — build + test en imagen bcompiler.

## Imagen base

La imagen `ghcr.io/zen0x7/bcompiler` ya trae compilados Boost (json, program_options,
charconv), GoogleTest y Google Benchmark para la variante elegida. No se hace
FetchContent: usar siempre `find_package`.

Variantes de imagen: `latest-{variant}-{distro}-{arch}` con:
- variant ∈ {debug, release, static, shared, asan, tsan, ubsan, msan}
- distro ∈ {alpine, ubuntu, debian}
- arch ∈ {amd64, arm64}

Alpine solo soporta asan/ubsan (musl). TSAN/MSAN requieren glibc (ubuntu/debian);
MSAN además requiere clang.

## CI / CD

- Matrix `distro` (alpine/ubuntu/debian) × `arch` (amd64/arm64) × `variant`
  (debug/release/static/shared/asan/tsan/ubsan/msan) en runners nativos **sin QEMU**.
- Cada job corre dentro del contenedor bcompiler correspondiente (`container.image`).
- Checkout manual (git init + fetch) para funcionar dentro del contenedor.
- Disparadores: push a `master`, tags `v*`, PRs, `workflow_dispatch`.

## Cómo comitear

El agente **no** debe commitear salvo que el usuario lo pida explícitamente.
Cuando se pida:

```bash
git status                 # revisar cambios
git diff                   # revisar el contenido
git add <archivos>         # stagear solo lo intencional
git commit -m "mensaje"    # mensaje descriptivo, estilo del repo (ver git log)
```

Reglas:
- No commitear secretos ni archivos de entorno.
- Mensajes claros, imperativo, primera línea corta.
- No amendear commits fallidos: crear commit nuevo.
