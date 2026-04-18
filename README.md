# CVM

CVM is an interpreter for CHance `.cclib` packages.

## What it does

- Loads `.cclib` files.
- Reads embedded `.ccbin` module payloads.
- Executes an entry function (default: `main`) without assembly generation.
- Resolves extern calls through dynamic imports (`dlopen`/`dlsym`).

## Build

```sh
cmake -S . -B build
cmake --build build
```

### Windows notes

By default, the build pulls prebuilt `libffi` binaries from GitHub releases (`CVM_BUNDLED_LIBFFI=ON`) and links against those local files instead of a globally installed `libffi`.

The archive is selected by target architecture:

- x64: `libffi-3.5.2-x86-64bit-msvc-binaries.zip`
- x86: `libffi-3.5.2-x86-32bit-msvc-binaries.zip`

The matching `libffi-8.dll` is copied next to the built `cvm` executable after build.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

If you want to use a preinstalled libffi instead, disable bundled mode and pass its prefix:

```powershell
cmake -S . -B build -DCVM_BUNDLED_LIBFFI=OFF -DLIBFFI_ROOT=C:/path/to/libffi
```

## Run

```sh
./build/cvm path/to/program.cclib
./build/cvm path/to/program.cclib --entry main --import /path/to/libsomething.so
./build/cvm path/to/program.cclib --entry main --import C:/path/to/something.dll
./build/cvm path/to/program.cclib --import-cclib /path/to/stdlib.cclib --import-cclib /path/to/runtime.cclib
./build/cvm path/to/program.cclib --jit
./build/cvm --version
```

## Notes

- Extern calls currently support up to 8 integer/pointer arguments.
- Dynamic imports include process symbols by default.
- CVM attempts to auto-load a platform C runtime (`libSystem`/`libc`/`ucrt` family) for common libc-style symbols.
- CVM auto-loads `stdlib.cclib` and `runtime.cclib` when found next to the `cvm` executable or under `/usr/local/share/chance/...`.
- Interpreter support is focused on core instruction flow (`const`, locals, arithmetic, compare, jumps, calls, returns, globals).
- `--jit` compiles embedded bytecode to native code via `chancecodec` and links a temporary shared object, then runs the entrypoint.
- JIT tool discovery uses `CHANCEC_HOME`, `CHANCECODEC_HOME`, `CLD_HOME`, and `CHS_HOME` when set.
