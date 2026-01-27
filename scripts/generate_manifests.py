#!/usr/bin/env python3
"""
Generate ESP Web Tools manifests for firmware flashing.

Usage:
    python generate_manifests.py                    # Generate manifests
    python generate_manifests.py --dev              # Generate both stable and dev
    python generate_manifests.py --no-copy          # Skip copying firmware
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

# Import version detection functions from get_version module
import get_version as version_module


def check_firmware_exists(firmware_path):
    """Check if firmware file exists."""
    if not os.path.exists(firmware_path):
        print(f"Warning: Firmware file not found: {firmware_path}")
        print("Please build the firmware first with: idf.py build")
        return False
    return True


def copy_firmware_to_demo(build_dir, demo_dir):
    """Copy firmware files from build directory to demo."""
    import shutil

    # Source files
    bootloader = os.path.join(build_dir, "bootloader", "bootloader.bin")
    partition_table = os.path.join(build_dir, "partition_table", "partition-table.bin")
    app_bin = os.path.join(build_dir, "photoframe-api.bin")

    # Check if files exist
    if not all(os.path.exists(f) for f in [bootloader, partition_table, app_bin]):
        print("Error: Firmware files not found. Please build first with: idf.py build")
        return False

    # Create merged firmware using esptool
    merged_bin = os.path.join(demo_dir, "photoframe-firmware-merged.bin")

    try:
        subprocess.run(
            [
                "python3",
                "-m",
                "esptool",
                "--chip",
                "esp32s3",
                "merge_bin",
                "-o",
                merged_bin,
                "--flash_mode",
                "dio",
                "--flash_freq",
                "80m",
                "--flash_size",
                "16MB",
                "0x0",
                bootloader,
                "0x8000",
                partition_table,
                "0x10000",
                app_bin,
            ],
            check=True,
        )

        print(f"Created merged firmware: {merged_bin}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error creating merged firmware: {e}")
        return False


def generate_manifest(output_path, version, firmware_file, is_dev=False):
    """Generate a manifest.json file."""

    manifest = {
        "name": f"ESP32 PhotoFrame{' (Development)' if is_dev else ''}",
        "version": version,
        "home_assistant_domain": "esphome",
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 15,
        "builds": [
            {"chipFamily": "ESP32-S3", "parts": [{"path": firmware_file, "offset": 0}]}
        ],
    }

    with open(output_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"Generated manifest: {output_path}")
    print(f"  Version: {version}")
    print(f"  Firmware: {firmware_file}")


def generate_manifests(demo_dir, build_dir=None, dev_mode=False):
    """Generate manifest files for web flasher."""

    demo_path = Path(demo_dir)
    demo_path.mkdir(exist_ok=True)

    # Get stable version (latest tag)
    stable_version = version_module.get_stable_version()

    # Copy firmware if build_dir provided
    if build_dir:
        if not copy_firmware_to_demo(build_dir, demo_dir):
            return False

    # Check if firmware exists
    firmware_file = "photoframe-firmware-merged.bin"
    firmware_path = demo_path / firmware_file

    if not check_firmware_exists(firmware_path):
        return False

    # Generate stable manifest
    manifest_path = demo_path / "manifest.json"
    generate_manifest(manifest_path, stable_version, firmware_file, is_dev=False)

    # Generate dev manifest if in dev mode
    if dev_mode:
        # Get dev version (commit hash)
        dev_version = version_module.get_dev_version()
        dev_manifest_path = demo_path / "manifest-dev.json"
        # Dev manifest points to dev firmware file
        dev_firmware_file = "photoframe-firmware-dev.bin"
        # Check if dev firmware exists, fallback to merged if not
        if not (demo_path / dev_firmware_file).exists():
            dev_firmware_file = firmware_file
        generate_manifest(
            dev_manifest_path, dev_version, dev_firmware_file, is_dev=True
        )

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Generate ESP Web Tools manifests for firmware flashing"
    )
    parser.add_argument(
        "--demo-dir", default="demo", help="Demo directory (default: demo)"
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Build directory with firmware files (default: build)",
    )
    parser.add_argument(
        "--dev",
        action="store_true",
        help="Generate development manifest in addition to stable",
    )
    parser.add_argument(
        "--no-copy",
        action="store_true",
        help="Skip copying firmware from build directory",
    )

    args = parser.parse_args()

    # Get absolute paths - resolve relative to project root (parent of scripts dir)
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    demo_dir = project_root / args.demo_dir
    build_dir = project_root / args.build_dir if not args.no_copy else None

    # Generate manifests
    print("Generating manifests...")
    if not generate_manifests(demo_dir, build_dir, args.dev):
        sys.exit(1)

    print("\nManifests generated successfully!")
    print("\nTo test locally, run:")
    print("  python3 scripts/launch_demo.py")


if __name__ == "__main__":
    main()
