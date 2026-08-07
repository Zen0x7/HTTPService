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

## Flujo de trabajo: ramas + PR

El agente trabaja **siempre en una rama de feature/fix**, nunca directo en
`master`. El usuario revisa y aprueba los cambios vía Pull Request en GitHub.

### Al comenzar

- Partir desde `master` actualizado:

  ```bash
  git fetch origin
  git checkout master
  git pull --ff-only origin master
  git status   # confirmar que el árbol está limpio
  ```

- Crear rama con nombre descriptivo y corto, estilo del repo (`git log --oneline`):

  ```bash
  git checkout -b <prefijo>/<descripcion>
  # prefijo: feat, fix, refactor, chore, docs, test, ci
  # ej: git checkout -b fix/version-header
  ```

### Al terminar el cambio

1. Verificar que compila y pasa tests antes de push:

   ```bash
   cmake --preset debug
   cmake --build --preset debug
   ctest --preset debug
   ```

2. Revisar y stagear solo lo intencional:

   ```bash
   git status
   git diff
   git add <archivos>
   git commit -m "mensaje"
   ```

3. Subir rama y abrir PR:

   ```bash
   git push -u origin <rama>
   gh pr create --title "..." --body "..."
   ```

   El PR debe describir qué hace y por qué; linkear issue si aplica.

### Durante la revisión

- El CI corre automáticamente sobre la rama del PR; reportar si falla.
- Ante feedback: crear **commits nuevos** (nunca `--amend` ni force-push) y
  `git push`; el PR se actualiza solo.
- No mergear el PR salvo que el usuario lo pida explícitamente.

### Reglas

- No commitear salvo que el usuario lo pida explícitamente o el flujo de PR
  esté en curso.
- No commitear secretos ni archivos de entorno.
- Mensajes claros, imperativo, primera línea corta.
- No amendear commits fallidos: crear commit nuevo.
- No force-push a `master` ni a ramas compartidas.
