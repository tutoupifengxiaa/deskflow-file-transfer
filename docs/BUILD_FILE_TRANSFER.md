# 构建文件传输增强版

使用本 Release 标签的完整源码，包括 LICENSE、LICENSES、REUSE.toml 和 deploy 目录。下面使用示例路径，按本机安装位置设置 Qt/OpenSSL；不需要个人配置、证书或 SSH 密钥。原有各平台构建材料也保留在 docs 和 .github 中。

## macOS Apple Silicon

本次使用 Xcode Command Line Tools、CMake/Ninja、Qt 6.10.3 macOS 和静态 OpenSSL 3.5.5。**发布包的最低部署目标为 macOS 26.0**，更早系统需要另行构建并验证。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSKIP_BUILD_TESTS=ON -DBUILD_INSTALLER=ON \
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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSKIP_BUILD_TESTS=ON -DBUILD_INSTALLER=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64 -DOPENSSL_ROOT_DIR=C:/OpenSSL -DCMAKE_INSTALL_PREFIX=%CD%/stage
cmake --build build --parallel 4
cmake --install build
cd build
cpack -G WIX -D CPACK_PACKAGE_VERSION=1.26.917 -D "CPACK_PACKAGE_NAME=Deskflow File Transfer (Unofficial)" -D CPACK_PACKAGE_FILE_NAME=Deskflow-drag-20260917-Windows-x64
```

MSI 使用上游 UpgradeCode，会替换已有 Deskflow 并管理同名服务，不是并行安装。安装目标需要 Visual C++ 2022 x64 运行库；MSI 包未做可信发布者签名。已验证原路径升级、服务启动和程序哈希，全新机器及卸载尚未验收。

## 交付检查

核对 Release 的 SHA256 清单；双端使用配套版本。测试普通文件、空文件、中文文件名、300 MiB 文件双向复制和拖拽，并确认松手后切屏及键盘正常。测试时使用临时文件。每个二进制 Release 必须提供对应源码标签及本构建说明，并保留上游与依赖许可。
