#!/usr/bin/env python3
"""
tools/gen_ldpc_matrix.py
========================
Fetches the FT8 (174,91) LDPC parity-check matrix from the ft8lib GitHub
repository and regenerates wasm/src/ldpc_matrix.rs with correct, 0-indexed
Rust static arrays.

Run once before building:
    python3 tools/gen_ldpc_matrix.py

Requires: Python 3.8+, requests  (pip install requests)
"""

import re
import sys
import textwrap

try:
    import requests
except ImportError:
    sys.exit("Install requests:  pip install requests")

# --------------------------------------------------------------------------
# Source: kgoba/ft8_lib — the canonical clean-room C++ implementation.
# We pull the raw matrix file directly from GitHub.
# --------------------------------------------------------------------------

# Candidate URLs in priority order — the repo reorganised after initial release.
# v2+: matrix moved to constants.c with kFTX_ prefix (generic FT protocol).
# v1:  matrix was in lib/ldpc.cpp with kFT8_ prefix.
CANDIDATE_URLS = [
    "https://raw.githubusercontent.com/kgoba/ft8_lib/master/ft8/constants.c",
    "https://raw.githubusercontent.com/kgoba/ft8_lib/main/ft8/constants.c",
    "https://raw.githubusercontent.com/kgoba/ft8_lib/master/lib/ldpc.cpp",
    "https://raw.githubusercontent.com/kgoba/ft8_lib/main/lib/ldpc.cpp",
    "https://raw.githubusercontent.com/kgoba/ft8_lib/master/ft8/ldpc.cpp",
    "https://raw.githubusercontent.com/kgoba/ft8_lib/main/ft8/ldpc.cpp",
]

N = 174   # codeword length
M =  83   # number of parity checks
K =  91   # information bits


def fetch_source(candidates: list) -> tuple:
    """Try each URL in order; return (url, text) for the first 200 response."""
    for url in candidates:
        try:
            r = requests.get(url, timeout=30)
            if r.status_code == 200:
                return url, r.text
            print(f"  {url} → {r.status_code}")
        except Exception as e:
            print(f"  {url} → {e}")
    raise SystemExit(
        "All candidate URLs returned non-200. Check that kgoba/ft8_lib is "
        "still accessible and update CANDIDATE_URLS in this script."
    )


def parse_array(src: str, name: str) -> list[list[int]]:
    """
    Extract a 2-D integer array from C++ source by name.
    Handles both kFT8_LDPC_Nm and kFT8_LDPC_Mn.
    Converts 1-indexed → 0-indexed (subtracts 1 from every element).
    """
    # Match the whole initializer block { ... };
    # Dimensions may be integer literals ([][3]) or identifier macros ([FTX_LDPC_N][3]).
    pattern = rf'{re.escape(name)}\s*\[\s*\w*\s*\]\s*\[\s*\w+\s*\]\s*=\s*\{{([^;]+)\}}\s*;'
    m = re.search(pattern, src, re.DOTALL)
    if not m:
        raise ValueError(f"Array {name} not found in source")

    body = m.group(1)
    rows = []
    for row_m in re.finditer(r'\{([^}]+)\}', body):
        # Skip source zeros — they are 1-indexed padding, not real entries.
        # Subtracting 1 from 0 would give -1, which is invalid in a u8 Rust array.
        # pad_row() will fill unused slots with the 255 sentinel instead.
        vals = [int(v.strip()) - 1 for v in row_m.group(1).split(',')
                if v.strip() and int(v.strip()) != 0]
        rows.append(vals)
    return rows


def pad_row(row: list[int], width: int, sentinel: int = 255) -> list[int]:
    return (row + [sentinel] * width)[:width]


def format_2d_array(name: str, dtype: str, rows: list[list[int]], width: int, sentinel: int = 255) -> str:
    n      = len(rows)
    padded = [pad_row(r, width, sentinel) for r in rows]
    lines  = []
    lines.append(f"static {name}: [[{dtype}; {width}]; {n}] = [")
    for row in padded:
        inner = ", ".join(str(v) for v in row)
        lines.append(f"    [{inner}],")
    lines.append("];")
    return "\n".join(lines)


