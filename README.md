# Deskflow 文件传输增强版

本项目是基于 [Deskflow](https://github.com/deskflow/deskflow) 的**非官方二次开发版本**，不是 Deskflow 官方发行版，也不代表上游项目。感谢 Deskflow 及其前身项目的开发者和贡献者；保留原作者版权和许可声明。

基于上游提交 `ce2f948c296217ac722215eacd9682b5d3e94171`，本轮修改日期为 2026-09-15 至 2026-09-17。

## 本次二次开发

- 调整图片剪贴板大小限制及旧客户端兼容处理。
- 修复 macOS 键盘输入线程问题和跨屏指针状态问题。
- 新增 Windows ↔ macOS 普通文件复制粘贴及接收进度。
- 新增 Finder / Windows 文件资源管理器之间的原生跨屏文件拖拽。
- 修复 Windows 服务读取文件、接收文件权限，以及拖放交接、松手和目标闪烁问题。

## 已验证的环境与功能

测试组合为 macOS 26.0+ Apple Silicon 服务器 + Windows x64 客户端。

双向普通文件、中文文件名、空文件和 300 MiB 文件拖拽已实际落地并通过 SHA256 校验。大文件传输未结束时可以松手，随后完成写入。连续拖动、闪烁、切屏及键盘输入已实机检查。此前双向图片和文件复制粘贴也已验证。

**范围限制：**仅支持普通文件，不支持文件夹或符号链接；批量文件、其它目标应用、同名冲突、多客户端及其它平台组合尚未完整实机验证。不能将本分支的验证结果视为上游或所有环境的保证。

## 下载与安装

在 [Releases](https://github.com/tutoupifengxiaa/deskflow-file-transfer/releases) 下载配套的两个安装包及 SHA256 校验清单：

- `Deskflow-drag-20260917-macOS-arm64.dmg`
- `Deskflow-drag-20260917-Windows-x64.msi`
- `Deskflow-drag-20260917-SHA256.txt`

Mac：打开 DMG，将 Deskflow.app 拖入 Applications；应用为 ad-hoc 本地签名，未经 Apple 公证。Windows：双击 MSI 按向导安装，需要管理员权限及 Visual C++ 2022 x64 运行库。MSI 会升级现有 Deskflow 并管理同名服务，不支持与官方版并行安装；安装器版本为 1.26.917。已验证现有机器原位升级，全新机器安装尚未验证。两端需要配套升级，更新前保留自己的配置。

详细安装说明随 Release 提供；[构建说明](docs/BUILD_FILE_TRANSFER.md) 与 [二次开发说明](docs/SECONDARY_DEVELOPMENT.md) 随源码提供。源码及构建定义保留在本仓库；每个二进制 Release 应关联对应源码提交/标签。

## 许可和来源

项目代码继续遵守上游 [GPL-2.0](LICENSE) 及适用的 [OpenSSL 例外](LICENSES/LicenseRef-OpenSSL-Exception.txt)。各文件、文档、图标和依赖也可能有各自许可，参见原有文件头、[LICENSES](LICENSES) 和 [REUSE.toml](REUSE.toml)。本分支不声称拥有上游代码、名称或图标的原创权利。

发布或再次分发修改版时，应保留版权/许可声明，标明修改，并按适用许可证提供对应源码及构建材料。分发不提供额外质量保证。

上游说明保留在 [.github/README.md](.github/README.md)；本分支问题请反馈到本仓库，以免与上游官方版本混淆。
