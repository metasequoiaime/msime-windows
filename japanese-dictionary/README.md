# 日语候选释义词库

在设置的“输入 → 候选词翻译”中，点击日语释义词库的“下载并构建”。脚本下载开放词典，在本机合并为 SQLite 词库。页面显示构建状态及错误；完成后点击“重启生效”，开启候选词翻译并选择日语。

构建需要 Python 3.10 或以上，可通过 `py -3` 或 PATH 中的 `python` 启动。首次构建需要联网，下载和处理原始词典可能需要几分钟。再次点击按钮会重新下载并构建，用于更新词库。

下载使用 Python 默认的系统或环境变量代理，暂不读取输入法中的自定义代理设置。

“学习”等中文候选显示为 `習う（ならう）`；翻译快捷键上屏时提交 `習う`。纯假名释义不重复显示读音。英文候选沿用 `english.db` 的英中释义，本地未命中时使用所选在线服务。

## 词典来源与授权

| 来源 | 用途 | 下载位置 |
| --- | --- | --- |
| 英文 Wiktionary，经 Kaikki/Wiktextract 提取 | 配对同一义项翻译表中的中文和日语，作为主词库 | [offline-glosses-2026.09.02 的 zh-ja.db](https://github.com/metasequoiaime/chinese-ime-lm/releases/download/offline-glosses-2026.09.02/zh-ja.db) |
| 日文 Wiktionary 的中文词条 | 提取中文词头对应的短日语释义 | [Kaikki 中国語](https://kaikki.org/jawiktionary/%E4%B8%AD%E5%9B%BD%E8%AA%9E/index.html) |
| 中文 Wiktionary 的日语词条 | 反向对应日语词头的短中文释义 | [Kaikki 日语](https://kaikki.org/zhwiktionary/%E6%97%A5%E8%AF%AD/index.html) |
| Chinese Open Wordnet 与 Japanese Wordnet | 按相同义项编号配对中日词条 | [OMW，版本 406bf83](https://github.com/omwn/omw-data/tree/406bf83b3c507a3d1f26e88252d5d66893fd36bf/wns) |
| JMdict / EDRDG | 日语词形筛选及假名读音 | [jmdict-simplified 的最新 JMdict English JSON](https://github.com/scriptin/jmdict-simplified/releases/latest) |

“英文 Wiktionary”是站点版本名称；原始义项的翻译表包含中文、日语等多种语言。生成词库保存其中的中日对应关系。JMdict English JSON 中提供的日语词形和假名用于筛选及读音。

合并时按上表顺序补缺，保留主词库优先级。补充词条限定为 JMdict 收录的日语词形，截取首个释义，读音优先采用常用读音。词条数随下载数据更新而变化，多义词和反向释义不进行上下文消歧。

Wiktionary 和 JMdict 的改编数据采用 CC BY-SA 4.0；Chinese Open Wordnet、Japanese Wordnet 保留各自声明。JMdict 版权归 EDRDG / Jim Breen，使用遵循 [EDRDG 授权](https://www.edrdg.org/edrdg/licence.html)。本机生成的词库同样保留这些授权条件；再次分发时需保留来源及相应授权说明。

脚本将下载地址、日期、改编说明和上游授权文件写入数据目录中的 `japanese-licenses/`。Wiktionary 记录在 `source` 列中保存来源词条地址；Wordnet 记录保存义项编号。

## 脚本使用

脚本随安装包放到数据目录的 `assets/dictionaries/build_japanese_dictionary.py`，也可从源码直接运行：

```powershell
python server/assets/dictionaries/build_japanese_dictionary.py --output-root <输入法数据目录>
```

下载和合并完成后生成 `japanese.db.pending`。Server 重启时将它替换为 `japanese.db` 后以只读方式打开，避免构建期间覆盖正在使用的词库。设置页执行脚本的输出保存在数据目录的 `japanese-dictionary-build.log`。

开发时，先将 `server/assets/dictionaries/` 复制到开发数据目录的 `assets/dictionaries/`，再使用设置页按钮。

## 数据格式

```sql
CREATE TABLE zh_glosses(
    chinese TEXT COLLATE BINARY PRIMARY KEY,
    gloss TEXT NOT NULL,
    reading TEXT NOT NULL,
    source TEXT NOT NULL
) WITHOUT ROWID;
```

Server 在后台线程中按中文词头精确匹配，分别返回释义和读音。词库缺失或 SQLite 读取失败时，按本地未命中处理。
