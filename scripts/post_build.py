"""
post_build.py — PlatformIO post-build script
Runs after a successful build. Prints firmware size summary
and reminds the user to upload the filesystem if needed.
"""
Import("env")

def post_build(source, target, env):
    print("\n=== T-Embed SI4732 Post-Build ===")
    print("[OK] Firmware build complete.")
    print("[>>] To flash firmware:    pio run --target upload")
    print("[>>] To flash web UI:      pio run --target uploadfs")
    print("[>>] To do both at once:   pio run --target upload && pio run --target uploadfs")
    print("=================================\n")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", post_build)
