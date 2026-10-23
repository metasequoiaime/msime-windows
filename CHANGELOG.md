# Changelog

## [0.12.0] - 2026-10-22

### Features

- **翻译子系统重构**：`server/src/translate/` 九个单元取代原来 600 行的单体文件，`cloud_translation.cpp` 只保留兼容转发，不再持有状态。
- **自定义服务四协议**：Chat Completions / Responses / Anthropic / Gemini，地址只填主机或 `/v1` 时自动补全，支持 `{model}` 占位符。
- **整句翻译**：超过 12 字或带句读单独成批，12 秒超时、1024 token 上限，写库长度门放宽到 128 字符。
- **AI 校对**：译后交给同一模型复核，独立线程、按词条/整句分组、每批 12 条，修正先过长度门再写缓存。
- **本地多语种释义**：`zh_glosses(chinese, lang, gloss)`，日/法/德/韩等九种语言本地优先，未命中才走云端。
- **AI 联想与语音润色接入四协议**：`ai_wire` 提供统一的请求构造、鉴权头与发送层。
- **词库导出**：拼音、五笔、英文三份用户词条合并成一个带来源列的 TSV。

### Bug Fixes

- **「AI 校对」开关保存失败**：`custom_translation.review` 是布尔键，却落进了字符串分支，类型不匹配直接返回 false。现在有独立的 setter 与精确路由。
- **整句校对超时**：校对沿用词条的 6 秒档，整句重写常常超时。现在按句子使用 12 秒档。
- **校对写回不一致**：修正先写缓存再被长度门拒掉，导致内存与词库不同步；现在先过门再写。
- **模糊音下调频永远不生效（#1172）**：去重先到先得，整句 Fallback（权重 1）把同名的词库真身（权重几十万）挤掉。改为按来源优先 + 权重择优保留，位置不动、内容替换。
- **从开始菜单打开设置后保存失败、图标缺失（#1174）**：`LOCALAPPDATA` 环境变量可能被启动器换成别的值。环境变量指向的目录没有数据、系统 Known Folder 有数据时，以系统为准。
- **微软双拼 t/v 键位（#1175）**：Windows 11 微软拼音里 t / v 都能打 ue 与 üe，水杉只能 t 打 ue、v 打 üe。`ShuangpinProfile` 新增 `final_aliases`，解码时一个韵母可接受多个键。

### Performance

- **输入停顿 120 ms**：连打的中间状态合并掉，候选译文接近瞬时出现。
- **分批上屏**：英→中、中→目标、整句各自译完立刻刷新，不再等最长的一批。

### Scripts

- **`scripts/local-gloss/migrate_local_glosses.py`**：把旧版 `english.db` 里 225 万条多语种释义抽成独立包，或直接合并进目标库。

## Unreleased

### Behaviour Changes

- **全拼漏字 / 多字纠错改为独立开关且默认关闭**：新增 `quanpin.autocorrect_deletion`（漏敲一个字母，如 `shng → shang`）与 `quanpin.autocorrect_insertion`（多敲一个相邻按键，如 `sjhang → shang`），设置页「输入纠错」各一个勾，与乱序 / 邻键并列；四个开关各管一类手误、互不联动。两个新键出厂与老配置缺失时都按关闭处理，**不继承**已有的乱序 / 邻键开关——升级后漏字 / 多字纠错会从「随这两个开关一起开」变为关闭。如需保留原行为，请在设置页手动勾选「漏字纠错」与「多字纠错」。默认更保守，误报更少。

## [0.6.7](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.6...v0.6.7) (2026-09-08)


### Bug Fixes

