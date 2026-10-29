# XmlMergeTool

Robot Framework XML 桌面合并工具，正式应用采用 **C++17 + Qt 6 Widgets**。
XML 读取、修复、合并、HTML 日志/报告和 Excel 导出均为原生实现；运行时不需要 Python、Rebot 或 Node。
Python 实现保留在 `src/`，仅作为兼容性测试基准。

## 功能

- 打开或拖放多个 XML，支持平铺表格、路径树、搜索、状态/冲突/已勾选筛选、框选和右键操作。
- 以完整套件路径分组，PASS/SKIP 同级优先，随后取最新时间；时间回退顺序为套件结束、直接测试结束、文件修改时间。用户可修改默认勾选。
- 合并保留选中套件的完整子树并重新编号，输出 `merged-*.xml`、`log-*.html`、`report-*.html` 和 `conflict_report-*.csv`。父子套件同时选中时维持旧版的子树复制行为。
- 双击生成单套件日志，右键可打开源文件所在目录、导出单套件 XML，或将当前高亮选中的多个套件合并为一个 XML 并导出到指定目录。该合并导出仅生成 `merged-*.xml`，不改变勾选状态或全局输出目录；遇到同名文件自动添加序号。源 XML 及其符号链接/硬链接不得被覆盖。
- Excel 导出包含全部勾选项，包括筛选隐藏项。每个直接测试一行，耗时为可计算的数值，格式 `[h]:mm:ss.000`；保留冻结表头、筛选、交替底色和状态高亮。
- 全部原有高级选项：日志阈值/默认可见级别、标题、拆分日志、套件统计层级、报告背景、关键词裁剪和循环扁平化。
- 支持 RF 6.0/6.1 使用的 schema 3/4，以 Robot Framework 6.1.1 为报告语义基准。桌面加载保留损坏 XML 的恢复流程；独立报告 CLI 严格拒绝损坏输入。

HTML 支持 `file://` 离线打开。拆分日志须与同名 `-N.js` 文件一起保留；外部附件保持原链接，不自动复制。各文件分别原子替换，不提供跨文件事务保证。

## macOS 构建与打包

需要 Apple Silicon Mac、Xcode Command Line Tools、CMake >= 3.24、Ninja 和 Qt >= 6.9。CI 固定 Qt 6.11.2；macOS 的 libxml2/zlib 来自 SDK。

```sh
brew install cmake ninja qtbase
cmake -S cpp --preset desktop
cmake --build build/desktop --parallel 4
ctest --test-dir build/desktop --output-on-failure
open build/desktop/XmlMergeTool.app

# 收集运行库、签名、打包
./scripts/build_macos.sh
```

使用非默认 Qt SDK 时设置 `QT_ROOT_DIR`（打包脚本）或 `CMAKE_PREFIX_PATH`（直接 CMake）。
新增依赖 libxlsxwriter 1.2.4 和 miniz 3.1.2 由 CMake 下载、校验 SHA256 并静态链接。

输出 `dist/native/XmlMergeTool.app` 和 `dist/native/XmlMergeTool-macos-arm64.zip`。
包内包含 Qt 和所需运行库，不依赖构建机的 Homebrew 路径。使用 ad-hoc 签名，未进行 Apple notarization。
macOS 日志写入 `~/Library/Logs/XmlMergeTool/debug.log`。

macOS 26+ 默认保留窗口 Accessibility 隔离，以规避已知 Qt 树视图原生崩溃；鼠标和键盘操作不受影响。
设置 `ROBOT_XML_MERGE_ENABLE_MACOS_ACCESSIBILITY=1` 可恢复 VoiceOver/Accessibility 自动化访问；该设置会重新开放原崩溃路径。

## Windows 单文件构建

需要 Visual Studio 2022 C++ 工具、CMake、Qt 6.11.2 MSVC2022 x64 SDK 和已 bootstrap 的 vcpkg。

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\6.11.2\msvc2022_64'
$env:VCPKG_ROOT = 'C:\vcpkg'
./scripts/build_windows.ps1
```

vcpkg manifest 固定 libxml2/zlib 依赖基线。脚本构建应用、运行 CTest、使用 Qt 部署工具收集运行库，再生成 `dist/native/XmlMergeTool.exe`。
该 exe 是无需安装的原生启动器：每次启动将完整运行包释放到独占临时目录，程序退出后清理；支持中文路径和同时启动，不需要管理员权限。
它包含动态 Qt 运行库，并非将 Qt 静态链接到单个进程。运行不依赖 Python、PowerShell 或外部解压工具。

## 验证

```sh
uv sync --frozen --group dev
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest tests -q
RF_REPORT_CPP="$PWD/build/desktop/rf-report-cpp" \
XML_MERGE_DRIVER="$PWD/build/desktop/xml-merge-driver" \
.venv/bin/python -m pytest cpp/tests/test_compat.py cpp/tests/test_app_compat.py -q
.venv/bin/python cpp/tests/test_package.py \
  dist/native/XmlMergeTool.app/Contents/MacOS/XmlMergeTool
```

`--smoke-test INPUT_XML OUTPUT_DIR` 是分发包验证入口：使用真实 Qt 平台插件和后台服务执行加载、视图切换、合并、Excel、单项 XML/日志，输出 `smoke-results.json`，测试设置与用户偏好隔离。
测试控制脚本可使用 Python，但被测进程移除 Python 环境及开发工具 PATH。

报告 CLI、高级选项、浏览器对照与验证边界见 [cpp/README.md](cpp/README.md) 和 [cpp/VALIDATION.md](cpp/VALIDATION.md)。
合成大文件的耗时与峰值内存测量方法见 [cpp/BENCHMARKS.md](cpp/BENCHMARKS.md)。
自动流水线为 `native-desktop.yml`；旧 Nuitka 流水线仅保留手工触发。旧 macOS Python 打包入口为 `scripts/build_macos_python.sh`。
第三方来源及授权见 [cpp/THIRD_PARTY_NOTICES.md](cpp/THIRD_PARTY_NOTICES.md)。
