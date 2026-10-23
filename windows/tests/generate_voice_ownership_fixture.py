"""Compile the production preview writes/cancel against text-store doubles; no TIP registration."""

from pathlib import Path
import sys

from generate_reconnect_fixture import extract_body


composition, keys, output = map(Path, sys.argv[1:])
generated = "// Generated from production voice ownership paths; do not edit.\n"
for path, names in (
    (composition, ("_AddComposingAndChar", "_AddCharAndFinalize", "_InsertTextToComposition",
                   "_SetCompositionTextAndSelection")),
    (keys, ("_HandleUpdateVoiceComposition", "_HandleCommitVoiceComposition", "_HandleCancelVoiceComposition")),
):
    source = path.read_text(encoding="utf-8")
    for name in names:
        marker = "HRESULT CMetasequoiaIME::" + name + "("
        start = source.index(marker)
        signature = source[start:source.index("{", start)].strip().replace("_In_ ", "")
        body = extract_body(source, marker)
        generated += signature + "\n" + body + "\n"
output.write_text(generated, encoding="utf-8")
