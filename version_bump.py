"""
PlatformIO pre-build script.

Increments APP_VERSION in src/main.cpp by 0.01 on every build, as described in
README.md ("Each build updates version by 0.01").

The source file is rewritten before compilation, so the new value is compiled
into the firmware. Disable with an environment variable if required:

    PLATFORMIO_SKIP_VERSION_BUMP=1 pio run
"""

import os
import re
from pathlib import Path

# Matches:  #define APP_VERSION "0.03"
VERSION_PATTERN = re.compile(r'(#define\s+APP_VERSION\s+")(\d+)\.(\d+)(")')


def find_project_dir() -> Path:
    """Locates the directory holding platformio.ini.

    PlatformIO executes extra scripts with exec(), so __file__ is not
    available; walk up from the working directory instead.
    """
    current = Path(os.getcwd()).resolve()

    for candidate in [current, *current.parents]:
        if (candidate / "platformio.ini").is_file():
            return candidate

    return current


def main() -> None:
    if os.environ.get("PLATFORMIO_SKIP_VERSION_BUMP"):
        print("[version-bump] Skipped (PLATFORMIO_SKIP_VERSION_BUMP set).")
        return

    try:
        project_dir = find_project_dir()
        source_file = project_dir / "src" / "main.cpp"

        if not source_file.is_file():
            print(f"[version-bump] ERROR: {source_file} not found.")
            return

        text = source_file.read_text(encoding="utf-8")
        match = VERSION_PATTERN.search(text)

        if match is None:
            print("[version-bump] ERROR: APP_VERSION define not found in main.cpp.")
            return

        major = int(match.group(2))
        minor = int(match.group(3))
        current = f"{major}.{minor:02d}"

        new_minor = minor + 1
        if new_minor > 99:  # carry into the major number
            major += 1
            new_minor = 0

        new_version = f"{major}.{new_minor:02d}"

        if new_version == current:
            return

        updated = text[: match.start(2)] + new_version + text[match.end(3) :]
        source_file.write_text(updated, encoding="utf-8")

        print(f"[version-bump] APP_VERSION {current} -> {new_version}")

    except Exception as exc:  # never block the build
        print(f"[version-bump] WARNING: {exc}")


main()