* **ci:** stop the self-hosted jobs racing on the global git config ([f7cd4e6](https://github.com/metasequoiaime/MSIME-Windows/commit/f7cd4e6174893f05c7733cb1abf31bdee7a36a89))
* **ci:** stop the self-hosted jobs racing on the global git config ([d7e7b3f](https://github.com/metasequoiaime/MSIME-Windows/commit/d7e7b3f0d961372fa25d76523d743bfc9262c1fb))

## [0.6.6](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.5...v0.6.6) (2026-09-08)


### Bug Fixes

* **ci:** unblock packaging on the self-hosted runners ([7e9f0cd](https://github.com/metasequoiaime/MSIME-Windows/commit/7e9f0cd8927edece437f26408c62ae4eb5be2e44))
* **ci:** unblock packaging on the self-hosted runners ([2868936](https://github.com/metasequoiaime/MSIME-Windows/commit/28689361add8f510d1f2103cafe74957e88fc02e))

## [0.6.5](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.4...v0.6.5) (2026-09-08)


### Bug Fixes

* **ci:** stop the pipe probe from handshaking with a leftover Server ([1c4643b](https://github.com/metasequoiaime/MSIME-Windows/commit/1c4643b0e4c16204421306d6e44e46f37f76fadc))
* **ci:** stop the pipe probe from handshaking with a leftover Server ([f91dae7](https://github.com/metasequoiaime/MSIME-Windows/commit/f91dae76449ab7c75f265a9a2e8c36944a13d21d))

## [0.6.4](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.3...v0.6.4) (2026-09-08)


### Bug Fixes

* **server:** only grace-delay a candidate hide the worker delivered late ([bb53763](https://github.com/metasequoiaime/MSIME-Windows/commit/bb53763e663629ecb94211e6a2b2e369a5ebeea3))
* **server:** stop candidate strobing when the worker queue backs up ([cc81f79](https://github.com/metasequoiaime/MSIME-Windows/commit/cc81f794bb8566d06411c18fb8d3a212a4d46d97))
* stop candidate-window flicker and dropped commits when the machine is loaded ([a763580](https://github.com/metasequoiaime/MSIME-Windows/commit/a763580a0ce1bb106aa1808588d67006761114fb))
* **tsf:** stop the candidate window flickering when the host lags ([58fa715](https://github.com/metasequoiaime/MSIME-Windows/commit/58fa71592122087d3b590efa32f51ccb30e58657))


### Performance Improvements

* **deps:** pick up the cached user-dictionary journal connection ([0bf0928](https://github.com/metasequoiaime/MSIME-Windows/commit/0bf0928fef636586c40aefbb4e08b374717620dd))
* **server:** time each segment of the per-keystroke candidate build ([0e003fa](https://github.com/metasequoiaime/MSIME-Windows/commit/0e003fa0957e793b9d71e59a025a61cd4d245093))

## [0.6.3](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.2...v0.6.3) (2026-09-08)


### Bug Fixes

* **config:** handle non-ASCII (e.g. Chinese) user profile paths ([0ef8b70](https://github.com/metasequoiaime/MSIME-Windows/commit/0ef8b70e9b0d69fe649ac348500216f54171f1c5))
* **config:** handle non-ASCII (e.g. Chinese) user profile paths ([de4d3ac](https://github.com/metasequoiaime/MSIME-Windows/commit/de4d3acf453d2efe0ad1463ed1744c475e6180a9))
* **emoji-panel:** use Chinese nav titles to match the tab labels ([4b3da57](https://github.com/metasequoiaime/MSIME-Windows/commit/4b3da57134625d3c472d597cb903e373a6dc4d65))
* **release:** use beta tags for automatic builds ([#252](https://github.com/metasequoiaime/MSIME-Windows/issues/252)) ([2fb4560](https://github.com/metasequoiaime/MSIME-Windows/commit/2fb4560d994043219f4702dbcacce1d34be87020))
* **scripts:** keep automatic releases as prereleases ([560b730](https://github.com/metasequoiaime/MSIME-Windows/commit/560b730ea1e1d27e7e767df5ccae515e9e4fa86d))
* **scripts:** keep automatic releases as prereleases ([b777008](https://github.com/metasequoiaime/MSIME-Windows/commit/b777008abb15773ae4dc7ab27cdc0e90c4432e30))
* **server:** fall back to text when the icon font lacks the glyph ([880c04f](https://github.com/metasequoiaime/MSIME-Windows/commit/880c04f6bbda0d03c0c37652c18b8ceefb757297))
* **server:** fall back to text when the icon font lacks the glyph ([4355546](https://github.com/metasequoiaime/MSIME-Windows/commit/43555460dbb8289e3ec05236d0cb4dafcd79da39))
* **ui:** make the icon font resolver recover from DirectWrite failures ([43576b7](https://github.com/metasequoiaime/MSIME-Windows/commit/43576b75cac8d9b645a6f8cee252896a7063704c))

## [0.6.2](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.1...v0.6.2) (2026-09-07)


### Bug Fixes

* **ci:** align release matrix with selected vcpkg ([#244](https://github.com/metasequoiaime/MSIME-Windows/issues/244)) ([fbd6b39](https://github.com/metasequoiaime/MSIME-Windows/commit/fbd6b390f111e2ad390be494d3cca901b1b0c7e4))
* **ci:** guard the release vcpkg selection ([#247](https://github.com/metasequoiaime/MSIME-Windows/issues/247)) ([d1ac527](https://github.com/metasequoiaime/MSIME-Windows/commit/d1ac5273e46ab9c2623d64b1ca2aff0551d7758b))
* **installer:** test server before embedding uiAccess manifest ([#243](https://github.com/metasequoiaime/MSIME-Windows/issues/243)) ([f62e8e5](https://github.com/metasequoiaime/MSIME-Windows/commit/f62e8e5fc8c7e724e1fbbe95d33b76070c1a25f1))

## [0.6.1](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.6.0...v0.6.1) (2026-09-07)


### Bug Fixes

* **server:** make release probes non-interactive ([ba62a6a](https://github.com/metasequoiaime/MSIME-Windows/commit/ba62a6a720b88250f7703f208116119735ec3939))
* **server:** make release probes non-interactive ([818069f](https://github.com/metasequoiaime/MSIME-Windows/commit/818069f9c3ffd44a1993295d5c923c6b35eabd85))
* **server:** test valid escaped CSS URLs ([84ef90c](https://github.com/metasequoiaime/MSIME-Windows/commit/84ef90cc8d09a600c35e799897ea9d104987b6eb))

## [0.6.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.5.4...v0.6.0) (2026-09-07)


### Features

* **contracts:** sync engine product lock contract ([9bd309e](https://github.com/metasequoiaime/MSIME-Windows/commit/9bd309e7735147d2816beae6d89cec847d43d2a6))
* **contracts:** verify shared product lock primitives ([2ff0509](https://github.com/metasequoiaime/MSIME-Windows/commit/2ff0509e679fe17dae7aafe0b66562a457200ae3))
* **installer:** include release PDB symbols ([4b95c90](https://github.com/metasequoiaime/MSIME-Windows/commit/4b95c90fde0ce35247fd75e676d63c8f5ad118f0))
* **installer:** include release PDB symbols ([e9a2c91](https://github.com/metasequoiaime/MSIME-Windows/commit/e9a2c91214e699e9d026a390364ddc13dab9db6f))


### Bug Fixes

* **product-lock:** update engine commit ([f1aefec](https://github.com/metasequoiaime/MSIME-Windows/commit/f1aefec3190f4ebd9b7cb7dc1433d2ff2eca891a))
* **server:** embed uiAccess manifest before signing ([ad48153](https://github.com/metasequoiaime/MSIME-Windows/commit/ad48153c8e4e1697f1eca45dcbed3a1a472df0a9))
* **server:** embed uiAccess manifest before signing ([84de68f](https://github.com/metasequoiaime/MSIME-Windows/commit/84de68fd1b0467e726226e7b52703489d0963455))
* **server:** save custom translation settings ([5a0b4dc](https://github.com/metasequoiaime/MSIME-Windows/commit/5a0b4dc4c27c0af24723703dd41dd8fa68b067e7))

## [0.5.4](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.5.3...v0.5.4) (2026-09-06)


### Bug Fixes

* **release:** publish automatic builds as stable releases ([7ad9de8](https://github.com/metasequoiaime/MSIME-Windows/commit/7ad9de84dd175eb26c670f94c1dc8560fab4cbf8))
* **release:** publish automatic builds as stable releases ([d55a0b1](https://github.com/metasequoiaime/MSIME-Windows/commit/d55a0b17a98bbc2c3cc2a4c65dd931a83cec24a3))

## [0.5.3](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.5.2...v0.5.3) (2026-09-06)


### Bug Fixes

* **installer:** fail when configured signing key is unavailable ([f3a34cf](https://github.com/metasequoiaime/MSIME-Windows/commit/f3a34cfb5684e2c5118ec57d997325b2a09af8ed))
* **installer:** fail when configured signing key is unavailable ([a45a8cb](https://github.com/metasequoiaime/MSIME-Windows/commit/a45a8cbd3442e2703a83d092b23eb70878039be4))
* **server:** avoid showing raw preedit for sentence candidates ([2098206](https://github.com/metasequoiaime/MSIME-Windows/commit/2098206019c067b9ffa9fb30c1a16290b525aa39))
* 改进长句候选展示与候选排序，本地联想的长句后面不应跟着 raw preedit ([ec29a5b](https://github.com/metasequoiaime/MSIME-Windows/commit/ec29a5bc6043c3ef44b7191657e0303a245a4381))

## [0.5.2](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.5.1...v0.5.2) (2026-09-06)


### Bug Fixes

* **ci:** defer next release PR after publish ([3784d7f](https://github.com/metasequoiaime/MSIME-Windows/commit/3784d7f00684abf4b9f022aaf41388fe78895d3a))
* **ci:** defer next release PR after publish ([0973d3d](https://github.com/metasequoiaime/MSIME-Windows/commit/0973d3d4ef190d35bbc417aa39fcc32386c1e603))

## [0.5.1](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.5.0...v0.5.1) (2026-09-06)


### Bug Fixes

* **ci:** correct isolated vcpkg command syntax ([5b5960e](https://github.com/metasequoiaime/MSIME-Windows/commit/5b5960e5a048679e95d42ad677d04fd82d3930ae))
* **ci:** isolate release vcpkg installations ([5c85d29](https://github.com/metasequoiaime/MSIME-Windows/commit/5c85d297cde9cdaca865b7881492e9aabf474bee))
* **ci:** serialize release builds sharing vcpkg ([82452f7](https://github.com/metasequoiaime/MSIME-Windows/commit/82452f7f02095c4654caf9dd22bd1a288aa1e04b))
* **ci:** serialize release builds sharing vcpkg ([b827bbe](https://github.com/metasequoiaime/MSIME-Windows/commit/b827bbee3c7c73a43075aeab4ac3c6935140b2b4))
* **installer:** sign releases from runner certificate store ([e895d14](https://github.com/metasequoiaime/MSIME-Windows/commit/e895d14b6074cdefb4f05679ce71925e945e6799))
* **installer:** sign releases from runner certificate store ([7dc68ff](https://github.com/metasequoiaime/MSIME-Windows/commit/7dc68ff498f9ab30c9ee5c1c557ff05464fdf5bd))

## [0.5.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.4.0...v0.5.0) (2026-09-06)


### Features

* **server:** show translations in horizontal candidate windows ([8f19010](https://github.com/metasequoiaime/MSIME-Windows/commit/8f190102d0dc8ceafc36f95ac2a1f3acd2dec931))
* **server:** show translations in horizontal candidate windows ([3fc7ba2](https://github.com/metasequoiaime/MSIME-Windows/commit/3fc7ba295fc721425be8babe4e98c39c403bcb20))


### Bug Fixes

* **skin:** [@import](https://github.com/import) 剥离只匹配真正的 at-rule；补转义绕过的不变量说明与用例 ([205cafa](https://github.com/metasequoiaime/MSIME-Windows/commit/205cafa4f5e4a446f0fdb2fc80b915d17c18bca4))
* **windows:** include Chinese in IME display names ([40862b1](https://github.com/metasequoiaime/MSIME-Windows/commit/40862b13ee5b89022b963a0f0a3d80bed6dc71c7))
* **windows:** include Chinese in IME display names ([f90c5c3](https://github.com/metasequoiaime/MSIME-Windows/commit/f90c5c39b7e4fbbcd98354c94f0454cbd6589247))

## [0.4.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.3.2...v0.4.0) (2026-09-06)


### Features

* **installer:** 首次安装时询问云候选，不再默认静默联网 ([e24913e](https://github.com/metasequoiaime/MSIME-Windows/commit/e24913e9e33008f14bb26b77f6bfcdd1f4501dc2))


### Bug Fixes

* **ci:** separate automatic and manual release locks ([39c94b0](https://github.com/metasequoiaime/MSIME-Windows/commit/39c94b092d80b46f7d4e594eebd0ed054d04049f))
* **ci:** separate automatic and manual release locks ([276625f](https://github.com/metasequoiaime/MSIME-Windows/commit/276625fe319eec7a9fb44fe9dd828548c2efcec6))
* **ci:** 修复引擎 bump 自动合并的守卫，并补回 Issue 模板的脱敏警告 ([88720a8](https://github.com/metasequoiaime/MSIME-Windows/commit/88720a80dff6273fe5ae6347b7ddd7047ea9f2e5))
* **settings:** 检查更新时消费 update.json 的校验值与签名状态 ([1ec7bdd](https://github.com/metasequoiaime/MSIME-Windows/commit/1ec7bdd690d89a5efeda7e1bf9b7c2a2031c7894))
* **skin:** 丢弃第三方皮肤 CSS 里的远程引用，不再原样注入候选窗 ([680fdfa](https://github.com/metasequoiaime/MSIME-Windows/commit/680fdfa64891b24ad19b0c9059c38a8ca1428da2))
* **skin:** 修复 GCC 下的符号比较告警与 clang-format ([e645c9a](https://github.com/metasequoiaime/MSIME-Windows/commit/e645c9abaa45d99afa9b970dcd11adb832c801bf))
* **ui:** 给候选窗、悬浮工具栏与托盘菜单加 Content-Security-Policy ([5d5b661](https://github.com/metasequoiaime/MSIME-Windows/commit/5d5b661e140ed7192aeedbaea687f545b268815f))

## [0.3.2](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.3.1...v0.3.2) (2026-09-06)


### Bug Fixes

* **installer:** document uiAccess signing requirements ([23c62e4](https://github.com/metasequoiaime/MSIME-Windows/commit/23c62e4e96e2ff29d4d2736f51503d1b12eb5278))
* **installer:** document uiAccess signing requirements ([0a9b912](https://github.com/metasequoiaime/MSIME-Windows/commit/0a9b912a97eb0805f5b060daa22fa7175d01bd71))

## [0.3.1](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.3.0...v0.3.1) (2026-09-06)


### Bug Fixes

* **server:** revalidate queued online candidate generations on apply ([9785ecd](https://github.com/metasequoiaime/MSIME-Windows/commit/9785ecd4bf1a2c8f1056682d18fd184ff6aad733))

## [0.3.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.2.1...v0.3.0) (2026-09-06)


### Features

* **release:** separate the automatic build channel from the release channel ([2d51c53](https://github.com/metasequoiaime/MSIME-Windows/commit/2d51c5383925ce8174a60f5527cbda5ff03b7d9f))
* **release:** separate the automatic build channel from the release channel ([d0e7cfd](https://github.com/metasequoiaime/MSIME-Windows/commit/d0e7cfdbdfaae9e5c296c83e58707932128c9b71))

## [0.2.1](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.2.0...v0.2.1) (2026-09-06)


### Bug Fixes

* **server:** adopt isolated engine sessions and helpcode annotations ([#178](https://github.com/metasequoiaime/MSIME-Windows/issues/178)) ([2b8bf93](https://github.com/metasequoiaime/MSIME-Windows/commit/2b8bf937d96e5ff80e19f97de5be594802d90b5d))

## [0.2.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.1.4...v0.2.0) (2026-09-06)


### Features

* **release:** mark automatically built releases in the title and notes ([fd9236f](https://github.com/metasequoiaime/MSIME-Windows/commit/fd9236f175a629a33d611b3748781d3623e1c497))
* **release:** mark automatically built releases in the title and notes ([568fd37](https://github.com/metasequoiaime/MSIME-Windows/commit/568fd3742cdbbb6c466f16fb3c4a9707e3590f52))


### Bug Fixes

* **ci:** attach automatic release checks to the pull request ([#175](https://github.com/metasequoiaime/MSIME-Windows/issues/175)) ([ee4334d](https://github.com/metasequoiaime/MSIME-Windows/commit/ee4334dddd29b0ab6bd4ff6c887e41a716000f5f))
* **ci:** stop the dependency review from skipping the release pull request ([cc5af53](https://github.com/metasequoiaime/MSIME-Windows/commit/cc5af5356d084255170fd67e88d674a8de3a933f))
* **ci:** stop the engine bump triage from executing engine file names ([#174](https://github.com/metasequoiaime/MSIME-Windows/issues/174)) ([5a4484f](https://github.com/metasequoiaime/MSIME-Windows/commit/5a4484feea2cd735eae061472983905897863892))
* 修复审计发现的候选页竞态、管道权限、TSF 空指针与皮肤 CSS 逃逸等八项缺陷 ([#169](https://github.com/metasequoiaime/MSIME-Windows/issues/169)) ([2562026](https://github.com/metasequoiaime/MSIME-Windows/commit/2562026a2a1c393e387fcd8ccd2dc30024a44463))

## [0.1.4](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.1.3...v0.1.4) (2026-09-06)


### Bug Fixes

* **server:** 将本地测试入口接回合仓后的主工程 ([0baaf9a](https://github.com/metasequoiaime/MSIME-Windows/commit/0baaf9a1d0e572fd9a7fa63db934fc365df65f46))
* **server:** 补齐合仓后的本地测试和开发入口 ([2297ef4](https://github.com/metasequoiaime/MSIME-Windows/commit/2297ef421a2205148afc2d1ef4a3b3f8b8dcf351))

## [0.1.3](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.1.2...v0.1.3) (2026-09-06)


### Bug Fixes

* **build:** finish local tooling migration to the monorepo ([8155380](https://github.com/metasequoiaime/MSIME-Windows/commit/81553802ba3f6e0e0ef1e4768fbb94398d2f3658))
* **build:** finish local tooling migration to the monorepo ([b2d736c](https://github.com/metasequoiaime/MSIME-Windows/commit/b2d736cd797d5ebce0a8c5e14b4c7c690d77275f))

## [0.1.2](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.1.1...v0.1.2) (2026-09-05)


### Bug Fixes

* **ci:** match native Windows job names to the required checks ([1e7632b](https://github.com/metasequoiaime/MSIME-Windows/commit/1e7632b564f272c7b1e356fbe4b15fb53c73e42c))
* connect Windows monorepo to consolidated Engine voice and data ([b690980](https://github.com/metasequoiaime/MSIME-Windows/commit/b690980ab80e3f8619f15546c95276dc9205b964))
* finish local monorepo packaging and required Windows checks ([e1946bb](https://github.com/metasequoiaime/MSIME-Windows/commit/e1946bbec8c9ad2003ba3a64a7f8777f967be08a))
* **installer:** use Engine tables in the local monorepo layout ([5f8d75d](https://github.com/metasequoiaime/MSIME-Windows/commit/5f8d75d79e3d6fd3ae2e2cb4008594fa91b6e705))
* **server:** include the shared voice error declaration ([eca353d](https://github.com/metasequoiaime/MSIME-Windows/commit/eca353daed983386e01198b36fae144ab277583f))

## [0.1.1](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.1.0...v0.1.1) (2026-09-05)


### Bug Fixes

* **ci:** check out the engine submodule in the product inputs job ([6bb0c1e](https://github.com/metasequoiaime/MSIME-Windows/commit/6bb0c1e10ec9fdf5a59f5793f0392c506641dba9))
* **ci:** persist vcpkg binaries before application builds ([947aa16](https://github.com/metasequoiaime/MSIME-Windows/commit/947aa16fcbd7f13d171bbd459c0a73e9471c4eeb))
* **ci:** resolve the pipe probe contracts from the repository, not the server ([b48dc82](https://github.com/metasequoiaime/MSIME-Windows/commit/b48dc826526fdb41e268f92d60bf49530d00291d))
* **server:** always answer explicit settings snapshot requests ([9a9c6f3](https://github.com/metasequoiaime/MSIME-Windows/commit/9a9c6f328f7e05949bfda35ab7f9e70e479ee6da))
* **server:** keep settings I/O off UI thread and bound dictionary rendering ([06bba15](https://github.com/metasequoiaime/MSIME-Windows/commit/06bba15d4f2738d3e93c34819c7bad8813a137c5))
* **server:** point the panel resources at the ui component ([ecc1bf4](https://github.com/metasequoiaime/MSIME-Windows/commit/ecc1bf4f6bae9b7e9e385af67743c153bf12f7f3))
* **server:** reduce WebView message and first-paint overhead ([b5d07f1](https://github.com/metasequoiaime/MSIME-Windows/commit/b5d07f11eb4fe3e98aec48c84f7e0870f24ea42f))

## [0.1.0](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.17...v0.1.0) (2026-09-05)


### Features

* also sync double/single char status when switch windows ([7890e81](https://github.com/metasequoiaime/MSIME-Windows/commit/7890e815de1370a8cb969c32cf400a2cf6a00cbf))
* **langbar:** switch IME mode icons by system light/dark theme ([0b00c00](https://github.com/metasequoiaime/MSIME-Windows/commit/0b00c0042b6664b17e1d05a46128c7051b449b14))
* notify UI process on punctuation mode change ([d299f53](https://github.com/metasequoiaime/MSIME-Windows/commit/d299f5328661be7c998163b622d979b732b2c4bc))
* **product:** keep WebView and native contracts in one locked combination ([9b28445](https://github.com/metasequoiaime/MSIME-Windows/commit/9b2844534ef796cfaf90cbc4b62e7c2849e4275f))
* **product:** lock release inputs and share negotiated IPC contracts ([487011c](https://github.com/metasequoiaime/MSIME-Windows/commit/487011c80269322127629c6e878653ee61726431))
* **product:** lock the shared-session stack and validate dictionary formats ([1c6c956](https://github.com/metasequoiaime/MSIME-Windows/commit/1c6c95653070c931e00783dd87e63b0a1d5ed5b5))
* **product:** require locked commits to be on their default branch at release ([a81bc47](https://github.com/metasequoiaime/MSIME-Windows/commit/a81bc47c719228c93bf2dfcad1d1c55511e06346))
* **product:** validate and lock the shared-session release combination ([e08f434](https://github.com/metasequoiaime/MSIME-Windows/commit/e08f434e311b55668c5dcd7951563a01a65c102a))
* set max len limit for pinyin input ([bde777f](https://github.com/metasequoiaime/MSIME-Windows/commit/bde777f359c22bbc9f000dc1d1760d23029b5aa2))
* support exact candidate character commits ([5e36ba1](https://github.com/metasequoiaime/MSIME-Windows/commit/5e36ba1d01f229214be543c1d35a4748275868ed))
* sync IME status to UI on thread focus and punctuation switch ([55dae98](https://github.com/metasequoiaime/MSIME-Windows/commit/55dae980691b5754ba56acfe61a5591ab94e9bb9))
* synchronous tsf and server double/single char status via ipc ([25ea1ef](https://github.com/metasequoiaime/MSIME-Windows/commit/25ea1efe39f5b90015e036df4532f10fb260fa53))


### Bug Fixes

* **ci:** build the Server in build/, the only directory name it links in ([ca0a20b](https://github.com/metasequoiaime/MSIME-Windows/commit/ca0a20b50e9f65bb43be97940a334a5944322ec5))
* **ci:** build the Server in build/, the only directory name it links in ([053c987](https://github.com/metasequoiaime/MSIME-Windows/commit/053c987af3b629a6d91d97c9ccd0f5d1e85236f4))
* **ci:** find the release pull request by branch, not by prs_created ([0b9b272](https://github.com/metasequoiaime/MSIME-Windows/commit/0b9b2728befbfe5afe08668944b31c8e6fd5504b))
* **ci:** find the release pull request by branch, not by prs_created ([550e0a1](https://github.com/metasequoiaime/MSIME-Windows/commit/550e0a1c70b675184efab104942c1c4bcf1e7ab6))
* **ci:** install the Chinese language file Inno Setup needs ([22a132e](https://github.com/metasequoiaime/MSIME-Windows/commit/22a132e3bd4d48c6185176c0855debdc3a1cfd73))
* **ci:** supply the Inno Setup language file, and move release shell into scripts/ci ([0761b41](https://github.com/metasequoiaime/MSIME-Windows/commit/0761b410e378ac71ab1c6239b5c2cd7db81dab61))
* **ci:** take the release scripts from the default branch, not the built commit ([ca87304](https://github.com/metasequoiaime/MSIME-Windows/commit/ca87304f0df71f0aacde4c0ff4a3ab6db6547669))
* **ci:** take the release scripts from the default branch, not the built commit ([e90ac18](https://github.com/metasequoiaime/MSIME-Windows/commit/e90ac18723a119ff7217f3ce84f2902b0ca7a89d))
* **ci:** verify the version resource at the source, not on the built DLL ([aca9c8a](https://github.com/metasequoiaime/MSIME-Windows/commit/aca9c8a31debde83bd50583c054cefd7f0914148))
* **ci:** verify the version resource at the source, not on the built DLL ([2b17384](https://github.com/metasequoiaime/MSIME-Windows/commit/2b173841f59402b1d3f5abdfb818369bd72537a3))
* **composition:** deduplicate consecutive pinyin separators ([ccedf77](https://github.com/metasequoiaime/MSIME-Windows/commit/ccedf777602f567ac88cfd750bb7ebcc7f77fe49))
* **image:** add small icon frames to msime.ico ([facfe60](https://github.com/metasequoiaime/MSIME-Windows/commit/facfe60761c4f3b23e2ab58cbfa66d25385dcee0))
* **image:** add small icon frames to msime.ico ([f6a4567](https://github.com/metasequoiaime/MSIME-Windows/commit/f6a4567b5e7fcf58454d394a12aed9ba3a20e9ae))
* **ime:** defer CN→EN close until after Shift composition commit ([79b3155](https://github.com/metasequoiaime/MSIME-Windows/commit/79b31550994965144093740c3faf5aee9ae8c787))
* **ime:** keep first-key candidate window when entering Excel cell edit, do not suspend client on transient focus loss during Excel cell edit. ([0c95aa2](https://github.com/metasequoiaime/MSIME-Windows/commit/0c95aa272a46fe86270ef7f8c0072e89a3e233c7))
* **ime:** keep numpad decimal as '.' in Chinese punctuation mode ([4ea2386](https://github.com/metasequoiaime/MSIME-Windows/commit/4ea238661128e913a64c5164167860d7e92f980a))
* **ime:** launch Server off the UI thread and wake it on TIP activation ([881dba5](https://github.com/metasequoiaime/MSIME-Windows/commit/881dba501f2d1bb0ebf7cac836c9952c3e4497e4))
* **langbar:** refresh theme icons immediately on system theme change ([34ff194](https://github.com/metasequoiaime/MSIME-Windows/commit/34ff19482d8991c0a175d4a82d94c5d46d1f334f))
* make debugview-like 32-bit apps get proper coordinates for caret ([315f202](https://github.com/metasequoiaime/MSIME-Windows/commit/315f202195da0dca60bf6cd5436cf976e6c9474a))
* **product:** pin Server with corrected subproject test registration ([1f126a1](https://github.com/metasequoiaime/MSIME-Windows/commit/1f126a193a428c393c6f06bb1d7325b622098fff))
* **punctuation:** let the book title nest pair actually run ([b2d5c55](https://github.com/metasequoiaime/MSIME-Windows/commit/b2d5c55971b0a9635a44d60ea6ff104c8257cab0))
* **release:** install the Inno Setup language file where ISCC looks ([7c6dc8b](https://github.com/metasequoiaime/MSIME-Windows/commit/7c6dc8b6a06b170282aeff4e4e37f754171e3b31))
* **release:** 语言文件装到真正的 Inno Setup 目录,修掉八个版本都卡住的打包失败 ([a3610b7](https://github.com/metasequoiaime/MSIME-Windows/commit/a3610b7180f856246db94932613574ca8109ed25))
* reliably notify server when switching away from IME ([87ba31f](https://github.com/metasequoiaime/MSIME-Windows/commit/87ba31f2ec0d7c71947b5f033242beaacb93a60a))
* reset pipe handle in some cases, to ensure reconnect ([9512ee4](https://github.com/metasequoiaime/MSIME-Windows/commit/9512ee4a7ece3c4e18226010b4ca148a62a2573b))
* **scripts:** read the vcpkg root from the environment ([0259f7a](https://github.com/metasequoiaime/MSIME-Windows/commit/0259f7a9c57d29445f91896662c31f616e0ab7b0))
* **scripts:** read vcpkg and boost roots from the environment ([f9e52d9](https://github.com/metasequoiaime/MSIME-Windows/commit/f9e52d98a84b99b36800aff6ff55ecd71b2b26b9))
* show langbar menu via session-less Aux pipe ([30c0942](https://github.com/metasequoiaime/MSIME-Windows/commit/30c09425c520170d594c62758d4717503a5881df))
* **tsf:** commit the create-word prefix when Enter finalizes the composition ([75d8366](https://github.com/metasequoiaime/MSIME-Windows/commit/75d83660cd5b41be32e3089936fd22f5bd3a91b8))
* **tsf:** commit the create-word prefix when Enter finalizes the composition ([634b6ce](https://github.com/metasequoiaime/MSIME-Windows/commit/634b6ceaef48e87fd46349bcf371801fa2f5e340))
* **tsf:** convert candidate anchors to physical coordinates ([089e47b](https://github.com/metasequoiaime/MSIME-Windows/commit/089e47b60cd12cf3f876cd89dd1f19fdf81612ee))
* **tsf:** let the host see the bare Ctrl release that toggles input mode ([a13ae2f](https://github.com/metasequoiaime/MSIME-Windows/commit/a13ae2fdbee620f2280e0d21cae3bb632d87b16a))
* **tsf:** let the host see the bare Ctrl release that toggles input mode ([3854331](https://github.com/metasequoiaime/MSIME-Windows/commit/38543317a2aadd94f15f8ee68e95b1e83f203976))
* **tsf:** pass application modifier shortcuts through ([4b1696c](https://github.com/metasequoiaime/MSIME-Windows/commit/4b1696ced320360c8490d453ca407145b4ed0f4e))
* **tsf:** pass application modifier shortcuts through in Chinese mode ([469aefe](https://github.com/metasequoiaime/MSIME-Windows/commit/469aefed86c9cdf203217bccc69c91abcd26762d))
* **tsf:** place candidate window with per-monitor DPI coordinates ([a6f94de](https://github.com/metasequoiaime/MSIME-Windows/commit/a6f94debbaf3ffc7c18419f620a6c8c2156be669))
* **tsf:** support Shift language switching in mintty (refs [#32](https://github.com/metasequoiaime/MSIME-Windows/issues/32)) ([99cbc17](https://github.com/metasequoiaime/MSIME-Windows/commit/99cbc172e1555bc616627797258a7e3fa1d6c9ee))
* Wrong interface conversion (ITfcompositionSink) ([27e7110](https://github.com/metasequoiaime/MSIME-Windows/commit/27e7110f047b9eb859a8ad2c956c6700a70aa844))


### Reverts

* leave the unused boost_path alone ([a7a3fc4](https://github.com/metasequoiaime/MSIME-Windows/commit/a7a3fc42b53d852910640fdd21e1cd5b4dfae812))
* move back off calendar versioning ([07d9ce9](https://github.com/metasequoiaime/MSIME-Windows/commit/07d9ce91e5d04d9db91e22bd7eaa80070f6d37ef))

## [0.0.17](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.16...v0.0.17) (2026-09-05)


### Bug Fixes

* **ci:** find the release pull request by branch, not by prs_created ([0b9b272](https://github.com/metasequoiaime/MSIME-Windows/commit/0b9b2728befbfe5afe08668944b31c8e6fd5504b))
* **ci:** find the release pull request by branch, not by prs_created ([550e0a1](https://github.com/metasequoiaime/MSIME-Windows/commit/550e0a1c70b675184efab104942c1c4bcf1e7ab6))

## [0.0.16](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.15...v0.0.16) (2026-09-05)


### Bug Fixes

* **tsf:** commit the create-word prefix when Enter finalizes the composition ([75d8366](https://github.com/metasequoiaime/MSIME-Windows/commit/75d83660cd5b41be32e3089936fd22f5bd3a91b8))

## [0.0.15](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.14...v0.0.15) (2026-09-05)


### Bug Fixes

* **ci:** take the release scripts from the default branch, not the built commit ([ca87304](https://github.com/metasequoiaime/MSIME-Windows/commit/ca87304f0df71f0aacde4c0ff4a3ab6db6547669))

## [0.0.14](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.13...v0.0.14) (2026-09-05)


### Bug Fixes

* **image:** add small icon frames to msime.ico ([facfe60](https://github.com/metasequoiaime/MSIME-Windows/commit/facfe60761c4f3b23e2ab58cbfa66d25385dcee0))
* **image:** add small icon frames to msime.ico ([f6a4567](https://github.com/metasequoiaime/MSIME-Windows/commit/f6a4567b5e7fcf58454d394a12aed9ba3a20e9ae))

## [0.0.13](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.12...v0.0.13) (2026-09-05)


### Bug Fixes

* **ci:** install the Chinese language file Inno Setup needs ([22a132e](https://github.com/metasequoiaime/MSIME-Windows/commit/22a132e3bd4d48c6185176c0855debdc3a1cfd73))
* **ci:** supply the Inno Setup language file, and move release shell into scripts/ci ([0761b41](https://github.com/metasequoiaime/MSIME-Windows/commit/0761b410e378ac71ab1c6239b5c2cd7db81dab61))

## [0.0.12](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.11...v0.0.12) (2026-09-05)


### Bug Fixes

* **scripts:** read the vcpkg root from the environment ([0259f7a](https://github.com/metasequoiaime/MSIME-Windows/commit/0259f7a9c57d29445f91896662c31f616e0ab7b0))

## [0.0.11](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.10...v0.0.11) (2026-09-05)


### Bug Fixes

* **ci:** build the Server in build/, the only directory name it links in ([ca0a20b](https://github.com/metasequoiaime/MSIME-Windows/commit/ca0a20b50e9f65bb43be97940a334a5944322ec5))
* **ci:** build the Server in build/, the only directory name it links in ([053c987](https://github.com/metasequoiaime/MSIME-Windows/commit/053c987af3b629a6d91d97c9ccd0f5d1e85236f4))
* **ci:** verify the version resource at the source, not on the built DLL ([aca9c8a](https://github.com/metasequoiaime/MSIME-Windows/commit/aca9c8a31debde83bd50583c054cefd7f0914148))
* **ci:** verify the version resource at the source, not on the built DLL ([2b17384](https://github.com/metasequoiaime/MSIME-Windows/commit/2b173841f59402b1d3f5abdfb818369bd72537a3))

## [0.0.10](https://github.com/metasequoiaime/MSIME-Windows/compare/v0.0.9...v0.0.10) (2026-09-05)


### Bug Fixes

* **tsf:** convert candidate anchors to physical coordinates ([089e47b](https://github.com/metasequoiaime/MSIME-Windows/commit/089e47b60cd12cf3f876cd89dd1f19fdf81612ee))
* **tsf:** pass application modifier shortcuts through ([4b1696c](https://github.com/metasequoiaime/MSIME-Windows/commit/4b1696ced320360c8490d453ca407145b4ed0f4e))
* **tsf:** pass application modifier shortcuts through in Chinese mode ([469aefe](https://github.com/metasequoiaime/MSIME-Windows/commit/469aefed86c9cdf203217bccc69c91abcd26762d))
* **tsf:** place candidate window with per-monitor DPI coordinates ([a6f94de](https://github.com/metasequoiaime/MSIME-Windows/commit/a6f94debbaf3ffc7c18419f620a6c8c2156be669))
