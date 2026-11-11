#pragma once

#include "streaming_asr.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Streams 16 kHz mono PCM to Doubao ASR while the microphone is recording.
// Finish() sends the protocol's negative final sequence and returns the final transcript.
class DoubaoAsrClient : public StreamingAsr
{
  public:
    using TranscriptCallback = std::function<void(const std::string &)>;

    // legacy_auth selects the old console's App ID + Access Token headers. Otherwise the new
    // console's single API Key is sent as X-Api-Key and app_key is ignored.
    DoubaoAsrClient(std::string endpoint, bool legacy_auth, std::string app_key, std::string access_key,
                    std::string resource_id, bool enable_itn, bool enable_punc, bool enable_ddc,
                    std::string boosting_table_id, TranscriptCallback transcript_callback = {});
    ~DoubaoAsrClient() override;

    DoubaoAsrClient(const DoubaoAsrClient &) = delete;
    DoubaoAsrClient &operator=(const DoubaoAsrClient &) = delete;

    bool Start() override;
    void PushFloatSamples(const float *samples, std::size_t count) override;
    std::string Finish() override;
    std::string LastError() const override;
    void Cancel() override;

    static std::string TestCredentials(const std::string &endpoint, bool legacy_auth, const std::string &app_key,
                                       const std::string &access_key, const std::string &resource_id);

  private:
    void Run();

    std::string endpoint_;
    bool legacy_auth_ = false;
    std::string app_key_;
    std::string access_key_;
    std::string resource_id_;
    bool enable_itn_ = true;
    bool enable_punc_ = true;
    bool enable_ddc_ = false;
    std::string boosting_table_id_;
    TranscriptCallback transcript_callback_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> audio_queue_;
    bool finishing_ = false;
    bool canceled_ = false;
    bool started_ = false;
    std::thread worker_;
    std::string result_;
    std::string error_;
};
