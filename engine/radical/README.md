# U 模式部件拆字反查表

`decomposition.txt` 是 U 模式的运行时数据：用户输入一个字各组成部分的拼音连写，就能候选出
该字，候选上再反显它的读音。例如 晶 = 日 + 日 + 日 → `ririri`。

## 文件格式

```
<部件拼音连写>\t<汉字>\t<字频>
```

- 整表按字频降序排列。查询时全码命中排在仅前缀命中之前，各自按字频；一个字有几种拆法时只出一次。
- 首字符 `#` 为注释行；码只含纯小写 a–z，撇号已在生成时去掉。
- 只收汉字：上游在 `abc` 码下的数字、字母和数学符号在生成时滤掉。
- 加载与查询见 `engine/local_modes/radical_query.{h,cpp}`。

## 重新生成

数据表是**提交产物**，构建流水线不进本仓。需要随上游更新时手工重建：

```bash
python scripts/build_decomposition.py \
    --source radical_pinyin.dict.yaml \
    --output ../decomposition.txt \
    --version <上游版本>
```

`--version` 是对上游 YAML 头部 `version:` 的显式校验，避免拿旧表静默覆盖。上游来源与授权
见 [NOTICE.md](NOTICE.md)。改表后同步 `engine/contracts/assets/assets.json` 里
`radical_decomposition` 的路径（如文件名有变），并重跑 `engine/contracts/assets/generate.py`。
