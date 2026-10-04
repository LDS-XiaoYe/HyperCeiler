# dynamic-address-audit

Proves that every Dart-side hook site and heap-field offset used by
`app/src/main/cpp/targets/home/` is **derived at runtime** rather than stored as a
constant. The project rule is that a launcher build bump must not silently turn
into a wrong-address write, so a hardcoded offset is treated as a defect, not a
tradeoff.

The audit is a three-legged A/B:

| leg | script | asserts |
|---|---|---|
| static address derivation | `verify.py` | each `needle`/`xref` walk lands on a named body, and the decode of the immediate/branch it feeds is right |
| real production binders | `verify_bindings.py` | the resolver functions in the shipped `home_*.cpp` return the addresses the static walk predicts |
| host geometry | `verify_geometry.py` | the geometry entry points reject an unresolved field set instead of computing on garbage |

`verify.py` and `verify_bindings.py` are **baseline-sensitive**: run against the
pre-change sources they must fail, against the working tree they must pass.
`verify_geometry.py` is not - it is a pure gate.

## Running

```sh
python -X utf8 tests/dynamic-address-audit/verify.py         <source-root>
python -X utf8 tests/dynamic-address-audit/verify_bindings.py <source-root>
python -X utf8 tests/dynamic-address-audit/verify_geometry.py
```

`verify_geometry.py` builds its own freestanding host objects under `geometry/`
and needs the vendored zig at `runtime/zig-windows-x86_64-0.13.0/zig.exe` (see
`.gitignore` - the toolchain and its caches are not tracked, so a fresh clone has
to supply its own).

`verify.py` and `verify_bindings.py` read the offline `libapp.so` image that
`verify.py`'s harness expects under the source root; without it they report a
missing-image error rather than a false pass.

## `transaction.py`

Regenerates the change artifacts (`MODIFIED_FILE.zip`, `DIFF_FILE.patch`,
`ROLLBACK.sh`, `VERIFICATION.txt`) and then runs the full A/B in both directions -
baseline must fail, modified must pass, and an independent rollback copy must fail
again. `python transaction.py rollback <dir>` restores the baseline into a
directory that must be `rollback-copy/`; it refuses any other target so it cannot
clobber the working tree.

## `device-YYYYMMDD/`

`modules_config.db` copies pulled off a device as LSPosed-module evidence. These
are per-run forensic captures: large, binary, and worthless a week later. They are
deliberately **not** tracked - see `.gitignore`.

## `*.asm`

Disassembly listings of the launcher symbols the audit resolves, kept as the
readable record of what each address actually is. They are inputs to review, not
to the build.
