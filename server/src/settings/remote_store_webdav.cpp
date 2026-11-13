#include "settings/remote_store_internal.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
using detail::IsRetryableStatus;
using detail::IsSuccess;
using detail::IsTransient;
using detail::JoinUrl;
using detail::Perform;
using detail::Retry;
using detail::UrlRoot;

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
        // 文本节点要解实体：href 里的 &amp;、getetag 里的 &quot; 不解就和 HEAD 拿到的对不上。
        // 标签之间的一段文本不会把实体切开，所以逐段解码是安全的；CDATA 不解。
        if (open == std::string::npos)
        {
            stack.back()->text += detail::XmlDecodeEntities(xml.substr(position));
            break;
        }
        if (open > position)
            stack.back()->text += detail::XmlDecodeEntities(xml.substr(position, open - position));

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

// 取 URL 的路径部分（不含 scheme://authority、查询串与片段），解码成原文，去掉首尾斜杠。
std::string DecodedUrlPath(const std::string &url)
{
    std::string path = url;
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos)
    {
        const std::size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? std::string() : path.substr(slash);
    }
    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos)
        path.erase(query);
    path = detail::PercentDecode(path);
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

// 把 base_url 规范成"路径只编码一次"的写法。用户可能填原文（我的坚果云）也可能填
// 编码后的（%E6%88%91…），两种都先解码再统一编码，后面 JoinUrl 拼 key 就不会出现
// %25 这种二次编码。
std::string CanonicalBaseUrl(const std::string &url)
{
    const std::string root = UrlRoot(url);
    if (root.empty())
        return NormalizeBase(url);
    const std::string path = DecodedUrlPath(url);
    return path.empty() ? root : root + "/" + detail::UriEncode(path, false);
}

// PROPFIND 一个 response 里可能有多个 propstat（200 的一块、404 的一块），顺序不保证；
// 只认状态为 200 的那块，没写 status 的按 200 处理。
const XmlNode *SuccessfulProp(const XmlNode &response)
{
    for (const XmlNode &propstat : response.children)
    {
        if (propstat.name != "propstat")
            continue;
        const XmlNode *status = FindChild(propstat, "status");
        if (status && Trimmed(status->text).find(" 200 ") == std::string::npos)
            continue;
        if (const XmlNode *prop = FindChild(propstat, "prop"))
            return prop;
    }
    return nullptr;
}
} // namespace

