# rf-report-cpp

独立的 C++17 Robot Framework HTML 日志/报告生成器。支持 RF 6.0/6.1
使用的 **schema 3/4 XML**，输出复用本项目原有 RF 6.1.1 HTML/JS/CSS。
运行时不使用 Python、不调用 Rebot，也不需要项目资源目录；静态资源由
CMake 在构建时嵌入。同一内核也由本项目的 C++ Qt 桌面应用直接调用；
桌面应用、合并服务和分发包构建见根目录 README。

## 构建与运行

在项目根目录执行，依赖 CMake >= 3.24、C++17 编译器、libxml2、zlib：

```sh
cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Release -DXMLMERGETOOL_BUILD_APP=OFF
cmake --build build/cpp -j 4
ctest --test-dir build/cpp --output-on-failure
build/cpp/rf-report-cpp input.xml --outputdir out
```

单独构建报告 CLI 不需要 Qt、Excel 库或 Python。省略
`XMLMERGETOOL_BUILD_APP=OFF` 时默认同时构建桌面应用，需要 Qt SDK。

```sh
build/cpp/rf-report-cpp input.xml --outputdir out --report NONE
build/cpp/rf-report-cpp input.xml --outputdir out \
  --log 日志.html --report 报告.html \
  --loglevel TRACE:INFO --logtitle '测试日志' --reporttitle '测试报告'
```

- 默认生成 `log.html` 和 `report.html`，路径相对于 `--outputdir`，亦接受绝对路径。
- `--log NONE` / `--report NONE` 可分别关闭输出。
- 保留日志级别：`TRACE`、`DEBUG`、`INFO`、`WARN`、`NONE`；默认 `TRACE`。
  冒号后可指定默认可见级别 `TRACE`、`DEBUG`、`INFO`、`WARN`；未指定则跟随保留级别。
- 退出码 `0..250` 是失败测试数（上限 250），报告生成成功也可能返回非零。
  `252` 表示输入或参数错误，`255` 表示生成或文件写入失败。
- 两个输出各自使用临时文件和替换操作，全部页面写入成功后才开始发布。
  替换按文件原子完成，不提供两个文件之间的事务保证；源文件及其硬链接/符号链接不得被覆盖。
- `file://` 离线可打开 HTML。图片等外部附件保持原链接，不自动复制；若需查看附件，仍需保留对应文件或可用的网络地址。

## 兼容范围

支持 suite/test/keyword、setup/teardown、RF 6.1 控制流、PASS/FAIL/SKIP/NOT RUN、
错误链接、日志级别、标题、文档格式、标签/套件/总计统计、原生页面搜索/排序/展开和跨页定位。
状态与统计根据实际测试重新计算，不信任 XML 中缓存的 `<statistics>`。
匹配 Rebot 对父套件缺失时间、套件 teardown 失败、仅生成 report 时的行为。
schema 3 和 4 共用结果模型及 RF 6.1.1 页面协议；无需修改输入 XML 的版本号。

本版不支持 RF 7 schema、损坏 XML 自动修复、DTD 或通过报告 CLI 合并多个输入。
合并和恢复由桌面应用的独立服务提供。
不支持的参数或 XML 版本明确报错，XML 层级上限为 512。
兼容指显示内容和行为一致，不要求 HTML 字节、字符串索引或生成时间相同。

XML DOM 读取后转换为独立结果模型并释放 DOM；生成阶段仍保留完整结果模型和当前页面数据，
不是常量内存流式实现。大于验收样本的文件应单独测量，不能由有限样本推断性能上限。

模块：`reader.cpp` 负责结果读取和语义，`encoder.cpp` 负责 RF 6.1.1 前端数据协议，
`format.cpp` 负责文本/时间/压缩，`html.cpp` 负责模板内联和输出，`main.cpp` 提供 CLI。
Unicode 归一化表已提交，构建无需生成；升级对照 Python/Unicode 版本时可运行
`tests/generate_unicode.py` 更新该表。

## 对照及浏览器验收

