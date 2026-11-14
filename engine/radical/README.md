# U 模式部件拆字反查表

`decomposition.txt` 是 U 模式的运行时数据：用户输入一个字各组成部分的拼音连写，就能候选出
该字，候选上再反显它的读音。例如 晶 = 日 + 日 + 日 → `ririri`。

## 文件格式

```
<部件拼音连写>\t<汉字>\t<字频>\t<读音>
```

- 整表按字频降序排列。查询时全码命中排在仅前缀命中之前，各自按字频；一个字有几种拆法时只出一次。
- 首字符 `#` 为注释行；码只含纯小写 a–z，撇号已在生成时去掉。
- 只收汉字：上游在 `abc` 码下的数字、字母和数学符号在生成时滤掉。
- 第 4 列读音是该字的拼音（轻声字如「的 de」没有调号），多音字最多取**两个**，逗号分隔
  （`长 = zhǎng,cháng`）。常用字的第二读音必须在现代规范字典里**至少被两本共同收录**，
  且不取轻声，避免把只在单本出现的又音、古读标成常用读法；规范字典未收录的生僻字才退回
  `pinyin.txt` 的第二项。来源见 [NOTICE.md](NOTICE.md)。上游拼音数据没有收录的字留空，
  候选上不显示注音。
- 加载与查询见 `engine/local_modes/radical_query.{h,cpp}`。

## 重新生成

数据表是**提交产物**，构建流水线不进本仓。需要随上游更新时手工重建：

```bash
python scripts/build_decomposition.py \
    --source radical_pinyin.dict.yaml \
    --readings-dir <pinyin-data 仓库根目录> \
    --output ../decomposition.txt \
    --version <上游拆字表版本> \
    --readings-version <pinyin-data 版本>
```

`--readings-dir` 下必须同时有 `pinyin.txt`、`kHanyuPinlu.txt`、`kXHC1983.txt`、
`kTGHZ2013.txt`（同一个 pinyin-data 版本发布）。`--version` / `--readings-version` 分别对
上游 YAML 头部 `version:` 与 `pinyin.txt` 头部 `# version:` 做显式校验，避免拿旧表静默覆盖。
两个上游来源与授权见 [NOTICE.md](NOTICE.md)。
改表后同步 `engine/contracts/assets/assets.json` 里 `radical_decomposition` 的路径（如文件名有变），
并重跑 `engine/contracts/assets/generate.py`。
