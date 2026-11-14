# 来源与授权说明

`decomposition.txt` 是 U 模式（部件拆字反查）使用的「部件拼音连写 → 汉字」表。它由上游拆字
数据与拼音数据合成，不是本项目原创，权利随上游。

## 数据表

| 文件 | 内容 | 来源与权利归属 |
| --- | --- | --- |
| `decomposition.txt` | `<部件拼音连写>\t<汉字>\t<字频>\t<读音>`，整表按字频降序 | 拆字部分由 [mirtlecn/rime-radical-pinyin](https://github.com/mirtlecn/rime-radical-pinyin) 的 `radical_pinyin.dict.yaml` 转换。上游为 **GPL-3.0**，其拆字数据又聚合自 [chaizi](https://github.com/kfcd/chaizi)（CC-BY-3.0）、[CHISE](https://www.chise.org/)（GPL-2.0-or-later）、[yi-bai/ids](https://github.com/yi-bai/ids)（MIT）。本项目按 GPL-3.0 分发，转换后的表沿用同一许可。 |
| 同上，第 4 列读音 | 最多**两个**读音（拼音，轻声无调号，如 `jīng`、`de`、`zhǎng,cháng`） | 来自 [mozillazg/pinyin-data](https://github.com/mozillazg/pinyin-data)（**MIT**）：首选取 `pinyin.txt` 第一项（上游合并出的常用读音）；第二读音取 `kHanyuPinlu.txt`（现代汉语频率词典）、`kXHC1983.txt`（现代汉语词典）、`kTGHZ2013.txt`（通用规范汉字字典）里**至少被两本共同收录**的另一读法，规范字典未收录的生僻字才退回 `pinyin.txt` 第二项。 |

`engine/helpcode/` 里的 `jiajia` 表也用到了同一份 rime-radical-pinyin 拆字数据来重建，
那边的登记见 `engine/helpcode/NOTICE.md`。

## 本项目自建部分

`scripts/build_decomposition.py` 由本项目编写，依据 GPL-3.0 提供。它只做格式转换：
读取上游 YAML 的 `<汉字>\t<部件拼音>\t<字频>` 列，去掉撇号压成连写码，按字频降序输出；
再按汉字从 pinyin-data 的四个文件里选出首读音与一个补充读音写进第 4 列。两个上游源都随发布
变化，重建时必须带 `--version` 与 `--readings-version` 显式对齐。

读音最多两个：候选窗的空间只够排两种读法，更多数目的读法多是异体、通假或极生僻读音，标出来
反而误导。第二读音不直接取 `pinyin.txt` 的第二项——那里是《汉语大字典》的全集，会给出
`而 ér/néng`、`餐 cān/sūn` 这类古音；只在现代规范字典里选被两本以上共同收录的读音，才得到
`长 zhǎng/cháng`、`好 hǎo/hào` 这类真正常用的读法。只出现在一本字典里的 `市 fú`、`广 ān`、
`鸟 diǎo` 多是又音、古读或方言，不作为第二读音。第二读音跳过轻声音节（无调号）；首选仍按上游
常用读法保留轻声（「的 de」）。上游没有读音的字（大量扩展区生僻字）第 4 列留空，候选上也不
显示注音——这是数据本身的边界，不是转换遗漏。

## 下游影响

该表随包安装到数据目录的 `radical/decomposition.txt`，引擎据此做前缀匹配并直接带回读音。Windows
前端与其他平台前端共用同一份数据，候选上的注音不再依赖运行时的 `cpp-pinyin` 词典。
