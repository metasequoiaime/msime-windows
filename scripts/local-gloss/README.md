# 多语种本地候选词释义（local-gloss）

上游的水杉输入法只有中英双向的本地释义表。这个目录把同样的本地优先扩展到
`en / fr / ja / es / ru / de / ko / th / vi / it` 十个目标语种，本地查不到时
再交给设置里的在线服务（选 `ai` 就是直接走 AI 辅助那套接口）。

## 工具

`tools/glossctl` 是这套数据的命令行工具，纯 Go + `sqlite3.dll`，不引第三方依赖：

```
glossctl stats  --db english.db
glossctl export --source 旧english.db --out local_glosses.db
glossctl merge  --source 旧english.db --target english.db
glossctl verify --db english.db
```

实测：225.92 万条导出 33.3 秒（约 16 万条/秒），导出库 182.5 MB，校验 0 问题。
把 `sqlite3.dll` 放到 exe 同目录即可运行；建表语句与 engine 侧 `ensure_schema` 逐字一致
（`COLLATE BINARY` + `WITHOUT ROWID`）。

## 数据形状

新增一张表，和现有两张释义表并列存在 `english.db` 里：

```sql
CREATE TABLE zh_glosses (
    chinese TEXT COLLATE BINARY NOT NULL,
    lang    TEXT COLLATE BINARY NOT NULL,
    gloss   TEXT NOT NULL,
    PRIMARY KEY(chinese, lang)
) WITHOUT ROWID;
```

- 英语目标仍读原来的 `zh_en_glosses`，行为完全不变。
- 其余语种读 `zh_glosses`，`lang` 用设置页那套代码（`fr` / `ja` / …）。
- 查不到 → 空串 → 走云端；云端的译文会被写回这张表（`PersistGloss`），
  所以没预置的词用过一次之后也变成本地命中。

## 生成流程

1. `build_local_glosses.py`：从 `zh_en_glosses` 取出全部中文词头，把已有的英文
   释义翻成九个语种（TokenHub 上的 `hy-mt2-pro` 之类），增量写 `glosses.tsv`。
   以「中文 + 语种」为键去重，中断后重跑会自动续上。

   ```powershell
   $env:OH_KEY = '<token>'
   python scripts/local-gloss/build_local_glosses.py `
       --source <原 english.db> --tsv build/gloss/glosses.tsv --workers 10
   ```

2. `merge_glosses.py`：把 TSV 合并进一份 `english.db` 副本，只保留
   `zh_en_glosses` 里确实存在的词头，写完做 `integrity_check` 并把
   `user_version` 置为 4。

   ```powershell
   python scripts/local-gloss/merge_glosses.py `
       --source <原 english.db> --tsv build/gloss/glosses.tsv --out english.local.db
   ```

3. 把产物替换掉安装目录里的 `english.db`（Server 启动时 `ensure_schema` 也会
   在旧库上补建这张空表，所以换库不是必须的，只是预置才需要）。

## 术语与长度

- 一个词最多两个义项，用 `; ` 连接，和英语释义的呈现一致。
- 超过 40 字符的译文会在词边界截断，避免撑坏候选窗。
- 只保留 `zh_en_glosses` 里已有的词头：拼音词典打不出的词不进本地包。

## 已知边界

- 只覆盖「中文候选 → 目标语种」；反向（外语候选 → 中文）仍然只有英语。
- 词形变化重的语种（俄语、德语）单条释义偏「提示」而非「译文」，这是短释义
  这一形态本身的限制，不是数据缺失。
- 词头集合沿用上游用 ECDICT 推出的那 3 万多条，`一击 → sock` 这类上游就错的
  义项会被原样翻过去。
