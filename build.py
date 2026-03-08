#!/usr/bin/env python3
import argparse
import subprocess
import sys
import os

# Add scripts to sys.path to import boards
sys.path.append(os.path.join(os.path.dirname(__file__), "scripts"))
from boards import SUPPORTED_BOARDS

BOARDS = list(SUPPORTED_BOARDS.keys())

def main():
    parser = argparse.ArgumentParser(description="Build firmware for different boards")
    parser.add_argument(
        "--board",
        choices=BOARDS,
        default="waveshare_photopainter_73",
        help="Board type to build",
    )
    parser.add_argument(
        "--fullclean",
        action="store_true",
        help="Remove sdkconfig and run idf.py fullclean before building",
    )
    # Allow passing extra arguments to idf.py
    args, extra_args = parser.parse_known_args()

    board = args.board

    if args.fullclean:
        print("Performing full clean...")
        for f in ["sdkconfig", "partitions.csv"]:
            if os.path.exists(f):
                os.remove(f)
                print(f"  ✓ Removed {f}")

        try:
            subprocess.run(["idf.py", "fullclean"], check=True)
            print("  ✓ idf.py fullclean completed")
        except subprocess.CalledProcessError as e:
            print(f"  ✗ idf.py fullclean failed with exit code {e.returncode}")
            sys.exit(e.returncode)

    sdkconfig_defaults = f"sdkconfig.defaults;boards/sdkconfig.defaults.{board}"

    try:
        subprocess.run("npm run build", shell=True, check=True, cwd="webapp")
    except subprocess.CalledProcessError as e:
        print(f"  ✗ npm run build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("  ✗ 'npm' not found. Please ensure Node.js is installed and in your PATH.")
        sys.exit(1)

    idf_base = [
        "idf.py",
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
    ]

    # Always build first to ensure sdkconfig and partitions.csv are generated,
    # then run any extra commands (e.g. flash, monitor) separately.
    build_cmd = idf_base + ["build"]
    print(f"Building for board: {board}")
    print(f"Running: {' '.join(build_cmd)}")

    try:
        subprocess.run(build_cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("Error: 'idf.py' not found. Please ensure ESP-IDF is correctly installed and activated.")
        print("Hint: Try adding ~/Work/esp/esp-idf/tools to your PATH.")
        sys.exit(1)

    if extra_args:
        post_cmd = idf_base + extra_args
        print(f"Running: {' '.join(post_cmd)}")
        try:
            subprocess.run(post_cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Post-build command failed with exit code {e.returncode}")
            sys.exit(e.returncode)

if __name__ == "__main__":
    main()