def main():
    print("Fetching ft8lib source (trying candidate URLs) …")
    url, src = fetch_source(CANDIDATE_URLS)
    print(f"  Using: {url}")

    # Parse all candidate array names and assign by row count.
    #
    # ft8lib naming changed between versions:
    #   v1 (lib/ldpc.cpp):    kFT8_LDPC_Nm = 174 rows (var→check), kFT8_LDPC_Mn = 83 rows (check→var)
    #   v2 (ft8/constants.c): kFTX_LDPC_Nm =  83 rows (check→var), kFTX_LDPC_Mn = 174 rows (var→check)
    # Assignment is by row count to be version-agnostic:
    #   NM = 174 rows = variable→check connections (used as NM[variable_node])
    #   MN =  83 rows = check→variable connections (used as MN[check_node])
    all_names = (
        "kFTX_LDPC_Nm", "kFTX_LDPC_Mn",
        "kFT8_LDPC_Nm", "kFT8_LDPC_Mn",
        "kFTX_LDPC_NM", "kFTX_LDPC_MN",
        "kFT8_LDPC_NM", "kFT8_LDPC_MN",
    )
    nm_raw = None  # 174 rows, variable→check
    mn_raw = None  # 83 rows,  check→variable
    for aname in all_names:
        try:
            rows = parse_array(src, aname)
        except ValueError:
            continue
        if len(rows) == N and nm_raw is None:
            nm_raw = rows
            print(f"  {aname}: {len(rows)} rows → NM (variable→check)")
        elif len(rows) == M and mn_raw is None:
            mn_raw = rows
            print(f"  {aname}: {len(rows)} rows → MN (check→variable)")
        if nm_raw is not None and mn_raw is not None:
            break

    if nm_raw is None or mn_raw is None:
        sys.exit(
            "Could not parse the matrix arrays.  "
            "The ft8lib source layout may have changed — inspect the URL manually and "
            "update the regex in this script."
        )

    # Validate dimensions.
    if len(nm_raw) != N:
        sys.exit(f"NM has {len(nm_raw)} rows, expected {N}")
    if len(mn_raw) != M:
        sys.exit(f"MN has {len(mn_raw)} rows, expected {M}")

    max_nm_degree = max(len(r) for r in nm_raw)
    max_mn_degree = max(len(r) for r in mn_raw)
    print(f"  NM max degree: {max_nm_degree}")
    print(f"  MN max degree: {max_mn_degree}")

    # Sanity check: all values in NM should be in [0, M-1].
    for i, row in enumerate(nm_raw):
        for v in row:
            if v < 0 or v >= M:
                sys.exit(f"NM[{i}] contains out-of-range value {v} (should be 0-{M-1})")

    # All values in MN should be in [0, N-1] or be the sentinel (255 after 0-index).
    for j, row in enumerate(mn_raw):
        for v in row:
            if v < -1 or v >= N:  # -1 means was 0 in 1-indexed (shouldn't happen)
                sys.exit(f"MN[{j}] contains out-of-range value {v} (should be 0-{N-1})")

    nm_str = format_2d_array("NM", "u8", nm_raw, max_nm_degree, 255)
    mn_str = format_2d_array("MN", "u8", mn_raw, max_mn_degree, 255)

    output = textwrap.dedent(f"""\
        // AUTO-GENERATED by tools/gen_ldpc_matrix.py — DO NOT EDIT.
        // Source: {url}
        //
        // FT8 (174, 91) LDPC parity-check matrix.
        //   N = {N}  (codeword length)
        //   K = {K}  (information bits)
        //   M = {M}  (parity checks)
        //
        // NM[i]  = check nodes connected to variable node i  (0-indexed, 255 = padding)
        // MN[j]  = variable nodes connected to check node j  (0-indexed, 255 = padding)

        pub const N: usize = {N};
        pub const M: usize = {M};
        pub const MAX_ITER: usize = 50;

        #[rustfmt::skip]
        {nm_str}

        #[rustfmt::skip]
        {mn_str}
    """)

    out_path = "wasm/src/ldpc_matrix.rs"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(output)
    print(f"\nWrote {out_path}  ({len(output):,} bytes)")
    print("Now rebuild WASM:  npm run wasm:build")


if __name__ == "__main__":
    main()
