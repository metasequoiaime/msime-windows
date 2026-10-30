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
    L"/metasequoiaime/chinese-ime-lm/releases/download/model-v1/sentence-model-desktop.safetensors",
    25480184,
    "0a6ecba69bf1d39c7eb49549c716477dd6c03fdf05757773e1f50435262fb469",
    "chinese-ime-lm model-v1",
    "chinese-ime-lm 神经整句模型（效果优先）",
    "Apache-2.0",
    "",
    L"神经整句模型 (*.safetensors)",
    L"*.safetensors",
    L"选择已下载的 sentence-model-desktop.safetensors",
    // 模型被 Server 读进内存后不占文件句柄，随时删得掉。
    false,
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
    return {status.state, status.progress, status.error};
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
