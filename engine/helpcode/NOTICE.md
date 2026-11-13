# 来源与授权说明

本仓库**不对外提供统一的开源许可**。`helpcodes/` 下的辅助码表复现的是各家已发表的输入方案，权利归各方案作者，本项目只是整理成统一格式供引擎读取。给整个仓库挂一份 LICENSE 等于替方案作者重新授权，因此这里改为逐项说明来源。

## 辅助码表

| 文件 | 方案 | 来源与权利归属 |
| --- | --- | --- |
| `helpcode.txt` | `lantian` | 蓝天小雨点辅助码，权利归方案作者 |
| `zrm_helpcode_big_unique.txt` | `ziranma` | 自然码辅助码，整理自 [copperay/ZRM_Aux-code](https://github.com/copperay/ZRM_Aux-code)（上游**未声明许可**） |
| `shouyou2_0_helpcode.txt` | `shouyou2_0` | 首右 2.0 辅助码，权利归方案作者 |
| `shouyouplus_helpcode.txt` | `shouyouplus` | 首右 plus 辅助码，权利归方案作者 |
| `xiaohe_helpcode.txt` | `xiaohe` | 小鹤形码，权利归小鹤方案作者 |
| `jiajia_helpcode.txt` | `jiajia` | 加加辅助码。规则是「按笔顺拆出前两个部件、各取读音首字母」。原有 7968 字沿用旧版构造：按拼音加加输入法 5.x 安装包内的辅助码表 `fzm.bin` 对齐，该表覆盖 GB2312 全部 6763 字并含部分扩展字（基本区共 7259 字有码，其中 6576 字为两码）；其中 6487 字从该表取到两码、6349 字逐字一致，138 个独体/部首字保留本表规则（取「本字声母 + 起笔」，其中 13 个成字部件按俗称读音取音），未采用加加对这些字给出的另一套拆法，其余不在该表覆盖内的字按同一规则由公开拆字数据重建。扩展部分新增 13359 字，由 [`scripts/build_jiajia_helpcode.py`](./scripts/build_jiajia_helpcode.py) 按同一规则从公开拆字数据生成，覆盖 GBK 字集（按 CP936 解码得到 20924 个汉字）中的 20901 个，缺 23 个没有拆字数据的字（〇 丣 丱 乀 乁 乄 乚 乛 亅 兯 冎 匸 厁 夊 栆 桛 氺 癶 稥 篐 鎼 﨎 﨟），扩展 A 的 䱷 同样没有拆字数据、未收入；另含 GBK 之外的 426 个字符（扩展 A 133、扩展 B 46、扩展 C–G 170、兼容汉字 69，以及 GB18030 有而 GBK 没有的 8 个 BMP 字，如 鿍、鿕）；字符集来自同目录其他辅助码表与出货词库。`fzm.bin` 内是加加双拼键位（`zh=v`/`ch=u`/`sh=i`），对照时已换算为声母 z/c/s。部件拆分数据来自 [rime-radical-pinyin](https://github.com/mirtlecn/rime-radical-pinyin)（GPL-3.0，上游含 chaizi/CC-BY-3.0、CHISE/GPL-2+、yi-bai/ids/MIT），笔顺数据来自 cnchar（MIT）。拼音加加为商业软件，本表是格式转换与整理，**不是加加官方码表**。 |

## 下游影响

这些文件被 [MSIME-Linux](https://github.com/metasequoiaime/MSIME-Linux) 的 DEB／RPM 包安装到 `share/metasequoiaime/helpcodes/` 下，也被 Windows 与 Apple 前端使用。前端本身以 GPL-3.0 分发，但该许可**不覆盖**这些辅助码表的内容。

## 待解决

上表中没有任何一项拿到了明确的再分发授权。需要逐个与方案作者确认，或改为在运行时由用户自行导入而不随包分发。在澄清之前，请不要假定这些数据可以自由再分发。

`jiajia` 一行与其余各表性质不同：它的一部分条目直接来自商业软件安装包内的数据表（`fzm.bin`），
其余各表只是复现已发表的输入方案。若后续要做权利澄清，应优先处理这一项。

## 本项目自建部分

`scripts/` 下的整理脚本由本项目编写，依据 GPL-3.0 提供，与组织内其他仓库一致。
