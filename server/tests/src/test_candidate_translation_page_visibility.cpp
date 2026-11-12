#include "ipc/translation_page_visibility.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

TEST_CASE(translation_page_visibility_only_sees_current_page_keys)
{
    const std::vector<std::string> page{"跑通", "炮筒"};
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page, {"跑通"}));
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page, {"炮筒", "跑通"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page, {"泡桐"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page, {}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage({}, {"跑通"}));
}
