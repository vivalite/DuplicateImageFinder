# 重复图片查找器

一个中文 Windows 11 便携程序，用于扫描本地固定硬盘上的图片文件，找出内容完全相同的重复图片，并支持手动勾选或自动勾选后放入回收站。

## 功能

- 扫描所有本地固定硬盘，不扫描网络盘和可移动盘。
- 支持常见图片扩展名：JPG、PNG、BMP、GIF、WEBP、TIFF、HEIC、ICO。
- 先按文件大小分组，再计算 SHA-256，只有文件内容完全一致才判定为重复。
- 每组重复图片可查看文件路径、大小、创建时间和图片预览。
- “自动勾选”会保留每组创建时间最早的文件，勾选其余重复文件。
- “删除所选”会把勾选文件放入 Windows 回收站。

## 构建

需要 Windows 上的 CMake 和 MSVC 工具链。

```powershell
cmake -S . -B build
cmake --build build --config Release
```

生成的便携 EXE：

```text
build\Release\DuplicateImageFinder.exe
```

Release 配置使用 MSVC 静态运行库，发布目录只需要这个 EXE 文件。
