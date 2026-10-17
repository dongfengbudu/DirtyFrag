# 汉化层（tools/i18n）

DirtyFrag 是 [diabl0w/DFRoot](https://github.com/diabl0w/DFRoot) 的 fork。汉化做成**可重复重建的层**，不依赖一次性手工改动：每次从上游同步后跑一条命令即可恢复，上游新增的英文界面文字会被脚本报告出来。

## 结构

| 文件 | 角色 |
|---|---|
| `translations.tsv` | **唯一手工维护的文件**：中英对照表，`key<TAB>英文<TAB>中文`，`#` 为注释 |
| `localize.py` | 从对照表重建汉化层的脚本 |
| `../../app/src/main/res/values/strings.xml` | 生成物：英文默认资源（勿手改） |
| `../../app/src/main/res/values-zh/strings.xml` | 生成物：中文资源（勿手改） |

布局（`layout/`、`menu/`）与 Java（`MainActivity.java`、`TwoStepProgressView.java`）里的界面文字改为 `@string/...` / `getString(R.string....)` 引用，脚本会把对照表里的英文原文重新替换成引用。

## 同步上游后怎么做

```
git pull upstream main        # 有冲突先解决（汉化层文件冲突时保 fork 版 + 手动移植上游改动）
make localize                 # 重建 strings.xml / values-zh 并重新替换字面量
make localize-check           # 验证树已汉化到位（CI/提交前自查用）
./gradlew :app:assembleDebug  # 编译验证
```

## 脚本报告怎么读

- `UNTRANSLATED <文件> '<原文>'`：上游新增的界面文字，对照表里还没有 —— 在 `translations.tsv` 加一行（key、英文、中文），重跑 `make localize`，再重跑 `make localize-check` 直到干净。
- `MANUAL <文件> <行>`：该行的英文出现在 String 字段初始化里（如 `private static final String LOG_HEADER = "SYSTEM";`），不能简单替换（静态常量拿不到 context），需要手动改成实例字段 + `getString(...)`——这类是代码改动，属 fork 自身历史，脚本不代劳。
- `UNUSED <key>`：对照表里有、界面上已无引用的条目，确认后可从表里删掉。

## 一次性结构改动（脚本不重建，同步冲突时保 fork 版）

以下改动是让 `values-zh` 生效的前提，已随 fork 提交，属于代码而非字面量：

- `TwoStepProgressView.setSeg2` 增加了 `verified` 布尔重载——灰色渐隐原来靠比较字符串 `"Verified"` 触发，汉化后字符串变了会失效；脚本会在重新替换时自动补回第 4 个参数。
- `stripHeader` 把原生日志的阶段标题（`=== setup ===` 等）翻译成显示层文字（准备/打补丁/初始化/清理/漏洞利用失败）；**内部匹配保持英文原样**，进度条驱动和失败检测依赖原始英文行。
- `MainActivity.LOG_HEADER` 从静态常量改为实例字段 `logHeader`（`getString()` 需要实例 context）。
- `D2Status.Result` 的英文标签方法（Yes/No、On/Off、Supported）已删除，改由 `MainActivity` 直接用布尔值映射资源。
- 日志正文（`ExploitRunner` 打印的 `app version:` 等键值行）保持英文——那是贴给别人排查 bug 用的，不汉化。

## 已知取舍

- 专有名词不翻译：DirtyFrag、Odin、KernelSU、KSU/KSUD、libksud.so（`odin_label` 在表里英文=中文，仅为让替换覆盖完整）。
- 换语言跟随系统：设备语言为中文显示中文，否则显示英文；想要界面强制中文，把 `res/values-zh` 改成 `values`（不推荐，会失去上游对照）。
