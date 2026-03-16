"""
pre_build.py — PlatformIO pre-build script
Runs before compilation. Validates that required files exist
and prints a build summary.
"""
Import("env")
import os

print("\n=== T-Embed SI4732 Pre-Build ===")

# Check that data/ directory has web UI files
data_dir = os.path.join(env.get("PROJECT_DIR"), "data")
required = ["index.html", "js/app.js", "js/waterfall.js",
            "js/audio.js", "js/ft8.js", "css/style.css"]

missing = []
for f in required:
    path = os.path.join(data_dir, f)
    if not os.path.isfile(path):
        missing.append(f)

if missing:
    print("[WARN] Missing web UI files in data/ (upload with: pio run -t uploadfs):")
    for f in missing:
        print(f"       data/{f}")
else:
    print("[OK] All web UI files present in data/")

print(f"[OK] Build environment: {env.get('PIOENV')}")
print("================================\n")
