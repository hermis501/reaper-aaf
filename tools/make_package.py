"""Builds the release package from a Release build.

    cmake -S . -B build -A x64
    cmake --build build --config Release
    python tools/make_package.py [build/Release folder]

Writes dist/AAF-for-REAPER/ (DLL, command-line tools, README, third-party notices) and
dist/AAF-for-REAPER-<version>-win64.zip. The version comes from CMakeLists.txt.
"""

import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

FOLDER = "AAF-for-REAPER"
DLL = "reaper_aaf-x64.dll"
TOOLS = ["aaf2rpp.exe", "rpp2aaf.exe", "aafdump.exe"]
# (source in the repository, path in the package)
EXTRA = []
NOTICES = [
    ("AAF object model tables, generated from pyaaf2", "third_party/pyaaf2-model/LICENSE"),
    ("pugixml (XML parser)", "third_party/pugixml/LICENSE.md"),
    ("REAPER extension SDK header (Cockos)", "third_party/reaper-sdk/LICENSE"),
]


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(\w+ VERSION ([\d.]+)", f.read())
    return m.group(1) if m else "0.0.0"


def main():
    bin_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "Release")
    ver = version()
    out = os.path.join(DIST, FOLDER)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "command-line"))
    for name in [DLL] + TOOLS:
        if not os.path.isfile(os.path.join(bin_dir, name)):
            sys.exit(f"{os.path.join(bin_dir, name)} not found - build the Release configuration first")
    shutil.copy2(os.path.join(bin_dir, DLL), out)
    for t in TOOLS:
        shutil.copy2(os.path.join(bin_dir, t), os.path.join(out, "command-line"))
    for src, dst in EXTRA:
        os.makedirs(os.path.dirname(os.path.join(out, dst)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, src), os.path.join(out, dst))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))

    with open(os.path.join(ROOT, "packaging", "README.md"), encoding="utf-8") as f:
        readme = f.read().replace("{VERSION}", ver)
    with open(os.path.join(out, "README.md"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(readme)

    notices = [f"{FOLDER} includes the following third-party code.\n"]
    for title, path in NOTICES:
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            notices.append("=" * 72 + f"\n{title}\n" + "=" * 72 + "\n\n" + f.read().strip() + "\n")
    with open(os.path.join(out, "THIRD-PARTY-NOTICES.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(notices))

    zip_path = os.path.join(DIST, f"{FOLDER}-{ver}-win64.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(out):
            for name in sorted(files):
                full = os.path.join(base, name)
                z.write(full, os.path.join(FOLDER, os.path.relpath(full, out)))
    print(zip_path)


if __name__ == "__main__":
    main()
