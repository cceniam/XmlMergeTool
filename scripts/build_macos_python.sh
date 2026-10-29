#!/bin/bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS packaging must run on macOS." >&2
    exit 1
fi

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
python_exe="${PYTHON_EXE:-$project_root/.venv/bin/python}"
export PYTHONPATH="$project_root/src${PYTHONPATH:+:$PYTHONPATH}"

version="$("$python_exe" -c 'import tomllib; print(tomllib.load(open("pyproject.toml", "rb"))["project"]["version"])')"
arch="$("$python_exe" -c 'import platform; print(platform.machine())')"
iconset="$project_root/tmp/XmlMergeTool.iconset"
mkdir -p "$iconset"
sips -s format png -z 128 128 resources/image/logo.ico --out "$iconset/icon_128x128.png" >/dev/null
sips -s format png -z 256 256 resources/image/logo.ico --out "$iconset/icon_128x128@2x.png" >/dev/null
iconutil -c icns "$iconset" -o tmp/XmlMergeTool.icns

"$python_exe" -m PyInstaller \
    --noconfirm \
    --windowed \
    --name XmlMergeTool \
    --icon "$project_root/tmp/XmlMergeTool.icns" \
    --osx-bundle-identifier us.cceniam.xmlmergetool \
    --distpath dist \
    --workpath build/macos \
    --specpath tmp \
    --paths src \
    --add-data "$project_root/resources:resources" \
    --collect-all robot \
    src/xml_merge_tool/main.py

/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $version" dist/XmlMergeTool.app/Contents/Info.plist
codesign --force --deep --sign - dist/XmlMergeTool.app
codesign --verify --deep --strict dist/XmlMergeTool.app
archive="dist/XmlMergeTool-macos-$arch.zip"
ditto -c -k --sequesterRsrc --keepParent dist/XmlMergeTool.app "$archive"
echo "Build complete: dist/XmlMergeTool.app"
echo "Distribution archive: $archive"
