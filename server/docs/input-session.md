# Shared input session

`EngineInputSession` adapts Windows key types and persisted settings to Engine `metasequoia::InputSession`.
The old Shuangpin session implementation is removed; stored `legacy` backend values resolve to the same
shared engine. Scheme profiles remain explicit constructor inputs.

The shared session owns composition advancement, canonical phrase progress, cloud query preparation,
candidate storage and engine cache state. Radical (U), date/time, quick phrases, emoji, kaomoji and super-jianpin
queries all delegate to Engine local-mode APIs. Windows keeps focus epochs, key swallowing, TSF insertion,
worker scheduling and UI ownership. In particular, delayed insertion/learning runs on the existing Windows
queues; moving query algorithms does not make those operations synchronous on the pipe thread.

Portable clients use character/command calls. Asynchronous hosts use the advanced selection transition and
phrase-progress API on the same class; the host chooses when to insert text and reset a completed sequence.
Regression coverage includes partial selection with manual delimiters, canonical readings across multiple
selections, unknown-reading suppression, stale online results, and seven/eight/nine-syllable dictionary replay.
The Windows suite additionally exercises real dictionaries and each supported Shuangpin profile.
