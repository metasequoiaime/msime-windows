#include "neural_model.h"

#include "model_download.h"

namespace neural_model
{

namespace
{

// 与 neural-model/lock.json 的 desktop 条目一致。
constexpr model_download::ModelSpec kSpec{
    kModelDirectoryName,
    kModelFileName,
    L"github.com",
    L"/metasequoiaime/chinese-ime-lm/releases/download/rerank-v2-safetensors/rerank-8x448.safetensors",
    25710000,
    "94f76e88f8a3fca46c8b1e704f189ce32fedd24d1ea281508733ee3cab986b84",
    "chinese-ime-lm rerank-v2-safetensors",
    "chinese-ime-lm 神经整句模型（效果优先，rerank-8x448 v2）",
    "Apache-2.0",
    "",
    L"神经整句模型 (*.safetensors)",
    L"*.safetensors",
    L"选择已下载的 rerank-8x448.safetensors",
    // 模型被 Server 读进内存后不占文件句柄，随时删得掉。
    false,
    kLegacyModelFileName,
};

model_download::ModelDownloader &Downloader()
{
    static model_download::ModelDownloader downloader(kSpec);
    return downloader;
}

} // namespace

std::string SourceUrl()
{
    return Downloader().SourceUrl();
}

ModelStatus GetModelStatus()
{
    const auto status = Downloader().GetStatus();
    std::error_code error;
    const bool legacy = status.state != "ready" && std::filesystem::is_regular_file(LegacyModelFile(), error) && !error;
    return {status.state, status.progress, status.error, legacy};
}

bool VerifyModelFile(const std::filesystem::path &file, std::string &error)
{
    return Downloader().Verify(file, error);
}

bool StartDownload()
{
    return Downloader().StartDownload();
}

bool ImportModel(const std::filesystem::path &source)
{
    return Downloader().Import(source);
}

std::filesystem::path PromptForModelFile(HWND owner)
{
    return Downloader().PromptForFile(owner);
}

bool DeleteModel()
{
    return Downloader().Delete();
}

} // namespace neural_model
