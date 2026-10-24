#pragma once

#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/traditional_variant_choice_policy.h"
#include "ipc/traditional_variant_choice_store.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
/**
 * 繁体写法记录：日志库 traditional_variant_choices 表加它在内存里的副本，可以从多个线程同时调用。
 * 改动立即进副本；写库与读表都交给 post_to_writer 排到同一条串行线程上，按调用顺序执行。
 * user_db_path 在那条线程上执行任务时调用，给出日志库的路径；read_journal(path) 读出整张表，读不出时返回空，
 * 默认是 user_dictionary::traditional_variant_choices。
 */
class PersistentTraditionalVariantChoices
{
  public:
    using PostToWriter = std::function<void(std::function<void()>)>;
    using UserDbPath = std::function<std::string()>;
    using ReadJournal =
        std::function<std::optional<std::vector<user_dictionary::TraditionalVariantChoice>>(const std::string &)>;

    PersistentTraditionalVariantChoices(PostToWriter post_to_writer, UserDbPath user_db_path,
                                        ReadJournal read_journal = user_dictionary::traditional_variant_choices)
        : post_to_writer_(std::move(post_to_writer)), user_db_path_(std::move(user_db_path)),
          read_journal_(std::move(read_journal))
    {
    }

    /**
     * （key, word）记住的写法；没有记录时返回空。第一次调用时排一次读表，读完之前只查得到这个进程里
     * 新做的改动。读表失败时下一次调用重新排；排着的读表还没执行完时不再排第二个。
     */
    std::optional<std::string> RememberedSpelling(const std::string &key, const std::string &word)
    {
        EnsureLoading();
        return store_.Find(key, word);
    }

    /** 记住或删掉一条记录。字段不齐的改动（见 IsCompleteVariantChoiceChange）什么都不做。 */
    void Change(const VariantChoiceChange &change)
    {
        if (!IsCompleteVariantChoiceChange(change))
            return;
        std::lock_guard order(order_mutex_);
        store_.Apply(change);
        post_to_writer_([this, change] { WriteToJournal(change); });
    }

    /** 删掉底词 word 在所有键下的记录。word 为空时什么都不做。 */
    void ForgetWord(const std::string &word)
    {
        if (word.empty())
            return;
        std::lock_guard order(order_mutex_);
        store_.ForgetWord(word);
        post_to_writer_(
            [this, word] { (void)user_dictionary::forget_traditional_variants_of_word(user_db_path_(), word); });
    }

  private:
    // 副本的改动与写库的投递在同一把锁里，ClaimLoad 与读表的投递也是：ClaimLoad 之前的改动，写库一定排在
    // 读表前面，这正是 TraditionalVariantChoiceStore 要求调用方保证的顺序。
    void EnsureLoading()
    {
        std::lock_guard order(order_mutex_);
        if (!store_.ClaimLoad())
            return;
        post_to_writer_([this] {
            AbandonLoadUnlessLoaded outcome{store_};
            const auto rows = read_journal_(user_db_path_());
            if (!rows)
                return;
            store_.Load(*rows);
            outcome.loaded = true;
        });
    }

    // 读表任务不论怎样结束，Load 与 AbandonLoad 必走其一：任务中途抛异常时按读表失败处理，
    // 副本不会停在读表中（停在那里时 ClaimLoad 永远返回假，重放队列一直增长）。
    struct AbandonLoadUnlessLoaded
    {
        explicit AbandonLoadUnlessLoaded(TraditionalVariantChoiceStore &store_to_finish) : store(store_to_finish)
        {
        }
        AbandonLoadUnlessLoaded(const AbandonLoadUnlessLoaded &) = delete;
        AbandonLoadUnlessLoaded &operator=(const AbandonLoadUnlessLoaded &) = delete;
        ~AbandonLoadUnlessLoaded()
        {
            if (!loaded)
                store.AbandonLoad();
        }

        TraditionalVariantChoiceStore &store;
        bool loaded = false;
    };

    void WriteToJournal(const VariantChoiceChange &change) const
    {
        const std::string user_db_path = user_db_path_();
        if (change.action == VariantChoiceAction::Remember)
            (void)user_dictionary::remember_traditional_variant(user_db_path, change.key, change.word, change.spelling);
        else
            (void)user_dictionary::forget_traditional_variant(user_db_path, change.key, change.word);
    }

    PostToWriter post_to_writer_;
    UserDbPath user_db_path_;
    ReadJournal read_journal_;
    std::mutex order_mutex_;
    TraditionalVariantChoiceStore store_;
};
} // namespace FanyImeIpc
