# Development

`ekk-runtime` is intentionally small and should stay easy to inspect.

## Tooling

- C compiler with C99 support (`clang` or `gcc`)
- `cmake >= 3.16`
- `ctest`

## Canonical smoke path

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This is the main public proof path for the repo.

## Verification path

The Isabelle/AutoCorres session lives under `docs/isabelle/`.

```bash
<isabelle>/bin/isabelle build \
  -d <l4v> \
  -d docs/isabelle \
  EkkVerification
```

The proof layer is important, but it is not the shortest path for first-time
validation.

## Review posture

- treat `docs/SEL4_REVIEW.md` as the main current gap register
- keep bounded loops / static configuration / no dynamic allocation posture
- prefer explicit hardening over feature growth
