"""Convert a chinese-ime-lm keystroke reranker (`rerank-*.ort`) into the sentence-model safetensors format.

The `rerank-v*` releases ship only ONNX Runtime ORT files of `ChoiceModel.lm_step`. Its LM path is the
CharLM that `docs/format.md` of chinese-ime-lm describes and `engine/neural/sentence_model.cpp` runs:
the character table is `tok`, the position table plus the context segment's embedding is `pos`, and
`mlp.0` / `mlp.2` are `fc` / `out`. So the weights can be read back out of the graph and written as a
model this engine loads, without the original checkpoint.

The ORT weights are uint8 with one scale and zero point per tensor; the fp32 they came from is gone.
They are rewritten as the format's per-output-row symmetric int8. A row whose offsets from the zero
point all lie within +-127 keeps its integers and the tensor's scale, so it is bit-exact; the few rows
that reach past 127 are rescaled to fit and rounded once, and the script reports how many and by how
much. `pos.weight` (with the segment embedding folded in), LayerNorm parameters and biases stay fp32,
as the format requires.

Two properties of the rerank models are not in format version 1 and are written into `config` so a
runtime can refuse what it cannot compute: `gelu_tanh` (the 6x256 model uses GELU's tanh form) and
`self_normalized` (trained so a character's raw logit is its log-probability; the softmax still gives
a valid distribution). `context` is the graph's `max_positions`.

usage:
  python -I scripts/convert-rerank-model.py --ort rerank-8x448.ort --model-json rerank-8x448.model.json \
      --vocab rerank-8x448.vocab.json --source-tag rerank-v2 --out rerank-8x448.safetensors

Needs numpy and onnxruntime (for its bundled ORT flatbuffers bindings). The output is byte-for-byte
reproducible: the same `.ort` always converts to the same SHA256.
"""

import argparse
import hashlib
import json
import math
import re
from pathlib import Path

import numpy as np
from onnxruntime.tools.ort_format_model.ort_flatbuffers_py.fbs import InferenceSession

NUMPY_TYPES = {1: np.float32, 2: np.uint8, 3: np.int8, 7: np.int64}
ROLES = {"qkv": "qkv", "proj": "proj", "mlp.0": "fc", "mlp.2": "out"}
BIAS = re.compile(r"^model\.blocks\.(\d+)\.(qkv|proj|mlp\.0|mlp\.2)\.bias$")


class Graph:
    def __init__(self, path):
        data = Path(path).read_bytes()
        model = InferenceSession.InferenceSession.GetRootAsInferenceSession(data, 0).Model()
        graph = model.Graph()
        self.metadata = {}
        for i in range(model.MetadataPropsLength()):
            prop = model.MetadataProps(i)
            self.metadata[prop.Key().decode()] = (prop.Value() or b"").decode()
        self.tensors = {}
        for i in range(graph.InitializersLength()):
            t = graph.Initializers(i)
            dtype = NUMPY_TYPES.get(t.DataType())
            if dtype is None or t.RawDataLength() == 0:
                continue
            dims = [t.Dims(j) for j in range(t.DimsLength())]
            self.tensors[t.Name().decode()] = t.RawDataAsNumpy().view(dtype).reshape(dims).copy()
        self.nodes = []
        for i in range(graph.NodesLength()):
            n = graph.Nodes(i)
            attrs = {}
            for j in range(n.AttributesLength()):
                a = n.Attributes(j)
                attrs[a.Name().decode()] = (a.F(), a.I())
            self.nodes.append(
                {
                    "op": n.OpType().decode(),
                    "inputs": [n.Inputs(j).decode() for j in range(n.InputsLength())],
                    "outputs": [n.Outputs(j).decode() for j in range(n.OutputsLength())],
                    "attrs": attrs,
                }
            )

    def only(self, predicate, what):
        found = [n for n in self.nodes if predicate(n)]
        if len(found) != 1:
            raise SystemExit(f"expected one {what}, found {len(found)}")
        return found[0]

    def scalar(self, name):
        return self.tensors[name].reshape(()).item()


def table(graph, name, indices):
    """The quantized embedding table `name` as gathered by `indices`: (integer offsets, scale, the dequantized output's name)."""
    gather = graph.only(
        lambda n: n["op"] == "Gather" and n["inputs"] == [f"{name}_quantized", indices], f"Gather of {name}"
    )
    dequant = graph.only(
        lambda n: n["op"] == "DequantizeLinear" and n["inputs"][0] == gather["outputs"][0], f"dequantize of {name}"
    )
    q = graph.tensors[f"{name}_quantized"].astype(np.int32)
    zero = int(graph.scalar(dequant["inputs"][2]))
    return q - zero, np.float32(graph.scalar(dequant["inputs"][1])), dequant["outputs"][0]


