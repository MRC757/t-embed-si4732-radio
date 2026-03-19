# Building the JS8Call WASM Decoder

This document describes how to compile the JS8Call WebAssembly decoder
and deploy it to the ESP32 LittleFS filesystem.

Only needs to be done **once**. Repeat Steps 5–7 only if you modify the
Rust source in `js8call-decoder/wasm/src/`.

---

## What This Produces

Two files are copied to `data/js/` and served from the ESP32:

| File | Size | Purpose |
|------|------|---------|
| `js8call_wasm.js` | ~30 KB | JavaScript glue (wasm-bindgen) |
| `js8call_wasm_bg.wasm` | ~350 KB | Compiled binary (LDPC + callsign decode) |

---

## Prerequisites (install once)

### 1. Install Rust

Download and run the installer from **https://rustup.rs**

At the prompt, press **Enter** to accept the default installation (option 1).

Close and reopen your terminal after installation.

Verify:
```
rustc --version
```

### 2. Add the WebAssembly compile target

```
rustup target add wasm32-unknown-unknown
```

### 3. Install the Visual Studio C++ build tools

`cargo install wasm-pack` compiles a native Windows binary and requires the
MSVC C++ runtime libraries. If you see `LNK1104: cannot open file 'msvcrt.lib'`
during the next step, this is the cause.

**Fix:** Open the **Visual Studio Installer** (search Start menu), click
**Modify** next to Visual Studio 2022 Community, check
**Desktop development with C++**, then click **Modify** and wait for the
install to finish (~2–4 GB). Close all terminals and reopen before continuing.

> **Quick test first:** Open **x64 Native Tools Command Prompt for VS 2022**
> (search Start menu) instead of a regular terminal. If `cargo install wasm-pack`
> succeeds there, the workload is already installed — just always run the build
> from that prompt.

### 4. Install wasm-pack

```
cargo install wasm-pack
```

Takes 2–5 minutes — compiles wasm-pack from source.

Verify:
```
wasm-pack --version
```

### 4. Install Node.js

Download the LTS installer from **https://nodejs.org** and run it (accept all defaults).

Close and reopen your terminal after installation.

Verify:
```
node --version
npm --version
```

---

## Build Steps

### 5. Open a terminal and navigate to the decoder directory

```
cd "c:\Macon Files\Code Projects\ESP32_Receiver\js8call-decoder"
```

### 6. Install npm dependencies (first time only)

```
npm install
```

### 6a. Install the Python `requests` library (first time only)

The matrix generator script uses `requests` to fetch data from GitHub.

```
pip install requests
```

If `pip` is not found, install Python 3.8+ from **https://python.org** (check
**Add Python to PATH** during install), then reopen your terminal.

### 6b. Generate the LDPC matrix source file (first time only)

The LDPC parity-check matrix used by the decoder is **not stored in the repo**.
It is fetched from the ft8lib source on GitHub and written to
`wasm/src/ldpc_matrix.rs` by a script. This step is **required** before the
first build — without it the Rust compiler will error:

```
error: couldn't read `src\ldpc_matrix.rs`: The system cannot find the file specified.
```

Run from inside the `js8call-decoder/` directory:

```
python tools/gen_ldpc_matrix.py
```

Expected output:
```
Fetching ft8lib source from https://raw.githubusercontent.com/...
  Parsed kFT8_LDPC_Nm: 174 rows
  Parsed kFT8_LDPC_Mn: 83 rows
  NM max degree: 3
  MN max degree: 7

Wrote wasm/src/ldpc_matrix.rs  (X,XXX bytes)
Now rebuild WASM:  npm run wasm:build
```

> Only needs to be re-run if the ft8lib LDPC matrix changes (essentially never).

### 7. Compile the Rust WASM library

```
npm run wasm:build
```

Output files are written to `js8call-decoder\wasm\pkg\`.

Expected output ends with:
```
[INFO]: :-) Done in ~45s
[INFO]: :-) Your wasm pkg is ready to publish at ./wasm/pkg.
```

---

## Deploy Steps

### 8. Copy the two output files to data/js/

**Command Prompt:**
```
copy wasm\pkg\js8call_wasm.js "..\data\js\js8call_wasm.js"
copy wasm\pkg\js8call_wasm_bg.wasm "..\data\js\js8call_wasm_bg.wasm"
```

**PowerShell:**
```powershell
Copy-Item wasm\pkg\js8call_wasm.js      ..\data\js\
Copy-Item wasm\pkg\js8call_wasm_bg.wasm ..\data\js\
```

### 9. Upload LittleFS to the ESP32

```
cd "c:\Macon Files\Code Projects\ESP32_Receiver"
pio run -t uploadfs
```

This flashes the full `data/` directory (including the new WASM files) to the
ESP32's LittleFS partition over USB.

---

## Verify

Open the radio UI in your browser and scroll to **JS8Call Decoder**.
Press **Start JS8Call**. The status line should show:

```
✓ JS8Call WASM ready — Slow / Normal / Fast / Turbo
```

If it shows `js8call_wasm.js not found`, the files are missing or the
filesystem upload did not complete — repeat Steps 8–9.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `couldn't read src\ldpc_matrix.rs` | Run `python tools/gen_ldpc_matrix.py` first (Step 6b) |
| `stream did not contain valid UTF-8` | Re-run `python tools/gen_ldpc_matrix.py` — file was written with wrong encoding (now fixed) |
| `E0600: cannot apply unary - to u8` (59 errors) | Re-run `python tools/gen_ldpc_matrix.py` — padding zeros were not filtered (now fixed) |
| `rustc` not found after install | Close and reopen terminal — PATH update requires a new shell |
| `LNK1104: cannot open file 'msvcrt.lib'` | Open VS Installer → Modify → add **Desktop development with C++** workload, then retry in a new terminal |
| Linker error but only in regular terminal | Use **x64 Native Tools Command Prompt for VS 2022** which sets MSVC paths automatically |
| `wasm-pack` build fails with other linker error | Run `rustup update` then retry |
| `npm install` fails | Check Node.js version — must be v18 or newer |
| WASM loads but no decodes | Tune to a JS8Call frequency in USB mode; press Start at the beginning of a slot |
| Status shows `<bits:hex>` instead of callsigns | Directed/relay message types — standard callsign pairs decode correctly |

---

## File Locations Summary

```
ESP32_Receiver/
├── js8call-decoder/            Source for the WASM decoder
│   ├── wasm/
│   │   ├── src/                Rust source (ldpc.rs, sync.rs, message.rs, …)
│   │   └── pkg/                Build output (generated — do not edit)
│   │       ├── js8call_wasm.js
│   │       └── js8call_wasm_bg.wasm
│   ├── package.json
│   └── BUILD_JS8CALL_WASM.md   ← this file
└── data/js/
    ├── js8call_wasm.js         ← copy here (step 8)
    ├── js8call_wasm_bg.wasm    ← copy here (step 8)
    ├── js8call-worker.js       (already in repo)
    └── js8call.js              (already in repo)
```
