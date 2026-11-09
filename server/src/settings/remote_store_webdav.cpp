#include "settings/remote_store_internal.h"

#include <Windows.h>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace user_data_sync
{
namespace
{
using detail::Attempt;
using detail::AttemptResult;
using detail::DescribeHttpFailure;
using detail::HeaderValue;
using detail::HttpRequest;
using detail::HttpResponse;
using detail::IsNotFound;
using detail::IsSuccess;
using detail::IsTransient;
using detail::JoinUrl;
using detail::Perform;
using detail::Retry;

std::string NormalizeBase(std::string base)
{
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base;
}

// Basic 认证头。用户名或密码都为空时不发，留给匿名 WebDAV。
std::string AuthorizationHeader(const WebDavOptions &options)
{
    if (options.username.empty() && options.password.empty())
        return {};
    const std::string credentials = options.username + ":" + options.password;
    static const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    std::size_t i = 0;
    for (; i + 2 < credentials.size(); i += 3)
    {
        const unsigned int value = (static_cast<unsigned char>(credentials[i]) << 16) |
                                   (static_cast<unsigned char>(credentials[i + 1]) << 8) |
                                   static_cast<unsigned char>(credentials[i + 2]);
        encoded.push_back(kBase64[(value >> 18) & 0x3F]);
        encoded.push_back(kBase64[(value >> 12) & 0x3F]);
        encoded.push_back(kBase64[(value >> 6) & 0x3F]);
        encoded.push_back(kBase64[value & 0x3F]);
    }
    if (i < credentials.size())
    {
        const std::size_t remaining = credentials.size() - i;
        unsigned int value = static_cast<unsigned char>(credentials[i]) << 16;
        if (remaining == 2)
            value |= static_cast<unsigned char>(credentials[i + 1]) << 8;
        encoded.push_back(kBase64[(value >> 18) & 0x3F]);
        encoded.push_back(kBase64[(value >> 12) & 0x3F]);
        encoded.push_back(remaining == 2 ? kBase64[(value >> 6) & 0x3F] : '=');
        encoded.push_back('=');
    }
    return "Authorization: Basic " + encoded;
}

std::vector<std::string> BaseHeaders(const WebDavOptions &options)
{
    std::vector<std::string> headers;
    const std::string authorization = AuthorizationHeader(options);
    if (!authorization.empty())
        headers.push_back(authorization);
    return headers;
}

// PROPFIND 的响应按命名空间写，元素前缀随服务端而变（D:response / d:response / response）。
// 这里按本地名匹配，不依赖前缀。
std::string LocalName(const std::string &tag)
{
    const std::size_t colon = tag.find(':');
    return colon == std::string::npos ? tag : tag.substr(colon + 1);
}

struct XmlNode
{
    std::string name; // 本地名
    std::string text;
    std::vector<XmlNode> children;
};

// 够用的 XML 解析器：只处理元素、文本和自闭合标签，跳过属性、注释、CDATA 与声明。
// WebDAV 的 PROPFIND 响应就这么长，用不上完整实现。
bool ParseXml(const std::string &xml, XmlNode &root)
{
    std::vector<XmlNode *> stack;
    root = XmlNode{};
    stack.push_back(&root);
    std::size_t position = 0;
    while (position < xml.size())
    {
        const std::size_t open = xml.find('<', position);
        if (open == std::string::npos)
        {
            stack.back()->text += xml.substr(position);
            break;
        }
        if (open > position)
            stack.back()->text += xml.substr(position, open - position);

        if (xml.compare(open, 4, "<!--") == 0)
        {
            const std::size_t end = xml.find("-->", open);
            if (end == std::string::npos)
                return false;
            position = end + 3;
            continue;
        }
        if (xml.compare(open, 9, "<![CDATA[") == 0)
        {
            const std::size_t end = xml.find("]]>", open);
            if (end == std::string::npos)
                return false;
            stack.back()->text += xml.substr(open + 9, end - open - 9);
            position = end + 3;
            continue;
        }
        if (xml.compare(open, 2, "<?") == 0)
        {
            const std::size_t end = xml.find("?>", open);
            if (end == std::string::npos)
                return false;
            position = end + 2;
            continue;
        }

        const std::size_t close = xml.find('>', open);
        if (close == std::string::npos)
            return false;
        std::string tag = xml.substr(open + 1, close - open - 1);
        if (!tag.empty() && tag.front() == '/')
        {
            // 结束标签。名字对不上说明结构坏了，但服务端偶尔会省略前缀，所以按本地名比对。
            const std::string name = LocalName(tag.substr(1));
            std::size_t match = stack.size();
            while (match > 1 && stack[match - 1]->name != name)
                --match;
            if (match > 1)
                stack.resize(match - 1);
            position = close + 1;
            continue;
        }

        const bool self_closing = !tag.empty() && tag.back() == '/';
        if (self_closing)
            tag.pop_back();
        const std::size_t space = tag.find_first_of(" \t\r\n");
        const std::string name = LocalName(space == std::string::npos ? tag : tag.substr(0, space));
        XmlNode node;
        node.name = name;
        stack.back()->children.push_back(std::move(node));
        if (!self_closing)
            stack.push_back(&stack.back()->children.back());
        position = close + 1;
    }
    return true;
}

const XmlNode *FindChild(const XmlNode &node, const std::string &name)
{
    for (const XmlNode &child : node.children)
    {
        if (child.name == name)
            return &child;
    }
    return nullptr;
}

const XmlNode *FindDescendant(const XmlNode &node, const std::string &name)
{
    if (node.name == name)
        return &node;
    for (const XmlNode &child : node.children)
    {
        if (const XmlNode *found = FindDescendant(child, name))
            return found;
    }
    return nullptr;
}

std::string Trimmed(const std::string &text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

int HexDigit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

// 服务端给的 href 可能是完整 URL、绝对路径或相对路径，末尾可能带斜杠，也可能做了
// 百分号编码。统一成"相对于 base_url 的 key"。
std::string HrefToKey(const std::string &href, const std::string &base_url)
{
    std::string path = href;
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos)
    {
        const std::size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? std::string() : path.substr(slash);
    }
    // base_url 自己的路径前缀要去掉。
    const std::size_t base_scheme = base_url.find("://");
    const std::size_t base_slash =
        base_scheme == std::string::npos ? std::string::npos : base_url.find('/', base_scheme + 3);
    const std::string base_path =
        base_slash == std::string::npos ? std::string() : NormalizeBase(base_url.substr(base_slash));
    if (!base_path.empty() && path.compare(0, base_path.size(), base_path) == 0)
        path = path.substr(base_path.size());
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    if (path.empty())
        return {};

    std::string decoded;
    decoded.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' && i + 2 < path.size())
        {
            const int high = HexDigit(path[i + 1]);
            const int low = HexDigit(path[i + 2]);
            if (high >= 0 && low >= 0)
            {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }
    return decoded;
}

std::int64_t ParseHttpDate(const std::string &text)
{
    // Sun, 06 Nov 1994 08:49:37 GMT
    std::tm utc{};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char month[4]{};
    if (std::sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6)
        return 0;
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month_index = -1;
    for (int i = 0; i < 12; ++i)
    {
        if (_stricmp(month, kMonths[i]) == 0)
        {
            month_index = i;
            break;
        }
    }
    if (month_index < 0)
        return 0;
    utc.tm_year = year - 1900;
    utc.tm_mon = month_index;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    return static_cast<std::int64_t>(_mkgmtime(&utc));
}

// 服务器时间常见 RFC 1123；有的实现回 ISO 8601，两种都认。
std::int64_t ParseServerTime(const std::string &text)
{
    if (text.empty())
        return 0;
    if (text.find("GMT") != std::string::npos)
        return ParseHttpDate(text);
    std::tm utc{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        return 0;
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    return static_cast<std::int64_t>(_mkgmtime(&utc));
}

class WebDavStore final : public RemoteStore
{
  public:
    explicit WebDavStore(WebDavOptions options) : options_(std::move(options))
    {
        options_.base_url = NormalizeBase(options_.base_url);
    }

    bool Put(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
             std::string &message) override
    {
        return Retry(
            [&](std::string &attempt_message) {
                HttpRequest request;
                request.method = "PUT";
                request.url = JoinUrl(options_.base_url, key);
                request.headers = BaseHeaders(options_);
                request.upload_file = file;
                request.timeout_seconds = 600;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = DescribeHttpFailure(response);
                return IsTransient(response.code) || response.status >= 500 ? AttemptResult::Retry
                                                                            : AttemptResult::Fatal;
            },
            message);
    }

    bool Get(const std::string &key, const std::filesystem::path &file, const ProgressCallback &progress,
             std::string &message) override
    {
        return Retry(
            [&](std::string &attempt_message) {
                HttpRequest request;
                request.method = "GET";
                request.url = JoinUrl(options_.base_url, key);
                request.headers = BaseHeaders(options_);
                request.download_file = file;
                request.timeout_seconds = 600;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = IsNotFound(response.status) ? "云端没有这个文件" : DescribeHttpFailure(response);
                return IsTransient(response.code) || response.status >= 500 ? AttemptResult::Retry
                                                                            : AttemptResult::Fatal;
            },
            message);
    }

    bool Stat(const std::string &key, RemoteObject &object, std::string &message) override
    {
        bool found = false;
        const bool ok = Retry(
            [&](std::string &attempt_message) {
                HttpRequest request;
                request.method = "HEAD";
                request.url = JoinUrl(options_.base_url, key);
                request.headers = BaseHeaders(options_);
                request.timeout_seconds = 60;
                request.capture_headers = true;
                const HttpResponse response = Perform(request);
                if (IsNotFound(response.status))
                {
                    found = false;
                    attempt_message.clear();
                    return AttemptResult::Ok;
                }
                if (IsSuccess(response.status))
                {
                    found = true;
                    object.key = key;
                    const std::string length = HeaderValue(response.headers, "content-length");
                    object.size =
                        length.empty() ? 0 : static_cast<std::uint64_t>(std::strtoull(length.c_str(), nullptr, 10));
                    object.etag = HeaderValue(response.headers, "etag");
                    object.modified_unix = ParseServerTime(HeaderValue(response.headers, "last-modified"));
                    return AttemptResult::Ok;
                }
                attempt_message = DescribeHttpFailure(response);
                return IsTransient(response.code) || response.status >= 500 ? AttemptResult::Retry
                                                                            : AttemptResult::Fatal;
            },
            message);
        return ok && found;
    }

    bool List(const std::string &prefix, std::vector<RemoteObject> &objects, std::string &message) override
    {
        objects.clear();
        // PROPFIND 的是 prefix 指向的目录本身，Depth: 1 取它的直接子项。
        // 调用方用 prefix 表示"目录"，和 S3 的前缀语义对齐：devices 取设备清单，
        // devices/<id> 取该设备的快照。
        std::string body;
        const std::string request_body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                         "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                                         "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/><D:getetag/>"
                                         "</D:prop></D:propfind>";
        const bool ok = Retry(
            [&](std::string &attempt_message) {
                HttpRequest request;
                request.method = "PROPFIND";
                request.url = JoinUrl(options_.base_url, prefix);
                request.headers = BaseHeaders(options_);
                request.headers.push_back("Depth: 1");
                request.headers.push_back("Content-Type: application/xml; charset=utf-8");
                request.body = request_body;
                request.timeout_seconds = 120;
                const HttpResponse response = Perform(request);
                if (!IsSuccess(response.status))
                {
                    attempt_message = DescribeHttpFailure(response);
                    return IsTransient(response.code) || response.status >= 500 ? AttemptResult::Retry
                                                                                : AttemptResult::Fatal;
                }
                body = std::move(response.body);
                return AttemptResult::Ok;
            },
            message);
        if (!ok)
            return false;

        XmlNode root;
        if (!ParseXml(body, root))
        {
            message = "云端返回的目录列表无法解析";
            return false;
        }
        const XmlNode *multistatus = FindDescendant(root, "multistatus");
        if (!multistatus)
        {
            message = "云端返回的不是 WebDAV 目录列表";
            return false;
        }
        // prefix 目录自己也会出现在响应里，必须按 key 精确跳过它，否则会被当成子项。
        std::string directory_key = prefix;
        while (!directory_key.empty() && directory_key.front() == '/')
            directory_key.erase(directory_key.begin());
        while (!directory_key.empty() && directory_key.back() == '/')
            directory_key.pop_back();
        const std::string directory_prefix = directory_key.empty() ? std::string() : directory_key + "/";

        for (const XmlNode &response : multistatus->children)
        {
            if (response.name != "response")
                continue;
            const XmlNode *href_node = FindChild(response, "href");
            if (!href_node)
                continue;
            const std::string key = HrefToKey(Trimmed(href_node->text), options_.base_url);
            if (key.empty() || key == directory_key)
                continue; // 目录自己
            // 只取这一层的直接子项，不递归进子目录。
            if (!directory_prefix.empty())
            {
                if (key.compare(0, directory_prefix.size(), directory_prefix) != 0)
                    continue;
                if (key.find('/', directory_prefix.size()) != std::string::npos)
                    continue;
            }
            else if (key.find('/') != std::string::npos)
            {
                continue;
            }
            const XmlNode *propstat = FindChild(response, "propstat");
            if (!propstat)
                continue;
            const XmlNode *prop = FindChild(*propstat, "prop");
            if (!prop)
                continue;
            // 目录项（resourcetype 里有 collection）不算文件。
            const XmlNode *resource_type = FindChild(*prop, "resourcetype");
            if (resource_type && FindChild(*resource_type, "collection"))
                continue;
            RemoteObject object;
            object.key = key;
            if (const XmlNode *length = FindChild(*prop, "getcontentlength"))
                object.size = static_cast<std::uint64_t>(std::strtoull(Trimmed(length->text).c_str(), nullptr, 10));
            if (const XmlNode *modified = FindChild(*prop, "getlastmodified"))
                object.modified_unix = ParseServerTime(Trimmed(modified->text));
            if (const XmlNode *etag = FindChild(*prop, "getetag"))
                object.etag = Trimmed(etag->text);
            objects.push_back(std::move(object));
        }
        return true;
    }

    bool Delete(const std::string &key, std::string &message) override
    {
        return Retry(
            [&](std::string &attempt_message) {
                HttpRequest request;
                request.method = "DELETE";
                request.url = JoinUrl(options_.base_url, key);
                request.headers = BaseHeaders(options_);
                request.timeout_seconds = 60;
                const HttpResponse response = Perform(request);
                // 不存在也算删掉了。
                if (IsSuccess(response.status) || IsNotFound(response.status))
                    return AttemptResult::Ok;
                attempt_message = DescribeHttpFailure(response);
                return IsTransient(response.code) || response.status >= 500 ? AttemptResult::Retry
                                                                            : AttemptResult::Fatal;
            },
            message);
    }

    bool Probe(std::string &message) override
    {
        // PROPFIND 根目录：不存在是 404，凭据错是 401/403，网络不通是 curl 错误。
        HttpRequest request;
        request.method = "PROPFIND";
        request.url = options_.base_url;
        request.headers = BaseHeaders(options_);
        request.headers.push_back("Depth: 0");
        request.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop></D:propfind>";
        request.timeout_seconds = 30;
        const HttpResponse response = Perform(request);
        if (IsSuccess(response.status))
            return true;
        if (response.status == 401 || response.status == 403)
        {
            message = "用户名或密码不正确，或这个账号没有访问该目录的权限";
            return false;
        }
        if (IsNotFound(response.status))
        {
            message = "备份目录不存在，请先在网盘里建好这个文件夹";
            return false;
        }
        message = DescribeHttpFailure(response);
        return false;
    }

  private:
    WebDavOptions options_;
};

} // namespace

std::unique_ptr<RemoteStore> MakeWebDavStore(WebDavOptions options)
{
    return std::make_unique<WebDavStore>(std::move(options));
}
} // namespace user_data_sync
