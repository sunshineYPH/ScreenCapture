# Screen Capture

一个轻量级的 X11 截屏工具，纯 C++17 实现，无外部图像库依赖（除系统 X11 / zlib）。

## 功能特性

- 区域截屏：鼠标拖拽选择任意矩形区域s
- 半透明灰色遮罩：选区外为半透明效果，选区内保持原图
- 绘图工具：箭头、矩形、椭圆、文字（支持中文 IME 输入）
- 颜色选择：红/黄/蓝/绿/灰/白/黑 共 7 种
- 操作：确认（保存到剪贴板 + 文件）、保存（仅文件）、取消、贴图（Pin）
- 撤销 / 重做：Ctrl+Z / Ctrl+Y
- 方向键微调：拉框后按 ←/→/↑/↓ 移动选区（Shift 加速 10px）
- 贴图窗口：可拖动、可关闭的浮动图片窗口
- 截图自动保存到 `~/Pictures/Screenshots/YYYYMMDD_hhmmss.png`

## 系统要求

- Linux + X11（依赖 `libX11`）
- FreeType（系统库 `libfreetype.so.6`）
- zlib（用于 PNG 编码）
- 构建工具：`cmake >= 3.10`、`g++`（支持 C++17）

## 构建

```bash
cd /path/to/screen_shot
mkdir -p build && cd build
cmake ..
make -j4
```

产物：`build/screen_capture`

## 运行

```bash
./build/screen_capture
```

配置全局热键（默认 `Shift+S`，若被窗口管理器拦截会回退到 `Ctrl+Shift+S` / `Ctrl+S` / `S`）启动截屏，终端会打印实际注册的组合。
Ubuntu 上 Setting -> Keyboard -> Shortcuts -> Custom -> + -> `Shift+Alt+S`

## 使用流程

1. 启动后屏幕变暗（半透明灰色遮罩）
2. 鼠标按住左键拖拽，画出选区
3. 选区下方出现工具栏：`Arrow`, `Rect`, `Ellipse`, `Text`, `Size`, `Color`, `Undo`, `Redo`, `Save`, `Pin`, `Cancel`, `Confirm`
4. 拉框阶段可用方向键微调位置（Shift 加速 10px）
5. 完成编辑后点 `Confirm` 进入剪贴板，切换到任意窗口按 `Ctrl+V` 粘贴
6. `Save` 仅保存到文件，不进入剪贴板
7. `Cancel` 退出，不保存
8. `Pin` 在屏幕上贴一张可拖动 / 可关闭的图片
9. 任何时候按 `Esc` 退出

## 文字输入

1. 点击工具栏 `Text`
2. 在选区内点击任意位置，出现文本输入框
3. 键盘直接输入英文
4. 切换到中文输入法（fcitx / ibus）后输入拼音，commit 的汉字会自动出现在输入框
5. `Enter` 提交文字（成为选区内的一个文字标注）
6. `Backspace` 删除一个完整字符（中英文均正确处理 UTF-8）
7. `Esc` 取消当前文字输入

字号：在文字输入状态下点 `Size` 循环切换（输入框文字实时变化）。
颜色：点 `Color` 弹出子菜单选择（输入框文字立即按所选颜色显示）。

## 贴图窗口（Pin）

- 点击 `Pin` 后，主程序退出，当前选区图片作为一个独立窗口显示在屏幕 `(100, 100)` 位置
- 窗口带 `RGB(0,122,255)` 蓝色边框（2px）
- 拖动：除右上角 X 按钮外任意位置按下并拖动
- 关闭：点击右上角 X / 按 `Esc` / 从窗口管理器关闭
- 临时文件存于 `/tmp`，关闭时自动清理


## 截图文件命名

保存路径：`~/Pictures/Screenshots/YYYYMMDD_hhmmss.png`

例：`~/Pictures/Screenshots/20260724_203045.png`


## 已知行为

- 无 IME 时 `XOpenIM` 会失败并打印警告 `Warning: XOpenIM failed, IME input will not work`；英文输入仍正常工作（自动回退到 `XLookupString`）
- 无窗口管理器（如纯 Xvfb）下全局热键仍可工作（`XGrabKey` 不依赖 WM）
- 程序退出后约 2 秒内会自动终止（满足"按 Ctrl+V 后立即退出"的预期）

---

# English Version

A lightweight X11 screenshot tool, pure C++17 with no external image library dependencies (besides system X11 / zlib).

## Features

- Region capture: drag to select any rectangular area
- Semi-transparent gray mask outside the selection; original content preserved inside
- Drawing tools: Arrow, Rectangle, Ellipse, Text (with CJK / IME support)
- Color picker: 7 colors (Red / Yellow / Blue / Green / Gray / White / Black)
- Actions: Confirm (clipboard + file), Save (file only), Cancel, Pin (float on screen)
- Undo / Redo: Ctrl+Z / Ctrl+Y
- Arrow-key nudge: after drawing the selection, press ← / → / ↑ / ↓ to move it (Shift = 10px step)
- Pin window: draggable / closable floating image window
- Auto-saved to `~/Pictures/Screenshots/YYYYMMDD_hhmmss.png`

## System Requirements：

- Linux + X11 (`libX11`)
- FreeType (`libfreetype.so.6`)
- zlib (for PNG encoding)
- Build tools: `cmake >= 3.10`, `g++` with C++17 support

## Build

```bash
cd /path/to/screen_shot
mkdir -p build && cd build
cmake ..
make -j4
```

Output: `build/screen_capture`

## Run

```bash
./build/screen_capture
```

## Pin Window

- After clicking `Pin`, the main process exits and the selected image appears as an independent window at screen position (100, 100)
- The window has a 2px `RGB(0, 122, 255)` blue accent border
- Drag: press and hold anywhere outside the close button
- Close: click the X in the top-right, press `Esc`, or close via the window manager
- A temporary file is stored in `/tmp` and cleaned up on close

## Screenshot File Naming

Saved to: `~/Pictures/Screenshots/YYYYMMDD_hhmmss.png`

Example: `~/Pictures/Screenshots/20260724_203045.png`

## Known Behavior

- If no IME is installed, `XOpenIM` fails and prints `Warning: XOpenIM failed, IME input will not work`; ASCII input still works (automatic fallback to `XLookupString`)
- Global hotkeys work even without a window manager (e.g. plain Xvfb) because `XGrabKey` does not depend on the WM
- The program auto-terminates within ~2 seconds of responding to the clipboard TARGETS request
