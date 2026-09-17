# 二次开发说明

维护者：tutoupifengxiaa。修改日期：2026-09-15 至 2026-09-17。
上游： https://github.com/deskflow/deskflow
基线提交：`ce2f948c296217ac722215eacd9682b5d3e94171`。

这是非官方衍生版本，保留上游版权与所有适用许可证。修改包括图片剪贴板限制兼容、macOS 输入及指针修复、双向普通文件复制粘贴、传输进度，以及 Finder/Explorer 原生跨屏文件拖放。修改文件的文件头标注本次修改和日期；相对基线的 Git diff 提供完整变更。

发布的 MSI/DMG 来自本分支已实机验证的实现；发布整理新增的文件头注释、README 和构建说明不改变程序行为。安装器版本 1.26.917，应用本身仍基于上游 1.26.0。二进制不包含用户配置、配对证书或测试文件。

仅验证 macOS Apple Silicon 服务器与 Windows x64 客户端。支持普通文件，目录与符号链接不支持；多客户端及其它平台组合未验收。详见 README 与 Release 安装说明。

构建见 [BUILD_FILE_TRANSFER.md](BUILD_FILE_TRANSFER.md)。代码遵守原文件的许可；GPL 文件继续使用 GPL-2.0-only 及适用的 OpenSSL 例外，不改变 MIT 等文件的原许可。
