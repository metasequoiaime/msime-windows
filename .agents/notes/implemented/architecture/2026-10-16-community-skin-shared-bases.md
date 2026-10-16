# Agent Note: Windows 社区皮肤共享基底适配

Status: implemented

## Problem

独立 Windows 客户端把清单 `base` 限制为六个本地外观，社区服务发来的 `system`、`shuishan`、`light`、`paper`、`night`、`ink` 因而在加载时被拒绝。手改为 `fluent` 可以加载，但会丢掉基底配色。

## Decision

在目录加载层接受六个共享基底，将内部渲染基底统一适配为 `fluent`，并补齐固定基底缺省的候选、菜单与工具栏配色。清单显式配色优先。`system` 跟随宿主，不注入固定配色。固定明暗基底只保留对应的有效 `themes`，沿用现有布局与主题兼容判定。六个旧内置外观及其默认清单保持原有行为。

配色依据为 msime 提交 `1502128159` 的 `crates/client-core/src/theme.rs`，只取已提交来源的颜色定义，不复制相邻工作区的未提交实现。

## Alternatives considered

- **仅扩大白名单**：改动最少，但 WebView2 按 `base` 查找 CSS 文件，共享基底没有本地资源；D2D 与设置预览也只理解旧外观，不能保证配色一致。
- **未知基底全部回退 Fluent**：任何新包都能显示，但拼写错误和不受支持的基底被静默接受，而且原本的配色丢失；未知值仍应拒绝。
- **每个渲染器分别实现共享基底**：各渲染器可按需调整，但候选窗、浮层和两种设置预览需要重复维护同一张表；加载层的现有配色槽位已能表达这次兼容。

## Consequences

社区包不再需要手改 `base`；D2D、WebView2、工具栏、光标指示器和设置预览都读取加载器适配后的同一份数据。固定基底与清单声明的明暗取交集，空交集的包仍可列出，但不会在任何模式套用；适配后的 `base` 是内部渲染结构名，设置页也会显示为 `fluent`。

配色表由 Windows 保存副本，上游调整时需要同步。共享规范中超出现有 Windows 清单读取与渲染能力的扩展字段不因此获得支持。缺省 `selected` 与 `hover` 按上游 `resolve` 的规则，取清单覆盖后的 `accent` / `text` 加半透明，清单可显式覆盖这些槽位；独立的菜单和工具栏字段优先于由候选配色派生的缺省值。

## Verification

`server/tests/src/test_candidate_skin_catalog_bases.cpp` 注册进 Windows 的 `MetasequoiaImeServerTests`。同一测试源在 macOS 上用 `server/tests/portable/main.cpp`、真实 `candidate_skin_catalog.cpp` 和 toml++ 3.4.0 直接编译，以 AddressSanitizer、UndefinedBehaviorSanitizer 执行。覆盖共享基底、v1/v2、显式配色、旧外观、未知值、有效明暗交集，以及默认清单和包内开关的优先级。默认分支的加载器运行这些回归测试失败，修复后通过。

本地另执行现有 portable CTest、产品级 Python 单测、全量 clang-format 18.1.8 检查与构建输入路径检查。macOS 无法验证 Windows 原生宿主、安装或不同 DPI 下的实际绘制；PR 保留这一验证缺口，不把加载器测试当作 Windows 装机验收。
