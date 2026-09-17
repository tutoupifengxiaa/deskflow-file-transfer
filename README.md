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

详细安装说明随 Release 提供；下方构建说明与二次开发说明 随源码提供。源码及构建定义保留在本仓库；每个二进制 Release 应关联对应源码提交/标签。

## 许可和来源

项目代码继续遵守上游 [GPL-2.0](LICENSE) 及适用的 [OpenSSL 例外](LICENSES/LicenseRef-OpenSSL-Exception.txt)。各文件、文档、图标和依赖也可能有各自许可，参见原有文件头、[LICENSES](LICENSES) 和 [REUSE.toml](REUSE.toml)。本分支不声称拥有上游代码、名称或图标的原创权利。

发布或再次分发修改版时，应保留版权/许可声明，标明修改，并按适用许可证提供对应源码及构建材料。分发不提供额外质量保证。

上游说明见 [Deskflow 官方仓库](https://github.com/deskflow/deskflow)。本分支问题请反馈到本仓库。

## 源码发布范围

本仓库为功能源码快照，不含 docs、测试目录、临时文件和上游 CI。完整开发历史保留在本地，上游基线见上文。省略离线帮助文档时，程序使用已有的在线帮助入口。构建文件仅移除 docs 和 unittests 子目录引用，不改变功能实现。

## 构建文件传输增强版

使用本 Release 标签的完整源码，包括 LICENSE、LICENSES、REUSE.toml 和 deploy 目录。下面使用示例路径，按本机安装位置设置 Qt/OpenSSL；不需要个人配置、证书或 SSH 密钥。

## macOS Apple Silicon

本次使用 Xcode Command Line Tools、CMake/Ninja、Qt 6.10.3 macOS 和静态 OpenSSL 3.5.5。**发布包的最低部署目标为 macOS 26.0**，更早系统需要另行构建并验证。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_INSTALLER=ON \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.3/macos \
  -DOPENSSL_ROOT_DIR=/path/to/openssl \
  -DCMAKE_INSTALL_PREFIX="$PWD/stage"
cmake --build build --parallel 4
cmake --install build
# 本次本地签名采用不启用 hardened runtime 的方式，未做 Apple 公证。
codesign --force --deep --sign - --options 0 stage/Deskflow.app
mkdir -p dmg-stage
cp -R stage/Deskflow.app dmg-stage/
ln -s /Applications dmg-stage/Applications
hdiutil create -volname 'Deskflow File Transfer' -srcfolder dmg-stage \
  -ov -format UDZO Deskflow-drag-20260917-macOS-arm64.dmg
```

重新构建会受到编译器、Git 版本元数据和签名的影响，不保证逐字节相同。重新签名安装后可能需要重新授予辅助功能和输入监控权限。

## Windows x64

在 Visual Studio C++ x64 开发者命令提示符中运行。需要 CMake/Ninja、Qt 6.10.3 msvc2022_64、OpenSSL x64、.NET SDK 和 WiX 4.0.6。将它们的 bin 目录加入 PATH。

```bat
dotnet tool install wix --version 4.0.6 --tool-path installer-tools
set "PATH=%CD%\installer-tools;%PATH%"
set "DOTNET_ROLL_FORWARD=Major"
wix extension add -g WixToolset.Util.wixext/4.0.6
wix extension add -g WixToolset.Firewall.wixext/4.0.6
wix extension add -g WixToolset.UI.wixext/4.0.6
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_INSTALLER=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64 -DOPENSSL_ROOT_DIR=C:/OpenSSL -DCMAKE_INSTALL_PREFIX=%CD%/stage
cmake --build build --parallel 4
cmake --install build
cd build
cpack -G WIX -D CPACK_PACKAGE_VERSION=1.26.917 -D "CPACK_PACKAGE_NAME=Deskflow File Transfer (Unofficial)" -D CPACK_PACKAGE_FILE_NAME=Deskflow-drag-20260917-Windows-x64
```

MSI 使用上游 UpgradeCode，会替换已有 Deskflow 并管理同名服务，不是并行安装。安装目标需要 Visual C++ 2022 x64 运行库；MSI 包未做可信发布者签名。已验证原路径升级、服务启动和程序哈希，全新机器及卸载尚未验收。

## 交付检查

核对 Release 的 SHA256 清单；双端使用配套版本。测试普通文件、空文件、中文文件名、300 MiB 文件双向复制和拖拽，并确认松手后切屏及键盘正常。测试时使用临时文件。每个二进制 Release 必须提供对应源码标签及本构建说明，并保留上游与依赖许可。
