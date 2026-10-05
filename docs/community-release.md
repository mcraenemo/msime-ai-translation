# 社区版发布构建

本分支发布未签名的 Windows x64 包，Server uiAccess=false；不运行本地测试签名脚本，不分发个人证书。原作者、GPL-3.0 与第三方授权必须随包保留。

1. 在无个人信息的构建路径下编译（例如 C:\MSIME-Community-Build）。准备官方工具链和依赖，按 product-lock.json、language-model/lock.json、neural-model/lock.json 校验只读资源；禁止从用户数据目录整目录复制。
2. Server 配置开启 `-DMSIME_BUILD_BOOST_JSON_FROM_HEADERS=ON`，避免预编译 Boost.JSON 中的开发者路径。构建所有安装所需生产目标及测试。Windows 组件分别编译 x86/x64，使用静态 CRT。构建设置页并运行测试。
3. 执行 `Prepare-PackageFiles.ps1 -TargetVersion 0.9.5`，不加 IncludeSymbols。然后 `python installer/Remove-PdbPaths.py installer/server_exe installer/tsf_dll`，只清理未签名 PE 的 CodeView PDB 路径元数据，不修改程序代码。此步骤应在任何正式签名之前执行。
4. 检查暂存文件：出厂凭据为空；无用户配置、翻译库、OAuth JSON、令牌、用户词库、日志、备份、证书或个人路径。官方词库数据库仅允许匹配锁定摘要的只读版本。
5. `Compile-Installer.ps1` 使用 Inno Setup 6.6+ 编译。不要在开发用户现有安装上执行安装验收；全新系统和第三方应用的完整安装验收需另做。
6. 发布对应 Git tag 的源码、未签名安装器、独立浏览器扩展 ZIP 和 SHA256SUMS.txt。浏览器扩展无需包含测试文件，也不得包含 API 凭据。

本地翻译与 Google 同步用户数据由升级保留；更换数据路径亦迁移。本机浏览器桥接注册仍需扩展 install-native-host.ps1（兼容 Windows PowerShell 5.1/PowerShell 7）。
