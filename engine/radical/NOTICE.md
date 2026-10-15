# 来源与授权说明

`decomposition.txt` 是 U 模式（部件拆字反查）使用的「部件拼音连写 → 汉字」表。它由上游拆字
数据转换而来，不是本项目原创，权利随上游。

## 数据表

| 文件 | 内容 | 来源与权利归属 |
| --- | --- | --- |
| `decomposition.txt` | `<部件拼音连写>\t<汉字>\t<字频>`，整表按字频降序 | 由 [mirtlecn/rime-radical-pinyin](https://github.com/mirtlecn/rime-radical-pinyin) 的 `radical_pinyin.dict.yaml` 转换。上游为 **GPL-3.0**，其拆字数据又聚合自 [chaizi](https://github.com/kfcd/chaizi)（CC-BY-3.0）、[CHISE](https://www.chise.org/)（GPL-2.0-or-later）、[yi-bai/ids](https://github.com/yi-bai/ids)（MIT）。本项目按 GPL-3.0 分发，转换后的表沿用同一许可。 |

`engine/helpcode/` 里的 `jiajia` 表也用到了同一份 rime-radical-pinyin 拆字数据来重建，
那边的登记见 `engine/helpcode/NOTICE.md`。

## 本项目自建部分

`scripts/build_decomposition.py` 由本项目编写，依据 GPL-3.0 提供。它只做格式转换：
读取上游 YAML 的 `<汉字>\t<部件拼音>\t<字频>` 列，去掉撇号压成连写码，按字频降序输出。
上游版本随发布变化，重建时必须带 `--version` 显式对齐。

## 下游影响

该表随包安装到数据目录的 `radical/decomposition.txt`，引擎据此做前缀匹配。Windows 前端与
其他平台前端共用同一份数据。
