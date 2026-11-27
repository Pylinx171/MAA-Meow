# Pylinx171 自动同步与发布

日常调度在北京时间 09:19 运行。工作流从 Aliothmoon/MAA-Meow 选择最新正式版本标签，合并进临时分支，自动审查通过后才构建。Android 显示版本跟随上游标签，例如上游 `v0.28.2` 对应 `0.28.2`。发布使用独立且递增的 Android `versionCode`。失败重试可能留下版本号间隔。

## 自动审查与分支保护

`pylinx/automated-review` 是 `pylinx171` 的必需检查。定时更新检查候选 App 提交与 MaaCore 版本差异，限制构建入口、发布配置、可执行文件和高风险模式的变更，并用固定版本的 Gitleaks 扫描新增内容。通过后才把候选提交、审核后的构建计划和固定的 MaaCore 提交交给无签名 Secrets 的构建任务。发布任务会独立核对构建计划、APK 签名和制品哈希。审查失败时会停止并发送邮件。

针对 `pylinx171` 的 PR 也会自动检查。审查任务从目标分支加载工具，只读取 PR 提交中的 Git 对象。推送新提交后会重新检查。外部 PR 仍需要有人决定是否合并，定时任务只自动接收指定上游的正式版本。自动检查只能发现其规则覆盖的问题。

## GitHub 一次性配置

### 1. 推送当前分支

在本仓库根目录运行。当前 Mac 上已有但失效的 HTTPS 凭据需要重新授权。浏览器里选择有权写入 `Pylinx171/MAA-Meow` 的 GitHub 账号。

```sh
brew install gh
gh auth login --hostname github.com --git-protocol https --web --scopes workflow
gh auth setup-git
git push -u origin pylinx171
```

到 [仓库 Settings](https://github.com/Pylinx171/MAA-Meow/settings) 的 General 页面，将 Default branch 改为 `pylinx171`。到 [Actions](https://github.com/Pylinx171/MAA-Meow/actions) 启用 fork 的工作流。GitHub 定时工作流只从默认分支运行。仓库原 `main` 仍可用于查看上游。若 `pylinx171` 启用了分支保护，检查是否允许工作流身份快进写入该分支。[GitHub 默认分支文档](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-branches-in-your-repository/changing-the-default-branch) · [定时触发](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule)

### 2. 配置签名 Secrets

在 [Actions secrets](https://github.com/Pylinx171/MAA-Meow/settings/secrets/actions) 里选择 New repository secret，填入以下四项。要覆盖当前手机上的 `com.aliothmoon.maameow.pylinx171` 测试包，必须使用之前给它签名的那一份 keystore。

| Secret | 值 |
|---|---|
| `PYLINX_KEYSTORE_B64` | 原测试签名文件的 Base64，用下面的命令直接设置 |
| `PYLINX_KEYSTORE_PASSWORD` | 原 keystore 密码，本机标准 Android debug keystore 默认为 `android` |
| `PYLINX_KEY_PASSWORD` | 原私钥密码，标准 debug keystore 默认为 `android` |
| `PYLINX_KEY_ALIAS` | 原私钥别名，标准 debug keystore 为 `androiddebugkey` |

在原来打包的 Mac 上运行以下命令，将现有 keystore 直接写入 GitHub Secret。

```sh
base64 < "$HOME/.android/debug.keystore" | tr -d '\n' | gh secret set PYLINX_KEYSTORE_B64 --repo Pylinx171/MAA-Meow
```

工作流在独立的签名任务读取这些 Secrets，并对照当前测试版的公开证书 SHA-256 指纹 `7e70e6cf65424494f8dcbf8a37b51f20634ceceb32b7c1a134123af3394a0c11`。如果想换成新的发布签名，需要先处理手机上旧测试包的数据迁移。 [Android 覆盖更新条件](https://developer.android.com/google/play/app-updates)

### 3. 配置 Gmail 邮件

在 [Google 应用专用密码](https://myaccount.google.com/apppasswords) 为工作流创建专用密码。该页面要求 Google 账号开启两步验证。回到同一个 GitHub Actions Secrets 页面，新增两项。

| Secret | 值 |
|---|---|
| `PYLINX_GMAIL_ADDRESS` | 用于 SMTP 登录和接收告警的 Gmail 地址 |
| `PYLINX_GMAIL_APP_PASSWORD` | Google 生成的应用专用密码 |

邮件包含变更或冲突文件、失败阶段和 GitHub Actions 运行链接。工作流也会在 Actions 页面保留告警摘要。 [Google 应用专用密码说明](https://support.google.com/accounts/answer/185833)

### 4. 验证并发布

1. 在 [Pylinx171 auto release](https://github.com/Pylinx171/MAA-Meow/actions/workflows/pylinx-release.yml) 选择 Run workflow 和 `pylinx171`，勾选 `test_email`。确认收到测试邮件。
2. 再运行一次，两个复选框都留空。确认同步、接入检查、ARM64 定制 MaaCore、Android 回归测试和打包通过。
3. 再运行一次，勾选 `publish`。确认 GitHub Release 有签名 APK 和 MaaResource ZIP，[静态更新清单](https://raw.githubusercontent.com/Pylinx171/MAA-Meow/pylinx-updates/update.json) 指向该版本。
4. 手动安装第一个 fork Release 的 APK 覆盖当前测试版。以后在首页检查 App 或 MAA 资源更新，即使用同一份 fork 清单。

如遇代码冲突或自动审查失败，Actions 和邮件会标出对应文件。普通修复可提交到分支并向 `pylinx171` 发 PR，自动检查通过后合并并重跑发布。对安全审查自身、构建入口或签名发布配置的维护需要仓库所有者明确处理分支保护。出现 MaaCore 接入校验失败时，按 `stage.py` 的错误调整对应接入点。自动流程从被同步的上游提交记录识别 MaaCore 版本。如果上游改变了版本声明方式，手动运行时可填写 `maa_core`，例如 `v6.23.2`。

## 产物和测试范围

APK 采用 ARM64，适用于当前小米设备。构建把礼尚往来接进固定版本的 MaaCore，再部署同版本的官方 Android 资源。启动警告和游戏复用由 Android 应用层编译。工作流运行 C++ 礼尚往来策略检查、Android 启动和更新回归测试，并检查最终 APK 的包名、版本号、ABI、定制内核标记和签名。上游改过相关接入文件时发告警，遇到实际冲突或校验失败时终止发布。

App 与资源制品在同一 GitHub Release 中，先核对 GitHub 返回的资产 SHA-256，再推进静态更新清单。资源包与发布的 MaaCore 版本配对，手机只热更新与本机内核版本相同的新资源。本流程按上游正式版发出配套资源 ZIP。更新清单走 GitHub Raw，安装包走 GitHub Release 下载域名。手机更新无需登录 GitHub。

## 回退

需要暂停自动发布时，在 Actions 工作流页面选择 Disable workflow。当前静态更新清单仍指向最后一次成功发布的资产。要把修复版交付到已经安装较新版本的手机，基于修复后的分支重新发布并使用更高的 Android `versionCode`。