def int8_rows(offsets, scale, report, name):
    """Per-output-row symmetric int8 of `offsets * scale` (rows = first axis). Rows within +-127 are exact."""
    peak = np.abs(offsets).max(axis=1)
    wide = peak > 127
    ints = offsets.copy()
    scales = np.full(offsets.shape[0], scale, np.float32)
    if wide.any():
        factor = 127.0 / peak[wide]
        ints[wide] = np.round(offsets[wide] * factor[:, None])
        scales[wide] = (np.float64(scale) * peak[wide] / 127.0).astype(np.float32)
        exact = offsets[wide].astype(np.float64) * np.float64(scale)
        error = np.abs(ints[wide] * scales[wide, None].astype(np.float64) - exact).max()
        report.append(f"{name}: {wide.sum()}/{len(wide)} rows rescaled, max error {error / scale:.3f} of a source step")
    assert np.abs(ints).max() <= 127
    return ints.astype(np.int8), scales


def convert(graph, meta, tokens):
    cfg = meta["config"]
    n_layer, n_head, n_embd, vocab = cfg["n_layer"], cfg["n_head"], cfg["n_embd"], cfg["vocab"]
    if cfg.get("ngram_buckets") or cfg.get("lm_term"):
        raise SystemExit("n-gram tables and the LM term are not part of the sentence-model format")
    if len(tokens) != vocab:
        raise SystemExit(f"vocab.json has {len(tokens)} tokens, config says {vocab}")

    # What the format assumes and the graph has to agree with: LayerNorm eps, attention scale, GELU form.
    for n in graph.nodes:
        if n["op"] == "LayerNormalization" and not math.isclose(n["attrs"]["epsilon"][0], 1e-5, rel_tol=1e-4):
            raise SystemExit(f"LayerNorm epsilon {n['attrs']['epsilon'][0]} is not 1e-5")
        if n["op"] == "FusedMatMul":
            alpha = n["attrs"]["alpha"][0]
            if not math.isclose(alpha, 1.0 / math.sqrt(n_embd // n_head), rel_tol=1e-5):
                raise SystemExit(f"attention scale {alpha} is not 1/sqrt(head_dim)")
    # Exported tanh GELU stays as Tanh arithmetic (or fuses into FastGelu); the exact form is Erf, or the
    # contrib Gelu the ORT optimizer fuses it into, which is the erf form.
    tanh = any(n["op"] in ("Tanh", "FastGelu") for n in graph.nodes)
    erf = any(n["op"] in ("Erf", "Gelu") for n in graph.nodes)
    if tanh == erf or tanh != bool(cfg["gelu_tanh"]):
        raise SystemExit(f"GELU in the graph (tanh={tanh}, erf={erf}) does not match gelu_tanh={cfg['gelu_tanh']}")

    report = []
    out = {}

    chars, chars_scale, chars_out = table(graph, "model.chars.weight", "nodes")
    out["tok.weight"], out["tok.weight.scale"] = int8_rows(chars, chars_scale, report, "tok.weight")

    # x = chars[node] + segments[context] + positions[pos]: the segment row is a constant the export folded.
    add = graph.only(lambda n: n["op"] == "Add" and n["inputs"][0] == chars_out, "segment Add")
    segment = graph.tensors[add["inputs"][1]].astype(np.float64)
    pos, pos_scale, _ = table(graph, "model.positions.weight", "positions")
    out["pos.weight"] = (pos * np.float64(pos_scale) + segment).astype(np.float32)

    seen = set()
    for n in graph.nodes:
        if n["op"] != "DynamicQuantizeMatMul":
            continue
        weight, scale, zero, bias = n["inputs"][1:5]
        offsets = graph.tensors[weight].astype(np.int32) - int(graph.scalar(zero))
        if not bias:
            # The 8x448 graph's output projection through the tied table: must be the same table.
            if not np.array_equal(offsets.T, chars) or np.float32(graph.scalar(scale)) != chars_scale:
                raise SystemExit(f"{weight} is not the character table it is supposed to be tied to")
            continue
        match = BIAS.match(bias)
        if match is None:
            raise SystemExit(f"unexpected quantized MatMul with bias {bias}")
        layer, role = int(match.group(1)), ROLES[match.group(2)]
        name = f"blocks.{layer}.{role}.weight"
        # ONNX MatMul's B is [in, out]; the format's weights are [out, in].
        out[name], out[name + ".scale"] = int8_rows(
            np.ascontiguousarray(offsets.T), np.float32(graph.scalar(scale)), report, name
        )
        out[f"blocks.{layer}.{role}.bias"] = graph.tensors[bias].astype(np.float32)
        seen.add(name)
    for layer in range(n_layer):
        for role in ROLES.values():
            if f"blocks.{layer}.{role}.weight" not in seen:
                raise SystemExit(f"blocks.{layer}.{role}.weight not found in the graph")
        for norm in ("ln1", "ln2"):
            for part in ("weight", "bias"):
                out[f"blocks.{layer}.{norm}.{part}"] = graph.tensors[f"model.blocks.{layer}.{norm}.{part}"]
    for part in ("weight", "bias"):
        out[f"ln_f.{part}"] = graph.tensors[f"model.ln_f.{part}"]

    expected = {
        "tok.weight": (vocab, n_embd),
        "pos.weight": (cfg["max_positions"], n_embd),
        "blocks.0.qkv.weight": (3 * n_embd, n_embd),
        "blocks.0.fc.weight": (4 * n_embd, n_embd),
        "blocks.0.out.weight": (n_embd, 4 * n_embd),
    }
    for name, shape in expected.items():
        if out[name].shape != shape:
            raise SystemExit(f"{name} has shape {out[name].shape}, expected {shape}")
    return {k: np.ascontiguousarray(v) for k, v in out.items()}, report


SAFETENSORS_DTYPES = {np.dtype(np.float32): "F32", np.dtype(np.int8): "I8"}


def write_safetensors(path, tensors, metadata):
    """Write a safetensors file byte-for-byte reproducibly.

    The `safetensors` package serializes its header from a hash map, so the same tensors come out with
    their keys in a different order on every run and the file's SHA256 changes. The file is pinned by
    digest in the consumers' locks, so here the header is written by hand: metadata first, tensors in
    name order (their data in the same order), padded with spaces to 8 bytes as the format allows.
    """
    header = {"__metadata__": dict(sorted(metadata.items()))}
    offset = 0
    names = sorted(tensors)
    for name in names:
        array = tensors[name]
        size = array.nbytes
        header[name] = {
            "dtype": SAFETENSORS_DTYPES[array.dtype],
            "shape": list(array.shape),
            "data_offsets": [offset, offset + size],
        }
        offset += size
    text = json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    text += b" " * (-(8 + len(text)) % 8)
    with open(path, "wb") as handle:
        handle.write(len(text).to_bytes(8, "little"))
        handle.write(text)
        for name in names:
            handle.write(np.ascontiguousarray(tensors[name]).astype(tensors[name].dtype.newbyteorder("<")).tobytes())


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ort", required=True)
    parser.add_argument("--model-json", required=True)
    parser.add_argument("--vocab", required=True)
    parser.add_argument("--source-tag", required=True, help="the chinese-ime-lm release the files came from")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    meta = json.loads(Path(args.model_json).read_text(encoding="utf-8"))
    tokens = json.loads(Path(args.vocab).read_text(encoding="utf-8"))["tokens"]
    graph = Graph(args.ort)
    # The attribution must travel with the weights; the graph and model.json have to say the same thing.
    for key in ("attribution", "license"):
        if graph.metadata.get(key) != meta[key]:
            raise SystemExit(f"{key} differs between the ORT metadata and model.json")

    tensors, report = convert(graph, meta, tokens)
    cfg = meta["config"]
    config = {
        "vocab": cfg["vocab"],
        "n_layer": cfg["n_layer"],
        "n_head": cfg["n_head"],
        "n_embd": cfg["n_embd"],
        "context": cfg["max_positions"],
        "dropout": 0.0,
        "gelu_tanh": bool(cfg["gelu_tanh"]),
        "self_normalized": bool(cfg["self_normalized"]),
        "context_chars": meta["context_chars"],
    }
    metadata = {
        "format": "chinese-ime-lm",
        "version": "1",
        "precision": "int8",
        "config": json.dumps(config, separators=(",", ":")),
        "vocab": json.dumps(tokens, ensure_ascii=False, separators=(",", ":")),
        "tied_embeddings": "true",
        "license": meta["license"],
        "attribution": meta["attribution"],
        "source": f"chinese-ime-lm {args.source_tag} {Path(args.ort).name} sha256:{sha256(args.ort)}",
    }
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    write_safetensors(args.out, tensors, metadata)
    for line in report:
        print(line)
    print(json.dumps({"file": Path(args.out).name, "sha256": sha256(args.out), "size": Path(args.out).stat().st_size}))


if __name__ == "__main__":
    main()
