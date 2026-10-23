"""Run production voice control/delivery bodies with capture, network and UI doubles."""

from pathlib import Path
import re
import sys

# Reuse the repository's C++ body extractor rather than copy its parser.
source_path, helper_dir, output_path = map(Path, sys.argv[1:])
sys.path.insert(0, str(helper_dir))
from generate_reconnect_fixture import extract_body

source = source_path.read_text(encoding="utf-8")
generated = "// Generated from voice_input_service.cpp; do not edit.\n"
for name in (
    "LockTicketOf", "CurrentLockTicket", "ClearLockForRequest", "ClearRecordingLock",
    "ApplyLockRequest", "EnqueueControlCommand", "ControlLoop", "NormalizeCommitMode", "OwnerRouteValid",
    "SendVoiceCompositionToOwner", "ResetInlinePreeditState", "CancelInlinePreeditFor",
    "CancelInlinePreeditOnControlThread", "SendTextViaTsf", "CommitRecognizedText",
    "InvalidateVoiceSession", "EnqueueCancelFromUi", "StopRecording",
    "CancelRecordingForSession", "CancelRecording", "DiscardRecording",
):
    match = re.search(r"^\w[\w:<> ]*\b" + name + r"\([^;{]*\)\n\{", source, re.MULTILINE)
    if match is None:
        raise ValueError(f"Production function not found: {name}")
    signature = match.group()[:-1].strip()
    generated += signature + "\n" + extract_body(source[match.start():], signature) + "\n"
output_path.write_text(generated, encoding="utf-8")
