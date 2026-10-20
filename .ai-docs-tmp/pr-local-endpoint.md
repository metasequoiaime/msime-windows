# fix(server): 本机端点免凭据，输出上限字段按端点选择

## 背景

本机部署的 Ollama、LM Studio 等 OpenAI 兼容服务可以接入，但实际上用不了：配置上能填，请求也被静默拦截或字段发错。两处问题互相独立。

## 问题一：本地服务被凭据门槛拦住

`ai_assistant.cpp` 在发请求前检查 `token.empty()`。Ollama 的本地 API 不做鉴权（官方文档：*The local API at `http://localhost:11434` does not require authentication*），收到 `Authorization` 头也直接丢弃。用户填一个无意义的值，只是为了让请求不被本地拦住。

影响范围不止 AI 联想：候选词翻译（`ai_translation.cpp`）、语音润色（`voice_input_service.cpp`）、设置页的「测试连接」和「查询模型」（`api_credential_test.cpp`）各有自己的检查，判定逻辑一致。

## 问题二：输出上限字段发给了不认它的服务

`SetTokenLimit` 按渠道标签选字段：标签是 `openai` 就发 `max_completion_tokens`，否则发 `max_tokens`。

但 `openai` 槽可以填任意 OpenAI 兼容地址。指向本机 Ollama 时，请求会带上 `max_completion_tokens` —— Ollama 的 `ChatCompletionRequest` 没有这个字段，Go 的 `encoding/json` 默认忽略未知字段，于是配置的输出上限静默失效，请求仍返回 200。表面上一切正常，只是保险丝没了。

## 改动

### 新增 `NetworkProxy::IsLoopbackEndpoint`

只认 `localhost` / `127.0.0.1` / `[::1]`，精确匹配 host，不做后缀匹配（`localhost.evil.com` 必须判否）。

不含私网网段：公司局域网里那台谁都能访问的 Ollama 仍要凭据。

### 凭据门槛放宽

四处改为「本机端点免凭据」。只放宽「要不要拦」，**填了 Token 仍照常发送** —— 本机跑 one-api 这类需要鉴权的服务不受影响。

### 输出上限字段按端点选择

```cpp
return IsOpenAiProvider(provider) && !NetworkProxy::IsLoopbackEndpoint(endpoint);
```

判据**没有**复用 `VendorForRequest`。那个函数只认 `api.openai.com`，会把 Azure OpenAI（`*.openai.azure.com`）和 OpenRouter 一类中转判成其他厂商，降级成 `max_tokens` —— Azure 只接受 `max_completion_tokens`，会直接 400。这是评审中否决掉的更大改动。

### 空 Bearer 头

token 为空时不再发送 `Authorization: Bearer `。部分服务会把它当成非法鉴权返回 400，而不是忽略。

## 影响

| 场景 | 改前 | 改后 |
|---|---|---|
| 本机端点 + Token 空 | 不发请求 | 正常工作 |
| 本机端点 + Token 已填 | 正常 | 不变（Token 照发） |
| `openai` 标签 + 本机端点 | `max_completion_tokens`（被丢弃） | `max_tokens` |
| 云端端点 | 正常 | 不变 |
| Azure / OpenRouter 中转 | `max_completion_tokens` | 不变 |
| 局域网服务 | 需凭据 | 不变 |

无配置项新增，`config.default.toml` 不动；无跨进程协议变更。

## 测试

- `test_network_proxy.cpp`：13 个用例，覆盖回环写法、大小写、userinfo、`localhost.evil.com`、`127.0.0.1.evil.com`、无 scheme、空串
- `test_ai_assistant_request.cpp`：Ollama 地址发 `max_tokens`、真 OpenAI 发 `max_completion_tokens`、Azure 回归保护、探针与真实请求判据一致
- `tests/CMakeLists.txt`：测试目标补 `NOMINMAX`，抵消 `network_proxy.h` 带入的 `Windows.h` 对 nlohmann/json 的 `min`/`max` 宏污染

## 验证

- clang-format 18.1.8 `--check` 通过
- 判定逻辑用独立程序验证：`IsLoopbackEndpoint` 20 例、`UsesOpenAiOutputField` 11 例（含 Azure 回归保护）
- Server 在 MSVC 下编译通过，安装包实机验证 AI 联想可用

## 使用方式

服务商选「Custom」，接口地址填 `http://127.0.0.1:11434/v1/chat/completions`，模型名填已加载的模型，Token 留空。
