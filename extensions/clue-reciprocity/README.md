# 会客室礼尚往来

源码放在 `pylinx171` 分支的独立文件中。打包前用 `stage.py` 把入口接到 MaaMeow 和 MaaCore 的临时源码副本。脚本默认只预览，遇到上游入口改动会停止。

## 构建

在 MaaMeow 仓库根目录运行。下面以 arm64 真机和 MaaCore v6.23.1 为例。

```sh
stage=$(mktemp -d)
git clone --no-hardlinks --branch "$(git branch --show-current)" . "$stage/app"
git clone --depth 1 --branch v6.23.1 https://github.com/MaaAssistantArknights/MaaAssistantArknights.git "$stage/core"
git -C "$stage/core" submodule update --init --depth 1 src/MaaUtils
python3 extensions/clue-reciprocity/stage.py --app-tree "$stage/app" --core-tree "$stage/core" --apply
```

在暂存的内核目录里构建。将 `ANDROID_NDK_PATH` 指向本机 NDK r29。

```sh
cd "$stage/core"
python3 tools/maadeps-download.py arm64-android
cmake -B build --preset android-publish-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_PATH/build/cmake/android.toolchain.cmake" \
  -DMAA_HASH_VERSION=v6.23.1
cmake --build build --target MaaCore --parallel
```

在暂存 App 目录部署同版本的官方资源，再装入新增功能的 MaaCore。以下命令需要从原仓库根目录运行。

```sh
(cd "$stage/app" && python3 scripts/setup_maa_core.py --tag v6.23.1 --abi arm64-v8a)
python3 extensions/clue-reciprocity/stage.py --app-tree "$stage/app" \
  --install-core arm64-v8a "$stage/core/build/bin/libMaaCore.so" --apply
(cd "$stage/app" && bash gradlew :app:assembleDebug -Pmaa.abi=arm64-v8a)
```

真机测试时，在「基建换班 → 高级设置」勾选「礼尚往来」，同时保留「基建设施 → 会客室」。识别目前面向国服简体中文界面。

## 数据和验证

- 勾选状态按基建任务保存。Core 在其用户目录的 `clue-reciprocity/` 下按配置档和客户端保存登记。清墙和首次登记各有完成标记，登记中断后从登记继续。
- 按 MaaCore 的 1280×720 识别画面取搜索区域。按钮通过模板、OCR 或当前开关图形定位，点击矩形来自识别结果。比例无法归一化或关键控件识别不明确时停止对应步骤。
- 识别不到好友、库存、筛选状态或翻页不完整时，暂停本轮赠送。兜底要求自有库存达到 9 条，并筛选当前线索仍缺少且今日上线的好友。
- Core 日志可搜索 `PylinxClueReciprocity`。验收时检查首次清墙后的登记、后续运行是否跳过清墙、好友列表是否确实翻页，以及 8/10 与 9/10 时无可回赠对象的不同表现。
```sh
clang++ -std=c++20 -Wall -Wextra -Werror extensions/clue-reciprocity/check_policy.cpp -o /tmp/maa-clue-policy-check
/tmp/maa-clue-policy-check
```
