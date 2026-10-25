#include "utils/endpoint_host.h"

#include <string>

namespace NetworkProxy
{

bool IsLoopbackEndpoint(std::string_view endpoint)
{
    const size_t scheme = endpoint.find("://");
    if (scheme == std::string_view::npos)
        return false;
    std::string_view rest = endpoint.substr(scheme + 3);
    const size_t path = rest.find_first_of("/?#");
    if (path != std::string_view::npos)
        rest = rest.substr(0, path);
    // userinfo 里的 @ 只可能出现在 host 之前，取最后一个避免把密码里的 @ 当成分隔。
    const size_t at = rest.rfind('@');
    if (at != std::string_view::npos)
        rest = rest.substr(at + 1);

    std::string_view host;
    if (!rest.empty() && rest.front() == '[')
    {
        const size_t close = rest.find(']');
        if (close == std::string_view::npos)
            return false;
        host = rest.substr(1, close - 1);
    }
    else
    {
        const size_t colon = rest.find(':');
        host = rest.substr(0, colon == std::string_view::npos ? rest.size() : colon);
    }

    if (host == "127.0.0.1" || host == "::1")
        return true;
    if (host.size() != 9)
        return false;
    constexpr std::string_view kLocalhost = "localhost";
    for (size_t i = 0; i < kLocalhost.size(); ++i)
    {
        char ch = host[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
        if (ch != kLocalhost[i])
            return false;
    }
    return true;
}

} // namespace NetworkProxy
