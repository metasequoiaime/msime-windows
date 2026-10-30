#pragma once
// 神经整句重排的后台线程。
//
// 给十二条候选打一次分要几十毫秒——比一次按键的预算大一个数量级，所以它不能长在按键路径上。这里
// 把它挪开：查询照旧同步返回词格的静态顺序，同时把这一批候选交给后台线程；线程算完把顺序存进结
// 果表，再叫醒调用方，调用方丢掉候选缓存重查一次，这次就能在表里查到顺序了。第一次查询按原顺序
// 出，几十毫秒后原地重排——用户看到的是候选先出来、再自己理顺，而不是按键卡住。
//
// 「每个模型最新的赢」：用户还在打字时，同一模型前几次请求的结果已经没人要，所以新请求直接盖掉
// 该模型的旧待办。不同模型各有一个待办位和独立执行槽，速度优先不会排在效果优先后面等待。
//
// 正在算的那一批不再排第二份：回调一路走到重查，重查又会调 order_for，而前一批收尾到这次重查之间，
// 后台线程可能已经取走了刚排上的这一批。表里还没结果、待办位也空着，不拦的话同一批会再算一遍——
// 用户连续打字时每个回调都可能白算一次。
//
// 结果表按模型、前文、候选、静态分和重排参数做键。词库重排与整句重排共用模型，
// 同文本可能有不同的词频先验和批量上限，不能互相复用排序结果。

#include "neural_decoder.h"
#include "sentence_model.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace neural
{

class RescoreWorker
{
  public:
    using Ready = std::function<void()>;

    // 进程唯一，且故意不析构（见 .cpp）。
    static RescoreWorker &instance();

    // 把「算完了」的通知接出去，调用方据此重查。不设也行：结果照样算、照样进表，只是要等下一次
    // 按键才被取走。
    void set_ready_callback(Ready ready);

    // 有现成结果就返回；没有就把这一批排上队并返回空，调用方这一次照静态顺序走。
    std::optional<std::vector<std::size_t>> order_for(const SentenceModel &model, const std::string &context,
                                                      const std::vector<std::string> &sentences,
                                                      const std::vector<double> &static_scores,
                                                      const RerankOptions &options);

    // 上屏、换方案、换前文：存着的顺序对新语境没有意义了。
    void clear();

    // 停工，并等当前这一批算完。
    //
    // 必须有：进程退出时先跑静态析构、再终止别的线程，中间这一段里后台线程还活着，手上却握着已
    // 经析构掉的模型表和回调目标（回调一路通到服务端的任务队列）。第一次起线程时用 atexit 挂上
    // 它——atexit 是后注册先执行，而模型表、任务队列都是更早注册的，正好排在它后面。
    void shutdown();

  private:
    RescoreWorker() = default;
    ~RescoreWorker() = delete;
    RescoreWorker(const RescoreWorker &) = delete;
    RescoreWorker &operator=(const RescoreWorker &) = delete;

    struct Job
    {
        const SentenceModel *model = nullptr;
        std::string key;
        std::string context;
        std::vector<std::string> sentences;
        std::vector<double> static_scores;
        RerankOptions options;
    };

    void ensure_threads();
    void run();

    std::mutex mutex_;
    std::condition_variable wake_; // 有活干了，或者该收工了
    std::condition_variable idle_; // 手上这一批算完了
    bool threads_started_ = false;
    std::size_t busy_workers_ = 0;
    bool stopping_ = false;
    std::map<const SentenceModel *, Job> pending_;
    std::map<const SentenceModel *, std::string> active_keys_; // 每个模型正在算的那一批的键
    std::map<std::string, std::vector<std::size_t>> done_;
    Ready ready_;
};

} // namespace neural
