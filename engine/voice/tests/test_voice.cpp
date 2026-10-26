#include <msime/voice/cloud_stt_worker.h>
#include <msime/voice/text_polisher.h>
#include <msime/voice/vad.h>
#include <msime/voice/wav_writer.h>
#include <msime/voice/provider_protocol.h>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <thread>
using namespace metasequoia::voice;
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void fails(F action)
{
    bool failed = false;
    try
    {
        action();
    }
    catch (const VoiceError &)
    {
        failed = true;
    }
    require(failed, "Expected VoiceError");
}
int main(int argc, char **argv)
{
    try
    {
        if (argc == 1)
        {
            const std::string binary = std::string("RIFF\0binary", 11) + "----MetasequoiaVoice0";
            const auto upload = make_transcription_request(binary, "fixture-model", "zh");
            const auto boundary = upload.content_type.substr(upload.content_type.find("boundary=") + 9);
            require(binary.find(boundary) == std::string::npos, "Multipart boundary collides with audio");
            require(upload.body.find(binary) != std::string::npos, "Binary audio must retain NUL bytes");
            require(upload.body.find("name=\"language\"\r\n\r\nzh") != std::string::npos, "Explicit recognition language");
            require(make_transcription_request(binary, "model").body.find("name=\"language\"") == std::string::npos, "Omit language for providers that reject it");
            fails([] { make_transcription_request({}, "model"); });
            fails([] { make_transcription_request("audio", {}); });
            fails([] { make_transcription_request(std::string(maximum_encoded_audio_bytes + 1, 'x'), "model"); });
            for (const auto *response : {R"({"text":"水杉"})", R"({"transcription":"水杉"})", R"({"result":{"text":"水杉"}})"})
                require(parse_transcription(response) == "水杉", "Platform transcription response variants");
            for (const auto *response : {"invalid", "[]", R"({"text":42})", R"({"text":"","result":null})"})
                fails([&] { parse_transcription(response); });
            fails([] { parse_transcription(std::string(1024 * 1024 + 1, 'x')); });
            {
                const auto chat = make_chat_transcription_request(binary, "fixture-model");
                require(chat.find("\"type\":\"input_audio\"") != std::string::npos, "Chat transcription carries input_audio");
                require(chat.find("\"format\":\"wav\"") != std::string::npos, "Chat transcription labels the wav format");
                require(chat.find("\"data\":\"UklGR") != std::string::npos, "Chat transcription base64-encodes the clip");
                require(chat.find("language") == std::string::npos, "Chat transcription sends no language field");
                fails([] { make_chat_transcription_request({}, "model"); });
                fails([] { make_chat_transcription_request("audio", {}); });
                fails([] { make_chat_transcription_request(std::string(maximum_encoded_audio_bytes + 1, 'x'), "model"); });
            }
            require(parse_transcription(R"({"choices":[{"message":{"role":"assistant","content":"水杉"}}]})") == "水杉",
                    "Chat completion transcription response");
            require(parse_transcription(R"({"text":"a","choices":[{"message":{"content":"b"}}]})") == "a",
                    "OpenAI transcription shape keeps priority");
            for (const auto *response : {"{}", R"({"choices":[]})", R"({"choices":[{"message":{"content":""}}]})"})
                fails([&] { parse_transcription(response); });
            require(parse_polished_text(R"({"choices":[{"message":{"content":"水杉"}}]})") == "水杉", "Polish response");
            for (const auto *response : {"{}", R"({"choices":[]})", R"({"choices":[{"message":{"content":null}}]})", R"({"choices":[{"message":{"content":""}}]})"})
                fails([&] { parse_polished_text(response); });
            const auto polish_body = make_polish_request("model", "prompt", "quoted \"line\"\ntext");
            require(polish_body.find("quoted \\\"line\\\"\\ntext") != std::string::npos, "Polish JSON escaping");

            const auto wav = WavWriter::create_wav({0, 1, -1, 2});
            require(wav.size() == 52 && wav[0] == 'R' && wav[24] == 0x80 && wav[25] == 0x3e, "WAV header");
            require(wav[46] == 0xff && wav[47] == 0x7f && wav[48] == 1 && wav[49] == 0x80, "little-endian PCM");
            fails([] { WavWriter::create_wav({std::numeric_limits<float>::quiet_NaN()}); });
            fails([] { WavWriter::create_wav({0}, 48000); });
            fails([] { WavWriter::create_wav(std::vector<float>(maximum_samples + 1)); });
            const std::vector<float> longer_clip(maximum_samples + sample_rate / 2, 0.125f);
            require(WavWriter::create_wav(longer_clip, sample_rate, longer_clip.size()).size() == longer_clip.size() * 2 + 44, "Host upload budget preserves padding and longer clips");
            fails([] { WavWriter::create_wav({0, 0}, sample_rate, 1); });
            VadSegmenter vad;
            require(!vad.process(nullptr, 0), "Empty VAD block");
            std::vector<float> silence(3200), speech(1600, 0.1f), tail(8000);
            vad.process(silence.data(), silence.size());
            vad.process(speech.data(), speech.size());
            vad.process(tail.data(), tail.size());
            require(vad.should_flush(), "Speech ending");
            require(vad.take_audio().size() == 12800, "Pre-roll and silence retained");
            require(vad.take_audio().empty() && !vad.should_flush(), "VAD reset");
            vad.process(speech.data(), speech.size());
            require(vad.take_audio().size() == speech.size(), "Second utterance isolated");
            return 0;
        }
        const std::string base = argv[1];
        auto options = [&](const std::string &path) { return RequestOptions{base + path, "fixture-model", "fixture-token", 3000, {}}; };
        const std::vector<float> pcm(160, 0.125f);
        std::cerr << "HTTP: transcription and error cases\n";
        CloudSttWorker asr(options("/asr"));
        require(asr.recognize(pcm) == "水杉 voice", "ASR UTF-8");
        require(asr.recognize({}).empty(), "Empty audio needs no network");
        for (const auto *path : {"/denied", "/invalid", "/missing", "/oversize", "/redirect"})
        {
            CloudSttWorker invalid(options(path));
            fails([&] { invalid.recognize(pcm); });
        }
        std::cerr << "HTTP: timeout and cancellation\n";
        auto timeout = options("/slow");
        timeout.timeout_ms = 50;
        CloudSttWorker slow(timeout);
        fails([&] { slow.recognize(pcm); });
        auto cancelled = options("/asr");
        cancelled.cancelled = std::make_shared<std::atomic_bool>(true);
        CloudSttWorker stopped(cancelled);
        fails([&] { stopped.recognize(pcm); });
        auto during = options("/slow");
        during.cancelled = std::make_shared<std::atomic_bool>(false);
        CloudSttWorker cancelling(during);
        auto pending = std::async(std::launch::async, [&] { fails([&] { cancelling.recognize(pcm); }); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        during.cancelled->store(true);
        pending.get();
        std::cerr << "HTTP: polish and fallback\n";
        TextPolisher polish(options("/polish"), "Clean transcription only");
        require(polish.polish("嗯 水杉") == "水杉", "Polished text");
        for (const auto *path : {"/denied", "/invalid", "/missing"})
        {
            TextPolisher fallback(options(path), "prompt");
            require(fallback.polish("原始文本") == "原始文本", "Preserve original on error");
        }
        TextPolisher fallback(timeout, "prompt");
        require(fallback.polish("原始文本") == "原始文本", "Preserve original on timeout");
        // An independently destroyed provider must not tear down another provider's CURL runtime.
        std::cerr << "HTTP: concurrent provider lifetimes\n";
        std::vector<std::future<void>> workers;
        for (int i = 0; i < 4; ++i)
            workers.push_back(std::async(std::launch::async, [&] {
                for (int n = 0; n < 5; ++n)
                {
                    CloudSttWorker temporary(options("/asr"));
                    require(temporary.recognize(pcm) == "水杉 voice", "Independent provider lifetime");
                }
            }));
        for (auto &worker : workers)
            worker.get();
        require(asr.recognize(pcm) == "水杉 voice", "Original provider still works");
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
