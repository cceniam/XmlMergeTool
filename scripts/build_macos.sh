#!/bin/bash
set -euo pipefail
if [[ "$(uname -s)" != Darwin ]]; then
    echo 'This packaging script requires macOS.' >&2
    exit 1
fi
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
build_dir="${BUILD_DIR:-$project_root/build/desktop}"
output_dir="${OUTPUT_DIR:-$project_root/dist/native}"
iconset="$project_root/tmp/XmlMergeTool.iconset"
mkdir -p "$iconset" "$output_dir"
sips -s format png -z 128 128 resources/image/logo.ico --out "$iconset/icon_128x128.png" >/dev/null
sips -s format png -z 256 256 resources/image/logo.ico --out "$iconset/icon_128x128@2x.png" >/dev/null
iconutil -c icns "$iconset" -o "$project_root/tmp/XmlMergeTool.icns"
configure=(-S cpp -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=ON -DXMLMERGETOOL_BUILD_APP=ON -DCMAKE_OSX_ARCHITECTURES=arm64)
if [[ -n "${QT_ROOT_DIR:-}" ]]; then
    configure+=("-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR")
fi
cmake "${configure[@]}"
cmake --build "$build_dir" --parallel 4
ctest --test-dir "$build_dir" --output-on-failure
stage="$build_dir/package-stage"
cmake -E rm -rf "$stage"
cmake --install "$build_dir" --component Runtime --prefix "$stage"
codesign --force --deep --sign - "$stage/XmlMergeTool.app"
codesign --verify --deep --strict "$stage/XmlMergeTool.app"
cmake -E rm -rf "$output_dir/XmlMergeTool.app"
ditto "$stage/XmlMergeTool.app" "$output_dir/XmlMergeTool.app"
ditto -c -k --sequesterRsrc --keepParent "$output_dir/XmlMergeTool.app" "$output_dir/XmlMergeTool-macos-arm64.zip"
echo "Built: $output_dir/XmlMergeTool.app"
echo "Archive: $output_dir/XmlMergeTool-macos-arm64.zip"
