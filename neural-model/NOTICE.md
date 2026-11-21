# chinese-ime-lm sentence models

The two neural sentence models used here are from the **chinese-ime-lm** project
(<https://github.com/metasequoiaime/chinese-ime-lm>), release `rerank-v2-safetensors`, the
safetensors form of the `rerank-v2` keystroke rerankers:

- `sentence-model.safetensors` (`rerank-6x256.safetensors`) — "keyboard" preset (vocab 12288,
  6 layers, n_embd 256, tanh GELU, self-normalized), shipped with the installer.
- `sentence-model-desktop-v2.safetensors` (`rerank-8x448.safetensors`) — "desktop" preset (vocab 12288,
  8 layers, n_embd 448), downloaded on demand from the settings page.

Both are character-level Transformer language models used to rerank whole-sentence candidates. The
safetensors files were converted from the release's ONNX Runtime files by
`scripts/convert-rerank-model.py`; the weights were not retrained.

## License

The model **weights** are licensed under **Apache-2.0** (recorded in each file's
`__metadata__["license"]`). The C++ inference code in `engine/neural/` is a port of the project's
Apache-2.0 reference implementation (`reference/`).

## Attribution

Trained on the Chinese portion of C4 (ODC-BY, <https://huggingface.co/datasets/allenai/c4>),
FineWeb-2 (cmn_Hani) (ODC-BY, <https://huggingface.co/datasets/HuggingFaceFW/fineweb-2>),
LCCC (MIT, <https://github.com/thu-coai/CDial-GPT>) and synthetic one-sided chat messages written by
DeepSeek deepseek-v4-pro for the chinese-ime-lm project (DeepSeek Terms of Use, outputs assigned to
the user and usable to train other models,
<https://cdn.deepseek.com/policies/en-US/deepseek-terms-of-use.html>). The synthetic portion is
marked as synthetic.