Python、Robot Framework 6.1.1、pytest、Node、Playwright 仅用于测试：

```sh
.venv/bin/python -m pytest cpp/tests/test_compat.py -q
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest -q
.venv/bin/python cpp/tests/benchmark.py
npm ci --prefix cpp/tests
node cpp/tests/browser.mjs
```

完整拆分日志浏览器验收可独立运行，不依赖先执行基准脚本：

```sh
.venv/bin/python cpp/tests/browser_acceptance.py \
  --executable build/desktop/rf-report-cpp --outputdir build/browser-acceptance
# CI 使用 Playwright 自带 Chromium：
node cpp/tests/node_modules/playwright/cli.js install chromium
.venv/bin/python cpp/tests/browser_acceptance.py \
  --executable build/desktop/rf-report-cpp --browser-channel chromium
```

最后两步使用本机 Chrome；可通过 `PLAYWRIGHT_CHANNEL` 指定其他已安装的
Playwright Chromium 渠道。浏览器使用独立测试配置，不访问用户浏览器资料。
浏览器用例默认读取 `build/cpp/acceptance/features/{native,reference}`，验证两种实现
的页面交互，输出截图及 `browser-results.json`。

`fixtures/schema3.xml` 由 RF 6.0.2 运行 `fixtures/features.robot` 生成，
仅将 source 路径改为相对路径；常规对照测试无需安装旧版 Robot。
大文件可先分别生成 `native/`、`reference/` 两套页面，再抽样验证单个测试的交互，避免展开整个日志：

```sh
node cpp/tests/browser-sample.mjs build/cpp/acceptance/schema3-large 'Example test'
```

`benchmark.py` 是 macOS 验收脚本，使用 `/usr/bin/time -l` 记录独立进程峰值 RSS，
同时输出解析/生成阶段耗时、进程总耗时、输入和产物大小到
`build/cpp/acceptance/benchmark.json`。Python 阶段计时从结果处理开始，进程计时包含启动和导入。

仓库的 `tests/fixtures/test.xml` 是故意截断的合成样本，因此测试同时覆盖：原文件被严格拒绝；
测试程序使用现有 lxml 修复逻辑创建共同输入后，与 Rebot 对照；使用项目实际合并器
生成 XML 后再对照。C++ CLI 本身不实施修复。

静态资源已与本地 RF 6.1.1 包逐文件核对一致。更新前端资源时必须同步核对数据协议并重跑验收。
本地验证平台为 macOS arm64。`.github/workflows/cpp-report.yml` 覆盖 Linux GCC/libstdc++
长文本回归、RF 6.1.1 对照及跨时区时间转换，以及 Windows SDK 构建和 CTest。
跨平台验证结果以实际 CI 运行结果为准。

第三方来源与授权见 `THIRD_PARTY_NOTICES.md` 和 `ROBOT_FRAMEWORK_LICENSE.txt`。

## 高级选项

```sh
build/cpp/rf-report-cpp input.xml --outputdir out --splitlog \
  --removekeywords passed --flattenkeywords iteration \
  --suitestatlevel 2 --reportbackground 'green:red:yellow'
```

- `--splitlog`：HTML 与同目录 `<log-stem>-N.js` 分片一起使用，支持离线展开和错误定位。
- `--removekeywords`：`passed`、`wuks`、`for`、`while`、`all`。
- `--flattenkeywords`：`for`、`while`、`iteration`。
- `--suitestatlevel`：根套件为第 1 层，默认全部；只限制统计表，不改变实际测试和标签统计。
- `--reportbackground`：`passed:failed` 或 `passed:failed:skipped`，省略 skipped 时使用 RF 默认黄色。

上述范围与当前 GUI 一致，不支持完整 Rebot 的自定义关键字名称/标签匹配表达式。
语义转换先于前端协议归一化，保留 RF 6.1.1 对 fixtures、WARN/ERROR、最后循环迭代、WUKS 和仅报告模式的规则。
输出暂存成功后逐文件替换，分片先于引用它们的 HTML 发布；多文件发布中断可能留下部分新产物。