namespace detail
{
std::string WebDavHrefToKey(const std::string &href, const std::string &base_url)
{
    // 两边都先解码再比：服务端回的 href 是编码过的（大小写也不定），base 可能是原文。
    std::string path = DecodedUrlPath(href);
    const std::string base_path = DecodedUrlPath(base_url);
    if (!base_path.empty())
    {
        if (path == base_path)
            return {};
        if (path.size() > base_path.size() && path.compare(0, base_path.size(), base_path) == 0 &&
            path[base_path.size()] == '/')
            path = path.substr(base_path.size() + 1);
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    return path;
}
} // namespace detail

namespace
{
using detail::WebDavHrefToKey;

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
        options_.base_url = CanonicalBaseUrl(options_.base_url);
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
                request.timeout_seconds = 0;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = DescribeHttpFailure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
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
                request.timeout_seconds = 0;
                const HttpResponse response = Perform(request, progress);
                if (IsSuccess(response.status))
                    return AttemptResult::Ok;
                attempt_message = IsNotFound(response.status) ? "云端没有这个文件" : DescribeHttpFailure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
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
                    // 不少 WebDAV 实现（坚果云就是）对 HEAD 回 Content-Length: 0，
                    // 那不是对象大小。HEAD 给不出正数时用 PROPFIND 补一次。
                    if (object.size == 0)
                        (void)FillFromPropfind(key, object, attempt_message);
                    return AttemptResult::Ok;
                }
                attempt_message = DescribeHttpFailure(response);
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
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
        bool empty = false;
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
                    // 目录还不存在时 PROPFIND 会 404。首次使用就是这种状态，按空列表处理，
                    // 不要把一串 HTML 抛给用户。
                    if (IsNotFound(response.status))
                    {
                        empty = true;
                        return AttemptResult::Ok;
                    }
                    attempt_message = DescribeHttpFailure(response);
                    return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
                                                                                            : AttemptResult::Fatal;
                }
                body = std::move(response.body);
                return AttemptResult::Ok;
            },
            message);
        if (!ok)
            return false;
        if (empty)
            return true;

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
            const std::string key = WebDavHrefToKey(Trimmed(href_node->text), options_.base_url);
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
            const XmlNode *prop = SuccessfulProp(response);
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
                return IsTransient(response.code) || IsRetryableStatus(response.status) ? AttemptResult::Retry
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

    // WebDAV 必须先建集合，否则对不存在集合的 PUT 返回 409。逐级 MKCOL，
    // 405 表示集合已经存在（RFC 4918 允许这种响应），按成功处理。
    //
    // base_url 自己也可能不存在（用户填了一个还没建的子目录）。这里从 base 的路径
    // 末段开始逐级建，缺哪级补哪级，而不是假设用户已经建好。
    bool EnsureDirectory(const std::string &key, std::string &message) override
    {
        std::string directory = key;
        const std::size_t slash = directory.rfind('/');
        if (slash == std::string::npos)
            directory.clear();
        else
            directory = directory.substr(0, slash);

        // 建 base_url 路径的末段（例如 /dav/msime-service-e2e 里的 msime-service-e2e），
        // 再建 key 里的每一段。base 前面的部分由服务端保证存在：/dav 是坚果云的根集合，
        // 对根集合发 MKCOL 会返回 403 OperationNotAllowed。
        // 路径一律用解码后的原文拼，最后由 JoinUrl 只编码一次；直接拿 base_url 里已编码的
        // 路径再进 JoinUrl 会变成 %25E6… 这种二次编码。
        std::vector<std::string> segments;
        std::string base_prefix;
        {
            const std::string base_path = DecodedUrlPath(options_.base_url);
            if (!base_path.empty())
            {
                // 只取最后一段：前面的路径属于服务端已有的集合层级。
                const std::size_t last = base_path.rfind('/');
                base_prefix = last == std::string::npos ? std::string() : base_path.substr(0, last + 1);
                segments.push_back(last == std::string::npos ? base_path : base_path.substr(last + 1));
            }
        }
        std::size_t position = 0;
        while (position < directory.size())
        {
            const std::size_t next = directory.find('/', position);
            const std::string segment =
                directory.substr(position, next == std::string::npos ? std::string::npos : next - position);
            if (!segment.empty())
                segments.push_back(segment);
            if (next == std::string::npos)
                break;
            position = next + 1;
        }

        std::string accumulated = base_prefix;
        std::string first_failure;
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            accumulated += segments[index];
            HttpRequest request;
            request.method = "MKCOL";
            request.url = JoinUrl(UrlRoot(options_.base_url), accumulated);
            request.headers = BaseHeaders(options_);
            request.timeout_seconds = 60;
            const HttpResponse response = Perform(request);
            // 201 新建、405 已存在。中间层级再放过 409（上级还没建，下一段会补上）与 403
            // （服务端已有的根集合不许 MKCOL，坚果云的 /dav 就是）。最后一级必须真的存在：
            // 它若是 403 / 409，后面的 PUT 一定失败，在这里报出来比让 PUT 回一个 409 好懂。
            const bool is_leaf = index + 1 == segments.size();
            const bool exists = IsSuccess(response.status) || response.status == 405;
            const bool tolerated = !is_leaf && (response.status == 409 || response.status == 403);
            if (!exists && !tolerated)
            {
                if (first_failure.empty())
                    first_failure = accumulated + "：" + DescribeHttpFailure(response);
            }
            accumulated += "/";
        }
        if (!first_failure.empty())
        {
            message = "无法创建备份目录：" + first_failure;
            return false;
        }
        return true;
    }

  private:
    // HEAD 不可靠时用 PROPFIND Depth: 0 取单个对象的元信息。失败不改变已找到的结论：
    // Stat 的语义是"存不存在"，大小与时间只是附带信息。
    bool FillFromPropfind(const std::string &key, RemoteObject &object, std::string &message)
    {
        HttpRequest request;
        request.method = "PROPFIND";
        request.url = JoinUrl(options_.base_url, key);
        request.headers = BaseHeaders(options_);
        request.headers.push_back("Depth: 0");
        request.headers.push_back("Content-Type: application/xml; charset=utf-8");
        request.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
                       "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/><D:getetag/>"
                       "</D:prop></D:propfind>";
        request.timeout_seconds = 60;
        const HttpResponse response = Perform(request);
        if (!IsSuccess(response.status))
        {
            message.clear();
            return false;
        }
        XmlNode root;
        if (!ParseXml(response.body, root))
        {
            message.clear();
            return false;
        }
        const XmlNode *multistatus = FindDescendant(root, "multistatus");
        if (!multistatus)
        {
            message.clear();
            return false;
        }
        for (const XmlNode &entry : multistatus->children)
        {
            if (entry.name != "response")
                continue;
            if (const XmlNode *prop = SuccessfulProp(entry))
            {
                if (const XmlNode *length = FindChild(*prop, "getcontentlength"))
                    object.size = static_cast<std::uint64_t>(std::strtoull(Trimmed(length->text).c_str(), nullptr, 10));
                if (const XmlNode *modified = FindChild(*prop, "getlastmodified"))
                    object.modified_unix = ParseServerTime(Trimmed(modified->text));
                if (const XmlNode *etag = FindChild(*prop, "getetag"))
                    object.etag = Trimmed(etag->text);
                message.clear();
                return true;
            }
        }
        message.clear();
        return false;
    }

    WebDavOptions options_;
};

} // namespace

std::unique_ptr<RemoteStore> MakeWebDavStore(WebDavOptions options)
{
    return std::make_unique<WebDavStore>(std::move(options));
}
} // namespace user_data_sync
