#include "lattice_model.h"

namespace lattice_model
{

namespace
{

// 与 language-model/lock.json 的 output 一致。
constexpr model_download::ModelSpec kSpec{
    kModelDirectoryName,
    metasequoia::assets::language_model,
    L"github.com",
    L"/metasequoiaime/chinese-ime-lm/releases/download/language-model-libime-20260629/sc.lm",
    34736327,
    "3588b3942c8fd62e1a6bd3bae8c7cadc0faf928c175b245224ed475862218387",
    "chinese-ime-lm language-model-libime-20260629",
    "词格整句语言模型 sc.lm",
    "LGPL-2.1-or-later（Copyright (C) 2022~2026 CSSlayer）",
    "由 libime 发布的 lm_sc.arpa（https://download.fcitx-im.org/data/lm_sc.arpa-20260629.tar.zst）"
    "经 kenlm build_binary -s -a 22 -q 4 trie 转换，内容不变。LGPL-2.1 正文见安装目录 THIRD_PARTY_NOTICES.txt "
    "的 kenlm 一节，也随下载来源的 release 附带（LGPL-2.1.txt）。",
    L"词格语言模型 (sc.lm)",
    L"*.lm",
    L"选择已下载的 sc.lm",
    true,
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

model_download::ModelStatus GetModelStatus()
{
    return Downloader().GetStatus();
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

} // namespace lattice_model
