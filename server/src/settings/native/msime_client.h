#ifndef MSIME_CLIENT_H
#define MSIME_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#in
clude <stdint.h>
#ifdef __cplusplus
extern "C
" {
#endif

/* Host-neutral focus lease and k
ey event contract. Values are opaque to the A
BI. */
typedef struct msime_client_focus_leas
e {
  uint64_t client;
  uint64_t epoch;
  ui
nt64_t token;
} msime_client_focus_lease;

ty
pedef struct msime_client_key_event {
  msime
_client_focus_lease lease;
  uint32_t virtual
_key;
  uint32_t scan_code;
  uint32_t modifi
ers;
  uint32_t character;
  bool ui_less;
} 
msime_client_key_event;

/* Outcome for platf
orm key-router writes. Only definitely-not-se
nt may use a
 * local fallback; ambiguous del
ivery must wait for lease recovery. */
typede
f enum msime_client_key_dispatch_result {
  M
SIME_CLIENT_KEY_SENT = 0,
  MSIME_CLIENT_KEY_
DEFINITELY_NOT_SENT = 1,
  MSIME_CLIENT_KEY_D
ELIVERY_AMBIGUOUS = 2,
} msime_client_key_dis
patch_result;

static inline bool msime_clien
t_key_dispatch_allows_fallback(
    msime_cli
ent_key_dispatch_result result) {
  return re
sult == MSIME_CLIENT_KEY_DEFINITELY_NOT_SENT;

}

static inline bool msime_client_key_event
_valid(const msime_client_key_event *event) {

  return event != NULL && event->lease.clien
t != 0 && event->lease.epoch != 0 &&
        
 event->lease.token != 0 && event->virtual_ke
y <= 0xff &&
         (event->modifiers & ~UI
NT32_C(0x0f)) == 0;
}

/* ABI 3 (3 replaced m
sime_client_builtin_skins with the global the
me functions). All functions return owned, NU
L-terminated UTF-8 JSON. Free exactly once us
ing msime_client_string_free, including error
 responses. Never use free(). Responses: {"ok
":true,"value":...} or {"ok":false,"error":".
.."}. Creation returns a View; its session fi
eld is the handle. Handles are confined to th
eir creating thread. Dispatch, focus, view an
d destroy on that thread. Text and candidate 
values are copied; no Engine pointers escape.
 */
uint32_t msime_client_abi_version(void);

/* Worker-thread bootstrap: {resources: absol
ute path, state_root: absolute path}.
 * Veri
fies pinned resources, delegates working data
 preparation to Engine and
 * returns HostOpt
ions. Maximum 16384 bytes; no session may use
 state_root during
 * preparation. Caller pub
lishes the returned config atomically after s
uccess.
 * 可选的 edition 是版本 id（s
hared/contracts/editions.json），缺省为 
full；不是 full 时返回的 HostOptions �
�� edition 键，未知 id 返回错误。
 *
/
char *msime_client_prepare_host(const uint8
_t *options, size_t length);
/* path is an ab
solute UTF-8 runtime options file path of len
gth bytes; maximum 4096. When its dictionarie
s directory is not the installed resource gen
eration (after a package upgrade), prepares t
hat generation, replays the user dictionary i
nto it and atomically rewrites resources/dict
ionaries, keeping every other key. It also ke
eps language_dictionaries in step with the Ca
ntonese, Zhuyin and Stroke dictionaries insta
lled beside the resources, whatever the gener
ation; only an input method host may call it,
 because a host older than this library rejec
ts that key. Value is true when the file was 
rewritten. Call before creating any session f
rom the file. When the recorded resource dire
ctory does not match the compiled dictionary 
lock (downloaded dictionaries an upgrade did 
not replace) the error text begins with "dict
ionary_outdated:" and the file is left unchan
ged; the rest of that text may name private p
aths. */
char *msime_client_refresh_host(cons
t uint8_t *path, size_t length);
/* 宿主注
入的会话存储（ABI 不变，仍是 3，
只新增）。钥匙串、App Group、Androi
dKeyStore 这类只有平台能读写的地�
�由宿主经下面的回调提供字节和�
�进程锁，编解码、布局、合法性�
�验和刷新时的加锁顺序都在 Rust �
�。注册以后，msime_client_account、msi
me_client_community、msime_client_account_cl
oud_clipboard、msime_client_account_access_t
oken、msime_client_cloud_dictionary、msime_
client_ai_skin_generate、msime_client_ai_ski
n_job 和 msime_client_cloud_sync 的请求�
� {session_store: id} 代替 {directory}；�
�者恰好给一个，legacy_directory 只能
与 directory 一起给，都给、都不给�
�� id 没有注册时是该入口的「请求
不合法」码（account_invalid、community
_invalid、invalid_cloud_clipboard、invalid_
cloud_dictionary、ai_skin_invalid、cloud_sy
nc_invalid）。带 directory 的请求与以
前完全相同，Windows 和 Linux 不需要
改；msime_client_account 和 msime_client_a
ccount_chat_stream 带 directory 时还可以
给 client_agent（见 msime_client_account�
�，其余入口的目录模式仍发 MSIME/A
ndroid。 */
#define MSIME_CLIENT_SESSION_STO
RE_VERSION 1
/* flags：钥匙串、App Group
 这类多个进程读写同一份会话的�
�储必须置位。置位时每次访问都�
�新读，刷新、登录、退出、改资�
�和补登录 ID 时持 lock(item)；不置�
�时会话读一次后缓存在进程里，lo
ck/unlock 可以为 NULL。 */
#define MSIME_
CLIENT_SESSION_STORE_SHARED 1u
typedef enum m
sime_client_session_item {
  MSIME_CLIENT_SES
SION_ITEM_ACCOUNT = 1,            /* 登录�
�话；也是 lock/unlock 的一个 item */
 
 MSIME_CLIENT_SESSION_ITEM_CLIPBOARD_KEYSTORE
 = 2, /* 云剪贴板密钥串，UTF-8，不�
��换行；属于 ACCOUNT 槽，清掉登录�
��话时一起删除 */
  MSIME_CLIENT_SESSIO
N_ITEM_ANONYMOUS = 3,          /* 本机匿�
�账号的会话；也是 lock/unlock 的一�
�� item */
  MSIME_CLIENT_SESSION_ITEM_ANONYM
OUS_IDENTITY = 4, /* 匿名账号的身份 {s
ubject,secret}；save 是「不存在才写�
� */
} msime_client_session_item;
/* 64 位�
�标上 80 字节：version@0、layout@4、fl
ags@8、（填充 4 字节）、client_agent@
16、context@24、load@32、save@40、remove@
48、lock@56、unlock@64、release@72。 */
t
ypedef struct msime_client_session_store {
  
uint32_t version;         /* MSIME_CLIENT_SES
SION_STORE_VERSION */
  uint32_t layout;     
     /* 0 = native SavedAccountSession；1 = 
apple BackendSavedSession（expiresAt 为 200
1 年起的秒数，含 sessionID）。读取
时两种都接受，写入按这里选的布
局 */
  uint32_t flags;           /* MSIME_C
LIENT_SESSION_STORE_SHARED 或 0；其他位�
��须为 0 */
  const char *client_agent; /* 
可为 NULL，此时 User-Agent 与不带 cli
ent_agent 的目录模式相同（MSIME/Andro
id）；否则是以 NUL 结尾、不超过 2
56 字节的可打印 ASCII，形状为 msime
-<android|ios|macos|windows|linux|harmony>/<�
��本，1 到 32 个 [0-9A-Za-z.+_-]> (<设�
�>; <系统>; edition=<id>)，括号部分可
省略、里面不能有括号；注册时复
制，之后不再读 */
  void *context;    
        /* 原样交回每个回调；注册�
��功后要一直有效到 release 被调用 
*/
  intptr_t (*load)(void *context, uint32_t
 item, uint8_t *buffer, size_t capacity); /* 
把该项写进 buffer：>0 是写入的字�
�数，0 是不存在，<0 是失败；大于
 capacity 按失败处理 */
  int32_t (*save
)(void *context, uint32_t item, const uint8_t
 *bytes, size_t length); /* 0 是已写入；
ANONYMOUS_IDENTITY 已存在时不覆盖并�
�回 1；其他值是失败 */
  int32_t (*re
move)(void *context, uint32_t item); /* 0 是
成功，不存在也算成功；其他值是
失败 */
  int32_t (*lock)(void *context, ui
nt32_t item, uint32_t timeout_ms, uint64_t *t
oken); /* item 只会是 ACCOUNT 或 ANONYMOU
S，timeout_ms 是 20000；0 是已持有（�
��令牌写进 *token），1 是超时，其�
��值是失败。未置 SHARED 时可为 NULL
 */
  void (*unlock)(void *context, uint32_t 
item, uint64_t token); /* 释放 lock 拿到�
��锁。未置 SHARED 时可为 NULL */
  voi
d (*release)(void *context); /* 可为 NULL�
�只在注册成功后、最后一次使用�
�束时调用恰好一次 */
} msime_client_s
ession_store;
/* 回调的参数与内存：R
ust 给的 capacity 就是该项的上限，�
��个会话项和身份项是 65536，密钥�
��是 4096。buffer 和 bytes 只在本次调
用期间有效，不要保留。 */
/* 读�
�语义：save 对 ACCOUNT、CLIPBOARD_KEYSTO
RE、ANONYMOUS 是原子整体替换，其他
进程只能看到旧值或新值；对 ANONY
MOUS_IDENTITY 是「不存在才写」，已�
��有值时不覆盖、返回 1，Rust 随即
重新 load 并采用读到的那份（尚未
迁移的进程可能在锁外发布身份，
原子覆盖会造出两个身份和一个孤
儿账号）。load 必须原样返回最后�
��次成功 save 写入的字节；置了 SHA
RED 时「最后一次」指任何进程的�
�后一次，Android 的 SharedPreferences �
�进程不满足这一条，不能被两个�
�程当作同一个 store。编解码、布�
�和合法性都由 Rust 校验，解码失�
�、超过上限或不合法是 account_stora
ge。 */
/* 线程：load、save、remove、l
ock、unlock、release 可能在任意线程�
��被并发调用，包括 Rust 自己起的�
��程。JNI 宿主要缓存 JavaVM，每次�
�调先 GetEnv，当前线程没有 attach �
� AttachCurrentThread 并在本次回调返�
�前 DetachCurrentThread，返回前检查并
清掉挂起的 Java 异常、返回失败码
；不能直接用调用 msime_client_* 的�
�个线程的 env。Apple 宿主的回调是�
��步调用，不能 hop 到 MainActor，也�
��能 DispatchQueue.main.sync。每次调宿�
��的 lock 最多等 20 秒，持锁期间最
多一个 30 秒以内的 HTTP；但同一进
程里排在别的线程那次刷新后面的
调用要先等 Rust 进程内的锁，这一
段没有超时，前面每个线程的 20 �
�加 30 秒都会累加上来。所以所有�
��过 store 的 msime_client_* 调用都不�
�在主线程上调用。回调里不能重�
�任何 msime_client_*，也不能 unwind：C
++ 异常自己 catch，Swift 不能抛出。
AI 皮肤的出图线程遇到 401 时会刷�
��令牌，所以 msime_client_ai_skin_genera
te 进行中这些回调也会在 Rust 的线
程上发生。 */
/* 锁：lock(item) 只需
要保证跨进程互斥（本进程里的并
发由 Rust 自己的锁排斥），但必须
用 flock(2)，每次加锁新开一个 fd �
� flock(LOCK_EX)，这样同一进程里尚�
�迁移、同样用 flock 的代码也被排�
��；不能用 fcntl 或 lockf（按进程计
锁，同进程第二个 fd 直接成功，�
�任意一个 fd 会释放本进程在该文�
��上的所有锁），也不能用 Java 的 
FileChannel.lock（底层是 fcntl，与 floc
k 互不可见）。宿主按 item 选锁文�
��。Rust 的加锁顺序固定为：进程�
�按 (store, item) 的锁 → lock(item) → 
Rust 会话锁 → load/save/remove；Rust �
�持会话锁、不持宿主锁时也会调 l
oad。所以 load、save、remove 不得等�
�刷新锁，也不得等待 lock 回调在�
�待期间持有的任何宿主锁（例如 A
ndroid 不能用同一个 monitor 既实现 l
ock 又包住 load）。Rust 只在一次刷�
��、登录、退出、改资料或补登录 
ID 期间持宿主锁，其中最多一个 HT
TP 请求（30 秒以内）；持锁期间不
会对同一个 store 的任何 item 再加�
�，同一线程嵌套加锁时直接返回 a
ccount_unavailable。每次加锁成功后，
Rust 在同一线程上用同一个 item 和 
token 调用 unlock 恰好一次，出错或 
panic 时也一样。iOS 上挂起时还持�
� App Group 文件锁的进程会被系统杀
掉，宿主要在持锁期间保持后台执
行：App 里用 UIApplication.beginBackgroun
dTask，键盘扩展里改用 ProcessInfo.per
formExpiringActivity。 */
/* 共存：同一
份会话在新旧实现共存期间必须共
用同一把锁。凡是不经锁回调就能
刷新同一份会话的尚未迁移代码（
例如 Android 的 FutureTask 单飞），必
须和 Rust 在同一次迁移里一起切换
，不能共存。 */
/* 注册：store 不�
�为 NULL。version 不是 1、load/save/remo
ve 任一为 NULL、layout 不是 0 或 1、f
lags 有未知位、置了 SHARED 却没有 l
ock 或 unlock、client_agent 不合法时 er
ror 是 invalid_session_store；HTTP 客户�
�建不出来时是 account_unavailable。这
两种情况下 Rust 都没有接管 context�
��也不会调用 release。成功时结构�
�被复制，Value 是 {session_store: id}，
id 是小于 2^53 的正整数。每个 store
 在进程里有一个登录会话、一个�
�名会话和一组社区服务，共用一�
� HTTP 客户端；同一个 store 的 google
_login 和 google_cancel 作用在同一个�
�话对象上。同一份存储在一个进�
�里只注册一次：每次注册各有一�
�进程内的锁和会话缓存，两次注�
�同一份不置 SHARED 的存储（例如同
一个 Android SharedPreferences）会各自�
��存，可能拿同一个会轮换的刷新�
��牌各刷新一次，服务端随即吊销�
��个会话。不联网。 */
char *msime_cli
ent_session_store_register(const msime_client
_session_store *store);
/* 注销 msime_clien
t_session_store_register 给的 id，Value �
�它是否曾经注册。之后的新请求�
�到的是「请求不合法」码；正在�
�行的调用照常结束，最后一个结�
�时调用 release。release 可能在任意�
��程上，也可能就在本函数或某次 
msime_client_* 调用返回之前、在同一
线程上同步执行，所以它不能重入
 msime_client_*，也不能去拿宿主调用
本函数时手里持有的锁；Rust 调用 
release 时不持有任何 Rust 锁。注销�
��会先结束该 store 上进行中的 googl
e_login。不联网。 */
char *msime_client_
session_store_unregister(uint64_t session_sto
re);
/* directory is an absolute UTF-8 direct
ory path of length bytes; maximum 4096. Regis
ters the device's anonymous MSIME account at 
https://api.msime.app unless anonymous-sessio
n.json already exists there, keeping the iden
tity in anonymous-account.json and the sessio
n in anonymous-session.json (both owner-only)
. Blocks on the network: call off the input t
hread. Value is true once a session exists. A
 failure leaves the identity for the next cal
l, so call again on a later start. */
char *m
sime_client_ensure_anonymous_account(const ui
nt8_t *directory, size_t length);
/* 与 msim
e_client_ensure_anonymous_account 相同，�
�是宿主可以自报注册请求的 User-Ag
ent（#8201 的决定 1，服务端「我的�
��备」据此显示平台和设备）：requ
est 是 length 字节的 UTF-8 JSON {"directo
ry": 绝对路径, "client_agent": 可选}，
最多 32768 字节，directory 最多 16384 
字节。client_agent 的形状与 msime_clie
nt_account 的 client_agent 相同（msime-<�
��台>/<版本> (<设备>; <系统>; edition=
<id>)），不合法时是 account_invalid、
不碰目录；不给时发出的请求与 ms
ime_client_ensure_anonymous_account 相同（
MSIME/Android）。已有会话时只读一�
�会话文件、不联网。Value 是 true；
失败时 error 是账号错误码（与 msim
e_client_account 的 ensure_anonymous 相同�
��，不是旧入口那样的说明文字。�
��入口不变。 */
char *msime_client_ensur
e_anonymous_account_with_agent(const uint8_t 
*request, size_t length);
/* 拼出宿主自�
��的 User-Agent（#8201 迁移计划 P11）�
��各宿主不再各写一份：request 是 l
ength 字节（最多 16384）的 UTF-8 JSON 
{platform, version, device?, system?, edition
}，都是字符串，device 和 system 可�
�不给或为 null（与空串相同）。Val
ue 是 msime-<platform>/<version> (<device>; 
<system>; edition=<edition>)，总能当 msim
e_client_account、msime_client_account_chat_
stream、msime_client_ensure_anonymous_accoun
t_with_agent 和 msime_client_session_store �
�� client_agent 用。platform 必须是 andr
oid、ios、macos、windows、linux、harmony
 之一（区分大小写）；device 是设�
��型号（Android 的 Build.MODEL），syste
m 是系统名加版本（Android 传 "Androi
d " 加 Build.VERSION.RELEASE），edition �
� shared/contracts/editions.json 的版本 ID
。device、system 和 edition 只清洗、�
�拒绝：去掉首尾空白和控制字符�
�去掉不是可打印 ASCII 的字符和 ( )
 ;，保留 64 个字符后再去一次首尾
空白，为空时 device 和 system 是 unkn
own、edition 是 full；version 只保留 [0
-9A-Za-z.+_-]、最多 32 个，为空时是 
0。Android 照上面传参时与原来的 Ba
ckendAccount.loginUserAgent 逐字相同，�
�有三处不同：version 里有别的字符
或超过 32 个（原来那样的串服务�
�认不出），Build.VERSION.RELEASE 为空�
��原来是 Android unknown，这里是 Andro
id），以及 RELEASE 去掉字符后开头�
��下空白、或与 "Android " 合起来超�
�� 64 个字符（原来 RELEASE 单独清洗
、单独截断）。用例在 shared/contrac
ts/client-agent/cases.json。platform 不认�
��、JSON 不合法、缺字段、有多余�
�段、超过上限或 request 为 NULL 时 e
rror 是 account_invalid。不联网、不读
文件，可以在任意线程上调用。 */

char *msime_client_client_agent(const uint8_
t *request, size_t length);
/* 输入法进�
�取账号服务的访问令牌。request 是
 length 字节的 UTF-8 JSON {"directory": �
�对路径, "rejected_token": 可选}，最�
� 16384 字节；directory 是设置应用和
输入法共用的账号目录（Windows 是 
%LOCALAPPDATA%\<本版本的用户目录>\acc
ount，与 msime_client_ensure_anonymous_acco
unt 相同）。设置应用登录的 account
-session.json 优先，刷新时与设置应�
��共用 account-refresh.lock；没有登录�
��用 anonymous-session.json。rejected_token
 是服务端刚以 401 拒绝的令牌，传
入后强制刷新。每次调用都重新读
文件，登录、退出和换号下一次调
用即生效。也可以用 {"session_store":
 msime_client_session_store_register 给的 i
d, "rejected_token": 可选} 代替 directory
：登录会话和匿名会话都在宿主注
入的存储里，同样登录的优先，刷
新时持宿主的 lock(ACCOUNT) 或 lock(ANO
NYMOUS)。Value {access_token, user_id, anony
mous}；没有任何会话时 error 是 accou
nt_unauthorized。响应含凭据，不要写
进日志。刷新会阻塞网络：不要在
输入线程调用。 */
char *msime_client_a
ccount_access_token(const uint8_t *request, s
ize_t length);
/* 设置窗口自己的云剪
贴板请求，与云剪贴板面板发的相
同。request 是 length 字节（最多 1310
72）的 UTF-8 JSON {directory: 账号目录�
��绝对路径, device_name?, action}；direc
tory 是共享应用登录时写 account-sess
ion.json 的目录（Windows 是 %LOCALAPPDAT
A%\<本版本的用户目录>\account），�
�用登录的会话，不退回匿名账号�
�device_name 是可选的、端到端加密�
�记本机时给其他设备看的名称（�
�如鸿蒙的 marketName），规则与 msime
_client_relay_cloud_clipboard 的相同：不
给、为 null、只有空白、超过 256 �
�节或含控制字符时改用计算机名�
�平台名，不算请求不合法，登记�
�驱动还会再清理一遍；只在本机�
�没登记时用到。也可以用 {session_s
tore: id, device_name?, action} 代替 direct
ory，id 来自 msime_client_session_store_re
gister，用该 store 的登录会话（注�
�时建好的那一个，不每次新建）�
�两者都给、都不给或 id 没有注册�
�� error 是 invalid_cloud_clipboard。action
 是下面之一：{operation:"list",search}�
��search 不超过 1024 字节、不含控制
字符，在本机解密后匹配）→ {enab
led, retention_days, items:[{id,text,updated_
at,pinned,device}], e2e:{status,code,pending_
devices,undecryptable}}；{operation:"add",te
xt}（非空、不超过 4000 个 UTF-16 单�
��、不含控制字符，换行除外）→ 
新条目；{operation:"delete",id}（条目�
�� id，64 位小写十六进制；不超过 
256 字节的其他 ASCII 标识符不发出�
��求，error 是 account_invalid）→ {}；
{operation:"set_enabled",enabled} → {enable
d}；{operation:"pin",id,pinned}（id 是 64 
位小写十六进制，pinned 是布尔）�
� {}；{operation:"set_retention",days}（day
s 是 0、1、7、30 之一，0 表示一直�
��留）→ {retention_days}；{operation:"cl
ear"} 删掉云端全部条目、开关不变
 → {}；端到端加密的设备管理 {ope
ration:"devices"}、{operation:"approve"|"rej
ect"|"remove",device_id}（device_id 是 32 �
��小写十六进制）、{operation:"reset"}
 → 驱动 status 的结果 {enabled,status,
device_id,code,devices:[{device_id,name,creat
ed_at,approved,current,code}]}（见 docs/clo
ud-clipboard-e2e.md 的「共享面板的动�
��」）。action 不合法时 error 是 inva
lid_cloud_clipboard，不联网；没有登�
�或会话已失效时是 account_unauthorize
d，驱动给出的码原样交回（例如 c
lipboard_device_pending、clipboard_sync_disa
bled），其他失败是相应的 account_* 
代码（置顶的条目已经不在时服务
端回 404，是 account_unavailable）。按
 directory 时刷新令牌与共享应用共�
�� account-refresh.lock；按 session_store �
��持宿主的 lock(ACCOUNT)。阻塞网络�
�放在工作线程上调用。 */
char *msim
e_client_account_cloud_clipboard(const uint8_
t *request, size_t length);
/* 云剪贴板�
�走哪条路。directory 是 length 字节�
�最多 16384）的共享偏好目录绝对�
�径。偏好里的 cloud_clipboard_relay 配
置了中继时 Value 是 {source:"relay"}，
否则是 {source:"account"}；配置了中�
�就只走中继，不再用水杉账号。�
�好读不了时 error 是 storage，中继�
�置不合规时是 relay_not_configured。�
�联网。 */
char *msime_client_cloud_clipbo
ard_source(const uint8_t *directory, size_t l
ength);
/* 经自建中继（协议见 docs/c
loud-clipboard-relay.md）答一个云剪贴�
��动作。request 是 length 字节（最多
 131072）的 UTF-8 JSON {preferences_directo
ry: 共享偏好目录的绝对路径, direct
ory: 本机放中继密钥串 cloud-clipboard
-relay-keystore 的绝对路径目录, device
_name: 可选，登记设备时给其他设�
�看的名称, action}。中继地址和令�
�从偏好的 cloud_clipboard_relay 读，没
有配置时 error 是 relay_not_configured�
�directory 在同一台设备上的输入法�
��设置和面板之间要相同，否则会�
��记成几台设备；密钥串退出水杉�
��号时不删除。action 与 msime_client_a
ccount_cloud_clipboard 的相同，两个入�
��接受同样的十二个动作，结果形�
��也相同：list、add、delete、set_enabl
ed、pin、set_retention、clear，以及端�
��端加密的 devices、approve、reject、r
emove、reset（各自的字段和结果见 m
sime_client_account_cloud_clipboard）。acti
on 不合法时 error 是 invalid_cloud_clipb
oard；中继拒绝令牌时是 relay_unautho
rized，连不上是 relay_unavailable，密�
��串读写失败是 relay_storage，其余�
�驱动给出的码（例如 clipboard_device
_pending）。阻塞网络：放在工作线�
��上调用。 */
char *msime_client_relay_cl
oud_clipboard(const uint8_t *request, size_t 
length);
/* 向自建中继发端到端加密
驱动（msime_client_cloud_clipboard_e2e）�
��出的一个请求，给自己跑驱动、�
��己保存密钥串的宿主用：Android �
�中继的密钥串交给 AndroidKeyStore 加
密，不经上面那个由 Rust 把密钥串
存成明文文件的入口跑驱动。reques
t 是 length 字节（最多 131072）的 UTF
-8 JSON {url: 中继基地址, token: 中继�
��牌, method: GET|PUT|POST|DELETE, path: 驱
动给的路径（必须在 /v1/users/me/clip
board 之下）, body: 可选，驱动给的 
JSON 正文文本}。url 和 token 是宿主�
��这次操作开始时从偏好的 cloud_cli
pboard_relay 读到的那一份，整次操�
�都传同一份，中途改了偏好也不�
�发到另一个中继；校验规则与偏�
�相同，不合规时 error 是 relay_not_co
nfigured。Value 是 {status, body}，状态�
��和正文原样交回驱动。不读偏好�
��不读写任何密钥串。请求不合规�
�� error 是 invalid_cloud_clipboard（不发
出去），中继拒绝令牌时是 relay_un
authorized，连不上或响应过大是 rela
y_unavailable。阻塞网络：放在工作�
�程上调用。 */
char *msime_client_relay_
cloud_clipboard_exchange(const uint8_t *reque
st, size_t length);
/* Anonymous usage report
ing to https://api.msime.app/v1/telemetry/eve
nts (no credentials). Every request is UTF-8 
JSON of length bytes; maximum 16 KiB (256 KiB
 for record_crash). directory is an absolute 
directory the host owns for its telemetry fil
es (the queue telemetry.json, telemetry-state
.json with the random install id, the session
 marker and telemetry-crashes/). platform is 
windows, macos, linux, android, ios or harmon
y (aliases such as win, darwin, ohos are mapp
ed); version is the real app version. Consent
: enabled (the usage_reporting switch, defaul
t on) or, for hosts on the shared preferences
, preferences_directory to read usage_reporti
ng from; with reporting off, begin and flush 
clear everything and send nothing.
 * begin {
directory, platform, version, enabled?|prefer
ences_directory?}: call once at host start. C
loses the previous session (session_crash onl
y when it left a crash record; a leftover mar
ker alone is no crash), queues crash records 
as crash events with paths reduced to file na
mes, queues today's active, writes a new mark
er. Value {enabled, crash_record_path?, previ
ous_session_crashed?, crashes?}. No network.

 * end {directory}: the host is exiting norma
lly; queues the session event. Value true whe
n a session was running. No network.
 * recor
d_crash {directory, message, stack}: from a c
rash handler that may allocate (C++ terminate
 handler). Writes the session's crash record 
only; first line of message is the summary, s
tack is frames as module+offset or symbol. Va
lue false when no session runs or a record ex
ists. An async-signal handler instead writes 
crash_record_path from begin itself: open(O_W
RONLY|O_CREAT|O_EXCL, 0600), the summary line
, '\n', the frames.
 * flush (same request as
 begin): queues today's active and sends the 
queue oldest first; 400 drops an event, 429/5
xx/network keep it (Retry-After honoured acro
ss processes). Value {enabled, sent, dropped,
 remaining, deferred}. Blocks on the network:
 background thread only.
 * clear {directory}
: usage reporting was just turned off. */
cha
r *msime_client_telemetry_begin(const uint8_t
 *request, size_t length);
char *msime_client
_telemetry_end(const uint8_t *request, size_t
 length);
char *msime_client_telemetry_record
_crash(const uint8_t *request, size_t length)
;
char *msime_client_telemetry_flush(const ui
nt8_t *request, size_t length);
char *msime_c
lient_telemetry_clear(const uint8_t *request,
 size_t length);
/* Notices from GET https://
api.msime.app/v1/notices (no credentials). re
quest {directory, platform, channel?} (channe
l app by default); directory is an absolute d
irectory for notices.json. Value {items:[{id,
title,body,html,targets,channels,published_at
}]}, newest first, without the ones dismissed
; html is body rendered as by msime_client_ma
rkdown_to_html. The feed is fetched at most o
nce a minute (the cached copy otherwise, and 
when the request fails). Blocks on the networ
k: call off the input thread when the setting
s window or app home opens, never on a timer 
in the input method. */
char *msime_client_no
tices(const uint8_t *request, size_t length);

/* request {directory, id}: remember that th
e user dismissed notice id. */
char *msime_cl
ient_notice_dismiss(const uint8_t *request, s
ize_t length);
/* 对照 github.com/metasequo
iaime/msime 的发布列表检查更新（不
带凭据）。request 为 {platform, current
_version, edition?, arch?}：platform 是发�
�� tag 前缀（windows、linux、macos、har
mony 等），current_version 是当前运行
的版本号，edition 是版本 id（缺省�
�� full），arch 是 Rust 的架构名。返
回值为 {status:"available"|"current", upda
te:{version:{display,parts}, release_url, ins
taller_name, installer_sha256, signed}}，该
平台还没有发布时为 {status:"none"}�
�在 linux 和 windows 上 installer_name 和
 installer_sha256 是本版本唯一的安装
包（没有、有不止一个、或 GitHub �
��给摘要时为 null）；signed 在这两�
��平台为 false，其他平台为 null。�
�在网络上阻塞最多十秒，不要在 U
I 线程或输入线程上调用。 */
char *
msime_client_update_check(const uint8_t *requ
est, size_t length);
/* text is UTF-8 Markdow
n of length bytes; maximum 256 KiB. Value is 
HTML for a rich-text view: raw HTML in the so
urce is escaped, only http, https and mailto 
links are kept, images become links and are n
ever loaded. Open links externally. */
char *
msime_client_markdown_to_html(const uint8_t *
text, size_t length);
/* Community moderation
 for hosts that send community requests throu
gh their own HTTP stack. Request is UTF-8 JSO
N of length bytes; maximum 64 KiB. Publishing
 is post-moderated: an item is public at once
 and moderators may remove it. To learn the s
tate of the user's own items add fields=moder
ation to scope=mine lists and to the detail r
equest (skins, candidate-skins, plugins, reso
urces); each own item then carries moderation
 "approved"|"pending"|"removed". Show only a 
已下架 badge for removed and treat pending
 as approved; never show a reason. Without fi
elds=moderation the responses are unchanged.

 * {operation:"reasons"}: value is the report
 reasons in dialog order, each the exact stri
ng to send: ["侵权/抄袭","色情低俗","
违法违规","垃圾广告","恶意插件","
其他"].
 * {operation:"report",kind,item_id
,reason,detail?}: kind is skins, candidate-sk
ins, plugins, dictionaries or replies; item_i
d the item's UUID; reason one of the reasons;
 detail optional, at most 1000 characters. Va
lue {method:"POST",path:"/v1/community/report
s",body}: send body as JSON with the signed-i
n session's bearer (the device's anonymous ac
count counts). A bad field is error community
_invalid.
 * {operation:"error",status,body?}
: status and body of a failed community respo
nse. Value {code,message,retry}: code is acco
unt_blocked_content (422 blocked_content: the
 text must change; never say the service is d
own), account_screening_unavailable (503 scre
ening_unavailable: honour Retry-After), accou
nt_banned (403 account_banned) or the generic
 account_* code; message is the Chinese sente
nce to show for the first three and null othe
rwise; retry says whether the same request ma
y be sent again later. */
char *msime_client_
community_moderation(const uint8_t *request, 
size_t length);
/* Google 登录：由宿主�
��己监听回环端口、打开系统浏览�
��、申请 challenge 并提交授权码（�
�蒙）。这里给出与桌面端相同的�
�定；申请 challenge 和提交授权码可
以经自己的 HTTPS 栈，也可以用 msim
e_client_account 的 google_challenge {target
} 和 google_code_login。Request 是 UTF-8 J
SON，最多 32 KiB。
 * {operation:"target"
,port}: port 是宿主在 127.0.0.1 上监听
的端口（>=1024）。Value {target}: 向 P
OST /v1/auth/challenges 发 {provider:"google
",target,purpose:"login"} 时用的回跳地�
��。
 * {operation:"plan",authorization_url,
target,expires_in}: 后端返回的 authoriza
tion_url 与 expires_in。Value {state,wait_m
s,max_request_bytes,io_timeout_ms}: 链接通
过校验，可以用系统浏览器打开；
最多等 wait_ms 毫秒；每个连接最多
读 max_request_bytes 字节、最多 io_time
out_ms 毫秒拿到请求头，超出就以 h
ead:null 回复并关掉。链接不是指向
这个监听的 Google 授权链接、或 cha
llenge 太短时返回 account_unavailable，
不要打开它。
 * {operation:"reply",head
,state,return_link?}: head 是一个连接读
到的请求头（到第一个空行为止，
含），没读到完整请求头时为 null�
��state 是 plan 给的；return_link 可省�
��，是宿主注册的、能把应用拉回�
��台的 `scheme://路径` 链接（如鸿蒙
的 msime://auth/google），回跳页据此�
��「回到水杉输入法」按钮，形状�
��合格时不画，回复照常。Value {out
come,code?,error?,response}: 把 response 原
样写回这个连接再关闭。outcome 为 
ignored 时继续等下一个连接；code �
�用 POST /v1/auth/login {challenge_id,creden
tial:code} 完成登录；failed 时结束登
录，error 是 account_cancelled（用户在
 Google 页面拒绝）或 account_unavailabl
e。 */
char *msime_client_google_loopback(co
nst uint8_t *request, size_t length);
/* 原�
��设置窗口的账号操作，与共享应�
��（Tauri）的十四个 account_* 命令和
 Windows 的四个 account_preferences_* 设�
��同步命令相同，用同一份本用户�
��话文件。Request 是 length 字节（最
多 4 MiB）的 UTF-8 JSON {directory, legacy
_directory?, client_agent?, action}：directo
ry 是共享应用登录时写 account-sessio
n.json 的目录的绝对路径（Windows 是
 %LOCALAPPDATA%\<本版本的用户目录>\ac
count，与 msime_client_account_access_token
 相同）；legacy_directory 可选，是旧
版本放会话的目录（Windows 是 %LOCAL
APPDATA%\<tauri identifier>），本进程第
一次用到 directory 时，directory 还没
有会话而这里有，就把它搬过去。
client_agent 可选（#8201 的决定 1），
是这个宿主自报的 User-Agent，形状�
�� msime_client_session_store 的 client_agen
t 相同（msime-<android|ios|macos|windows|l
inux|harmony>/<版本> (<设备>; <系统>; e
dition=<id>)，括号部分可省略，最多
 256 字节可打印 ASCII，用 msime_client
_client_agent 拼出），不合法时是 acc
ount_invalid；这次调用的请求（包括
登录、绑定和匿名账号的注册与重
新登录，服务端「我的设备」按登
录时的 User-Agent 显示平台和设备）
都以它发出，不给时与以前一样是
 MSIME/Android。每个 directory、每个 cl
ient_agent 在进程里只有一个会话对�
��：同一个宿主的各次调用要么都�
��同一个 client_agent、要么都不带，
google_cancel 才结束得了同一个对象�
��的 google_login；会话文件每次都重
新读，刷新令牌时与共享应用和输
入法共用 account-refresh.lock。也可以
用 {session_store, action} 代替 {directory
, legacy_directory?, client_agent?}：session
_store 是 msime_client_session_store_registe
r 给的 id，会话在宿主注入的存储�
��，用注册时建好的会话对象；dire
ctory 和 session_store 恰好给一个，leg
acy_directory 和 client_agent 只能与 dire
ctory 一起给（注入存储的 User-Agent 
是注册时给的那个），否则是 accou
nt_invalid。响应不含令牌。目录模�
�读 account-session.json 时三种布局都�
��：Native、Apple，以及鸿蒙 ArkTS 桥�
��以前写的扁平布局 {access_token,refr
esh_token,token_type,expires_at(Unix 毫秒),
user}（只读，expires_in 按剩余时长�
�舍五入补出、至少 1，没有 session_
id；第一次认证请求前补上 session_i
d、之后每次保存都写成 Native）。�
��蒙上 directory 里的 account-session.jso
n 和 cloud-clipboard-keystore 带属组读�
�位也照样读（@ohos.file.fs 没有 chmod
，ArkTS 按应用 umask 写），与 anonymo
us-session.json 相同，其他用户位仍�
�拒绝；其他平台仍要求只有属主�
�读写。
 * action 是下列之一，value 
与共享账号页（packages/ui/src/account/
account-page.tsx）读到的形状相同；us
er 是 {id,displayName,createdAt,email?,avata
rUrl?,avatarUploaded?}，avatarUrl 只在是�
��客户端会去取的头像地址时出现�
��avatarUploaded 只在头像是用户上传�
��（可以移除）时为 true：
 * {operat
ion:"status"} → {user|null}，只读本地�
��件。{operation:"providers"} → {email,ph
one,apple,google,apple_web}：都是布尔值
，是服务端 GET /v1/auth/providers 对应
的开关（phone 也认服务端的 sms）�
�没有的算 false；apple_web 是 Apple 网
页登录（没有 Apple 原生登录的 Andr
oid 用，见下面的 apple_web_start），�
�� Apple 原生登录的 apple 分开。
 * {
operation:"request_code",provider:"email"|"ph
one",target} → {challengeId,expiresIn}。{o
peration:"login",challenge_id,code}（code �
� 6 位数字）→ {user}。
 * {operation:"
google_login"} → {user}：在 127.0.0.1 上
监听回环端口、申请 Google challenge�
��用系统浏览器打开校验过的授权�
��接（只在 Windows 上；其他平台是 
account_unavailable），等浏览器回跳�
�登录。最多阻塞约 5 分钟；用户�
� Google 页面拒绝或调用了 google_canc
el 时是 account_cancelled。{operation:"goo
gle_cancel"} → null，立即返回，结束
同一 directory 上进行中的 google_login
。两者都只有 operation 一个字段，�
��带的（例如 purpose）是 account_inval
id。
 * 宿主自己弹登录界面的登录
（#8201），挑战和凭据都经这里，�
��主不用自己发 /v1/auth 请求；登录
成功返回 {user}，与 login 一样存进 
directory 或 session_store 的登录会话�
�nonce、授权链接和 codeVerifier 只给�
��主自己用，不要交给网页、不要�
��进日志。
 * Apple 原生登录（iOS、
macOS 的 ASAuthorizationAppleIDProvider）�
�{operation:"apple_challenge"} → {challenge
Id,expiresIn,nonce}，nonce 原样放进 ASAu
thorizationAppleIDRequest 的 nonce、challen
geId 放进 state，回来的凭据 state 不
等于 challengeId 时丢掉；{operation:"ap
ple_login",challenge_id,identity_token}（ide
ntity_token 是凭据 identityToken 的 UTF-8
 文本，非空、最多 16 KiB、不含控�
��字符）→ {user}。每个 challenge 只�
��用一次，失败或取消后重新申请�
��
 * Google ID token（iOS 的 GoogleSignIn�
��Android 的 Credential Manager）：{operat
ion:"google_challenge"} → {challengeId,expi
resIn,nonce}，nonce 原样交给 Google；{o
peration:"google_token_login",challenge_id,id
_token}（id_token 是三段非空 base64url 
用 . 连成的 JWT，最多 12000 字节）�
�� {user}。
 * Google 回环（自己监听�
��环端口的宿主，鸿蒙）：{operation
:"google_challenge",target}（target 是 msim
e_client_google_loopback 的 target 给的回
跳地址）→ {challengeId,expiresIn,author
izationUrl}，授权链接已校验过是指�
�� target 的 Google 授权链接，否则是
 account_unavailable；再交给 msime_client
_google_loopback 的 plan，等到回跳的 c
ode 后 {operation:"google_code_login",challe
nge_id,code}（code 非空、最多 2048 个�
��打印 ASCII）→ {user}。target 不是 h
ttp://127.0.0.1:<端口>/callback 或 http://
[::1]:<端口>/callback（端口 1024–65535
）时是 account_invalid。
 * Apple 网页�
��录（没有 Apple 原生登录的 Android�
��：{operation:"apple_web_start",app}（app 
是发起登录的 applicationId，小写字�
��开头的点分段，最多 128 字节；�
�务端只认 shared/contracts/editions.json 
里各版本的 android.application_id，其�
��是 account_invalid）→ {authorizationUrl
,expiresIn,codeVerifier}：Rust 生成 32 字
节随机数的 codeVerifier，只把它的 S
256 摘要发给 POST /v1/auth/apple/web；au
thorizationUrl 已校验过是 https://applei
d.apple.com/auth/authorize、只带一个 sta
te，用浏览器打开它；codeVerifier 由
宿主私下保存（浏览器打开期间进
程可能被杀），回调 <app>://auth/appl
e?grant=… 到达后 {operation:"apple_web_l
ogin",grant,code_verifier}（grant 是 32 字
节的无填充 base64url，43 个字符）�
� {user}。服务端不认这个 grant（不�
��识、已用过、过期或不属于这个 
verifier）时是 account_invalid，这时保
留 codeVerifier 等真正的回调，其余�
��败可以丢掉它；回调带 error=… �
�不用调这里。
 * 给已登录的账号�
��加登录方式（#8201 的决定 9，Andro
id 的「添加登录方式」）：邮箱、
Google ID token 和 Apple 网页登录的申�
��和完成两步都可以带 purpose，取 "
login"（缺省，与不带相同）或 "link
"，其他值是 account_invalid。{operation
:"request_code",provider:"email",target,purpo
se:"link"}（只能是 email）→ {challenge
Id,expiresIn}，再 {operation:"login",challe
nge_id,code,purpose:"link"}；{operation:"goo
gle_challenge",purpose:"link"}（不能带 ta
rget，回环流程不能绑定）→ {challe
ngeId,expiresIn,nonce}，再 {operation:"goog
le_token_login",challenge_id,id_token,purpose
:"link"}；{operation:"apple_web_start",app,p
urpose:"link"} → {authorizationUrl,expiresI
n,codeVerifier}，回调到达后 {operation:
"apple_web_login",grant,code_verifier,purpose
:"link"}，宿主要和 codeVerifier 一起�
�住这次是绑定。Apple 原生登录、Go
ogle 回环、google_code_login 和 google_lo
gin 没有 purpose 字段，带了是 account
_invalid。两步都只用登录会话（dire
ctory 的 account-session.json 或该 store �
�� ACCOUNT 项）并带着它的令牌请求�
��快过期时先刷新；申请时令牌被 
401 拒绝就刷新一次、在同一个账�
�里重试，完成时不重试（验证码�
�对也是 401）；没有登录时是 accoun
t_unauthorized，不联网，不会用匿名�
��号。完成后 Value 是 {user}：服务�
�把这种登录方式绑到该账号，并�
�它签发一个新会话，Rust 在刷新锁
里用它替换本地的登录会话（同一
个账号、新的登录 ID，旧会话在服
务端留到过期）；完成请求进行中
本地退出、换了账号或重新登录时
是 account_cancelled，绑定在服务端已
经生效，新会话丢掉。服务端要求
登录会话是 10 分钟内建立的（刷�
�令牌不算），否则两步都是 account
_recent_login_required，宿主让用户以 p
urpose:"login" 重新登录同一个账号后
再从申请开始；这种登录方式已经
绑在别的账号上是 account_conflict；�
��证码不对、令牌失效或完成时的�
��号不是发起绑定的那个是 account_u
nauthorized；Apple 的 grant 不认识、用
过、过期或属于另一个账号发起的
绑定是 account_invalid（保留 codeVerifi
er，与登录相同），完成这一步的�
��他失败都已经用掉了 grant，要从 
apple_web_start 重新开始。失败按下�
� #8201 新操作的规则带细节：限流�
��带 retry_after_seconds 的 account_rate_li
mited，没拿到响应是 account_network。

 * {operation:"profile"} → {user,providers
:[...]}（providers 是已绑定的登录方�
��，去重）。{operation:"rename",display_
name}（非空、首尾无空白、最多 64 
个字符）→ 同 profile。
 * {operation:
"avatar"} → null 或 "data:<image/png|image
/jpeg|image/webp>;base64,<...>"（未登录�
�没有头像时为 null）：类型按下载
到的文件头定，下载时声明接受 We
bP，没上传过头像的 Google 登录用�
�的头像在 Google 的图片主机上，它
按 Accept 内容协商，多半回 WebP，�
�主要能解 WebP；上传仍只收 PNG 和 
JPEG。{operation:"upload_avatar",path}：pat
h 是宿主自己的文件对话框选中的 
PNG 或 JPEG 的绝对路径，最多 1 MiB �
�� 同 profile；不是这样的文件时是 
account_invalid。{operation:"remove_avatar"}
 → 同 profile。
 * {operation:"logout",al
l} → null（all 为 true 时退出所有设
备；本地会话总会先清掉）。{opera
tion:"delete"} → {voice_samples}（注销�
�号，成功后清掉本地会话；voice_sa
mples 见下面的 account_delete，以前 va
lue 是 null）。{operation:"forget"} → nu
ll（只清掉本地会话，用于会话已�
��效时）。
 * {operation:"ensure_anonymou
s"} → true：本机匿名账号还没有可
用的会话时注册它。带 directory 时�
�� msime_client_ensure_anonymous_account(dire
ctory) 相同（anonymous-account.json 和 an
onymous-session.json 在 directory 里）；�
�� session_store 时身份在宿主的 ANONYM
OUS_IDENTITY 项、会话在 ANONYMOUS 项，
身份按「不存在才写」在锁外确定
，登录时持 lock(ANONYMOUS)。已有会�
�时只读一次存储、不联网；否则�
�塞网络。失败时 error 是账号错误�
��，不是 msime_client_ensure_anonymous_acc
ount 那样的说明文字。
 * 设置同步
（共享账号页的设置同步卡片）：
{operation:"preferences_schema"} → {fields,
maximumBytes,updateMode,revisionRequired}，�
��以原样交给 msime_client_account_settin
gs_export、msime_client_account_settings_app
ly 和 iOS、鸿蒙的设置导出与应用�
�它们的 schema 既认服务端的 snake_ca
se（maximum_bytes、update_mode、revision_r
equired），也认这里的 camelCase，同�
��个键两种写法都给时按请求不合�
��处理。{operation:"preferences_load"} →
 云端设置文档 {revision,settings}。{op
eration:"preferences_upload",preferences_dire
ctory}：preferences_directory 是偏好目�
�的绝对路径；本机各平台共有的 i
nput.* 设置按本目录所属的版本过�
�、只留字段表收录的键，合并进�
�端文档后上传，返回上传后的云�
�文档；一个键都没有时是 account_un
available，云端文档已被别处更新时
是 account_conflict。{operation:"preference
s_apply",preferences_directory,user_id,prefer
ences}：preferences 是刚由 preferences_lo
ad 下载的文档，user_id 是下载时的�
��号（期间退出、换号或重新登录�
��是 account_cancelled）；按版本过滤�
��把其中的 input.* 键应用到本机偏�
��，按读到时的修订号比较并交换�
��回（关掉剪贴板历史时与 msime_cli
ent_save_preferences 一样清掉已存的历
史），返回保存后的 PreferencesSnapsh
ot。写回失败时 error 是 conflict（本
机设置已在别处更新）或 storage，�
��共享应用相同。
 * 以下是 #8201 �
�增的操作，value 用服务端的 snake_c
ase 形状（chat_models 除外，与共享�
�号页相同）。令牌从哪来：account_
delete、phrases_*、preferences_put、sessio
ns_list、session_revoke、data_*、download_
link 只用登录账号（注销成功后、d
ata_summary 和 data_delete 的 voice 分区�
��见下）；chat、chat_models、translate�
��feedback、diagnostics_upload 登录账号�
��先，没有登录时用本机匿名账号�
��带 directory 时是同一 directory 里的
 anonymous-session.json，directory 还不存
在时当作没有；带 session_store 时是
该 store 的 ANONYMOUS 项；Rust 不会自�
��注册匿名账号，宿主要先调 ensure
_anonymous）；voice_contribution 和 voice_
delete 只用本机匿名账号，登录了�
�一样；diagnostics_state、diagnostics_reg
enerate_token、diagnostics_delete 绑定到�
��传时返回的 user_id，登录会话和�
�名会话都不是它时是 account_identity
_changed，不会退回另一个账号。服�
��端以 401 拒绝令牌时带着它刷新�
�次、只在同一个账号里重试。这�
�操作失败时 error 之外可能还有 ret
ry_after_seconds，是服务端 Retry-After �
��整数秒，只在已知时出现：{"ok":f
alse,"error":"account_rate_limited","retry_af
ter_seconds":3600}。请求发出去以后没
有拿到响应（连不上、DNS 或 TLS 失
败、超时、连接在响应头到达之前
断开）时是 account_network（请求在�
�地就建不出来是 account_invalid，重�
��向被拒是 account_unavailable，都不�
�断网），服务端回了 5xx 时是 accou
nt_unavailable（503 server_busy 是带 retry
_after_seconds 的 account_rate_limited），
宿主据此分开「网络连接失败」和
「服务暂不可用」；令牌要先刷新
、而刷新请求本身没拿到响应时仍
是 account_unavailable。
 * {operation:"cha
t",messages:[{role:"system"|"user"|"assistant
",content}],model} → {content}：最多 16 
条、每条非空白且最多 16 KiB，序�
�化后的请求体超过 65536 字节是 acc
ount_too_large；一次交回整段回复，�
��能取消，最长阻塞 125 秒，要边�
�成边显示或能停下来时用 msime_clie
nt_account_chat_stream。{operation:"chat_mod
els"} → {data:[{id}],defaultModel}。两者
被服务端限流时是 account_rate_limited
，带 retry_after_seconds（超过每分钟�
��请求数时服务端给 60）；请求进�
��中给出令牌的那个账号退出或换�
��号时是 account_cancelled，回答不交�
��去。
 * {operation:"account_delete"} → 
{voice_samples}：注销账号，与已有的
 delete 相同（DELETE /v1/users/me，成功
后清掉本地会话），失败时按上面
的规则带细节：会话不是 10 分钟�
�登录的是 account_recent_login_required�
�这时本地会话留着，宿主让用户�
�出并重新登录后再注销。delete 的�
��败码不变，遇到需要最近登录时�
��是 account_unauthorized。两者注销成�
��以后都再删一次本机匿名账号名�
��的语音样本（与 voice_delete 相同�
�可能多一次匿名重新登录），尽�
�而为：这一步失败不撤销、也不�
�注销失败，ok 仍是 true，结果在 vo
ice_samples 里，形状与响应的信封相
同：{ok:true}（删掉了，或本机从来
没有匿名身份、没有样本可删）或
 {ok:false,error,retry_after_seconds?}，erro
r 是 voice_delete 会给的码；宿主据�
�提示语音样本没有删掉，稍后用 v
oice_delete 重试。注销失败时不做这
一步。
 * {operation:"translate",texts,sou
rce_lang,target_lang,timeout_ms?} → {transl
ations:[string|null]}：texts 1–32 条，�
�条非空白、最多 8192 字节；语言�
�码 2–16 个 [A-Za-z-]，发出时转成�
�写；timeout_ms 1000–30000，缺省 10000
；序列化后的请求体超过 65536 字�
�是 account_too_large。响应的 code 必�
�是整数 200、data 必须与 texts 等长�
��否则是 account_unavailable；每一项�
�制表符和换行折成空格、去掉首�
�空白，含其他控制字符、为空、�
�原文相同或规范化后超过 4096 字�
�（msime_client_apply_translations 每条释
义的上限，超过一条它就整批拒绝
）的那一项是 null，不让整批作废�
��单项超过服务端的 65536 字节仍是
 account_unavailable。
 * {operation:"phrase
s_load"} → {revision,phrases:[{id,text,grou
p,position}]}（新用户是 revision 0 和�
�列表）。{operation:"phrases_save",revisi
on,phrases} → 保存后的同一形状：�
�多 500 条；id 是 1–64 个 [A-Za-z0-9_-
] 且唯一；text 是 1–2000 个 UTF-16 �
�元、不含 NUL，只有空白也合法；g
roup 最多 32 个 UTF-16 单元、不含控�
��字符；position 在 0..=1000000；请求�
��超过 256 KiB 是 account_too_large；revi
sion 与云端不符是 account_conflict，�
�重试。
 * {operation:"preferences_put",do
cument,revision,user_id?} → 上传后的云
端设置文档 {revision,settings}：整份�
��换云端的设置文档（PUT /v1/users/me
/preferences，服务端的 update_mode 是 r
eplace），按 revision 比较并交换。do
cument 是文档的 settings（键是 1–128
 个 [A-Za-z0-9._-]，值只能是布尔、�
�数、有限的数或最多 1 MiB 的字符�
��，最多 512 个键），revision 是读�
�它时云端的修订号，通常就是设�
�导出入口给的 merged 的 revision 和 s
ettings；服务端只在云端的修订号�
�是 revision 时写入，写入后修订号�
�� 1，已被别处更新时是 account_confl
ict，不重试，宿主重新读、重新合
并。上传前不按字段表过滤，键不
在服务端字段表里或类型、范围不
对时服务端整份拒绝（account_invalid
）。revision 为负、键或值不合规是
 account_invalid，请求体超过 1 MiB 是 
account_too_large，都不联网。user_id �
�读文档时的账号（status 给的 user.i
d，1–128 字节）：给了时只写这个
账号，会话已经退出、换成别的账
号，或者请求期间退出、换号、重
新登录时是 account_cancelled（请求期
间变的，服务端可能已经写进了原
来那个账号）；不给时写当前登录
的账号。
 * {operation:"sessions_list"} �
�� {sessions:[{id,platform,name,app_version,c
reated_at,last_active,current}]}：最多 50 
条；网页登录的 platform 是 web，User
-Agent 认不出时 platform、name、app_ver
sion 是空串，都保留；类型不对的�
��跳过。{operation:"session_revoke",id}（
id 是 64 位小写十六进制）→ {signed
_out}：撤销的是当前会话时等同 log
out{all:false}，本地会话同时清掉，s
igned_out 为 true；否则为 false。会话
不存在是 account_not_found。
 * {operati
on:"data_summary"} → {bytes,sections:[{id,b
ytes,items}],anonymous_voice}，服务端新�
��的分区原样保留。语音贡献只用�
��名账号，所以登录账号的汇总之�
��再用本机匿名账号请求一次 GET /v
1/users/me/data，把它的 voice 分区的 b
ytes、items 加进 voice 分区（登录账�
��的汇总里没有时整个加进去），�
�� bytes 跟着加；anonymous_voice 说明�
�一步：included 是已经合进去，none 
是本机既没有匿名会话也没有匿名
身份、没有可合的，unknown 是这一�
��没有成功（读不了匿名会话或身�
��、断网、被拒等，或者身份还在�
��会话已经过期清掉：服务端的会�
�� 30 天后过期，名下的样本还在，
汇总不为此重新登录），这时 voice
 只是登录账号的，宿主显示「语�
�样本数暂不可知」。这一步的失�
�不让汇总失败；登录账号的汇总�
�败时整个失败，不做这一步。{oper
ation:"data_export",destination}：destinatio
n 是写入 zip 的绝对路径，Rust 不检
查扩展名（宿主自己取名，建议以
 .zip 结尾），父目录不能是符号链
接 → {path,bytes,filename}，filename 是�
��务端建议的文件名；流式写进同�
��录的临时文件，读得出 zip 的中�
�目录才换到 destination，失败时 dest
ination 上原有的文件不动；最长约 
130 秒；每天 3 次，超了是 account_ra
te_limited 带 retry_after_seconds。{operati
on:"data_delete",sections}：sections 非空�
��不重复，取自 preferences、dictionary
、phrases、clipboard、voice → 不含 voi
ce 时是 null，含 voice 时是 {voice_samp
les}；会话不是 10 分钟内登录的是 
account_recent_login_required。sections 含 
voice 时，删完登录账号的分区以后
还删本机匿名账号名下的语音样本
（voice_contribution 只用匿名账号）�
�与 voice_delete 相同；登录账号这一
步失败（包括需要最近登录、没有
登录时的 account_unauthorized）时整个
操作以它的码失败，不碰匿名账号
，也就不会白耗一次匿名重新登录
。匿名这一步失败时登录账号的分
区已经删掉，操作仍然成功，失败
在 voice_samples 里，形状与 account_del
ete 的相同；宿主用 voice_delete 重试
这一步，不要重做整个 data_delete（
词库会再写一条 reset）。删除后云
端的设置是 {}、常用语是 []、词库
有一条 reset，这个操作不碰本机的
同步状态。{operation:"voice_delete"} →
 null：删掉本机匿名账号名下的语�
��样本（DELETE /v1/users/me/data 的 voice
 分区），不要求登录，给从没登�
�过的用户和上面两处要重试的宿�
�。先用现有的匿名会话删，服务�
�要求最近登录（匿名会话也一样�
�或令牌已失效时，用保存的匿名�
�份（directory 里的 anonymous-account.jso
n 或该 store 的 ANONYMOUS_IDENTITY 项）�
��新登录同一个匿名账号、换一个�
��会话再删，新会话替换存储里的�
��会话；重新登录计入服务端按地�
��每天的匿名登录限额，超了是 acc
ount_rate_limited。本机从来没有匿名�
��份时没有样本可删，直接成功、�
��联网；有会话却没有身份、换不�
��新会话时交回那次失败。只删得�
��这台设备的匿名账号。
 * {operatio
n:"feedback",type:"bug"|"suggestion"|"diction
ary",text,platform,app_version,edition,diagno
stics?,screenshots?} → {id,status}：text �
��掉首尾空白后码点数和 UTF-16 单�
�数都不超过 500，控制字符只允许 
\n 和 \t；platform 是 android、ios、maco
s、windows、linux、harmony 之一；app_ve
rsion 是 1–64 个码点、不含控制字�
��；edition 是 0–32 个 [a-z0-9._-]；dia
gnostics 是字符串到字符串的对象，
键只留 device、os、app_version、edition
、scheme、keyboard_layout、skin、ime_enab
led、ime_default，控制字符换成空格�
��按字节截到 256、空值丢掉；screen
shots 最多 3 个 PNG 或 JPEG 文件的绝�
��路径，只读文件头和尺寸、不重�
��编码，一张超过 1 MiB、单边超过 
8192 或总像素超过 8388608 是 account_t
oo_large。以 multipart 发送，最长 60 �
��。
 * {operation:"diagnostics_upload",plat
form,app_version,ttl:"one_hour"|"one_day"|"se
ven_days",bundle:{state_root,include,sources}
} → {id,mcp_url,token,expires_at,counts,use
r_id,anonymous}：bundle 与 msime_client_dia
gnostic_bundle 的请求相同，只是没有
 destination；platform 和 app_version 是 [
A-Za-z0-9][A-Za-z0-9._+-]{0,63}；一个分�
�都没勾选是 account_invalid。上传前�
��服务端的规则重建：事件只留服�
��端认的 10 种 kind，只留 t_ms、kind�
��duration_ms，每类最多 5000 条；崩�
�记录最多 200 条，at 重新格式化成
 RFC 3339，解析不了的整条丢掉；配
置快照超过 8 层的子树换成 "<trunca
ted>"、条目满 2000 后不再加入、字�
��串截到 1024 字节，像凭据的键的�
��换成 "<redacted>"；整份超过 2 MiB �
�先截掉最旧的事件，去掉全部事�
�仍超过时是 account_too_large。counts �
��重建后各类保留、丢弃和截掉的�
��数，形状与 msime_client_diagnostic_bun
dle 的相同；user_id 和 anonymous 是上�
��所用的账号，之后的三个诊断操�
��要带上这个 user_id；token 只出现�
�一次，不要写进日志。读本机记�
�失败时 error 是 diagnostics_* 码。服�
��端每小时最多 6 次。
 * {operation:"
diagnostics_state",user_id} → {snapshot:nul
l|{id,created_at,expires_at,bytes,sections,to
ken_hint},accesses:[{at,tool,arguments,result
_count,bytes}]}。{operation:"diagnostics_reg
enerate_token",user_id} → {token}：旧令�
��立即失效，没有有效快照时是 acc
ount_not_found。{operation:"diagnostics_dele
te",user_id} → null，没有快照也算成
功。
 * {operation:"download_link",platform
}（platform 是 windows、macos、linux、ha
rmony-pc、ios、android、harmony 之一）�
�� {sent_to}：服务端把这个平台的下
载页链接（固定模板）发到账号已
验证的邮箱（邮箱登录身份优先，
其次是已验证的 Google 邮箱），收�
��地址由服务端决定；sent_to 是打�
�后的收件地址（例如 u***@example.com
），服务端已经发出、但这个字段
缺失或不合规（空、超过 320 字节�
��含控制字符）时为 null。没有已�
�证邮箱是 account_no_verified_email，宿
主改为提示复制链接；邮件没配置
或发送失败是 account_unavailable；每�
��用户每小时 3 次、每天 10 次。
 *
 {operation:"voice_contribution",language,pro
vider,transcript,app_version,audio} → {id,d
uration_ms}：只在用户打开语音贡献�
��且隐私判断允许时调用。audio 是 
WAV 文件的标准 base64，解码后最多 
2 MiB（超了是 account_too_large），按�
��务端的 RIFF 结构规则校验（恰好�
��个 fmt 和一个非空 data，PCM 8/16/24/
32 位或浮点 32/64 位，1–8 声道，80
00–192000 Hz）；duration_ms 由 Rust 从 
data 大小和字节率算出（向下取整�
��，要在 1–60000 之间，宿主不用�
�；不收 Ogg。language、provider、app_ve
rsion 是 [A-Za-z0-9][A-Za-z0-9._+-]{0,63}；
transcript 去掉首尾空白后发送，非�
��、最多 2000 个码点，控制字符只�
��许 \n 和 \t。以 multipart 发送，最�
�� 60 秒；只用本机匿名账号，没有
匿名会话时是 account_unauthorized（不
退回登录账号）；服务端每个账号
每小时 30 次，样本挂在这台设备�
�匿名账号下、保存 180 天，voice_del
ete、data_delete 的 voice 分区和注销�
�录账号（delete、account_delete）成功
以后都会删掉它们，data_summary 的 v
oice 分区算上它们。
 * 打字统计「
上传到云端」和「绑定 QQ 群助手�
�，给用 Rust 账号会话的原生宿主�
�Windows 原生设置），语义与共享设
置应用的 typing_statistics_upload_status�
��set_typing_statistics_upload、upload_typin
g_statistics、account_qq_link* 命令相同�
��只用登录账号，不退回匿名账号�
��statistics_directory 是打字统计目录�
��绝对路径（msime_client_typing_statisti
cs 的 directory），day 是宿主的本地�
��期 YYYY-MM-DD，platform 是 windows、mac
os、linux、android、ios、harmony 之一�
�不合法时是 account_invalid、不联网�
��{operation:"typing_upload_status",statistic
s_directory} → {enabled,last_upload_ms,sign
ed_in}：last_upload_ms 是上次上传成功
的 Unix 毫秒，还没有时为 null；sign
ed_in 说明本机是否登录了账号（没
登录时开关不能打开），不联网。
{operation:"typing_upload_set",statistics_dir
ectory,enabled,day,platform} → 同上：打
开要求已登录（否则 account_unauthori
zed，开关不动），在本地打开后立
刻上传最近有字数的至多 7 天（PUT
 /v1/users/me/typing-statistics），上传�
�败时开关保持打开、error 是那次�
�传的码，下一次自动上传再试；�
�闭时先在本地关掉，再 DELETE /v1/us
ers/me/typing-statistics 删掉云端副本�
�删除失败时开关仍是关的、error �
�删除的码，宿主据此提示云端副�
�没删掉。{operation:"typing_upload",stati
stics_directory,day,platform,force?} → 同�
��：打开了上传、已登录、离上次�
��传超过十分钟（force 为 true 时不�
��间隔）才发请求，否则什么也不�
��；没登录时不发也不报错，开关�
��持打开，登录后下一次照常上传�
��宿主在设置窗口打开和打字统计�
��显示时各调一次，不用自己计时�
��同一进程里的 typing_upload_set 和 ty
ping_upload 互相等待，关掉上传后的
删除不会和还在路上的上传交错。
本机统计目录读写失败时是 account_
storage。{operation:"qq_links"} → {links:[
{id,created_at}]}：本人绑定的 QQ 群友
，新的在前，服务端只存群和成员
标识的摘要，所以只有编号和绑定
时间。{operation:"qq_link_redeem",code} �
� {id,created_at}：code 是群友在 QQ 群�
�� @机器人 发 /bind 拿到的 8 位数字
，前后的空白和中间的空格、连字
符先去掉，其余不是 8 位数字时是
 account_invalid、不联网；码不存在�
�过期或用过是 account_qq_link_code_not_
found，账号已经绑了 10 个群友是 ac
count_qq_link_limit（码不作废），每�
�时 10 次、猜错的也算，超了是 acc
ount_rate_limited。{operation:"qq_link_delet
e",id} → null：id 是 qq_links 给的编�
�（1–19 位数字，不是时 account_inva
lid），别人的或不存在的是 account_
not_found。
 * {operation:"session_adopt",pa
th?,identity_path?,item:"account"|"anonymous"
,from:"settings"|"keyboard"} → {outcome:"ad
opted"|"unchanged"|"cleared",local_unreadable
?:true}：只能带 directory（带 session_s
tore 是 account_invalid），给鸿蒙设置
应用与键盘两个沙箱之间的会话镜
像。path 是另一边发布的会话文件�
��绝对路径，不给、或文件（连同�
��在目录）不存在，表示发布方没�
��这份会话；读它时不跟随符号链�
��、最多 64 KiB；鸿蒙上文件属主必
须是本进程的有效 uid（不看所在�
�录的属主：发布目录由发布方自�
�建，看有效 uid 是为了拒收别的 ui
d 先建好目录再放进的文件；前提�
��设置应用和键盘是同一个应用的�
��个进程、有效 uid 相同），只拒�
�其他用户位；其他平台与 directory 
里的会话文件相同，属主与所在目
录相同、只能属主可读写，Native、
Apple 和鸿蒙扁平布局都认，读不了
或不是合法会话时是 account_storage�
�本地不动。item 选 directory 里的 acc
ount-session.json（登录会话）或 anonym
ous-session.json（匿名会话）。identity
_path 只能和 item:"anonymous" 一起给（
和 item:"account" 一起是 account_invalid�
��，是另一边发布的匿名账号身份�
��件 anonymous-account.json 的绝对路径�
��读的规则与 path 相同，文件（连�
��所在目录）不存在表示发布方没�
��身份，读不了或不是合法身份时�
�� account_storage，本地不动；给了时
身份跟着匿名会话走：结果是 adopt
ed 时，在同一把锁里、写会话之前
把 directory 里的 anonymous-account.json �
��成发布方的身份，发布方没有身�
��而采用的会话换了账号（本地原�
��没有匿名会话、读不了，或 user.i
d 不同）时删掉本地身份，同一个 
user.id 时不动，会话和身份始终属�
��同一个匿名账号；写会话失败时�
��力还原身份；结果不是 adopted 时�
��份不动。不给 identity_path 时身份�
��件不动，与加这个字段之前相同�
��整个决定在 directory 的 account-refre
sh.lock 里做，与刷新、登录、退出�
��斥，不联网。两种 item 只看 user.i
d 和 expires_at，不看 session_id（有没
有、相不相同、v4 还是 v8 都一样�
�，规则除下面注明的两处外相同�
�同一个 user.id 时 expires_at 更晚的�
�，from:"settings" 和 from:"keyboard" 都�
�样，相同时 unchanged；不同的 user.i
d（换号）时 from:"settings" 直接替换
、from:"keyboard" 是 unchanged，设置应�
��里的换号不会被键盘的旧副本撤�
��；本地没有会话或读不了时：item
:"account" 与换号相同（读不了而没�
��采用时 outcome 仍是 unchanged，另带
 local_unreadable:true：这个结论取决�
�本地此刻读不读得了，本地恢复�
�读以后同一份发布物可能就该采�
�，宿主不能把它记成同一份发布�
�不用再试；其他结果不带这个字�
�）；item:"anonymous" 两个方向都是 ad
opted，用户没打开过设置应用就先�
��用了键盘时，设置应用第一次启�
��采用键盘注册的那个匿名账号，�
��再注册第二个、多用一次注册额�
��（带 identity_path 时连同键盘的身�
��）。expires_at 是拿到令牌的时刻�
�服务端给的固定时长，两个沙箱�
�同一台设备的时钟，后签发的会�
�一定过期得更晚，所以刷新过的�
�份胜过刷新前的、同一个账号重�
�登录的那份胜过上一次登录留下�
�：只要另一边给的是更新的那份�
�session_adopt 不会留下、也不会换回
用过的刷新令牌（中间把系统时钟
往回调过时除外）。发布方没有会
话时：item:"account" 且 from:"settings" �
��掉本地会话，与 forget 相同（连�
�云剪贴板密钥串），本地原来就�
�有时是 unchanged；item:"account" 且 fro
m:"keyboard" 是 unchanged；item:"anonymous"
 两个方向都是 unchanged、本地不动�
��匿名账号没有退出登录，清掉只�
��逼着重新注册、用掉每个 IP 每天
 5 次的注册额度，所以 cleared 只出
现在 item:"account"。采用的会话按 Na
tive 布局原样写进 directory（session_i
d 照抄，没有的在第一次认证请求�
��补上）。两个沙箱各用各的锁，�
��边各自拿同一枚刷新令牌刷新的�
��险这里消除不了。
 * 其余失败时
 error 是账号错误码：account_invalid�
�请求或参数不合法）、account_unauth
orized（未登录或会话失效）、accoun
t_rate_limited、account_storage、account_ca
ncelled、account_conflict、account_unavaila
ble、account_blocked_content、account_scree
ning_unavailable、account_banned；只有上
面新增的操作和 purpose:"link" 的几�
�还会返回 account_too_large、account_not
_found、account_recent_login_required、acco
unt_identity_changed、account_no_verified_em
ail 和 account_network（msime_client_accoun
t_chat_stream 另有 account_busy；已有操
作的码不变，例如 delete 遇到需要�
��近登录时仍是 account_unauthorized，�
��网时仍是 account_unavailable）。除 s
tatus、google_cancel、forget、session_adop
t 外都会阻塞网络（session_adopt 不�
�网，但可能等另一个进程的刷新�
�多 20 秒）：放在工作线程上调用�
�� */
char *msime_client_account(const uint8_
t *request, size_t length);
/* 昵称草稿�
�纯计算，不创建会话、不读文件�
�不联网。请求最多 16384 字节的 UTF
-8 JSON {name, id}，id 是账号 UUID，编�
��草稿时可传空串；未知字段、字�
��类型错误、无效缓冲区和 UTF-8 都
返回 account_invalid，不回显输入。Va
lue 为 {normalized, preferred, valid, maximu
m_chars, maximum_utf16_units}：normalized �
�既有设置界面的 JavaScript trim 去首
尾空白（含 BOM，不含 U+0085）；vali
d 直接复用账号改名的校验（规范�
��后非空、最多 64 个 Unicode 码点、
没有控制字符）；preferred 在 normali
zed 为空时是「水杉小鹿·」加 id �
�六个字符的大写，否则就是 normali
zed。空白或不合法的草稿仍成功返
回，只把 valid 设为 false；宿主只�
�提交 valid 为 true 时的 normalized。ma
ximum_utf16_units 是容纳 maximum_chars 个
码点所需的 UTF-16 单元上限（当前 
128），给控件限制输入长度；实际
合法性只看 valid。 */
char *msime_clien
t_account_name(const uint8_t *request, size_t
 length);
/* 流式对话（#8201 的决定 1
5）：与 msime_client_account 的 chat 相�
��的请求、预检和会话选择，只是�
�� stream:true 请求 POST /v1/chat/completio
ns，回复每到一段就交给 on_delta，�
��且能取消。request 最多 1 MiB：{dire
ctory?|session_store?, legacy_directory?, cli
ent_agent?, request_id, messages, model}，di
rectory、legacy_directory、client_agent、s
ession_store 与 msime_client_account 相同�
��恰好给 directory 或 session_store 之�
�）；request_id 是宿主为这次对话取
的 1–96 个 [A-Za-z0-9_-]，取消时用�
�；messages、model 与 chat 相同，序列
化后超过 65536 字节是 account_too_larg
e。登录账号优先，没有登录时用�
�机匿名账号，与 chat 相同；服务�
�以 401 拒绝令牌时刷新一次、在同
一个账号里重试，被拒的那次没有
交出增量。
 * on_delta 可为 NULL；否
则每收到一段非空的增量就在调用
本函数的线程上调用一次：text 是�
��段增量的 UTF-8 字节（length 个，�
�以 NUL 结尾，不会把一个字符拆在
两段里），只在回调期间有效；按
到达顺序依次调用，不会并发，本
函数返回以后不会再调用。回调里
不得重入 msime_client_*，唯一的例外
是 msime_client_account_chat_cancel（调用
回调时 Rust 不持有登记表的锁，也
不持有会话锁）。增量可能只有空
白；所有增量连起来就是成功时的
 content。
 * 成功时 Value 是 {content}�
��整段回复，最多 16 KiB，与 chat 相
同。不认识 stream 的旧后端回 400 in
valid_chat_request 时退回非流式请求�
�整段回复作为一段增量交出；服�
�端回了 200 却不是 text/event-stream �
�同样按非流式的 JSON 读。失败时 e
rror 是账号错误码，与 chat 相同的�
��则：开始输出之前的失败带状态�
��的细节（被限流是 account_rate_limit
ed，带 retry_after_seconds；503 server_bus
y 也是带 retry_after_seconds 的 account_r
ate_limited；没拿到响应是 account_netw
ork；5xx 是 account_unavailable）。开始
输出以后：服务端发来上游失败的
事件（data: {"error":...}）、回复超�
� 16 KiB、含换行、回车和制表符以�
��的控制字符、或只有空白就结束�
��是 account_unavailable；没等到 data: [
DONE] 连接就断开、读到一半出错或
到了 125 秒的总时限时是 account_netw
ork；这两种情况下已经交出的增量
不收回，宿主自己决定留不留。同
一个 request_id 正在进行时是 account_
busy，原来那次不受影响。回答完�
�以后与 chat 一样核对给出令牌的�
�号，期间退出或换了号时是 account
_cancelled，这时增量已经交出去了�
�宿主要丢掉它们。
 * 取消：msime_c
lient_account_chat_cancel 之后本函数立�
��以 account_cancelled 返回（等服务端
第一个字节、或两段增量之间的等
待都会立刻结束），不再调用 on_de
lta；在别的线程上取消时，正在进
行的那一次回调照常结束。连接在
后台关闭：Rust 的 HTTP 客户端不能�
��别的线程打断一次读取，读取线�
��在服务端下一次发来数据时（最�
��到 125 秒的总时限）发现调用方�
�经走了，断开连接，期间只占这�
�个线程和这一条连接。最长阻塞 1
25 秒；放在工作线程上调用。 */
ty
pedef void (*msime_client_account_chat_delta_
callback)(void *context, const uint8_t *text,
 size_t length);
char *msime_client_account_c
hat_stream(const uint8_t *request, size_t len
gth, msime_client_account_chat_delta_callback
 on_delta, void *context);
/* request 最多 
4096 字节：{request_id}。取消那次流�
��对话，Value 是当时是否有一个在�
��行。没有在进行的记下这个 id（�
��记最近 64 个），随后用同一个 re
quest_id 开始的 msime_client_account_chat_
stream 立即以 account_cancelled 返回：�
��主在调用 msime_client_account_chat_stre
am 之后、Rust 登记 id 之前取消，也
不会丢。所以取消过的 request_id 不
要再拿来开始新的对话。任意线程
上调用，立即返回，也可以在 on_de
lta 里调用。请求不合法时 error 是 
account_invalid。 */
char *msime_client_acco
unt_chat_cancel(const uint8_t *request, size_
t length);
/* 账号释义的 sans-IO 一半�
��给在按键路径上自己发 HTTP 的宿�
��（Windows 的 curl、Linux）：不联网�
��不读会话，bearer 由宿主自己加。
Request 是 length 字节（最多 4 MiB）�
� UTF-8 JSON。{operation:"request",texts,sou
rce_lang,target_lang} → {method:"POST",path
:"/v1/translate",body}：body 是要发的 JS
ON 文本，校验与 msime_client_account �
� translate 相同（序列化后超过 65536
 字节是 account_too_large）。{operation:
"parse",texts,status,body,retry_after?} → {
translations:[string|null]}：texts 是请求
时的原文，status 和 body 是收到的�
�态码和正文，retry_after 是 Retry-Afte
r 头的原文；2xx 按 translate 的规则�
��读（包括超过 4096 字节的一项是 
null，结果可以直接交给 msime_client_
apply_translations），其余按状态码和
服务端的错误码映射成账号错误码
，带 retry_after_seconds（只认整数秒�
��。失败时 error 是账号错误码（acc
ount_invalid、account_too_large、account_un
available、account_unauthorized、account_ra
te_limited 等）。可以在任意线程上�
��用。 */
char *msime_client_account_gloss(
const uint8_t *request, size_t length);
/* �
�生设置窗口的社区操作，与共享�
�用（Tauri）在 Windows 上的社区命令
相同：候选窗皮肤社区（candidate_sk
in_community_* 十四个）、插件社区（
plugin_community_* 七个）、社区词包�
�回复模板（community_resource_* 七个�
�和举报（community_report）。经 msime_
client_account 里同一个账号会话对象
认证（同一份 account-session.json，刷
新时共用 account-refresh.lock）；keyboa
rd_skin_download、keyboard_skin_install 和 
report 登录账号优先，没有登录时�
�本机匿名账号（带 directory 时是同
一 directory 里的 anonymous-session.json�
�带 session_store 时是该 store 的 ANONYM
OUS 项），两个都没有时仍是 communi
ty_unauthorized，其余要登录的操作只
认登录账号。这一点与共享应用的
 community_report 不同，那里仍只认登
录账号。Request 是 length 字节（最�
� 4 MiB）的 UTF-8 JSON {directory, legacy_d
irectory?, skins_directory?, state_root?, act
ion}（只有 keyboard_skin_publish 可以到
 9,000,000 字节，设计里可能带着照�
��背景）：directory 与 legacy_directory 
同 msime_client_account，也可以同样用
 session_store 代替它们（经该 store �
�登录会话认证，两者都给、都不�
�或 id 没有注册时是 community_invalid�
��；skins_directory 是候选窗皮肤根目
录的绝对路径（Windows 是 <状态目�
�>\skins，与共享应用相同），下面�
��了「要 skins_directory」的操作必需
；state_root 是放 plugins\ 的状态根目
录的绝对路径（与 msime_client_plugins
 的 state_root 相同），标了「要 stat
e_root」的操作必需。id 都是 UUID 字
符串；发布时的 id 由宿主随机生�
�，同一次发布重试时沿用，服务�
�据此去重。
 * 候选窗皮肤：{operat
ion:"candidate_skin_list",offset,search,mine,
category?} → {skins:[...],has_more}（categ
ory 不给时列出全部分类；mine 要登
录）。{operation:"candidate_skin_detail",i
d} → 一项。{operation:"candidate_skin_pr
eview",id} → {dataUrl}（PNG 或 JPEG 的 d
ata: URL，类型和文件头已校验）。{
operation:"candidate_skin_install",id,replace
}（要 skins_directory）→ 下载、装进
皮肤根目录并记下来源（同步不会
把别人的作品传进自己的库），返
回重新扫描的 {directory,packages,issues
}；replace 为 false 而同名文件夹已�
�在时 error 是 candidate_skin_exists，先
问用户再以 true 重试。{operation:"can
didate_skin_pack_preview",skin_id,visibility?
:"public"|"private"}（要 skins_directory，
visibility 缺省 public；私有的可以没
有素材许可）→ {suggestedName,license:
{code,assets,source},fileCount,size}，不上
传。{operation:"candidate_skin_add_preview"
,skin_id,image}（要 skins_directory）：im
age 是宿主为没有预览图的已装皮�
�画好的 PNG 或 JPEG（最多 256 KiB）�
�标准 base64，写进包并加进清单 →
 重新扫描的目录。{operation:"candidat
e_skin_add_license",skin_id,assets}（要 ski
ns_directory）：把用户选的素材许可
写进清单 → 重新扫描的目录。{ope
ration:"candidate_skin_publish",skin_id,id,na
me,description,visibility?,category?}（要 s
kins_directory）→ 发布后的一项；同
步已放进库里的同一个包就地更新
，不会留下第二行。{operation:"candid
ate_skin_rate",id,stars:1..5} → {stars}。{
operation:"candidate_skin_set_visibility",id,
visibility} / {operation:"candidate_skin_set_
category",id,category} → 更新后的一项
。{operation:"candidate_skin_unpublish",id}�
��要 skins_directory）→ {deleted:true}，
本地文件夹保留，同步状态里忘掉
这一行。{operation:"candidate_skin_sync"}
（要 skins_directory）→ {uploaded,downlo
aded,deleted_local,deleted_cloud,skipped:[{pa
ckage_id,code}],stopped}；downloaded 或 del
eted_local 非空时皮肤根目录变了，�
��主用 msime_client_skin_catalog 重新扫�
��。未登录时 sync 在读本地会话这�
��步就以 community_unauthorized 结束，�
��发请求。
 * 插件：{operation:"plugin
_list",offset,search,kind?,mine?} → {plugin
s:[...],has_more,skipped}。{operation:"plugi
n_detail",id} → 一项。{operation:"plugin
_pack_preview",kind,plugin_id}（要 state_ro
ot）→ {suggestedName,suggestedDescription,
version,license,fileCount,size}，不上传�
�{operation:"plugin_publish",kind,plugin_id,i
d,name,description}（要 state_root）→ �
�布后的一项；归档是确定的，同�
� id 重试时内容相同。{operation:"plug
in_install",id,kind,plugin_id}（要 state_ro
ot）：kind 与 plugin_id 是列表里那一
项的，下载到的若是别的包就拒绝
；整个替换已装的同 kind 同 id 的�
�（先问用户）→ 装好的 PluginSummar
y，与 msime_client_plugins 的 catalog 里�
��项相同。{operation:"plugin_rate",id,sta
rs} → {stars}。{operation:"plugin_delete",
id} → {deleted:true}，已装的包保留�
�
 * 词包与回复模板：{operation:"reso
urce_list",kind:"dictionary"|"reply"|"phrase"
,scope:""|"mine"|"saved",search,offset} → {
items:[...],has_more}。{operation:"resource_
detail",id} → 一项。{operation:"resource
_publish",id,kind,name,description,content,re
vision} → {id,revision}。{operation:"resou
rce_apply",id,resource_revision} → {revisio
n,imported,resource_revision}（把词包导�
��账号云端词库）。{operation:"resourc
e_save",id,saved}、{operation:"resource_rate
",id,stars}、{operation:"resource_unpublish"
,id} → null。
 * 键盘皮肤（与移动�
�� Tauri 的 community_skin_* 命令相同）
：{operation:"keyboard_skin_list",offset,sea
rch,mine?,category?} → {skins:[...],has_mor
e}（mine 缺省 false，为 true 时要登�
�；category 不给时列出全部分类）�
�{operation:"keyboard_skin_detail",id} → �
�项。{operation:"keyboard_skin_download",id
} → TouchKeyboardSkinDesign。{operation:"k
eyboard_skin_install",id,name}（要 state_ro
ot）→ {skin,trial}：下载后开始试用
并导入皮肤库，导入失败时结束试
用、不保留，与 msime_client_community_
skin_install 相同，本机这一步的失�
�码也相同（community_storage、community
_trial_format、community_skin_library_full�
�community_skin_invalid_name、community_skin
_duplicate_name、community_not_found、commu
nity_skin_library_format）。{operation:"key
board_skin_rate",id,stars:1..5} → null。{o
peration:"keyboard_skin_publish",id,name,desc
ription,design,category?} → null（category
 不给时由服务端归入默认分类）�
�{operation:"keyboard_skin_set_category",id,c
ategory} → 更新后的一项。{operation:
"keyboard_skin_unpublish",id} → null。
 * 
举报：{operation:"report",kind:"skins"|"ca
ndidate-skins"|"plugins"|"dictionaries"|"repl
ies"|"phrases",id,reason,detail?} → null；
reason 是 msime_client_community_moderation 
的 reasons 之一，detail 最多 1000 个�
�符。
 * 失败时 error 是社区错误码
：community_invalid（请求或参数不合�
��、缺了要的目录）、community_unauth
orized、community_forbidden、community_not_
found、community_rate_limited、community_ca
ncelled、community_storage、community_confl
ict、community_unavailable、community_block
ed_content、community_screening_unavailable�
��community_account_banned；本机打包、�
��装、补图、补许可的规则失败是 
candidate_skin_* 或 storage（皮肤），�
�插件的码（插件），插件失败时�
�能另有 detail，是违反的那条规则�
��中文说明。除 pack_preview、add_previ
ew、add_license 外都会阻塞网络；全�
��放在工作线程上调用。 */
char *msi
me_client_community(const uint8_t *request, s
ize_t length);
/* AI 键盘皮肤，与移动
端 Tauri 的 ai_skin_generate 相同；登�
�账号优先，没有登录时用本机匿�
�账号（带 directory 时是同一 director
y 里的 anonymous-session.json，带 session
_store 时是该 store 的 ANONYMOUS 项）�
�两个都没有时是 account_unauthorized�
�Tauri 的 ai_skin_generate 仍只用登录�
�号。一次生成从头到尾用开始时�
�中的那个账号；插画任务归创建�
�的账号，用匿名账号建的任务在�
�录以后 get、delete 是 account_not_found
。msime_client_ai_skin_generate 和 msime_cl
ient_ai_skin_job 按 msime_client_account 的
方式选会话：{directory, legacy_director
y?} 或 {session_store}，恰好给一个，�
��则是 ai_skin_invalid。失败时 error �
� ai_skin_invalid（请求、请求 id 或提
示词不合法，也包括服务端或模型
返回的方案、插画不合规，与 Tauri
 相同，两种情况共用这个码）、ai
_skin_busy（同一个 request_id 正在生�
�）、ai_skin_cancelled、ai_skin_unavailabl
e（进程内的请求登记表不可用，�
�在之前有线程 panic 时出现）或账�
��错误码，账号错误码与 msime_client
_account 的 #8201 新操作相同：account_
unauthorized、account_rate_limited（服务�
��限流，带 retry_after_seconds：{"ok":fa
lse,"error":"account_rate_limited","retry_aft
er_seconds":7200}）、account_unavailable（
服务端 5xx，例如没开 AI 皮肤或任�
��繁忙的 503，后者带 retry_after_secon
ds）、account_network（请求没有拿到�
��应）、account_not_found（任务不存�
�或已过期）等。Tauri 的 ai_skin_gener
ate 不变，那里的账号错误码与以�
�相同。除 cancel 外都会阻塞网络：
放在工作线程上调用。 */
typedef voi
d (*msime_client_ai_skin_progress_callback)(v
oid *context, uint32_t completed);
/* request
 最多 65536 字节：{directory?|session_st
ore?, legacy_directory?, request_id, prompt}�
��request_id 是宿主为这次生成取的 1
–96 个 [A-Za-z0-9_-]，取消时用它；p
rompt 是 1–500 个字符、不含控制字
符。Value 是三套 [{name,description,desi
gn,artworkPrompt,artwork:{b64_json,mime_type,
width,height}}]，与 Tauri 的 AiSkinProposa
l 相同，大约 30 MiB。progress 可为 NU
LL；否则每完成一张插画就在调用�
��函数的线程上调用一次，completed 
从 1 到 3 递增，不会并发，本函数
返回以后不会再调用。回调里不得
重入 msime_client_*，唯一的例外是 ms
ime_client_ai_skin_cancel（调用回调时 R
ust 不持有登记表的锁，也不持有�
�话锁）。出图线程遇到 401 时会刷
新令牌，这时注入存储的回调在 Ru
st 自己的线程上发生，按 msime_clien
t_session_store 的「任意线程」约定�
�理。最长约 5 分钟。 */
char *msime_c
lient_ai_skin_generate(const uint8_t *request
, size_t length, msime_client_ai_skin_progres
s_callback progress, void *context);
/* reque
st 最多 4096 字节：{request_id}。取消
那次生成，Value 是当时是否有一个
在跑；生成随后以 ai_skin_cancelled �
�束，已经开始的插画任务在返回�
�删掉。没有在跑的记下这个 id（�
�记最近 64 个），随后用同一个 req
uest_id 开始的生成立即以 ai_skin_canc
elled 结束：宿主在调用 msime_client_a
i_skin_generate 之后、Rust 登记 id 之�
�取消，也不会丢。所以取消过的 r
equest_id 不要再拿来开始新的生成�
�任意线程上调用，立即返回，也�
�以在进度回调里调用。 */
char *msim
e_client_ai_skin_cancel(const uint8_t *reques
t, size_t length);
/* 单个插画任务，�
�自己轮询的宿主。request 最多 65536
 字节：{directory?|session_store?, legacy_
directory?, action}，action 是 {operation:"
create",prompt}（prompt 去掉首尾空白�
� 1–1200 个码点、不含 NUL）、{opera
tion:"get",id} 或 {operation:"delete",id}（
id 是 48 位小写十六进制）。create �
�� get 的 Value 是 {id,state:"running"|"suc
ceeded"|"failed",artwork?,reason?,expires_at?
}：artwork 只在 succeeded 时有，形状�
��上；reason 是失败的原因码；expire
s_at 是任务过期的 RFC 3339 时间，服
务端没给或不合法时没有。delete �
� Value 是 null，进行中的任务同时�
�消。服务端每个账号每天最多 10 �
��任务，超了是 account_rate_limited，r
etry_after_seconds 是到配额恢复的秒�
�。 */
char *msime_client_ai_skin_job(const 
uint8_t *request, size_t length);
/* 云词�
�面板的请求，与 Windows 设置应用�
� cloud_dictionary_request 用同一份分发
，会话是 msime_client_account 里的同�
�个对象（同一份 account-session.json�
�或注入的存储）。Request 是 length �
��节（最多 4 MiB）的 UTF-8 JSON {direct
ory, legacy_directory?, snapshot_directory?, 
options?, action}：directory 与 legacy_dire
ctory 同 msime_client_account，也可以同
样用 session_store 代替它们；snapshot_
directory 是本用户私有的临时目录�
�绝对路径（Windows 设置应用用 %LOCA
LAPPDATA%\<tauri identifier>\dictionary-snaps
hots），放预览下载和导出、恢复�
�中转文件，不能是下面的快照队�
�目录，所有 snapshot_* 操作和 export_
file 必需；它在本进程里第一次用�
��时会清掉上次运行留下的 download-
*.ndjson、export-*.ndjson、export-*.tsv、r
estore-*.ndjson 和原子写入的临时文�
�，所以不要让两个同时运行的进�
�共用一个；options 是本机的 HostOpti
ons 文档（对象，user_data 必须是绝�
��路径），snapshot_preview、snapshot_enq
ueue、snapshot_status、snapshot_cancel 必�
��，快照队列放在 user_data 的上一�
�目录下的 dictionary-snapshots，暂存�
� dictionary-snapshot-staging。
 * 账号云
词库，action 与 cloud_dictionary_request 
的相同，返回形状也相同：{operatio
n:"list",kind,offset,search} → 一页 {entr
ies,has_more,offset}；{operation:"catalog",k
ind,code,offset,scheme,profile} → {catalog_
entries,has_more,offset,revision,normalized}�
��{operation:"changes",after,limit}；{operat
ion:"add"|"update"|"edit_catalog"|"candidates
"|"remove_candidate"|"fixed_positions"|"set_f
ixed_position"|"delete"|"import",...}；{oper
ation:"rank",...} → {revision,changed,selec
tion_count}；{operation:"export",kind,format
} → {text,filename}；{operation:"export_fi
le",kind,format,path} → {path,filename,byte
s}（#8201 的 P7）：与 export 相同的�
�求，正文最多 384 MiB，不放进响应
，而是边下载边写进 snapshot_director
y 里的 export-*.tsv（不进内存，要求
与 export 相同：非空、UTF-8、不含 N
UL），通过以后才经 path 旁边的临�
��文件 .msime-export-* 复制过去换入 p
ath；预检和清理与下面的 snapshot_ex
port_file 相同：path 是宿主选的保存
位置的绝对路径，上级目录必须已
经存在、不经过符号链接，否则在
下载之前就是 account_invalid；未登�
�、联网失败、正文不合格或写入�
�败时 path 上原有的文件原样保留�
�中转文件总会删掉；filename 是与 e
xport 相同的建议文件名 dictionary-<ki
nd>.tsv，bytes 是写入的字节数；要�
� snapshot_directory。kind 是 pinyin、wubi
、wubi98、quick、english 之一，format �
�� standard 或 windows。
 * 完整快照：
{operation:"snapshot_status"} → 快照队�
�状态（不含账号 id），面板轮询�
�它也会处理当前账号排队中的请�
�，最多每 60 秒一次；{operation:"snap
shot_preview"} → {previewToken, snapshot}�
�下载云端整份词库到 snapshot_directo
ry 并校验；{operation:"snapshot_enqueue",
token} → 队列状态：预览之后账号�
��换、云端也没改动时入列，并马�
��请输入会话放开词库、在本进程�
��准备并激活新的一代，放不开时�
��在队列里；{operation:"snapshot_cancel"
} → 队列状态，丢掉预览并取消当
前账号排队中的请求。整份快照可
能有 512 MiB，所以备份与恢复走文�
��：{operation:"snapshot_export_file",path} 
→ {path,filename,snapshot}：path 是宿主
选的保存位置的绝对路径（上级目
录必须已经存在、不经过符号链接
，否则在下载之前就是 account_invali
d），先下载到 snapshot_directory 校验
，通过以后才经 path 旁边的临时文
件 .msime-export-* 复制过去换入 path�
�未登录、联网失败、校验不过或�
�入失败时 path 上原有的文件原样�
�留；进程在复制中途被杀时留在 p
ath 旁边的临时文件，下次往同一�
�目录导出时删掉（只删超过一小�
�没有改动的）；{operation:"snapshot_re
store_preview_file",path} → {snapshot,expec
tedRevision}：path 是宿主选的备份文�
��（普通文件、不跟随符号链接、�
��多 512 MiB 的 UTF-8），复制进 snapsh
ot_directory 校验，再读云端当前的 r
evision；{operation:"snapshot_restore_file",
path,expected_sha256,revision} → {revision,
reset}：expected_sha256 是预览给出的 s
napshot.sha256，文件在预览之后变了�
�� account_invalid，revision 是预览给出
的 expectedRevision，云端在这之后被�
��过是 account_conflict。snapshot 是快�
�元数据，不含 fileSha256 和 engineReco
rds。文本版的 snapshot_export、snapshot
_restore_preview、snapshot_restore，以及 
macOS 的 snapshot_restore_native、snapshot_
restore_cancel、snapshot_choose_restore 都�
�� snapshot_unavailable。
 * 失败时 error
 是 invalid_cloud_dictionary（请求或参�
��不合法、缺了要的目录或 options�
�directory 与 session_store 不是恰好给�
��个、id 没有注册），会话问题是 
account_* 码，快照问题是 snapshot_busy
、snapshot_conflict、snapshot_invalid、sna
pshot_unavailable。会阻塞网络、读写�
��件，「应用到本机」还要等输入�
��话放开词库：放在工作线程上调�
��。 */
char *msime_client_cloud_dictionary(
const uint8_t *request, size_t length);
/* op
tions is a readable UTF-8 buffer of length by
tes; maximum 1 MiB.
 * Object: api_version=1,
 resources/user_data/cache/dictionaries (abso
lute paths),
 * preferences={scheme, candidat
e_page_size, learning, chinese_punctuation,
 
*              shuangpin_profile?, shuangpin_
custom_profile?, wubi_profile?}。缺省 shua
ngpin_profile 为 xiaohe，
 * 可选 xiaohe�
��ziranma、shoudao、microsoft、smart_abc�
�custom。custom 读 shuangpin_custom_profile
（{initials, finals, zero_initials}，各是
单位到键的对象），表缺失或不合
法时按 xiaohe 建会话。缺省 wubi_prof
ile 为 wubi86，可选 wubi86、wubi98。不
认识的取值一律拒绝。
 * Optional pr
eferences_directory is bootstrap metadata for
 host file monitoring;
 * session creation it
self does not monitor or load it.
 * Optional
 phrase_preedit=true asks for a half-composed
 phrase to stay in the
 * composition: pickin
g a candidate that covers only part of the in
put leaves the
 * chosen piece in view.phrase
_prefix instead of committing it, and the who
le
 * phrase commits at once when the composi
tion ends. The host must draw that
 * field a
head of editing_text - it is separate because
 caret_position is an
 * offset into editing_
text in the host's own string unit. Omitted m
eans each
 * piece is committed as it is pick
ed, as before.
 * The host must prepare and v
alidate its dictionary generation before crea
tion.
 * Creation acquires cooperative shared
 access to user_data and dictionaries until
 
* destroy. It fails immediately while a parti
cipating maintenance writer holds
 * exclusiv
e access. Existing sessions are never cancell
ed for maintenance.
 * Linux hosts may provid
e absolute online_provider_socket and
 * tran
slation_provider_socket paths for user-manage
d Unix-socket services;
 * translation may re
use the online socket when omitted.
 * prefer
ences_directory also names the plugins root, 
<preferences_directory>/plugins: the
 * insta
lled plugin packs, the enabled command tables
 the "/" mode reads and the "@" mode's
 * men
tions.json. They are read at creation, on pre
ference updates and when a field gains
 * foc
us, and only while the mode that uses them is
 on. Optional sound_packs is the absolute
 * 
path of the bundle's built-in sound packs; ab
sent means sound-packs beside resources.
 * O
ptional plugin_packs is the absolute path of 
the official plugin packs the desktop
 * inst
allers ship (<kind>/<id>/ folders: the uuid s
cript plugin, the latex-symbols phrase
 * tab
le); absent means plugin-packs beside the bui
lt-in sound packs, and nothing when that
 * d
irectory is not there (mobile, development bu
ilds without them).
 * Do not delete .msime-d
ictionary-access.lock files. Legacy/external 
writers do
 * not participate; preparation/up
grades still require stopped sessions.
 */
ch
ar *msime_client_create(const uint8_t *option
s, size_t length);
/* Management JSON (<=2248
576 bytes), trusted native caller only:
 * {o
ptions: <same HostOptions as create>, action:
 {operation:"list",offset:0,limit:100}}
 * Li
st 的 kind 与 query 可选：ASCII 查询�
�配编码前缀，含非 ASCII 字符时匹�
��词条字面内容。无查询时列个人�
��条；指定 kind 与非空查询时包含�
��置行，quick_phrase 空查询也包含内
置行。个人行优先；user_only:true 始
终只列个人词条，在整库筛选后分
页。
 * or action:{operation:"edit",previou
s:null|Entry,replacement:null|Entry,request_i
d:"..."}.
 * Batch import: action:{operation:
"import",kind:"pinyin"|"wubi"|"wubi98"|"quick
_phrase"|"english",
 * format:"standard"|"win
dows"|"rime"|"hans",text:"word<TAB>code<TAB>w
eight\\n",request_id:"..."}.
 * Standard rows
 are word, code, weight; Windows rows are cod
e, word, weight. Rime rows are
 * word, code,
 optional weight and may include YAML front m
atter between --- and ...; Rime's
 * metadata
 weights (for example c=3 d=0.12) use the def
ault 10000. Omitted weights use 10000.
 * The
 hans format is pinyin-only and accepts one p
ure Han phrase per line; Engine resolves
 * e
ach phrase to its highest-ranked canonical pi
nyin and uses weight 10000.
 * Import accepts
 at most 1000 rows and returns {applied}; row
s are committed
 * with deterministic receipt
 IDs derived from request_id so retries are s
afe.
 * Export: action:{operation:"export",ki
nd,format:"standard"|"windows",offset,limit} 
returns
 * {text,has_more}; use pages of at m
ost 1000 rows. Standard output is word, code,
 weight.
 * Entry:{kind:"pinyin"|"wubi"|"wubi
98"|"quick_phrase"|"english",key,value,weight
,source?:"user"|"bundled"}.
 * List returns {
entries,has_more} and sets source on every en
try; edit returns {applied:true}. Errors are 
redacted.
 * A bundled entry passed back as p
revious can only be re-weighted (replacement 
with the same kind, key and value) or deleted
 (replacement null); anything else fails with
 "bundled dictionary entry is read-only". Exp
ort of pinyin also carries the weights set or
 learned for bundled words and omits single c
haracters; the other kinds export user words 
only.
 * 计数（只读）：action:{operati
on:"count",kind?,user_only?:bool} 返回 {cou
nt,kinds:{kind:n},complete}；user_only:true 
只数用户自己的词，否则拼音还会
数上学到或设过权重的随包词（也
就是导出的那些行）。扫描在 1,000
,000 行处停下时 complete 为 false。
 *
 快照导出：action:{operation:"export_sna
pshot",destination:绝对路径,include_learn
ing?:bool} 把用户的词写成与 GET /v1/u
sers/me/dictionary/snapshot 相同的 NDJSON 
msime-dictionary-snapshot v1 文档（header�
��每个词一条 entry 和一条 overlay、�
��正文 SHA-256 的 footer；revision 为 1�
��，用云端格式自己的校验器检查�
��返回那份元数据加上 path。不需�
�账号。include_learning 缺省为 false，
这时不含位置和选择，输出与这个
参数加入之前逐字节相同，云同步
上传用的就是它；为 true 时（本地
备份用）在用户的词之后再写输入
记录：学习调权写成 user_inserted:fal
se 的 overlay、删除记录写成 deleted:t
rue 的 overlay、固定位置写成 position
、选词计数写成 selection（截到 0..1
0），格式装不下的行跳过，返回�
�多出 learning 和 learning_skipped 两个�
��数；日志整体读不出来时照样导�
��词，learning 为 0，原因在 learning_e
rror。
 * 输入记录：action:{operation:"
learning_count"} 只读，返回 {count}，�
�本机日志里学习调权、删除记录�
�固定位置和选词计数的条数（不�
�用户自己的词）。action:{operation:"q
ueue_learning_merge",source:绝对路径} 校
验快照文件 source，把其中的输入�
�录另存成 <preferences_directory>/pending
-learning-merge.ndjson（替换已有的一�
�），返回 {queued,learning}；没有输�
�记录时 queued 为 false、什么也不写
。action:{operation:"merge_pending_learning"
} 在没有会话时合并这份文件（要�
��占维护权，忙时报 dictionary mainten
ance busy、文件留着）：本机已有的
词、固定的词和被占用的位置保留
本机，选词计数取较大的那个，返
回 {merged:false}（没有待合并的）或
 {merged:true,entries,positions,selections,ke
pt,skipped}；合并期间新排的一份不�
��影响，连续失败三次放弃并删掉�
��件（learning merge abandoned）。
 * 本
地备份的校验与输入习惯：action:{o
peration:"inspect_snapshot",source:绝对路�
��} 只读地按云端格式完整校验一�
�词库快照并返回它的元数据。actio
n:{operation:"export_habits",destination:绝�
��路径} 把日志里的输入习惯（整�
�联想的二元/三元计数、连续选词�
��、拼写纠错计数、自动纠错抑制�
��置顶的候选）写成 NDJSON（header fo
rmat:"msime-learning-habits" version:1、每�
��一行、footer 带条数和正文 SHA-256�
��，装不下的行跳过，返回 {path,hab
its,skipped}。action:{operation:"inspect_hab
its",source} 只读校验，返回 {habits}�
�action:{operation:"queue_habits_merge",sourc
e} 校验后存成 <preferences_directory>/pe
nding-habits-merge.ndjson，返回 {queued,ha
bits}；merge_pending_learning 随后在空�
�时一并合并它（计数取大、置顶�
�留本机，写完压回各表上限），�
�果多一个 habits 字段。learning_count 
另返回 habits（输入习惯行数，读�
�出来时为 null）。
 * 五笔查码（�
�读）：action:{operation:"wubi_codes",text
} 返回 {phrase,characters,truncated}。char
acters 按 text 里第一次出现的顺序�
�出每个不同的汉字（最多 64 个，�
��出时 truncated 为 true），每项是 {t
ext,wubi86:[...],wubi98:[...]}，编码短码
在前、码表里没有时为空数组；phr
ase 是整段文字作为一个词的同样�
�项，只在整段都是汉字、2 到 16 �
�字、且至少一张表有它时给出，�
�则为 null。读会话用的同一份五笔
码表，不拿维护权；text 超过 4096 �
��节时报 invalid wubi lookup。msime_clien
t_personal_dictionary_request 对它给出同
样的回答，不进队列。
 * Native host
 owns/authorizes paths; never accept arbitrar
y webview paths or log payloads.
 * Run on a 
worker thread. Edit returns busy until all pa
rticipating sessions are
 * destroyed, then h
olds exclusive access; recreate sessions afte
r success.
 * Do not automatically cancel use
r input to obtain access. Retry ambiguous wri
tes
 * with the identical nonempty request ID
 and content.
 */
char *msime_client_dictiona
ry(const uint8_t *request, size_t length);
/*
 The same {options, action} as msime_client_d
ictionary, as a settings page sends it: an ed
it,
 * import or reset first has the input ho
sts let go of the dictionaries (the Windows S
erver
 * over its auxiliary pipe) and is retr
ied until they have; an import of a whole fil
e (up to
 * 48 MiB of text) is sent in batche
s and answered with one report; and a refusal
's error is
 * the settings pages' code: dict
ionary_busy, dictionary_too_large, dictionary
_import_rejected,
 * dictionary_read_rejected
, dictionary_pinyin_unavailable, dictionary_u
navailable,
 * dictionary_reset_rejected, dic
tionary_bundled_readonly, dictionary_invalid_
entry,
 * dictionary_invalid_word or storage.
 {operation:"reset"} restores the bundled dic
tionaries
 * and clears all learned data, ret
urning {reset:true}. Blocks: call off the UI 
thread. */
char *msime_client_settings_dictio
nary(const uint8_t *request, size_t length);

/* Smart punctuation follow-up gestures. The 
host holds the snapshots: they belong
 * to i
ts editor, not to Engine, and a session rebui
lt while the keyboard was away
 * must not ca
rry a gesture across the gap. The switches th
at gate them live in the
 * applied preferenc
es, so the session answers rather than the ho
st keeping a copy.
 * arm: {ascii, commit, ti
mestamp_ms, editor_generation, auto_closed_pa
ir} ->
 *      {repeat: {ascii, committed, ti
mestamp_ms, editor_generation}|null,
 *      
 space: {chinese, ascii, editor_generation}|n
ull}
 * decide: {character, preceding, timest
amp_ms, editor_generation, repeat, space} ->

 *      {replace_with: "，"|null, space_asci
i: 46|null}
 * `preceding` is what the editor
 holds before the caret at the moment of the 
press;
 * both decisions re-read it and decli
ne when it disagrees with the arming, so a
 *
 stale snapshot can never rewrite the wrong c
haracter. A non-null space_ascii means
 * rep
lace the preceding mark with it and swallow t
he space. Maximum 4096 bytes. */
char *msime_
client_smart_punctuation_arm(uint64_t handle,
 const uint8_t *request, size_t length);
char
 *msime_client_smart_punctuation_decide(uint6
4_t handle, const uint8_t *request, size_t le
ngth);
/* Pure Engine validation/normalizatio
n for one Entry object. No paths or
 * sessio
n are required and no dictionary state is cha
nged. */
char *msime_client_dictionary_valida
te(const uint8_t *request, size_t length);
/*
 Plain Chinese words, one per line, answered 
as the pinyin entries the "hans" import forma
t would produce: {entries:[Entry]}. Reads onl
y the packaged main dictionary under resource
s, read-only; needs no prepared host or sessi
on and changes no dictionary state. */
char *
msime_client_dictionary_hans_entries(const ui
nt8_t *text, size_t text_length,
            
                               const uint8_t 
*resources, size_t resources_length);
/* 搜�
��细胞词库（.scel）转成标准 TSV 文
本：请求 {bytes_base64}（文件不超过
 8 MiB），回答 {name, category, entries, 
text}。text 每个词一行（词<TAB>音节
'音节，不带权重列），读不出读�
�或词的那一行必定被导入拒绝，�
�以导入报告里的行号就是词序；�
�主把 text 当作 standard 格式交给自�
��现有的导入。纯函数：不需要宿�
��选项，不碰任何词库状态，不记�
��词条内容。错误：dictionary_scel_uns
upported（不是细胞词库或版本不支�
��）、dictionary_scel_malformed（不完整
或已损坏）、dictionary_scel_empty、dic
tionary_too_large、dictionary_scel_invalid�
�请求或 base64 坏了）。 */
char *msime
_client_dictionary_scel_text(const uint8_t *r
equest, size_t length);
/* A dictionary file 
({kind, format, text}, as the "import" dictio
nary action) answered as the words the person
al dictionary queue accepts, with the import 
report: {entries:[Entry], applied, failed, tr
uncated, swapped, first_failures}. Invalid ro
ws are counted with their line, repeated word
s appear once, and at most 128 words are retu
rned. Reads only the packaged main dictionary
 under resources; changes no dictionary state
. Maximum 1,200,000 bytes. */
char *msime_cli
ent_dictionary_import_entries(const uint8_t *
request, size_t request_length,
             
                                const uint8_t
 *resources, size_t resources_length);
/* 个
人词库队列同步：请求是与 msime_cl
ient_create 相同的 HostOptions。
 * 一�
�最多应用 4 项，返回 {synchronized,pe
nding_count,failed_count,snapshot_error}。
 
* 整批先取得维护租约；词库仍有�
��跃会话时返回 dictionary maintenance b
usy，队列保持原样，稍后重试。 */

char *msime_client_personal_dictionary_sync(
const uint8_t *request, size_t length);
/* Sn
apshot lifecycle. Version is a redacted SHA-2
56 binding the canonical
 * resource/user/cac
he/dictionary paths and one consistent Engine
 journal.
 * Prepare/discard are native-only;
 a prepared handle is not active until a
 * f
uture activation transaction publishes it. Th
e prepare callback returns
 * one UTF-8 JSON 
record into the supplied buffer, 0 only at ve
rified EOF, and
 * a negative value for cance
llation, truncation, or checksum failure. */

typedef intptr_t (*msime_client_snapshot_next
)(void *context, uint8_t *buffer, size_t capa
city);
char *msime_client_snapshot_version(co
nst uint8_t *options, size_t length);
/* Vali
dates a complete host-private NDJSON file and
 returns bounded metadata only. */
char *msim
e_client_snapshot_inspect(const uint8_t *path
, size_t length);
/* Revalidates the exact ho
st-private file, then streams it to the accou
nt service.
 * request: {revision,expected_sh
a256,access_token}; expected_sha256 identifie
s
 * the complete file returned by inspect, n
ot only the checksummed snapshot body.
 * Blo
cking network and file I/O: call only from a 
worker thread. */
char *msime_client_snapshot
_restore(const uint8_t *request, size_t reque
st_length,
                                  
  const uint8_t *path, size_t path_length);
/
* Crash-safe queue state, enqueue, cancellati
on and idle processing. */
/* {operation:"imp
ort_android_v1", directory} → {imported, st
ate}：把 Android 旧版 Java 队列（Dicti
onarySnapshotQueue.java）的 state.bin 就�
�转成本队列的 state.json，在 state.lo
ck 的 flock 里做（等正在持锁的激�
�做完）；目录里已经没有 state.bin 
时不取锁直接返回，转换过以后这
一步不会排在激活后面，快照文件
两边同名（<id>.ndjson），原样留着�
��不复制。先原子地写好 state.json�
�对目录 fsync，再删 state.bin，中途�
��断时下一次接着做，队列不丢。�
��录不存在（不会创建）、没有 sta
te.bin、或者已经有 state.json（只补�
��留下的 state.bin）时 imported 为 fals
e，什么也不改；state 是转换以后�
�队列状态，形状同其他操作。state
.bin 读不懂（截断、魔数或版本不�
��、字段不合法）是 snapshot_invalid�
�两份文件都不动。其余会改队列�
�操作持锁时也先做同样的转换，�
�没转换时只读的读取直接解码 stat
e.bin，所以 Android 的主进程和 :ime �
��先用到队列都不会把旧队列当成�
��的。{operation:"process", directory, stag
ing_root, options, account_id?} 的 account_i
d 是当前账号，不知道当前账号是�
��（读不到会话或未登录）时不给�
��属于某个账号的请求这时原样留�
��、下次再处理，属于别的账号时�
��消；账号是 local-backup 的请求（An
droid 本地备份恢复排的，与 Java 的
 DictionarySnapshotQueue.LOCAL_RESTORE_OWNER 
相同，转换时原样保留）不属于任
何账号，不给 account_id 时照样认领
和激活，也从不因账号不同被取消
，只有 cancel 给的 account_id 正好是 
local-backup 时才取消。 */
char *msime_c
lient_snapshot_queue(const uint8_t *request, 
size_t length);
char *msime_client_snapshot_p
repare(const uint8_t *request, size_t length,

                                     msime_c
lient_snapshot_next next, void *context);
cha
r *msime_client_snapshot_discard(uint64_t han
dle);
char *msime_client_snapshot_activate(ui
nt64_t handle, const uint8_t *expected_versio
n, size_t length);
/* The shared preference d
efaults as a JSON document. A host patching o
ne key of
 * a nested preference object needs
 that object's other fields: the object itsel
f
 * is optional, its members are not.
 */
ch
ar *msime_client_default_preferences(void);
/
* 「重置所有设置」：directory 是绝
对的偏好目录（UTF-8，<=16384 字节�
�。在偏好锁里读出当前文档，换�
�本目录所属版本的默认偏好（服�
�凭据与 fuzzy_pinyin.seeded 保留），�
�按 expected_revision 比较并交换写回�
��返回新的 PreferencesSnapshot。修订�
�不符时失败且不写入。词库、统�
�不受影响；默认关闭剪贴板历史�
�因此与 msime_client_save_preferences 一�
��清空已存的历史。会读写文件，�
��在工作线程调用。 */
char *msime_cli
ent_restore_default_preferences(const uint8_t
 *directory, size_t length,
                 
                              uint64_t expect
ed_revision);
/* 「导出设置」：directo
ry 是绝对的偏好目录（UTF-8，<=16384
 字节）。返回值 value 是设置文件�
��文（JSON 字符串，format 为 app.msime
.client.preferences），不含语音、AI �
�助和翻译服务的配置与密钥、诊�
�日志、使用统计和剪贴板历史开�
�。宿主负责让用户选位置并写盘�
�会读文件，请在工作线程调用。 *
/
char *msime_client_export_settings(const ui
nt8_t *directory, size_t length);
/* 「导�
�设置」：document 是用户选的设置�
�件内容（<=1048576 字节）。换算成�
��好（本机的服务配置与密钥、诊�
��日志、使用统计和剪贴板历史开�
��保留，按本目录所属的版本收窄�
��入方案；不会清空剪贴板历史）�
��按 expected_revision 比较并交换写回
，返回新的 PreferencesSnapshot。失败�
�� error 为 settings_document_invalid（不�
��设置文件）、settings_document_macos�
�macOS 原生设置窗口导出的文件）�
�settings_document_unsupported（更新版本
的文件或读不懂的设置）、settings_
conflict（设置已在别处更新）之一�
��或存储错误的说明；失败时不写�
��。会读写文件，请在工作线程调�
��。 */
char *msime_client_import_settings(c
onst uint8_t *directory, size_t directory_len
gth,
                                   uint6
4_t expected_revision, const uint8_t *documen
t,
                                   size_t 
document_length);
/* The transcription provid
er and optional rewrite this device is config
ured for, read from an
 * absolute preference
s directory. Response value: {provider:{...}|
null, polish:{...}|null}; both
 * absent mean
s nothing is configured and the host uses wha
tever it falls back to. Contains
 * credentia
ls: never log the response; release with msim
e_client_string_free. provider.requestFormat 
是请求格式："multipart"（OpenAI 兼容
的 /audio/transcriptions）、"chat_audio"�
�Chat Completions 带 input_audio，阿里云
百炼）、"doubao_websocket" 或 "local"；
宿主按它挑请求构造，不按 provider
 名字判断。 */
char *msime_client_mobile
_voice_configuration(const uint8_t *directory
, size_t length);
/* 全局主题选择器：
{themes:[{id,title,platforms,appearance,previ
ew,candidate,keyboard},...],default:"system"}
。只列出本库编译到的平台提供的
主题，按选择器顺序：system, native�
��仅 iOS）, shuishan, light, paper, night, 
ink, custom。platforms 为提供该主题的
宿主（如 ["ios"]），null 表示所有�
�主。appearance is "light"|"dark"|null; pre
view {background,panel,accent,text}, candidat
e and keyboard are the built-in palettes and 
are null for system, native and custom. Keys 
are snake_case. Hosts keep no copy of the ids
, titles or colours. */
char *msime_client_th
eme_catalog(void);
/* 解析所选全局主�
�的颜色。JSON 请求（<=1048576 字节�
�拒绝未知键）：{global_theme:string, c
ustom_theme?:Preferences.custom_theme, dark:b
ool, layout:"horizontal"|"vertical", skins_di
rectory?:皮肤根目录的绝对路径 | pac
kage?:candidate_skin_catalog 中的一项}。
global_theme 必须是八个主题 id 之一�
��system、native、shuishan、light、paper�
��night、ink、custom），其他 id 请求�
��败；native 不在本平台目录里时也
接受，解析结果与 system 相同，只�
�� id 为 native。custom_theme 按偏好同�
��校验。dark 是宿主当前的明暗模�
�，它决定自定义主题取哪个槽位�
�深色模式取 custom_theme.candidate_skin_
dark，没设时取 custom_theme.candidate_sk
in，浅色模式取 custom_theme.candidate_s
kin。皮肤包只在它清单 base 的明暗
下绘制：base 是固定明暗的内置主�
��（shuishan、night、ink 为深色，light
、paper 为浅色）且与 dark 不符时不
画，base 为 system 的包两种模式都�
�；设过皮肤却在这个模式下没有�
�画的包时，custom_theme.base 也只在�
�于当前明暗时作底，否则按 system�
��layout 是正在绘制的候选窗：皮肤
包只在清单声明的排列和模式下绘
制，只有画了它时 candidate_skin 才�
�空，所以宿主正好在 candidate_skin �
��空时画包的装饰和最小宽度。skin
s_directory 给扫描皮肤根目录的宿主
（除 Linux 外都是）；package 是 Linux
 candidate_skin_catalog 中当前模式槽位
指名的那一项，其他任何东西（包
括 msime_client_skin_catalog 的 SkinSummary
）都让请求失败。响应 value：{id,so
urce:"system"|"builtin"|"custom",appearance:"
light"|"dark"|null, candidate:{surface,border
,text,number,secondary,accent,selected,select
ed_text,selected_number,hover,show_selected_b
ar}|null, keyboard:{background,key,function_k
ey,text,secondary,accent,on_accent}|null, can
didate_skin:string|null}。appearance 非空�
��是返回的界面所处的明暗。keyboar
d.accent 是触屏候选条选中候选的文
字色（无填充）；回车键保留平台
强调色。on_accent 为黑或白，能在�
�调色填充上读清。每个颜色都是 #
RRGGBB 或 #RRGGBBAA。为 null 的调色板�
��槽位表示用宿主自己的原生 token�
��从不表示透明。根目录里没有、�
��盘上无效、或不是当前槽位指名�
��包会被略过而不是让调用失败。s
kins_directory 会读取皮肤包：在主题
、明暗或皮肤包变化时解析，不要
在绘制时调用。 */
char *msime_client_r
esolve_theme(const uint8_t *request, size_t l
ength);
/* 应用主题（Android 本地设�
�里的 app_theme，不在共享偏好里）�
��择器：{app_themes:[{id,title,season,seas
onal,light:{accent,accent_soft,on_accent,back
ground,card,hair},dark:{...同上}}],default:
"siji"}。id 按选择器顺序为 siji、chu
nya、xiayin、qiushan、dongxue；season 是
固定的那一季（"spring"|"summer"|"autum
n"|"winter"），siji 为 null 且 seasonal �
�� true，它的颜色固定画秋杉，目�
�因此不随时钟变化。颜色为 #RRGGBB
 或 #RRGGBBAA；accent_soft 是强调色加 
22（浅色）或 40（深色）透明度，�
��色 on_accent 为 #FFFFFF，深色 on_accen
t 是强调色与黑色 25% 比 75% 的混合
。纯计算。 */
char *msime_client_app_the
me_catalog(void);
/* 应用主题在宿主当
前月份与明暗模式下的颜色。JSON �
��求（<=4096 字节，拒绝未知键）{ap
p_theme:"siji"|"chunya"|"xiayin"|"qiushan"|"d
ongxue", month?:1-12, dark:bool}；month 是�
��主本地日历的月份，只影响 siji�
�省略时用 UTC 月份，超出 1-12 时请
求失败。返回 {id, season, accent, accen
t_soft, on_accent, background, card, hair}，
season 是实际画的那一季。纯计算�
�可在主线程调用。 */
char *msime_clie
nt_resolve_app_theme(const uint8_t *request, 
size_t length);
/* Per-key double-pinyin hint
 text for one profile name, as a JSON object 
mapping
 * an uppercase key to "initials / fi
nals" - or to whichever side that key carries
.
 * Read out of the Engine's own profile tab
les so a keyboard face never carries a
 * sec
ond copy of the keymap. An unknown profile na
me yields an empty object
 * rather than the 
default profile's hints. */
char *msime_clien
t_shuangpin_key_hints(const uint8_t *profile,
 size_t length);
/* The double-pinyin codes o
f the whole zero-initial syllables for one pr
ofile name, as a JSON object mapping each syl
lable to its two-key code, e.g. {"a":"aa","an
g":"ah",...}. Read out of the Engine's own pr
ofile tables. An unknown profile name yields 
an empty object. */
char *msime_client_shuang
pin_zero_initials(const uint8_t *profile, siz
e_t length);
/* 九键双拼方案在宿主�
�来的样子。request 是 JSON {"profile": 
"migala2"|"migala3"|"gushi2_low"|"custom", "t
able"?: {initials, finals, zero_initials}}，
table 是 custom 用的表（与 nine_key_shu
angpin_custom_profile 同形，键是数字�
�。Value 是 {effective, table, problem, key
s}：effective 是输入法实际用的方案
（custom 的表不能用时是默认的 miga
la3，与建会话时的回退相同），tab
le 是它的表，problem 是 custom 表的�
�题（形状同自定义双拼表的问题�
�只有 unknown_unit、duplicate_unit、missi
ng_unit、invalid_key 四种，能用时为 n
ull），keys 按 1 到 9、0 的顺序列出
每个用到的数字键的 {key, initials, f
inals, zero_initial}，键面从这里取，�
��主不另存键位。不留存任何东西�
��编辑表时每改一格都可以调用。�
��认识的方案名或多余的字段报错�
�� */
char *msime_client_nine_key_shuangpin_l
ayout(const uint8_t *request, size_t length);

/* 规范化中文触屏快捷符号：{symb
ols: string[]}，最多 64 KiB。标准响应
 value 为去重的单字素列表，最多�
�二项；无有效项时返回共享默认�
�。不落盘，用 string_free 释放。 */

char *msime_client_touch_quick_symbols(const 
uint8_t *value, size_t length);

/* Load Pref
erencesStore from an absolute UTF-8 directory
, without a session.
 * May block on disk/fil
e lock: use a worker thread. Returns Preferen
cesSnapshot.
 * Missing file returns shared d
efaults; malformed/future files return errors
.
 * Creates the directory/lock file if absen
t, never overwrites preference contents.
 */

char *msime_client_load_preferences(const uin
t8_t *directory, size_t length);
/* 私有的
聚合打字统计。JSON 请求（<=65536 �
�节）：
 * {directory:绝对路径,action:
{operation:"load"|"reset"}}
 * {directory,act
ion:{operation:"set_enabled",enabled:bool}}
 
* {directory,action:{operation:"set_retention
",retention,day:"YYYY-MM-DD"}}
 *   retention
 取 forever|30d|90d|180d|365d，其他值一
律按 forever 读，绝不当成更短的时
间窗。`day` 是调用方的本地日期。

 * {directory,action:{operation:"record",tex
t,source,day:"YYYY-MM-DD",hour?:0-23}}。
 * 
record 在内存里给上屏文本分类，�
�持久化聚合计数；文本既不返回�
�不存储。可能阻塞在磁盘或文件�
�上：请在工作线程调用。`hour` 是�
��次上屏的本地小时，必须与 `day` 
取自同一时刻；拿不准就省略，不
要猜，这一天的计数照样保留，只
是不按小时拆分。record 返回 {record
ed, milestone}：milestone 是总数刚刚越
过的成就计数（100、1000……），�
�有越过时为 null。只有本进程里有
会话开着成就音效时才计算它，而
在桌面宿主上此时提示音已经排进
队列。
 * {directory,action:{operation:"re
cord_keys",day:"YYYY-MM-DD",keys:{"KeyA":3,..
.}}}
 *   把各键的按键次数加到 `day
` 上，`day` 是这些按键发生的本地�
��期（跨过午夜的一批先按旧日期�
��出）。每个键只存每日按键次数�
��不存顺序、时间或文本。键 id 是
 W3C KeyboardEvent.code 名加上软键盘 id
（Nine0-Nine9、SoftPunctuation、SoftSymbol
、SoftLayer、SoftLanguage、SoftGlobe、Sof
tEmoji、SoftVoice）；完整列表是 clien
t-services typing_statistics.rs 里的 KEY_ID
S。只要有一个未知 id 或零计数就�
��绝整批；绝不要自造 id。返回 {re
corded:n}，统计关闭时为 0（什么都�
��写）。宿主在内存里攒批，从工�
��线程调用，绝不每按一个键调用�
��次。
 * {directory,action:{operation:"sum
mary",day:"YYYY-MM-DD",user_words?:n}} 返回
调用方本地 `day` 的派生指标（总�
�、习惯、按键、成就）；统计开�
�时，新解锁的成就在锁内写下。us
er_words 是词库 "count" 加 user_only:true
 的结果。隐私模式不影响它：它�
�读。
 * {directory,action:{operation:"reco
rd_voice",day,milliseconds}} 记一次语音�
��入的时长；{directory,action:{operation
:"record_skin",id}} 记一次用到的皮肤�
��两者都返回 {recorded}。
 * 「上传�
��云端」，给自己发 HTTP 的宿主（A
ndroid、iOS、鸿蒙）：开关、设备标
识和上次上传的时间存在 directory �
��的 typing-statistics-upload.json 里，构
建和十分钟的节流都在这里，宿主
只负责发请求。{directory,action:{opera
tion:"upload_status"}} → {enabled,lastUploa
dMs}（lastUploadMs 还没有上传过时为 
null）。{directory,action:{operation:"set_u
pload_enabled",enabled}} → 同上：只改�
��机状态、不联网；打开时第一次�
��成设备标识，关闭时清掉上次上�
��的时间（设备标识保留，重新打�
��时服务端仍认作同一台设备）。�
��主打开前要求用户已登录，打开�
��接着做一次 force 的 upload_request；
关闭后自己发 DELETE /v1/users/me/typing
-statistics 删掉云端副本，删除失败
时开关仍是关的。{directory,action:{op
eration:"upload_request",day,platform,force?}
} → 要发时 {method:"PUT",path:"/v1/users
/me/typing-statistics",device,body}，body �
�请求体对象（{device,platform,days:[{da
y,characters,active_seconds,hours?,categories
?,sources?}],selections?}），宿主带登�
�账号的令牌、以 application/json 发�
�；不发时 {skip}，取 disabled（没打�
��上传）、not_due（离上次上传成功
不到十分钟，force 为 true 时不看）
或 nothing（最近 60 天没有字数）。
day 是宿主的本地日期，platform 是 a
ndroid、ios、harmony 等服务端认的平�
�� id（不认识的平台发出去是空串�
��。这一步不记任何东西：发送失�
��时什么也不用做，下一次照样交�
��请求。{directory,action:{operation:"uplo
ad_succeeded",device}} → {enabled,lastUploa
dMs}：服务端回 204 以后调用，device
 是上一步交回的 device；期间用户�
��掉了上传或设备标识变了时不记�
��宿主要把 upload_request、发送、uplo
ad_succeeded 和关闭后的 DELETE 放在同
一个串行队列里，免得关掉后的删
除和还在路上的上传交错。
 * 请�
�顶层可加 status:true（只用于 load、
set_enabled、set_retention 和 reset，用�
�别的操作时整个请求被拒绝）：�
�答换成设置页读的状态 {statistics,a
vailability:"ready"|"neverWritten",lastWritte
nMs:n|null}，availability 和 lastWrittenMs 
来自统计文件的修改时间，与 Tauri
 外壳的 load_typing_statistics 相同。
 
*/
char *msime_client_typing_statistics(const
 uint8_t *request, size_t length);
/* Read on
ly the aggregate-statistics master switch fro
m an absolute UTF-8
 * directory. Returns 1 w
hen enabled, 0 when disabled/missing, and -1 
for an
 * invalid directory or unreadable doc
ument. Intended for native capture gates:
 * 
call on activation or a settings-change notif
ication, never per keystroke. */
int32_t msim
e_client_typing_statistics_enabled(const uint
8_t *directory, size_t length);
/* Read or up
date 背单词 wordbooks and review progress 
under an absolute UTF-8
 * application data d
irectory. Every action answers with the whole
 status
 * -- {wordbooks,settings,due,answere
dToday,introducing,remaining,queue} -- so a
 
* host keeps one request in flight and never 
follows a change with its own read.
 * Every 
request also carries `resources`, the staging
 root that holds both
 * `EngineResources/` a
nd the `wordbooks/` sibling with the bundled 
books (中考/高考/CET-4/CET-6/考研/雅�
�/
 * 托福/GRE, built by scripts/fetch_word
books.py). A host that stages none simply
 * 
offers the imported books; a bundled book is 
read-only and cannot be deleted.
 * {director
y,resources,day:"YYYY-MM-DD",action:{operatio
n:"load"}}
 * {directory,resources,day,action
:{operation:"answer",word,known}}
 *   known 
is the 认识 button; false is 不认识 and 
returns the card to the same day.
 * {directo
ry,resources,day,action:{operation:"set_setti
ngs",wordbook,new_per_day,session_limit}}
 * 
{directory,resources,day,action:{operation:"i
mport",name,text}}
 *   text is a CSV/TXT wor
d list; the library mints the id and selects 
the book.
 * {directory,resources,day,action:
{operation:"remove",wordbook}}
 * {directory,
resources,day,action:{operation:"reset"}} cle
ars progress, keeps the books.
 * 可选的 p
lugins 是插件目录的绝对路径：其�
�的单词本插件按 pack-<插件 id> 列�
�内置书之后、导入的书之前，带 p
ack:true；对它们的 remove 失败（在�
�件页卸载），卸载后复习进度保�
�。不传 plugins 的宿主只有内置和�
�入的书。
 * `day` is the caller's local 
day and is required by every action: the coun
ts and
 * the queue are per-day and this laye
r cannot resolve the host's timezone.
 * Requ
ests may be up to 8 MiB rather than the usual
 64 KiB, because an imported
 * word list is 
a few hundred kilobytes of text. Takes a file
 lock and reads the
 * library: call on a wor
ker, never on the input path. */
char *msime_
client_vocabulary_review(const uint8_t *reque
st, size_t length);
/* Scan an absolute UTF-8
 skin root and return the catalog the setting
s page sees: {packages:[...],issues:[...]}. R
eads the directory: use a worker. An unreadab
le root is an empty catalog; an invalid packa
ge becomes an issue and is never returned as 
renderable. Presenters must still check that 
a package supports the layout and theme befor
e adopting its colors. Keys are camelCase, th
e same document the settings page consumes. B
esides the colours (drawn through msime_clien
t_resolve_theme, where a palette's translatio
n becomes secondary) a package carries what a
 presenter draws itself: minWidthDip; cornerR
adiusDip (0-32, null keeps the host radius); 
decorationTopDip, decorationWidthDip and deco
rationImage (package-relative, null unless de
corated) placed by decorationAlign ("left"|"c
enter"|"right"), with decorationMotion ("none
"|"float"|"sway"|"breathe"|"bounce", an idle 
motion of the whole image while the window is
 on screen), decorationMotionPeriodMs (1200-8
000) and decorationFrames (0 without a decora
tion image, 1 for a still image, the animatio
n frame count of an APNG, GIF or WebP otherwi
se; a host that cannot play frames draws the 
first); background null or {image,fit:"cover"
|"contain"|"stretch",opacity:0-1}, drawn over
 the surface and under the candidates, clippe
d to the card outline; toolbar {cornerRadiusD
ip|null, light, dark} where each mode is {bac
kground,border,handle,divider,icon,hover}, ea
ch #RRGGBB, #RRGGBBAA or null for the host's 
own. A package may also carry notices, non-fa
tal strings about manifest keys it parsed but
 does not apply here (canvas-only keys on a n
on-canvas base, or a canvas package's nine-sl
ice panel and widgets, which no msime host dr
aws); the key is absent when there are none a
nd the package stays usable. Image paths are 
already confined to the package and are image
s; read them with msime_client_skin_resource.
 */
char *msime_client_skin_catalog(const uin
t8_t *directory, size_t length);
/* Scan an a
bsolute Engine resource directory for optiona
l custom helper-code tables. The
 * response 
value is an array of {schema,file_stem,name,n
ame_en}; missing or unreadable
 * helpcodes/c
ustom is an empty array. Display metadata com
es from leading # name: and
 * # name_en: com
ments, while the Engine remains the owner of 
table parsing. */
char *msime_client_helpcode
_schemas(const uint8_t *resources, size_t len
gth);
/* JSON {directory:absolute skin root,i
d:package folder}. Validates that one package
 with the same loader as msime_client_skin_ca
talog and returns one of its camelCase packag
es entries; an invalid, built-in, symlinked o
r missing package is {ok:false,error} with th
e loader's reason. Reads the package: resolve
 on a skin or appearance change, never while 
drawing. */
char *msime_client_skin_package(c
onst uint8_t *request, size_t length);
/* 同
步解码有界内存中的 PNG/APNG 默认�
�或 GIF/JPEG/WebP 首帧，不读文件。
 
* 输入最多 8 MiB，边长最多 2048。�
�功时只调用一次回调，rgba 为非预
乘 RGBA8，
 * length = width * height * 4�
�字节只在回调期间有效，宿主必�
�当场复制；回调不得抛异常。
 * �
��有回调返回 1 时整个调用返回 1�
�其他回调值返回 0。
 * 坏图、空�
�针和超限返回 0，且不调用回调。

 * 只能在皮肤资源变化时加载，�
�能在逐键绘制中解码；这不是动�
�播放接口。 */
typedef int32_t (*msime_c
lient_skin_image_callback)(void *context, con
st uint8_t *rgba,
                           
                        size_t length, uint32
_t width, uint32_t height);
int32_t msime_cli
ent_skin_image_decode(const uint8_t *bytes, s
ize_t length,
                               
       msime_client_skin_image_callback on_fr
ame, void *context);
/* 同步解码整张 PN
G/APNG、GIF、JPEG、WebP，全部验证成�
��后才按序回调。
 * 每帧为完整画
布，像素格式、借用期和线程要求
与单帧接口一致；最多 120 帧、800 
万像素。
 * index 从 0 开始，count �
�总帧数；delay_ms 是该帧持续时间�
�静态图为 0。
 * play_count 是完整播
放次数，0 表示无限；有限播放结�
��保持末帧。
 * GIF 无循环声明时�
�放一次；APNG/GIF <=10ms 归一化为 100
ms；WebP 多帧固定 100ms 无限。
 * 坏
图、空指针和超限返回 0 且无回调
。任一回调不返回 1 时停止并返回
 0，宿主须丢弃已收帧。
 * 宿主必
须对同一包的不同图片累计像素预
算；不能在逐键绘制中调用。 */
ty
pedef int32_t (*msime_client_skin_animation_c
allback)(void *context, const uint8_t *rgba,

    size_t length, uint32_t width, uint32_t h
eight, uint32_t index, uint32_t count,
    ui
nt32_t delay_ms, uint32_t play_count);
int32_
t msime_client_skin_animation_decode(const ui
nt8_t *bytes, size_t length,
    msime_client
_skin_animation_callback on_frame, void *cont
ext);
/* JSON {directory:absolute path,id:ski
n id,relative:package asset,kind:"image"|"fon
t"}.
 * Returns {contentType,bytes}; the mani
fest and package containment are
 * revalidat
ed for every call and the requested kind must
 match the asset. */
char *msime_client_skin_
resource(const uint8_t *request, size_t lengt
h);
/* JSON {directory:absolute path,id:skin 
id}; returns a nullable stylesheet
 * string 
from the manifest, after revalidating the pac
kage and path. */
char *msime_client_skin_too
lbar_stylesheet(const uint8_t *request, size_
t length);
/* JSON {source:absolute picked fo
lder,directory:absolute skin root}. Copies th
e
 * folder under its own name, replacing a s
kin of that name whole; returns {id}.
 * The 
error message is skin_name, skin_manifest or 
storage. Touches the disk. */
char *msime_cli
ent_skin_import(const uint8_t *request, size_
t length);
/* The shared settings page's 「�
��作皮肤」, ruled by client-services skin
::candidate_editor. JSON {operation:"check",p
ackage_id,manifest,files} reads the draft wit
h the catalog loader in a scratch folder, wit
hout touching the skin root, and returns the 
camelCase package entry msime_client_skin_cat
alog would list (a template manifest such as 
base = "wechat" with no files resolves that l
ook's colours). {operation:"create",directory
:absolute skin root,package_id, manifest,file
s,replace} installs it with the installer com
munity downloads and MCP use and returns {id,
replaced}. files maps package-relative image 
paths to standard base64. A refusal is {ok:fa
lse,error,detail?}: error is a candidate_skin
_* code or storage (candidate_skin_exists whe
n the id is taken and replace is false), deta
il the loader's reason when the manifest did 
not load. Requests may be up to 4 MiB. Decode
s images and touches the disk: use a worker. 
*/
char *msime_client_candidate_skin_editor(c
onst uint8_t *request, size_t length);
/* The
 queued personal dictionary, for a host that 
cannot take the Engine's
 * maintenance lock 
when the request arrives. Same request shape 
as
 * msime_client_dictionary - {options,acti
on} - but the operations act on
 * <preferenc
es_directory>/PersonalDictionary instead of t
he Engine, and the
 * keyboard applies them a
t its next session start. Use this for import
_personal
 * in particular: "maintenance busy
" is not an answer to "add these words", and

 * the user is as likely to import with the k
eyboard up as with it down. */
char *msime_cl
ient_personal_dictionary_request(const uint8_
t *request, size_t length);
/* JSON {director
y:absolute state root} reads the named custom
 touch-keyboard
 * designs; adding action:{op
eration:"create"|"rename"|"update"|"delete",.
..}
 * applies one change first. Both answer 
with the whole library, because every
 * call
er redraws the list. Takes the library's file
 lock and rewrites it
 * atomically: use a wo
rker. Failures carry the shared community_* c
odes the
 * settings pages already have wordi
ng for, not a Display string. */
char *msime_
client_custom_skin_library(const uint8_t *req
uest, size_t length);
/* JSON {directory:abso
lute state root,id:publication uuid,name,desi
gn}. Starts
 * a skin trial and imports the d
esign into the library, answering
 * {skin,tr
ial}. One call rather than two: the trial is 
what remembers the skin
 * being replaced, so
 a failed import has to end it or the user we
ars a design
 * that was never saved. The dow
nload itself is the caller's, because only th
e
 * surrounding platform's HTTPS stack can f
etch it. Writes preferences and two
 * locked
 files: use a worker. */
char *msime_client_c
ommunity_skin_install(const uint8_t *request,
 size_t length);
/* JSON {directory,action:{o
peration:"finish",id,keep}} or
 * {operation:
"restore_pending"}. Declining a trial puts th
e previous skin back;
 * restore_pending is t
he crash recovery and is safe with no trial p
ending.
 * Answers {revision} so a caller hol
ding the document can tell whether what it
 *
 is showing is still what is on disk. Writes 
preferences: use a worker. */
char *msime_cli
ent_keyboard_skin_trial(const uint8_t *reques
t, size_t length);
/* JSON {file:absolute Com
munityLibrary.json,action:{operation:"load"}}
 or
 * {operation:"save_reply",item} or {oper
ation:"remove",id}. The reply templates
 * th
e user explicitly kept, which is the one thin
g the settings surface and the
 * keyboard pr
ocess share about the community. Every operat
ion answers with the
 * whole library. Takes 
the library's file lock: use a worker. */
cha
r *msime_client_community_resource_library(co
nst uint8_t *request, size_t length);
/* The 
decisions in AI skin generation, for a host w
hose HTTP must go through
 * the surrounding 
platform and so performs the four requests it
self.
 * {operation:"compose",prompt,model} r
eturns {path,body} carrying the shared
 * sys
tem prompt; {operation:"parse",text} returns 
the three validated plans or
 * refuses; {ope
ration:"artwork",artwork} says whether a retu
rned image is one
 * this client will show. T
he instruction and the parser must not be sep
arated:
 * the prompt names the exact documen
t the parser accepts. */
char *msime_client_a
i_skin_plan(const uint8_t *request, size_t le
ngth);
/* Absolute staged engine resources; r
eturns {profile,sourceCommit} from the
 * pac
kaged msime-dictionary-manifest.json. Two fie
lds only: the page is asking what
 * dictiona
ry is installed and where it came from, not f
or journal modes or
 * third-party references
. Missing or unreadable is reported, never gu
essed -
 * showing the wrong dictionary versi
on is worse than showing none. */
char *msime
_client_dictionary_manifest(const uint8_t *re
sources, size_t length);
/* Read saved histor
y only; disabled preferences return an empty 
entries array. */
char *msime_client_load_cli
pboard_history(const uint8_t *directory, size
_t length);
/* JSON {directory,text}; removes
 exact saved entry, not the system clipboard.
 */
char *msime_client_remove_clipboard_histo
ry(const uint8_t *request, size_t length);
/*
 JSON {directory,text}; capture under the sha
red preference/history locks. */
char *msime_
client_capture_clipboard_history(const uint8_
t *request, size_t length);
/* 桌面剪贴�
�历史，供「剪贴板」设置页使用�
�与 Tauri 外壳的剪贴板命令读写同�
��个 directory/clipboard_history.json。requ
est 是 length 字节（最多 131072）的 U
TF-8 JSON {directory: 偏好目录的绝对�
�径, action}，action 是 {operation:"list"|
"sync"|"clear"}、{operation:"set_pinned",tex
t,pinned} 或 {operation:"remove",text}；tex
t 按内容找条目，非空且不超过 120
00 字节。sync 只在 clipboard_history 开
着时读系统剪贴板（只有 Windows 读
，带密码管理器等隐私标记的内容
不记录，末尾换行去掉），关着时
 error 是 clipboard_history_disabled，其�
�平台读不到时是 clipboard_history_unav
ailable。clear、set_pinned 和 remove 不�
�开关。每个操作都返回 {enabled, ent
ries:[{text,timestampMs,pinned}]}，是操作
后的历史，先固定的、再按时间从
新到旧；开关关着时 entries 为空。
请求不合法时 error 是 clipboard_histor
y_invalid，读写失败时是 clipboard_hist
ory_unavailable。读写磁盘、sync 还要�
��开剪贴板：放在工作线程上调用�
�� */
char *msime_client_desktop_clipboard_hi
story(const uint8_t *request, size_t length);

/* 移动端的结构化剪贴板历史，�
�桌面的自动记录偏好无关。请求 J
SON 为 {directory: App Group 根的绝对路
径, action}，action 是 {operation:"load"|"
clear"}、{operation:"capture"|"remove",text}
、{operation:"remove_many",texts}、{operati
on:"restore_many",entries}、{operation:"set_
pinned",text,pinned} 或 {operation:"replace"
,text,replacement}。remove_many 原子返回
 removed 完整条目，restore_many 原子�
�复其文字、时间戳和固定状态；�
�有同文条目不覆盖。replace 按内容
找到原条目原地改字，时间戳和固
定状态不变；新文字与另一条相同
时两条合并，固定状态取两者之或
；返回 replaced、merged 和 reason（"not
_found"|"invalid"|null）。历史存在 dire
ctory/MSIME/clipboard_history.json。 */
char
 *msime_client_mobile_clipboard_history(const
 uint8_t *request, size_t length);
/* Same va
lidation as load_preferences; ok:true,value:n
ull means lock busy.
 * Does not wait for the
 writer lock. Disk I/O may still block: use a
 worker.
 * Busy is not missing/corrupt and m
ust not reset preferences to defaults. */
cha
r *msime_client_try_load_preferences(const ui
nt8_t *directory, size_t length);
/* Repair a
 preferences.json that is not well-formed JSO
N (truncated, empty, overwritten). May block 
on disk and the writer lock: use a worker. Ba
ckup first: the damaged bytes are copied verb
atim to <directory>/preferences.json.corrupt-
YYYYMMDD-HHMMSS (UTC, -N suffix when taken) a
nd nothing is rewritten if that copy fails. T
hen every setting and service key the schema 
still accepts is carried onto the defaults. A
 valid or missing document is a no-op: {recov
ered:false, snapshot}. A repair returns {reco
vered:true, snapshot, backup_path, backup_nam
e, salvaged}. A well-formed document the sche
ma rejects (unknown fields, newer format_vers
ion) is most likely a newer build's and retur
ns the load error unchanged; the settings pag
e repairs it explicitly with msime_client_rep
air_preferences. Storage errors are returned 
and never lead to a rewrite. */
char *msime_c
lient_recover_preferences(const uint8_t *dire
ctory, size_t length);
/* The settings page's
 explicit 修复配置文件: the same repair
 for any document load_preferences rejects, a
 newer build's (unknown fields, newer format_
version) included. Same backup, salvage, resu
lt shape and worker requirement as msime_clie
nt_recover_preferences. Call it only when the
 user asks: what this schema does not accept 
takes its default. */
char *msime_client_repa
ir_preferences(const uint8_t *directory, size
_t length);
/* Compare-and-swap save of Prefe
rencesSnapshot.preferences. The snapshot's
 *
 format_version is validated; expected_revisi
on must match the store.
 * A disabled clipbo
ard-history save clears the default history f
ile if history
 * is still disabled. Cleanup 
errors may be returned after preferences are 
saved.
 * Directory <=16384 bytes; snapshot <
=1048576 bytes, enough for a custom skin phot
o. */
char *msime_client_save_preferences(con
st uint8_t *directory, size_t directory_lengt
h,
                                    uint64
_t expected_revision,
                       
             const uint8_t *snapshot, size_t 
snapshot_length);
/* 在会话线程传入 Pr
eferencesSnapshot JSON（最多 1048576 字�
�）：{format_version:1, revision, preferenc
es:{...}}。修订号按会话递增；相同
快照可重试，旧版或冲突快照会被
拒绝。返回 {revision, deferred, view, di
agnostic?}；仅首选方案无法运行并�
�退时带 diagnostic。组字期间的 Engin
e 偏好应用延后到派发或焦点调用�
��会话空闲，但关闭云候选会立即�
��除现有云行和旧查询，AI 配置或�
��牌变化也会立即清除旧 AI 行和查
询；释义开关、目标语言或翻译服
务变化会清掉旧释义并推进候选代
次，旧释义回包将被拒绝。宿主应
使用返回的新 view。较新快照替换�
��应用快照。构建失败时保留旧会�
��和待应用快照供重试，派发通过 
diagnostic 报告重试错误，不丢失已�
��成输入。本接口不读写偏好文件�
��宿主提供已加载的快照。 */
char *
msime_client_update_preferences(uint64_t sess
ion, const uint8_t *snapshot, size_t length);

/* 获得焦点（true）只重置旧组字�
��包括 phrase_preedit 暂存的词段，不
返回旧组字的提交；失焦（false）�
��当前方案结束组字，并返回应送�
��的提交，包括已经选好的暂存词�
��。 */
char *msime_client_focus(uint64_t se
ssion, bool focused);
/* Clear the current se
ssion's Engine candidate cache and refresh it
s view. */
char *msime_client_reset_cache(uin
t64_t session);
/* Release process-level mode
l caches without a session handle (memory pre
ssure). */
void msime_client_release_process_
cache(void);
/* 语音设置服务目录。�
�函数，无文件、网络或设备访问�
�响应 value 为 {asr:[preset],polish:[prese
t],doubao_stream_endpoints:[{id,label,title,e
ndpoint}]}。
 * preset 为 {id,title,endpoin
t,model,models:[string],documentation}，仅�
��公开预设。
 * 数组顺序即设置页
顺序；不含平台特有的 system/local�
�宿主按能力追加。
 * models 仅作建
议，不限制用户自定义模型。宿主
复制字段后用 msime_client_string_free �
��放返回的 JSON 缓冲区。 */
char *msi
me_client_voice_provider_catalog(void);
/* �
�换语音服务的纯函数；请求 <=1 MiB
：{service:"voice.asr"|"voice.polish",provid
er:string,
 * current:{provider?,endpoint?,mo
del?,token?,tokens?:{id:string}},field_policy
?:"fill_defaults"|"clear"}。
 * current 缺�
��的字段按空值处理。field_policy 缺
省为 fill_defaults，只替换空白或出�
��值；clear 在换服务时清空地址和�
��型。
 * value 为 {provider,token,tokens,
endpoint?,model?}：省略地址/模型表示
保留原值，空串表示清空。
 * 不�
�写偏好、不联网；宿主负责把补�
�字段映射到 asr_ 或 polish_ 并保存�
�
 * 请求和返回含凭据，禁止记录�
��返回缓冲区用 msime_client_string_free
 释放。 */
char *msime_client_voice_provid
er_update(const uint8_t *request, size_t leng
th);
/* 语音设置接口预检；输入是 
<=64 KiB 的 UTF-8 地址，websocket 为 tru
e 时按 wss 校验，否则按 https 校验�
��
 * 首尾空白与运行时解析一样去
除，空地址表示使用默认值；非空
地址复用运行时的 2048 字节和 URL �
��状规则，并保留移动宿主的小写�
��议、无 @ 限制。
 * value 为 {accepte
d:boolean,issue:string|null}；失败提示�
�定，不回显输入，不访问偏好或�
�络。
 * 空引用、超限和非法 UTF-8 
返回错误；返回缓冲区用 msime_clien
t_string_free 释放。 */
char *msime_client
_voice_endpoint_issue(const uint8_t *endpoint
, size_t length, bool websocket);
char *msime
_client_voice_start(uint64_t session);
char *
msime_client_voice_cancel(uint64_t session);

/* Capture bounded mono 16 kHz samples. The J
SON result is transient audio and
 * must nev
er be logged or persisted. */
char *msime_cli
ent_voice_capture(uint32_t milliseconds);
/* 
The microphones a settings page offers under 
「可用录音设备」 (the Tauri settings 
shell's
 * list_voice_capture_devices on Wind
ows). Value [{backend:"windows",id,label}], a
t most 128,
 * [] when there is none: id is "
wasapi:" then the WASAPI endpoint id as four 
lowercase hex
 * digits per UTF-16 unit, the 
value voice_input.capture_device holds with c
apture_backend
 * "windows". Other targets fa
il with "voice capture devices unavailable". 
Enumerates through
 * COM: call off the UI th
read. Ids and labels may identify the user's 
hardware: never log them. */
char *msime_clie
nt_voice_capture_devices(void);
char *msime_c
lient_voice_apply(uint64_t session, uint64_t 
generation,
                               co
nst uint8_t *text, size_t length);
/* On-devi
ce speech models and user-dictionary hotwords
. JSON request buffers of length bytes; stand
ard responses. Error text of the model calls 
is a stable code beginning with "local_model_
".
 * voice_hotwords: {options: HostOptions a
s msime_client_dictionary, limit?: 200} -> {h
otwords:[{text,pinyin}]}, the user's own piny
in words (two or more Chinese characters), he
aviest first. Worker thread; fails with "dict
ionary maintenance busy" while maintenance ho
lds the store. <=1 MiB.
 * voice_hotword_corr
ect: {text, hotwords:[{text,pinyin}]} -> {tex
t}. Pinyin-similarity replacement for models 
whose msime-model.json has "hotwords":"pinyin
". Pure. The transcript is limited to 10000 U
nicode scalar values, with at most 1000 hotwo
rds; each hotword text is <=256 bytes and pin
yin <=1024 bytes. The JSON request is <=10485
76 bytes.
 * voice_local_models: {root: absol
ute dir} -> {models:[{id,title,description,la
nguages,streaming,default,desktop_only,instal
led,path,installed_size,archive_size,memory,l
icense_spdx,license_source,license_terms,lice
nse_notice,hotwords,import_files:[{name,url,s
ize}]}], default: id}. path is <root>/<id>, t
he value for voice_input.asr_model_path. impo
rt_files 是不联网安装时要用户自己
下载的文件（压缩包和带地址的附
加文件），url 是上游下载地址。
 
* voice_local_model_install: {root, id, mirro
r?: "https://..." prefix, files?: [absolute p
ath]} -> {path}。阻塞到安装结束，只
能在工作线程调用。progress 可为空
，在调用线程收到 {id,stage:"download"
|"import"|"verify"|"extract"|"done",downloade
d,total}，返回前需复制缓冲区。登�
��后、访问文件或网络前先回调 dow
nload（导入为 import）的零字节进度
，total 为目录里的归档字节数；回
调可重入 voice_local_model_cancel 取消�
��本次安装随即报 "local_model_cancelle
d"。每个 id 同时只允许一次安装（
"local_model_install_running"）。带 files�
��至多 16 个）时不联网，用这些本
地文件安装：按长度和 SHA-256 认文
件、不看文件名，进度阶段 download
 报成 "import"，缺文件时失败为 "loc
al_model_import_missing: <name>"，所选文�
��打不开或读出错时为 "local_model_im
port_unreadable: <detail>"。
 * voice_local_
model_cancel: {id} cancels that install, NULL
/0 or {} cancels all; value is whether one wa
s running. Any thread.
 * voice_local_model_r
emove: {root, id} -> null. Only catalog ids; 
refused while that id is installing.
 * voice
_model_setting_check: {asr_model_mirror?, asr
_model_path?} (<=64 KiB) -> {asr_model_mirror
?: bool, asr_model_path?: bool}, one key per 
field asked: whether the preferences store ac
cepts that value (the same checks as saving).
 Pure. */
typedef void (*msime_client_voice_l
ocal_model_progress_callback)(const uint8_t *
json, size_t length,
                        
                                         void
 *context);
char *msime_client_voice_hotwords
(const uint8_t *request, size_t length);
char
 *msime_client_voice_hotword_correct(const ui
nt8_t *request, size_t length);
/* voice_norm
alize_numbers: {text, kind} -> {text}. 本地
模型最终文本里汉字写的数字改成
数字（二零零二年 → 2002年，百分
之五十七 → 57%），一个、十分有�
��这类词不动。只改写不自带 ITN �
� kind（online_transducer、offline_transduc
er），其他 kind 原样返回，宿主对�
��个本地模型的最终文本都可以调�
��；只用于最终文本，不用于中间�
��果。Pure. text 上限 10000 个 Unicode �
��量，kind 上限 64 字节，请求 <=1048
576 字节。 */
char *msime_client_voice_nor
malize_numbers(const uint8_t *request, size_t
 length);
char *msime_client_voice_local_mode
ls(const uint8_t *request, size_t length);
ch
ar *msime_client_voice_local_model_install(
 
   const uint8_t *request, size_t length,
   
 msime_client_voice_local_model_progress_call
back progress, void *context);
char *msime_cl
ient_voice_local_model_cancel(const uint8_t *
request, size_t length);
char *msime_client_v
oice_local_model_remove(const uint8_t *reques
t, size_t length);
char *msime_client_voice_m
odel_setting_check(const uint8_t *request, si
ze_t length);
/* 按需资源包存放在 <st
ate_root>/resource-packs/<id>/；state_root �
�� HostOptions 的 preferences_directory（An
droid: files/bootstrap/state），会话下�
�取得焦点时读取新发布的包。
 * �
�� id：japanese、language-dictionaries、ha
ndwriting、settled-model、offline-glosses�
�voice-runtime、wubi、voice-runtime-macos�
�锁文件固定了重签的 macOS 运行库�
��后才有）。请求为 length 字节的 J
SON（<=16384），响应使用标准封装�
�install 和 adopt 是阻塞调用，只能�
�在工作线程。
 * 错误为 "invalid res
ource pack request"、"invalid state root"、
"invalid source"、"resource_pack_unknown"、
"resource_pack_busy"（本进程正在安装�
��收编该包）或 "local_model_*"。
 * re
source_packs: {state_root} -> [{id, state:"mi
ssing"|"installed"|"outdated", size, schemes:
[...], import_files:[{name,url,size}]}]，每
包一项；size 为下载字节数，import_
files 为离线安装所需文件（voice-run
time 使用归档），url 为上游下载地
址。
 * resource_pack_install: {state_root,
 pack, sources?:["https://mirror/", ...], fil
es?:[absolute path]} -> {path}。
 * sources 
是依次尝试的镜像前缀（最多 8 个
，跳过空项），之后尝试项目镜像
和编译锁中的 URL；始终按锁中的�
�小和 SHA-256 校验，未完成文件在�
�次调用时用 HTTP Range 续传。
 * file
s（最多 16 个绝对路径）表示改从�
��地安装，按大小和 SHA-256 匹配而�
��文件名，忽略 sources，下载阶段�
�报 "import"；缺文件返回 "local_model_
import_missing: <name>"，不可读返回 "lo
cal_model_import_unreadable: <detail>"。
 * 
progress 可为空，在调用线程收到 (c
ontext, phase "download"|"import"|"verify"|"d
one", done, total)，阶段字符串仅在回
调期间有效。
 * 首次进度在任务�
�记后、任何文件或网络 I/O 前发送
（done=0，total=资源包大小），回调
可重入 resource_pack_cancel；取消后返
回 "local_model_cancelled"。
 * resource_pa
ck_cancel: pack 是原始 UTF-8 id（不是 J
SON）；NULL/0 取消本进程全部资源�
�安装。可在任何线程调用，立即�
�回，不触发进度回调。
 * resource_p
ack_adopt: {state_root, pack, source: absolut
e dir} -> {path}。在同一文件系统内�
�包文件从 source 重命名移入（不复
制），按编译锁校验后发布；失败
则全部移回，不发布任何内容。已
有相同字节的安装时直接返回，不
改动 source。 */
typedef void (*msime_clie
nt_resource_pack_progress_callback)(void *con
text, const char *phase,
                    
                                         uint
64_t done, uint64_t total);
char *msime_clien
t_resource_packs(const uint8_t *request, size
_t length);
char *msime_client_resource_pack_
install(const uint8_t *request, size_t length
,
                                         ms
ime_client_resource_pack_progress_callback pr
ogress,
                                     
    void *context);
void msime_client_resourc
e_pack_cancel(const uint8_t *pack, size_t len
gth);
char *msime_client_resource_pack_adopt(
const uint8_t *request, size_t length);
/* �
�定义翻译请求描述器（不做网络 I
/O）：api.deepl.com / api-free.deepl.com �
� DeepL v2 格式（DeepL-Auth-Key 鉴权、t
ext 数组，没有密钥时报错），其�
�地址用 DeepLX 兼容格式。Request <=16
 KiB:
 * {config:{enabled,endpoint,api_key},t
ext,source_language,target_language}.
 * Retu
rns null if disabled; otherwise {url,method,h
eaders,body,timeout_ms,max_response_bytes}.
 
* Descriptor can contain credentials: never l
og it. Host enforces timeout/size,
 * rejects
 redirects and checks HTTP status before pars
ing. Text <=256 scalars. */
char *msime_clien
t_custom_translation_http_request(const uint8
_t *request, size_t length);
/* Pure visible-
page plan <=64 KiB: {target_language,candidat
es:[{text,source}]}.
 * Returns [{text,key,so
urce_language,target_language}]; at most nine
 candidates. */
char *msime_client_custom_tra
nslation_plan(const uint8_t *request, size_t 
length);
/* Pure signed TMT descriptor <=64 K
iB. Input: {config:{enabled,secret_id,
 * sec
ret_key,region},texts:[string],source_languag
e,target_language,timestamp}.
 * Send body_ut
f8 bytes unchanged. Never log this credential
-bearing descriptor. */
char *msime_client_te
ncent_translation_http_request(const uint8_t 
*request, size_t length);
/* Pure NiuTrans v2
 descriptor <=64 KiB. Input: {config:{enabled
,app_id,apikey},
 * text,source_language,targ
et_language,timestamp}. */
char *msime_client
_niutrans_translation_http_request(const uint
8_t *request, size_t length);
/* OpenAI 兼�
�的 AI 翻译描述器和解析器。请求�
��超过 64 KiB：
 * {config: AiAssistantPre
ferences, input:{text,source_language,target_
language}}.
 * It reuses the configured AI en
dpoint and token, and returns one JSON string
 or null. */
char *msime_client_ai_translatio
n_http_request(const uint8_t *request, size_t
 length);
char *msime_client_parse_ai_transla
tion_response(const uint8_t *body, size_t len
gth);
bool msime_client_ai_translation_reply_
failed(const uint8_t *body, size_t length);
/
* A settings page's 「测试配置」 check,
 the one the Tauri settings shell runs on Win
dows and
 * macOS. Request <=64 KiB: {service
,config}, service one of translation.tencent 
({secret_id,
 * secret_key,region}), translat
ion.niutrans ({app_id,apikey}), translation.c
ustom ({endpoint,
 * api_key}), voice.asr, vo
ice.polish or ai.assistant, config the object
 the shared page sends.
 * Value {ok,message}
: message is the Chinese result line to show;
 it never carries the
 * provider's reply or 
the credential. An incomplete config is answe
red without the network;
 * otherwise this se
nds one request and blocks up to about 15 s: 
call off the UI thread.
 * The request carrie
s credentials: never log it. */
char *msime_c
lient_test_api_credential(const uint8_t *requ
est, size_t length);
/* The same check for a 
settings page without its own copy of the pro
vider defaults (the Android native app). Requ
est <=64 KiB: {preferences_directory, service
}, service voice.asr or voice.polish; the con
figuration is resolved from the stored prefer
ences exactly as the keyboard resolves it. Va
lue {ok,message} as above; a provider with no
thing to test (system, local, unknown) is ans
wered without the network. Blocks up to about
 15 s: call off the UI thread. */
char *msime
_client_test_voice_credential(const uint8_t *
request, size_t length);
/* Pure check of a c
ustom translation endpoint (UTF-8, <=64 KiB),
 for the settings page. Value
 * {issue,accep
ted}: issue is the Chinese hint the shared pa
ge shows under the endpoint field
 * (transla
tionEndpointIssue: empty endpoint, too long, 
control characters, not a URL,
 * credentials
 or fragment, remote http), "" when there is 
none; accepted is whether the
 * preferences 
store saves it (empty, https, or http to loca
lhost/127.0.0.1/[::1]). No I/O. */
char *msim
e_client_translation_endpoint_issue(const uin
t8_t *endpoint, size_t length);
/* Pure check
 of an AI assistant endpoint (UTF-8, <=64 KiB
), for the AI settings page. Value
 * {origin
,problem,hint}: origin is the "scheme://host:
port" key ai_assistant.tokens stores the
 * t
oken under (aiCredentialOrigin), null when th
e endpoint is not usable; problem is null,
 *
 "invalid" or "cleartext_public" (http to a h
ost that is not this machine or the local net
work);
 * hint is the Chinese line the shared
 page shows under the field (aiEndpointHint),
 "" when usable.
 * No I/O. */
char *msime_cl
ient_ai_endpoint_check(const uint8_t *endpoin
t, size_t length);
/* The AI settings page's 
「获取模型」, the Tauri settings shell'
s ai_models. Request <=256 KiB:
 * {endpoint,
token}, the token being ai_assistant.tokens[o
rigin]. GETs the /models sibling of the
 * en
dpoint without following redirects (http only
 to this machine or the local network, never

 * through a proxy). Value: up to 128 model i
ds, in the service's order. Error: a stable c
ode,
 * ai_invalid (request or endpoint/token
 rejected, nothing sent), ai_models_unavailab
le (no
 * connection, timeout or error status
) or ai_models_invalid (reply too large or no
 ids). Blocks
 * up to about 15 s: call off t
he UI thread. The request carries the token: 
never log it. */
char *msime_client_ai_models
(const uint8_t *request, size_t length);
/* T
he AI settings page's 「测试」, the Tauri
 settings shell's ai_test. Request <=256 KiB:

 * {endpoint,model,prompt,token,text}: model
 non-empty <=256 bytes; prompt (may be empty)
 and
 * text (non-empty) <=16 KiB each, newli
nes and tabs allowed. POSTs one non-streaming
 chat
 * request (prompt as system, text as u
ser) to the endpoint without following redire
cts. Value:
 * the trimmed reply text. Error:
 ai_invalid, ai_test_unavailable or ai_test_i
nvalid, as for
 * msime_client_ai_models. Blo
cks up to about 15 s: call off the UI thread.
 The request carries
 * the token and the use
r's text: never log it or the reply. */
char 
*msime_client_ai_test(const uint8_t *request,
 size_t length);
// Pure AI descriptor from {
config:AI preferences,input:{segmented_pinyin
,context,candidate_limit}}.
// Input <=64KiB.
 Result contains credentials; never log it. H
ost must forbid redirects.
char *msime_client
_ai_http_request(const uint8_t *request, size
_t length);
// Successful HTTP body <=1MiB, l
imit 1..10. Returns a string array or null.
c
har *msime_client_parse_ai_response(const uin
t8_t *body, size_t length, uint8_t limit);
//
 Worker-thread disk I/O. JSON <=64KiB: {direc
tory:absolute private user path,
// action:lo
okup|remember,target_language,generation,item
s:[{text,direction,translation?}]}.
// At mos
t 9 items, directions english_to_chinese/chin
ese_to_english. Remember
// requires translat
ion; lookup forbids it. Returns {generation,t
ranslations,saved}.
// Never pass packaged re
sources. Learned text is private; never log r
equests.
// A malformed batch makes no writes
; an I/O failure can leave earlier items save
d.
// New writes use Engine translation-gloss
es.db; old JSON records remain read-only fall
back.
char *msime_client_learned_translation_
request(const uint8_t *request, size_t length
);
/* Returns [string|null] with exact expect
ed count (1..9), or null for invalid body. */

char *msime_client_parse_tencent_translation
_response(const uint8_t *body, size_t length,
 size_t expected);
/* Provider body <=1 MiB. 
Returns a formatted translation string or nul
l. */
char *msime_client_parse_niutrans_trans
lation_response(const uint8_t *body, size_t l
ength);
/* Host-produced gloss <=64 KiB UTF-8
. Returns it formatted like provider replies 
(whitespace collapsed, ends trimmed) or null 
when empty or it has a control char. */
char 
*msime_client_format_translation_gloss(const 
uint8_t *text, size_t length);
/* Provider bo
dy <=1 MiB. True when the reply reports a fai
lure (malformed, non-200 code, errorCode/erro
rMsg) rather than an answer, empty or not. On
ly answers may be negative-cached. */
bool ms
ime_client_custom_translation_reply_failed(co
nst uint8_t *body, size_t length);
bool msime
_client_niutrans_translation_reply_failed(con
st uint8_t *body, size_t length);
/* Provider
 body <=1 MiB. Returns translation string <=4
096 bytes or null when
 * malformed/no result
. No session mutation; host validates origina
l identity. */
char *msime_client_parse_custo
m_translation_response(const uint8_t *body, s
ize_t length);
/* 同代次的非空 JSON [{"
text":"candidate","translation":"gloss"}] 合
并候选释义。
 * [] 主动清空；宿�
�异步无结果时跳过调用，旧代次�
�应用。 */
char *msime_client_apply_transl
ations(uint64_t session, uint64_t generation,

                                      const 
uint8_t *translations, size_t length);
/* Res
olve copied candidates against the packaged o
ffline English dictionary.
 * JSON request: {
generation,candidates:[{text,source}],user_da
ta?,target_language?,state_root?}; the genera
tion is echoed for the host to pass to apply_
translations on the session thread. This func
tion owns no session handle and may run on a 
worker thread.
 * target_language absent or "
en" reads msime-english.db and the user's glo
sses. fr/ja/es/ru/de/ko read only offline-glo
sses/zh-<lang>.db beside resources, or, when 
that is absent and state_root (absolute; pass
 the translation query's state_root) is given
, the same file in the downloaded <state_root
>/resource-packs/offline-glosses pack; they i
gnore user_data. When neither is installed th
e result is {generation,translations:[]}, not
 an error. Any other value is an invalid requ
est. Only Chinese candidates get a non-Englis
h gloss. */
char *msime_client_candidate_glos
s_request(const uint8_t *request, size_t requ
est_length,
                                 
          const uint8_t *resources, size_t re
sources_length);
/* Pronounce copied English 
texts without a session, for the line a host 
draws beside a gloss.
 * JSON request: {gener
ation,items:[{text,language}]}; language must
 be "en". text is an English candidate or one
 line of an English gloss; only its first ter
m is pronounced, without a leading to/a/an/th
e, and a term with any unknown word gets noth
ing. The response is {generation,pronunciatio
ns:[{text,language,pronunciation}]} with "/�
�/" IPA, omitting texts that have none. The t
able is pronunciations/en-phonetic.db beside 
resources; when it is not installed the resul
t is {generation,pronunciations:[]}, not an e
rror. May run on a worker thread. */
char *ms
ime_client_pronunciation_request(const uint8_
t *request, size_t request_length,
          
                               const uint8_t 
*resources, size_t resources_length);
/* Brea
k copied Chinese candidates into words with t
heir English, without a session, for a line a
 host draws under the gloss lines (never a co
mmittable gloss column).
 * JSON request: {ge
neration,texts:[...]}, at most 64 texts. Only
 all-Han texts of 2 to 32 characters are cons
idered; each is cut left to right by longest 
match (up to 8 characters) against character-
glosses/zh-en.db (single characters first) an
d word-glosses/zh-en.db beside resources, and
 each piece shows the first phrase of its glo
ss: "我 I · 喜欢 to like · 你 you". A t
ext with fewer than two glossed pieces or mor
e than eight pieces is omitted. The response 
is {generation,breakdowns:[{text,breakdown}]}
; with neither table installed it is empty, n
ot an error. May run on a worker thread. */
c
har *msime_client_gloss_breakdown_request(con
st uint8_t *request, size_t request_length,
 
                                          con
st uint8_t *resources, size_t resources_lengt
h);
/* 查询固定英文词库，不创建�
�改变输入会话。
 * JSON 请求：{pref
ix,limit,correction?:false}；limit 为 1..32
，响应为 {prefix,items:[...]}。
 * 缺�
�按原有方式补全 ASCII 字母前缀；c
orrection:true 时接受字母与撇号，第
一行始终原文，最多附加一行纠错
。
 * 纠错仅处理不超过 32 字节的�
��入，33..128 字节原样返回；缺词�
�也保留原文。
 * resources 必须是资
源分代目录的绝对路径；词库只读
，检索结果可以缓存，可在工作线
程调用。 */
char *msime_client_english_co
mpletions_request(const uint8_t *request,
   
                                            s
ize_t request_length,
                       
                        const uint8_t *resour
ces,
                                        
       size_t resources_length);
// Live per-
session mode, not a persisted preference. Pre
serves composition and
// candidate generatio
n; remains authoritative across preference re
placement.
char *msime_client_set_chinese_pun
ctuation(uint64_t session, bool enabled);
cha
r *msime_client_set_character_width(uint64_t 
session, bool fullwidth);
char *msime_client_
set_english_mode(uint64_t session, bool enabl
ed);
/* 引擎负责的九键模式，用于�
��拼九宫格或注音九键（注音下 Vie
w.nine_key_spellings 是目标音节的候选
读音）。只在组字结束后调用。Vie
w.nine_key 和 View.nine_key_spellings 是权
威状态。对其他方案开启、或在组
字中切换模式都会被拒绝。等同于
 msime_client_set_key_grid 的 1（开）和 
0（关），所以关掉时 14 键也一并�
��掉。
 * 创建和重建会话时按偏好
自动开启：全拼看 touch_keyboard_layou
t 是否为 nine_key（fourteen_key 则开 14
 键）；注音还要求 touch_keyboard_sche
mes 选中（或在没有选中项时启用�
�）zhuyin_nine_key，这个方案只有 Andr
oid 写，桌面宿主为全拼九宫格写�
�的 nine_key 不会让注音会话离开大�
��键位。宿主设下的网格只保留到�
��案或 touch_keyboard_layout 改变为止�
�
 * View.touch_keyboard_layout 是已应用�
��宿主呈现偏好；日文九键宿主只�
��它，不开启引擎的九键模式。 */

char *msime_client_set_nine_key_mode(uint64_t
 session, bool enabled);
/* 换引擎的组�
�网格：0 关，1 九键（同 msime_client
_set_nine_key_mode），2 全拼 14 键（QWE
RTY 上相邻两个字母一个键，L、M �
�独一键），3 九键双拼（表按已应
用的 nine_key_shuangpin_profile 取）。�
�在组字结束后调用；14 键和九键�
�拼只给全拼，对其他方案开启、�
�组字中切换都会被拒绝，其他取�
�报错，会话不变。Value 是 View。
 *
 View.key_grid 是权威状态："none"、"ni
ne_key"、"fourteen_key" 或 "nine_key_shuang
pin"。View.nine_key 只在九键下为真。
几种网格共用 View.nine_key_spellings、
nine_key_reading、nine_key_single_character�
��nine_key_strokes，以及 msime_client_choo
se_nine_key_spelling 和 msime_client_set_nin
e_key_filter；14 键的分词键照九键送
 '\''。14 键下数字不是输入，照常�
��选词键。九键双拼的数字从 msime_
client_character 进来（方案用到的数�
��，可以有 1 和 0），和九键一样�
�当选词键，没有英文九键；创建�
�重建会话时 touch_keyboard_layout 为 ni
ne_key_shuangpin 的全拼会话自动开启�
��。 */
char *msime_client_set_key_grid(uint
64_t session, uint8_t grid);
/* 14 键的一�
��：letter 是这一组里的任一小写字
母，引擎归成这一组，宿主约定送
首字母（q e t u o a d g j l z c b m）。
返回与 msime_client_character 相同的输
入响应。不在 14 键下、方案不是�
�拼、在本地模式或专用英文里、26
 键正在组字、或 letter 不是 a-z 时 
handled=false，会话不变。
 * 硬件键�
��的字母照常经 msime_client_character �
��全拼 26 键：14 键组字进行中它们
 handled=false，同九键。 */
char *msime_
client_grid_key(uint64_t session, uint8_t let
ter);
/* 标出隐私会话（隐私模式、
不允许学习的输入框）：这个会话
的选词位置和上屏效率不记入打字
统计。用户在设置里关掉学习不算
隐私会话。学习本身仍由偏好里的
 learning 决定。Value 是设下的布尔�
�。 */
char *msime_client_set_private_sessio
n(uint64_t session, bool enabled);
/* 报告�
��写锁定状态：偏好 caps_lock_ascii_pu
nctuation 打开时，大写锁定期间没�
�组字的标点键（经 msime_client_charac
ter、msime_client_punctuation 或 msime_clie
nt_punctuation_with_context 送来）改走�
�面 ASCII 路线，和英文模式一样；p
unctuation_lock 为 chinese 时仍是中文�
�点。会话状态而非偏好，会话重�
�后由宿主重新报告。Value 是设下�
�布尔值。接上这个调用的宿主在 H
ostCapabilities.caps_lock_punctuation 里声�
��。 */
char *msime_client_set_caps_lock(uin
t64_t session, bool enabled);
char *msime_cli
ent_set_paired_punctuation(uint64_t session, 
bool enabled);
char *msime_client_set_punctua
tion_lock(uint64_t session, uint8_t lock);
/*
 每页候选数的会话内覆盖，size 为
 1–10；本平台排不下那么多时（Wi
ndows、iOS、鸿蒙最多 9）按平台上�
�截断，view.page_size 是实际值。组�
�中调用时等组字结束才生效。 */
c
har *msime_client_set_candidate_page_size(uin
t64_t session, uint8_t size);
char *msime_cli
ent_character(uint64_t session, uint8_t ascii
, bool shift);
// Explicit native punctuation
: finish the highlighted composition, then tr
anslate.
// Invalid non-punctuation bytes fai
l without modifying the session.
char *msime_
client_punctuation(uint64_t session, uint8_t 
ascii);
/* Resolve native punctuation using t
he immediately preceding Unicode scalar
 * su
pplied by the platform editor. Zero means una
vailable. Only the scalar is
 * inspected and
 no document text is retained. */
char *msime
_client_punctuation_with_context(uint64_t ses
sion, uint8_t ascii,
                        
                    uint32_t preceding);
/* B
alance Engine nesting after the host emitted 
a paired closing mark. Only
 * the ASCII book
-title opening '<' is accepted. */
char *msim
e_client_balance_paired_punctuation_after_aut
o_close(uint64_t session,
                   
                                            u
int8_t opening);
// Finish the highlighted co
mposition, then append the literal ASCII mark
.
// Hosts use this for platform smart-punctu
ation decisions based on editor
// context; i
nvalid non-punctuation bytes fail without mod
ifying the session.
char *msime_client_punctu
ation_ascii(uint64_t session, uint8_t ascii);

/* View.scheme and commit_context.scheme: 0 
quanpin, 1 shuangpin, 2 wubi, 3 japanese, 4 k
orean (preferences scheme "korean"). Korean i
s a Dubeolsik Hangul automaton: send every le
tter through msime_client_character with its 
case (Shift+Q/W/E/R/T/O/P type ㅃ ㅉ ㄸ �
� ㅆ ㅒ ㅖ); View.reading and View.preedit
 hold the composing Hangul to mark inline wit
h the caret at its end, while editing_text ho
lds the key letters of the open syllable and 
is non-empty exactly while composing. A trans
ition may carry a commit together with a new 
composition (the previous syllable finished w
hen a new one started) and a commit with hand
led=false (Space, Enter, a caret key, Delete 
or a digit ended the syllable): always insert
 the commit first, then let an unhandled key 
do its normal work in the application. Punctu
ation is always half-width ASCII and never co
nverted to full width. With no Hanja list ope
n there are no candidates: MSIME_BACKSPACE re
moves one jamo; MSIME_CANCEL discards the ope
n syllable; msime_client_focus(false) commits
 it, while msime_client_focus(true) discards 
it so a syllable left open in one client neve
r reaches the next.
 * Hanja: candidates appe
ar only after MSIME_CONVERT_HANJA while a syl
lable is composing, so a host reads scheme 4 
with a non-empty candidate list as "the Hanja
 list is open". The list holds the Hanja of t
he composing syllable (a syllable already com
mitted is not converted), each candidate's an
notation is its 훈음 when it has one, and i
ts code is the key letters, which a host shou
ld not draw. MSIME_CONVERT_HANJA answers hand
led=false when the composition has no Hanja (
a lone jamo) or nothing is composing; a host 
should then swallow its trigger key while com
posing rather than pass it on. Sending it aga
in closes the list. While the list is open th
e candidate commands work as for any list (MS
IME_COMMIT_CANDIDATE, digits 1-9 on the visib
le page, paging and MSIME_NEXT/PREVIOUS_CANDI
DATE), and choosing commits the Hanja with ha
ndled=true; send MSIME_COMMIT_CANDIDATE for R
eturn as well, since only the session knows t
he highlight. MSIME_CANCEL and MSIME_BACKSPAC
E only close the list and keep the syllable c
omposing. A letter closes the list and compos
es as usual. Punctuation, MSIME_FINISH_COMPOS
ITION and msime_client_focus(false) close the
 list and commit the Hangul, never a Hanja, w
hatever is highlighted; msime_client_focus(tr
ue) still discards the syllable. */
/* View.s
cheme 和 commit_context.scheme 续：8 tibet
an（藏文，偏好方案 "tibetan"），在
拉丁字母键盘上按 EWTS（扩展威利�
��写）输入，不需要词库，所有宿�
��都提供，和越南文一样默认不启�
��。威利转写区分大小写，大写字�
��（T D N Sh A I U M H 等）是拼写：带
大小写原样送进 msime_client_character�
��不要当成 Shift 命令。组字保存当
前音节串的威利原文，View.preedit �
� editing_text 显示转换后的藏文，候
选列表始终为空。View.spelling_symbols
 空闲时为 "'/"，组字时为 "'+-./"，�
��些键要作为字符交给引擎，尤其 
'/' 不能由宿主自己插入 ASCII 斜杠�
��组字时空格（字符 ' ' 或 MSIME_COMM
IT_CANDIDATE）上屏藏文加音节点 U+0F0
B，'/' 上屏藏文加垂符 U+0F0D，两者
都是 handled=true；没有组字时 '/' 单
独上屏垂符，空格和数字交回宿主
。MSIME_COMMIT_RAW（回车）只上屏藏�
�、不加音节点，handled=true。MSIME_BA
CKSPACE 删掉一个原文按键；第一次 
MSIME_CANCEL 把显示退回威利原文，�
�二次丢弃组字。数字、光标键、De
lete、Tab 和 MSIME_COMMIT_READING 按显示
上屏后 handled=false：先插入 commit，
再让按键在应用里照常生效。其他
标点先上屏藏文再跟半角 ASCII 标�
�，从不转成中文或全角标点。msime
_client_focus(false) 上屏组字。数字保
持原样，不转成藏文数字。 */
/* Vi
ew.scheme and commit_context.scheme, continue
d: 5 cantonese (Jyutping), 6 zhuyin (Dachen b
opomofo), 7 vietnamese (Telex or VNI), 9 stro
ke (笔画: five-stroke order 横竖撇点折
 typed as the letters h s p n z, with x match
ing any one stroke), each the preferences sch
eme of the same name. Every host offers them,
 and Cantonese, Zhuyin and Stroke only with t
heir language dictionaries installed; where a
 scheme cannot run, a preferences document na
ming it runs the last Chinese scheme or quanp
in instead and msime_client_update_preference
s says so in its diagnostic.
 * Stroke: only 
h s p n z start a composition; x and other le
tters return handled=false while idle, so a h
ost inserts them as usual. While composing x 
appends a wildcard, other letters are swallow
ed with handled=true, and digits 1-9 pick can
didates. View.reading and View.preedit hold t
he stroke glyphs 一丨丿丶乛＊, one per 
typed letter, which a host draws inline; edit
ing_text and caret_position count the ASCII l
etters. MSIME_COMMIT_RAW commits the typed le
tters, MSIME_COMMIT_CANDIDATE the highlighted
 character (the letters when there is none), 
and focus loss does not commit.
 * MSIME_OPEN
_CANDIDATE_LIST is MSIME_CONVERT_HANJA under 
the name that says what it does in every sche
me: open the active scheme's candidate list. 
Korean opens and closes its Hanja list as des
cribed above; Zhuyin opens the candidate list
 of its composition; every other scheme answe
rs handled=false. */
/* View.display_reading 
是宿主画在行内的读音。日文罗马
字（scheme 3，非本地模式、非英文�
��组字时末尾单个 n 还没定成ん，�
��里留作字母（kan 是 かn，kann 才�
� かん），与微软、Google 日文输入
法一致；其余方案和状态下与 View.
reading 相同。View.reading 仍是 MSIME_CO
MMIT_READING 上屏的读音，末尾的 n �
�那里已是ん。宿主不经引擎结束�
�字区时：光标被移走、编辑器仍�
�告着组字区，应把组字区换成 View
.reading 再结束；确认不了组字区还
在（编辑器报告没有组字区、会话
停下）时只结束组字，不要把 View.
reading 写进可能已被应用清空或改�
��的输入框，这时文档里可能留下�
��出的读音。只读 View.reading 的宿�
�行为不变。 */
/* 整句改字（全拼�
��双拼）：桌面宿主把不带修饰键�
��左右方向键发成 MSIME_CONVERSION_LEFT
 / MSIME_CONVERSION_RIGHT，Ctrl+左右发成
 MSIME_MOVE_LEFT / MSIME_MOVE_RIGHT 编辑拼
音字母。首选是一个字对一个完整
音节、覆盖整个组字、至少两个字
的句子时，MSIME_CONVERSION_LEFT 进入�
�字：View.conversion 是整句，光标在 
View.conversion_focus_start 个字（Unicode 
标量）之前，conversion_focus_start..con
version_focus_end 是候选所替换的那一
段，候选换成这一段（先是光标所
在的词，再是更短、更长的）的字
和词。之后左右键每次移一个字，
最远到句末（focus_start 等于字数，
没有候选）。选中候选（数字、MSI
ME_COMMIT_CANDIDATE、msime_client_select）�
��这段钉住，句子其余部分围绕它�
��新转换，光标移到这段后面，不�
��屏；光标在句末时 MSIME_COMMIT_CANDI
DATE 上屏整句。MSIME_COMMIT_RAW、MSIME_
FINISH_COMPOSITION、标点和失去焦点上
屏改好的整句，不是拼音字母。MSI
ME_CANCEL 和 MSIME_BACKSPACE 只退出改字
回到拼音（组字保留，钉住的段丢
掉）；键入字母或撇号、Ctrl+左右�
��Home/End 先退出改字再照常处理。�
��不了改字时（读音不完整、单字�
��光标在拼音中间、其他方案、本�
��模式）这两条命令就是 MSIME_MOVE_L
EFT / MSIME_MOVE_RIGHT。改字期间宿主�
� View.phrase_prefix 加 View.conversion 画�
��行内代替拼音，不管预编辑样式�
��的是什么；改字的候选不能置顶�
��删除或固定位置。 */
enum MsimeComma
nd {
    MSIME_BACKSPACE = 0, MSIME_COMMIT_CA
NDIDATE = 1, MSIME_COMMIT_RAW = 2,
    MSIME_
CANCEL = 3, MSIME_MOVE_LEFT = 4, MSIME_MOVE_R
IGHT = 5,
    MSIME_MOVE_HOME = 6, MSIME_MOVE
_END = 7, MSIME_DELETE_FORWARD = 8,
    MSIME
_FINISH_COMPOSITION = 9,
    MSIME_CYCLE_KANA
_VARIANT = 10, MSIME_COMMIT_READING = 11,
   
 MSIME_BACKSPACE_SEGMENT = 12, MSIME_MOVE_LEF
T_SEGMENT = 13, MSIME_MOVE_RIGHT_SEGMENT = 14
,
    MSIME_COMMIT_RAW_WITHOUT_LEARNING = 15,

    MSIME_CONVERT_HANJA = 16,
    MSIME_OPEN
_CANDIDATE_LIST = 16,
    MSIME_CONVERSION_LE
FT = 17, MSIME_CONVERSION_RIGHT = 18,
    /* 
宿主已经自己把文本写进了文档，
再丢弃组字：对会话的作用同 MSIME
_CANCEL（一次，组字没结束时照旧�
�发一次），区别只在生命周期里�
�束原因记 HostCommit、不出取消音。
 */
    MSIME_CANCEL_AFTER_HOST_COMMIT = 19,

    /* 焦点即将离开或会话即将关�
�，宿主替用户结束组字（macOS IMK �
�� commitComposition:，宿主为词库维护
关掉会话之前）：对会话的作用同
 MSIME_FINISH_COMPOSITION（只有一处不�
�：高亮的是脚本插件在 / 指令里�
�的行，即来源 14 时，上屏用户打�
��原文，例如 /uuid，因为这时用户�
��在看候选窗），生命周期里上屏�
��源记 FocusLoss，同 msime_client_focus(f
alse) 的上屏：不可改写、不出提交
音、不画上屏特效，回复里 feedback
 的 typing_effect 为 0。用户按键或点
选引起的结束组字仍发 MSIME_FINISH_C
OMPOSITION。 */
    MSIME_FINISH_ON_FOCUS_LO
SS = 20,
    MSIME_NEXT_PAGE = 100, MSIME_PRE
VIOUS_PAGE = 101,
    MSIME_NEXT_CANDIDATE = 
102, MSIME_PREVIOUS_CANDIDATE = 103,
    MSIM
E_FIRST_CANDIDATE = 104, MSIME_LAST_CANDIDATE
 = 105
};
char *msime_client_command(uint64_t
 session, uint32_t command);
/* Re-rank the v
isible candidates with the settled model, onc
e the host's typing pause elapses.
 * The hos
t owns the clock: it is the only side that kn
ows whether a keystroke arrived while the
 * 
pass was being decided. Answers {"moved": fal
se} when the order did not change, or
 * {"mo
ved": true, "view": ...} after a reorder. The
 false case lets the host leave the candidate

 * window alone without serializing a view i
t will discard. Inert, and immediately false,
 when no
 * settled model is installed. */
ch
ar *msime_client_rerank_settled(uint64_t sess
ion);
/* Pass the generation and global index
 from the displayed candidate's id. */
char *
msime_client_select(uint64_t session, uint64_
t generation, size_t index);
/* Select any en
try copied by msime_client_all_candidates for
 this exact
 * generation. Normal select rema
ins restricted to the current View page. */
c
har *msime_client_select_any_candidate(uint64
_t session, uint64_t generation, size_t index
);
char *msime_client_pin_candidate(uint64_t 
session, uint64_t generation, size_t index);

char *msime_client_remove_candidate(uint64_t 
session, uint64_t generation, size_t index);

/* Fix a dictionary candidate to slot 1..5 fo
r the current input context. */
char *msime_c
lient_fix_candidate_position(uint64_t session
, uint64_t generation, size_t index,
        
                                  uint8_t pos
ition);
/* Clear a previously fixed dictionar
y candidate position. */
char *msime_client_c
lear_candidate_position(uint64_t session, uin
t64_t generation, size_t index);
/* 从 View.
nine_key_spellings 里选一项，generation 
拒绝过期的界面。全拼下把这个拼
写锁进数字；注音下只钉住目标音
节的读音（不上屏），nine_key_spelli
ngs 随后换成下一个有歧义的音节�
� */
char *msime_client_choose_nine_key_spell
ing(uint64_t session, uint64_t generation, si
ze_t index);
/* 全拼九键组字时筛选�
�选，作用到这次组字结束：single_c
haracter 只留单字；strokes 是 length �
�节的笔顺前缀（h 横、s 竖、p 撇�
�n 点、z 折，<=64），只留首字笔顺
以它开头的候选，length 为 0 时不�
�笔画筛选（strokes 可为 NULL）。当�
��状态见 View.nine_key_single_character �
� View.nine_key_strokes。没有九键组字�
�� strokes 含其他字节时 handled=false�
�笔画字典不可用时 handled=true 并带
 diagnostic LANGUAGE_DICTIONARY_UNAVAILABLE�
�筛选不变。
 * View.nine_key_spellings �
��全拼九键下的含义：先是完整音�
��；再是下一个数字键上能起头一�
��音节的字母，大写（M N O），选�
�限定下一个音节的首字母；最后�
�这个数字本身（6），只在没有锁�
��的音节时出现，选它直接上屏这�
��数字。数字全部锁定后它是最后�
��次锁定时的选项，选其中一项就�
��掉那次锁定。这时退格先撤销最�
��一次锁定，其余情况退格删数字�
��
 * 14 键下含义相同，只是没有最
后那一项「原样上屏」：完整音节
，再是下一键上能起头一个音节的
大写字母（Q W）。preedit 是组码字�
��（bugao），不是拼音，读音行用 V
iew.nine_key_reading。 */
char *msime_client
_set_nine_key_filter(uint64_t session, bool s
ingle_character,
                            
           const uint8_t *strokes, size_t len
gth);
/* 滑行输入：手指一笔滑过字
母键，由引擎解码成最可能拼出的
全拼字母，像打字一样写到组字的
光标处（与已有字母之间用 ' 隔开
）。request 是 length 字节的 UTF-8 JSON
（<=65536，拒绝未知键）：{"keys":[[x
,y] x 26],"key_width":w,"key_height":h,"point
s":[[x,y] 或 [x,y,ms]，2..1024 个]}，keys
 是 a..z 各键中心（按此次序），ke
y_width/key_height 是一个字母键的尺�
�，全部在宿主自选的同一个坐标�
�里；ms 是距笔画开始的毫秒数，�
�了它，手指在键上停一下就能确�
�那个键。返回与 msime_client_character
 相同的输入响应。方案不是全拼�
�在本地模式或专用英文里、九键�
�字正在组字，或者没有音节跟得�
�这一笔时 handled=false：宿主丢掉这
一笔，不得把它经过的键当作按键
输入。在会话线程上、手指抬起时
调用；一次触摸是滑行还是点按只
由宿主判断。 */
char *msime_client_glid
e(uint64_t session, const uint8_t *request, s
ize_t length);
enum MsimeCandidateEdge { MSIM
E_FIRST_HAN = 0, MSIME_LAST_HAN = 1 };
/* Eng
ine selects one Han character and clears comp
osition on success.
 * A candidate without Ha
n text is unhandled and keeps composition; no
 fallback
 * punctuation or candidate commit 
is manufactured. Uses the same current-page
 
* identity checks as select. Invalid edge val
ues fail before state changes.
 */
char *msim
e_client_select_edge(uint64_t session, uint64
_t generation, size_t index, uint8_t edge);
/
* On-demand {session,generation,preedit,candi
dates:[Candidate...]}. Unlike View,
 * candid
ates contains the complete cached Engine gene
ration with global IDs. */
char *msime_client
_all_candidates(uint64_t session);
/* Return 
read-only English completions for a bounded A
SCII prefix. */
char *msime_client_english_co
mpletions(uint64_t session, const uint8_t *pr
efix,
                                       
size_t prefix_length, size_t limit);
/* View.
local_mode 是 Engine 自己的模式，不�
�从预编辑前缀猜出来的；View.micros
oft_shuangpin 报告 Engine 已应用的配�
�，不是尚未生效的新偏好；它为�
�表示当前是双拼且韵母或零声母�
�码用 `;` 作第二键，宿主要把 `;` �
��字母键交给 msime_client_character。�
�主结合模式、编辑文本和光标使�
�它。取值为 none、unicode、date_time�
�quick_phrase、emoji、kaomoji、super_jianp
in、temporary_english、temporary_japanese�
�expression、command、mention、url、chaiz
i（拆字反查，Shift+C 进入；候选注
释是字的带调读音，由 Engine 给出�
��，未知取值视为不可用状态。
 * 
View.spelling_symbols 列出 Engine 在当前
状态下当作输入的非字母键：当前
模式的拼写（expression 的数字和运�
��符、unicode 的数字、url 的数字和�
��址符号）；没有组字时打开模式�
��键（`/` 和 `@`）；全拼、双拼、�
�笔组字中原文恰好是 www、http、htt
ps、ftp 时打开网址模式的键（`.` �
� `:`）。宿主要把它们当字符发送�
��列在里面的数字是输入，不是选�
��选的快捷键。转换结果的 commit_co
ntext.typing_statistics 对 expression、comm
and、mention 模式生成的文本为 false�
��这些文本不计入打字统计。
 * 中
文标点下 '\'' 与 '"' 出哪种引号、'
[' ']' 出哪种方头括号由偏好 quote_s
tyle、bracket_style 决定（契约 shared/c
ontracts/punctuation 的 styles 段）。View
.punctuation_marks 报告引擎已应用的�
�式：{"quotes": "curly"|"corner", "brackets
": "solid"|"hollow"}，corner_when_traditiona
l 已按繁体输出开关解析成 curly 或
 corner。宿主做成对补全、跨过后�
�个、空格转换、英文模式下的中�
�标点和画触屏键面时读它，不自�
�解析偏好；改了偏好要等空闲重�
�后它才变，和引擎出字同步。
 */

char *msime_client_view(uint64_t session);
/*
 在线候选查询包含 cloud_request（GET
/POST、请求头、JSON 请求体、超时�
�响应格式和可选缓存回包）。
 * �
��述符可能含自定义服务凭据，只�
��给宿主传输，不写日志或上传诊�
��。
 * 各接收 OnlineQuery 的接口最�
�接受 128 KiB UTF-8 JSON；候选列表与�
��包沿用各自上限。
 * 云与 AI 共�
�这份查询，宿主在后台执行后原�
�交回，组字或来源变化会拒绝迟�
�结果。 */
#define MSIME_ONLINE_QUERY_MAX_
BYTES 131072
char *msime_client_online_query(
uint64_t session);
/* Build a validated AI HT
TP descriptor for a copied OnlineQuery. The h
ost
 * resolves credentials from its current 
preferences; the query must still
 * match th
e active AI configuration. */
char *msime_cli
ent_ai_request_for_query(uint64_t session,
  
                                      const u
int8_t *query,
                              
          size_t query_length);
/* 把偏好�
��外（例如 iOS 钥匙串）的 AI 凭据�
��给会话；仅覆盖当前端点的令牌�
��
 * 不持久化或回传，也不会随偏
好切到别的端点。长度为零表示撤
销。
 * 凭据确实变化时，当前组�
�的旧 AI 候选和旧在线查询失效；�
��复设置同一凭据不变。 */
char *msi
me_client_set_ai_credential(uint64_t session,
 const uint8_t *token,
                      
               size_t token_length);
/* Retur
n null or {generation,target_language,candida
tes:[{text}], provider:"none"|"account"|"tenc
ent"|"niutrans"|"custom", translation_account
:bool, custom_translation:{enabled,endpoint,a
pi_key}|null, tencent_tmt:{enabled,secret_id,
secret_key,region}|null, niutrans:{enabled,ap
p_id,apikey}|null} for visible candidates.
 *
 provider names the service selected in prefe
rences even when its configuration is incompl
ete; a transport must ask that service or non
e, never fall back to another. Credentials ar
e returned only for the selected usable provi
der. These fields are for host-owned transpor
t; never log the query. The existing target_l
anguage applies to both providers. translatio
n_account is true only when the user explicit
ly chose the MSIME account, candidate_transla
tions is on and no service of the user's own 
applies; it is the whole decision for a host'
s account gloss path, which must send nothing
 when it is false. offline_gloss_languages is
 present only when non-empty: the non-English
 target languages, in preference order, whose
 offline dictionary is installed beside resou
rces or in the downloaded offline-glosses res
ource pack; ask msime_client_candidate_gloss_
request with that target_language for each. s
tate_root is present only when one of them co
mes from the pack; pass it on in that request
. candidate_pronunciation is present (true) o
nly when the user turned pronunciation on: th
e host then pronounces the English and Japane
se gloss lines it draws, English through msim
e_client_pronunciation_request, as display te
xt that is never committed. gloss_breakdown i
s present (true) only when candidate translat
ion or the offline English gloss is on and En
glish is a target language, as macOS requests
 breakdowns: the host then asks msime_client_
gloss_breakdown_request about the sentence ca
ndidates, with resources, which the query car
ries whenever gloss_breakdown is present.
 * 
While the composition is a /fy request (comma
nd mode) the query is that request alone, wha
tever the gloss switches say: sentence:true, 
target_language "zh", one candidate {text, on
line_gloss:false} holding the English typed a
fter the command, no resources, user_data or 
offline_gloss_languages, and null when no ser
vice of the user's own (tencent, niutrans or 
custom) is selected. Ask only that service, w
ithout the offline dictionary or the gloss ca
che, and hand the answer back through msime_c
lient_apply_translations, which shows it as t
he first row and commits the translation.
 */

char *msime_client_translation_query(uint64_
t session);
/* Google 的默认预算；cloud
_request 存在时按描述符中的毫秒预
算执行。 */
#define MSIME_CLOUD_CONNECT_T
IMEOUT_MS 2000
#define MSIME_CLOUD_REQUEST_TI
MEOUT_MS 2000
/* 兼容 GET 云候选的 URL 
入口；POST 宿主直接执行 OnlineQuery.
cloud_request。 */
char *msime_client_cloud_
request_url(const uint8_t *query, size_t quer
y_length);
/* 共享解析器处理宿主取�
��的云响应，返回 {applied,view}；空�
��无效响应不改变组字。
 * 查询最
多 128 KiB，响应最多 256 KiB。过期�
�关闭或切换来源的查询不应用候�
�。 */
char *msime_client_apply_cloud_respon
se(uint64_t session,
                        
              const uint8_t *query, size_t qu
ery_length,
                                 
     const uint8_t *body, size_t body_length)
;
/* The #if !defined(_WIN32) blocks below mi
rror #[cfg(unix)] exports, so a
 * Windows ho
st fails at compile time instead of with unre
solved externals. */
#if !defined(_WIN32)
/* 
Linux: perform one bounded request to a user-
owned Unix-socket provider.
 * Call from a wo
rker thread with a copied query; returns null
 value when no
 * candidate is available. Cre
dentials and network policy stay in that serv
ice. */
char *msime_client_online_provider_re
quest(const uint8_t *query,
                 
                          size_t query_length
,
                                           
const uint8_t *socket_path,
                 
                          size_t socket_lengt
h);
/* Linux: forward one validated account-b
acked dictionary operation to a
 * user-owned
 Unix-socket provider. The provider owns cred
entials and sync. */
char *msime_client_cloud
_dictionary_provider_request(const uint8_t *r
equest,
                                     
                size_t request_length,
      
                                             
  const uint8_t *socket_path,
               
                                      size_t 
socket_length);
/* Linux: forward one validat
ed cloud clipboard operation to a user-owned

 * Unix-socket provider. The provider owns cr
edentials and retention policy. */
char *msim
e_client_cloud_clipboard_provider_request(con
st uint8_t *request,
                        
                            size_t request_le
ngth,
                                       
             const uint8_t *socket_path,
    
                                             
   size_t socket_length);
#endif
/* 云剪贴
板端到端加密的 sans-IO 驱动，所有
平台都有。输入 {start,keystore,device_
name} 开始一个操作，或 {resume,respon
se} 交回上一步请求的响应；value �
� {request,state,keystore}、{done,keystore} 
或 {error,keystore}。宿主发请求并保�
�� keystore，驱动不访问会话、不联�
��。协议见 docs/cloud-clipboard-e2e.md。
请求最大 2 MiB。 */
char *msime_client_c
loud_clipboard_e2e(const uint8_t *request, si
ze_t request_length);
/* Persist {target_lang
uage,translations:[{text,translation}]} in an
 existing
 * absolute user-data directory. On
ly short changed English-target glosses are
 
* saved. Candidate gloss requests may include
 user_data to read this overlay.
 * Maximum r
equest size 128 KiB; path 4096 bytes. Does no
t access a session. */
char *msime_client_tra
nslation_gloss_save(const uint8_t *request, s
ize_t request_length,
                       
                  const uint8_t *user_data, s
ize_t user_data_length);
/* TranslationQuery 
accepts the optional sentence:true flag for a
n explicit
 * single-item sentence request (u
p to 512 Unicode characters); ordinary
 * can
didate gloss requests retain their normal lim
its. */
#if !defined(_WIN32)
char *msime_clie
nt_translation_provider_request(const uint8_t
 *query,
                                    
            size_t query_length,
            
                                    const uin
t8_t *socket_path,
                          
                      size_t socket_length);

/* Linux handwriting panel adapter. The query
 is a bounded JSON object with
 * language an
d normalized stroke arrays; the user-owned so
cket returns
 * {candidates:[...]} and owns r
ecognizer/model policy. */
char *msime_client
_handwriting_provider_request(const uint8_t *
query,
                                      
          size_t query_length,
              
                                  const uint8
_t *socket_path,
                            
                    size_t socket_length);
/*
 Run the optional offline Engine recognizer a
gainst a trusted packaged model.
 * The model
 path must be absolute; response is {candidat
es:[...]} or an error. */
char *msime_client_
handwriting_local_request(const uint8_t *quer
y,
                                          
   size_t query_length,
                     
                        const uint8_t *model_
path,
                                       
      size_t model_length);
/* Linux standalo
ne emoji panel adapter. The query contains se
arch/category
 * text and a bounded result li
mit; the socket returns {items:[...]}. */
cha
r *msime_client_emoji_provider_request(const 
uint8_t *query,
                             
             size_t query_length,
           
                               const uint8_t 
*socket_path,
                               
           size_t socket_length);
/* Query th
e verified local msime-others.db Emoji catalo
g. Resources is an absolute
 * generation dir
ectory containing msime-others.db; no provide
r socket is needed.
 * Optional offset is a n
onnegative SQL row offset (default 0); limit 
is 1..255.
 * Optional group filters a catalo
g subdivision; list_groups:true returns
 * {g
roups:[name,...]} in catalog order instead of
 an item page.
 * list_symbol_groups:true ret
urns {symbol_groups:[{parent,title},...]}.
 *
 Optional parent narrows symbols to a parent 
category before paging.
 * list_plugin_symbol
_groups:true with plugins (absolute plugins d
irectory) returns
 * {plugin_symbol_groups:[{
pack,pack_name,tab:"symbols"|"kaomoji",title,
keywords,items:[text,...]},...]}:
 * 已安�
�符号集插件的全部组，包按名字�
�序、组按清单顺序；keywords 没写�
�为空串。宿主把 symbols 组追加在�
�置符号之后、以 pack_name 为上级分
类，
 * kaomoji 组追加在颜文字 All �
��后，不与内置目录去重。没传 plu
gins 时为空列表。
 * Advance offset by 
limit, not returned item count: each page ded
uplicates text. */
char *msime_client_emoji_c
atalog_request(const uint8_t *query,
        
                                 size_t query
_length,
                                    
     const uint8_t *resources,
              
                           size_t resources_l
ength);
#endif
/* Shared Doubao authenticatio
n policy. Input (max 32768 bytes):
 * {auth_m
ode,app_id,token,resource_id}; an absent or e
mpty mode means api_key.
 * Response value: {
headers:[[name,value],...]}. Contains credent
ials: never
 * log/persist the response; rele
ase with msime_client_string_free. */
char *m
sime_client_doubao_auth_headers(const uint8_t
 *request, size_t length);
#if !defined(_WIN3
2)
/* Linux voice adapter. The user-owned soc
ket captures audio and runs ASR,
 * returning
 {text}; the query contains language and the 
active generation. */
char *msime_client_voic
e_provider_request(const uint8_t *query,
    
                                      size_t 
query_length,
                               
           const uint8_t *socket_path,
      
                                    size_t so
cket_length);
#endif
/* Decode one Doubao v1 
response frame. The value contains either {la
st,payload}
 * for a UTF-8 JSON response or {
error_code} for a type-0xF error frame. */
ch
ar *msime_client_doubao_decode_frame(const ui
nt8_t *frame,
                               
        size_t frame_length);
/* Build caller
-owned Doubao v1 start and PCM/final frames. 
If capacity is too
 * small, output_length re
ceives the required size and no bytes are wri
tten. */
bool msime_client_doubao_start_frame
(bool enable_itn, bool enable_punc,
         
                            bool enable_ddc,

                                     const ui
nt8_t *boosting_table_id,
                   
                  size_t boosting_table_id_le
ngth,
                                     ui
nt8_t *output, size_t output_capacity,
      
                               size_t *output
_length);
bool msime_client_doubao_audio_fram
e(int32_t sequence, const uint8_t *pcm,
     
                                size_t pcm_le
ngth, bool final_chunk,
                     
                uint8_t *output, size_t outpu
t_capacity,
                                 
    size_t *output_length);
#if !defined(_WIN
32)
typedef void (*msime_client_voice_update_
callback)(const uint8_t *text,
              
                                     size_t t
ext_length,
                                 
                  bool final,
               
                                    void *con
text);
/* Stream newline-delimited provider u
pdates. The callback runs on the caller
 * th
read and receives bounded interim/final UTF-8
 text; context is untouched. */
char *msime_c
lient_voice_provider_stream(
    const uint8_
t *query, size_t query_length, const uint8_t 
*socket_path,
    size_t socket_length, msime
_client_voice_update_callback callback,
    v
oid *context);
/* Optional phase notification
s: 0=recording, 1=recognizing, 2=polishing.
 
* Both callbacks run synchronously on the cal
ler thread and must not throw. */
typedef voi
d (*msime_client_voice_status_callback)(uint8
_t phase, void *context);
char *msime_client_
voice_provider_stream_events(
    const uint8
_t *query, size_t query_length, const uint8_t
 *socket_path,
    size_t socket_length, msim
e_client_voice_update_callback callback,
    
msime_client_voice_status_callback status_cal
lback, void *context);
/* Normalized micropho
ne level in [0, 1]; never transcript text or 
audio.
 * Callback runs synchronously on the 
caller thread and must not throw.
 * All thre
e stream calls return {"ok":true,"value":{"te
xt":...}} on success and value null when the 
provider gave no result. A provider that name
s a missing optional dependency returns {"ok"
:false,"error":"voice_dependency_missing:webs
ockets"}, "voice_dependency_missing:recorder"
 or "voice_dependency_missing:local_asr". */

typedef void (*msime_client_voice_level_callb
ack)(float level, void *context);
/* Return t
rue to stop a streaming provider read. The ca
llback runs on the
 * provider worker at leas
t every 100ms and must not throw. */
typedef 
bool (*msime_client_voice_cancellation_callba
ck)(void *context);
char *msime_client_voice_
provider_stream_feedback(
    const uint8_t *
query, size_t query_length, const uint8_t *so
cket_path,
    size_t socket_length, msime_cl
ient_voice_update_callback callback,
    msim
e_client_voice_status_callback status_callbac
k,
    msime_client_voice_level_callback leve
l_callback, void *context);
/* Cancellable va
riant used by detached hosts. The legacy feed
back entry point
 * remains equivalent to pas
sing a null cancellation callback. */
char *m
sime_client_voice_provider_stream_feedback_ca
ncelled(
    const uint8_t *query, size_t que
ry_length, const uint8_t *socket_path,
    si
ze_t socket_length, msime_client_voice_update
_callback callback,
    msime_client_voice_st
atus_callback status_callback,
    msime_clie
nt_voice_level_callback level_callback,
    m
sime_client_voice_cancellation_callback cance
llation_callback,
    void *context);
/* Requ
est cancellation of a provider capture sessio
n by generation. */
char *msime_client_voice_
provider_cancel(const uint8_t *socket_path,
 
                                        size_
t socket_length,
                            
             uint64_t generation);
/* Ask the
 provider to finish capture and deliver the f
inal stream result. */
char *msime_client_voi
ce_provider_stop(const uint8_t *socket_path,

                                       size_t
 socket_length,
                             
          uint64_t generation);
#endif
/* App
ly a UTF-8 cloud (source=0) or AI (source=1) 
result for a copied query. */
char *msime_cli
ent_apply_online_candidate(uint64_t session,

                                           co
nst uint8_t *query,
                         
                  size_t query_length,
      
                                     const ui
nt8_t *candidate,
                           
                size_t candidate_length,
    
                                       uint8_
t source);
/* Apply a JSON array of UTF-8 str
ings for one source (cloud=0, AI=1).
 * Buffe
rs are borrowed for the call; at most 16384 b
ytes each. */
char *msime_client_apply_online
_candidates(uint64_t session,
               
                           const uint8_t *que
ry,
                                         
 size_t query_length,
                       
                   const uint8_t *candidates,

                                          si
ze_t candidates_length,
                     
                     uint8_t source);
/* Remo
ve cached and visible rows for one source (so
urce=0 cloud, 1 AI). */
char *msime_client_cl
ear_online_candidates(uint64_t session, uint8
_t source);
/* AI 回答为空、失败，或
 apply_online_candidates 返回 applied:false
 时宿主会调用它（source 只认 0 和 
1，其余报错）。共享内核不再放 A
I 占位行（候选 source 12），AI 回答
到达时第一条直接插在第一页最后
一格；这个入口为已发布的宿主留
着，合法的调用一律返回 {applied:fa
lse, view}，不改候选、不换代。 */
c
har *msime_client_resolve_online_candidates(u
int64_t session,
                            
                 const uint8_t *query,
      
                                       size_t
 query_length,
                              
               uint8_t source);
/* Resolve a 
shared surface route ("settings", "settings:v
oice", "emoji", ...)
 * so a host launches th
e shared shell by name. Returns the canonical
 route and,
 * for panel surfaces, the window
 label, query and geometry. */
char *msime_cl
ient_parse_surface_route(const uint8_t *value
, size_t length);
/* Convert UTF-8 Simplified
 Chinese to Traditional Chinese with the shar
ed
 * OpenCC s2t tables (phrase-level, so 头
发 -> 頭髮 and 发展 -> 發展). Unlike
 
* the JSON calls, returns the converted NUL-t
erminated text directly because
 * hosts call
 it per candidate. Returns NULL for NULL, inp
uts over 1 MiB,
 * invalid UTF-8 or an embedd
ed NUL; keep the original text then. Free wit
h
 * string_free. The first call parses the t
ables (a few milliseconds in
 * release build
s). */
char *msime_client_simplified_to_tradi
tional(const uint8_t *text, size_t length);
/
* 同上，但按 variant 选 OpenCC 配置�
�0 s2t、1 s2tw（臺灣字形）、2 s2twp
 
* （臺灣字形加惯用词），与偏好 
traditional_chinese_variant 的 "s2t"、"s2tw
"、
 * "s2twp" 一一对应。未知的 vari
ant 与其它非法输入一样返回 NULL。
臺灣配置的
 * 附加词表在第一次�
�到时解析。Free with string_free. */
cha
r *msime_client_convert_to_traditional(const 
uint8_t *text, size_t length,
               
                           uint32_t variant);

/* Display-only font aliases. Input: JSON ar
ray, at most 33 names / 32 KiB.
 * Returns th
e standard response with an array value. Free
 with string_free.
 * Resolution failure reta
ins the corresponding original family name. *
/
char *msime_client_resolve_font_families(co
nst uint8_t *value, size_t length);
/* Instal
led font family names, the catalog the shared
 app's 刷新字体列表 reads.
 * Returns t
he standard response with a sorted array of d
istinct names, each
 * non-empty, at most 128
 UTF-8 bytes and free of control characters; 
error
 * "unsupported" on a host without a ca
talog, "font_catalog" or
 * "font_catalog_lim
it" (over 16384 families) when enumeration fa
ils.
 * Enumerates the system's fonts: call i
t off the UI thread. Free with
 * string_free
. */
char *msime_client_list_font_families(vo
id);
/* Describe what the named host ("window
s"/"macos"/"linux"/"android"/"ios") can
 * do
, so the shared UI renders from capabilities 
rather than the user agent. */
char *msime_cl
ient_host_capabilities(const uint8_t *platfor
m, size_t length);
/* 调用方可执行文�
�旁的 msime-mcp，供桌面外壳以外的�
��置宿主使用。JSON 请求（<=65536 字
节）{options:运行时选项的绝对路径
|null}。返回 {command,installed,options,co
nfig,clients:[{id,path,configured,flags}]}，
flags 是已写入条目带的权限参数。
会读取助手的配置文件，请在工作
线程调用。 */
char *msime_client_mcp_sta
tus(const uint8_t *request, size_t length);
/
* 把 msime 条目写进一个助手的配置
，保留其它所有键。JSON 请求 {optio
ns,client:"claude_desktop"|"cursor",flags?:["
--allow-write"|"--allow-dictionary-read"],rep
lace:bool}，flags 省略时写只读条目�
�返回 "added"|"updated"|"replaced"|"unchang
ed"；已有条目只差权限参数时直接
更新，其它不同的 msime 条目在未�
� replace 时以 mcp_entry_exists 失败。�
�写文件，请在工作线程调用。 */
c
har *msime_client_mcp_install(const uint8_t *
request, size_t length);
/* Where this librar
y reports a failure it recovers from by itsel
f, such as a sound or music pack that does no
t load, an audio device that does not open, o
r a helpcode pack the Engine falls back from.
 Without a sink each report is one line on st
derr prefixed "msime: "; with one, sink recei
ves the same line without the prefix, as NUL-
terminated UTF-8 valid only during the call. 
Each line is "<category>: <detail>": the cate
gory is a fixed English phrase saying what fa
iled, such as "sound pack not loaded", and ho
lds no colon, so the text before the first co
lon names no pack, file or cause; the detail 
names packs, files and error causes. Neither 
holds typed text, candidates or credentials; 
a host whose log must not keep the detail kee
ps only the category. Pass NULL to go back to
 stderr; a later call replaces the sink. A re
port already under way on another thread may 
still call the sink it read after the call th
at replaced or cleared it has returned, so ev
ery sink ever registered must stay callable f
or the rest of the process: do not unload or 
free its code. sink may be called on any thre
ad, including the audio player's own and seve
ral at once, and from inside other msime_clie
nt calls: it must not block, must not call an
y msime_client function, and must return norm
ally. Register it once at startup, before the
 first session. */
typedef void (*msime_clien
t_diagnostic_sink)(const char *line);
void ms
ime_client_set_diagnostic_sink(msime_client_d
iagnostic_sink sink);
/* Effect sounds and ba
ckground music, played by this library on mac
OS, Windows and Linux from the session's pref
erences.plugins. The three calls below are fo
r the key path: they return whether a request
 was queued, never block, decode or read file
s, and need no free. False means nothing is s
witched on, the session handle is unknown or 
on another thread, the queue is full, this pl
atform does not play (iOS, Android, HarmonyOS
), or sound failed earlier in this process, w
hich turns it off until the process restarts 
with one line through msime_client_set_diagno
stic_sink (stderr without one). Nothing start
s - no thread, no audio device - until a call
 finds something switched on, so a process th
at never calls them (the Windows TSF DLL) pay
s nothing; the device is let go again after 3
0 s without a sound or playing music. Do not 
call them for keys typed into a secure (passw
ord) field.
 * key_sound: key_class 0 any oth
er key, 1 space, 2 enter, 3 backspace; anythi
ng else queues nothing. Plays the key pack's 
sample for the class, or the melody pack's ne
xt note in melody mode.
 * commit_sound: call
 when a transition commits text. Plays the ke
y pack's commit sample when the commit sound 
is on, and the next note of a melody that adv
ances on commits.
 * music_set_active: true w
hile the input method is active in a field th
at is not a secure one, false when it deactiv
ates or a secure field gains focus; music pla
ys only in between.
 * Achievement sounds nee
d no call: msime_client_typing_statistics rec
ord plays one when the count passes a milesto
ne. */
bool msime_client_key_sound(uint64_t s
ession, uint32_t key_class);
bool msime_clien
t_commit_sound(uint64_t session);
bool msime_
client_music_set_active(uint64_t session, boo
l active);
/* 生命周期核心（桌面宿�
��选择加入；移动端宿主不调用，�
��为和 transition JSON 都不变）。以�
�调用都在会话所在线程上进行，�
�落盘，不分配要释放的内存；未�
�会话、错线程、重入调用或内部 p
anic 时返回 0 或 false。
 * msime_client
_set_lifecycle：让会话选择加入，或�
��一组位；返回实际启用的位，其�
��位被忽略。没调用过它的会话不�
��导任何事件。
 *   MSIME_LIFECYCLE_FEE
DBACK：按键音、上屏音、打字特效�
��背景音乐改由核心驱动。按键音�
��按键特效来自 msime_client_key；上�
�音和上屏特效（typing_effect 事件 4�
��来自核心的上屏事件，派发类调�
��的回复里多一个 "feedback":{"typing_e
ffect":N} 键，N 的位布局同 msime_clien
t_typing_effect，没有上屏时为 0；音�
��由 msime_client_set_active 仲裁所有者
。本会话的 msime_client_key_sound、msim
e_client_commit_sound、msime_client_typing_e
ffect（事件 0-4）和 msime_client_music_s
et_active 随之变为空操作（返回 fals
e 或 0）。typing_effect 事件 5（绕过�
��键音的删除）不经过 msime_client_ke
y，核心看不到，仍由宿主调用。�
�焦上屏（msime_client_focus(false) 和 MS
IME_FINISH_ON_FOCUS_LOSS）不出声也不画
。字段属性带 SECURE 时没有任何声�
��和特效，音乐关闭；带 FULLSCREEN �
��声音静音、特效照画（同 typing_ef
fect 的位 9）。
 *   MSIME_LIFECYCLE_COMM
IT_OBSERVE_ONLY：之后的上屏只能观察
、不能改写，可随时切换。TIP 已�
�从自己的会话把文字写进文档的�
�径（Windows），Server 在派发前设上
、派发后清掉。设着它时脚本插件
的候选变换也停下：派发里不运行
 candidates，旁路调用也不重放之前�
��重排、隐藏和注释，列表保持引�
��和内置排序的顺序。TIP 在自己的
会话里选词的列表（注音等 AlwaysIn
linePreedit 方案）按那个会话的顺序
选，Server 一侧重排或隐藏就会让�
�示的顺序和数字选中的对不上，�
�以这类列表的每一次派发都要设�
�它。
 *   MSIME_LIFECYCLE_HOST_HOLDS_PIECE
S：宿主自己暂存部分选词（Windows�
��，补齐整个词时才把各段拼好写�
��文档。这样的会话里每次部分选�
��都是一次上屏，commit 事件按段投
递，插件读到的 commit 只是这一段�
��这个词的各段，包括补齐它的那�
��段，before_commit 都不可改写。部�
�选词上屏的那一段还在宿主手里�
�没进文档，FEEDBACK 不为它出上屏�
�、不画上屏特效（transition 的 feedb
ack 为 0），整段送出的那次上屏再
出；生命周期事件照常发。
 * 顺�
�契约。轮次：宿主每把一个物理�
�下交给输入法，都要在为这个键�
�用任何派发之前先调一次 msime_clie
nt_key，它开启新的一轮；到下一次
 msime_client_key 之前的所有派发和 fo
cus 调用都属于这一轮，同一个键�
�能触发多次派发（如 Windows 对同�
�个键再发一次 MSIME_CANCEL）。指针�
��选和翻页先调 msime_client_key(session
, MSIME_KEY_POINTER)。带 MSIME_KEY_PASSTHRO
UGH 的键（Windows 交给应用、由 TIP �
�� Aux 管道异步上报的键）不开新�
�一轮，也不承诺先于应用处理。�
�以"打字前"只承诺先于本轮的第一
次派发。
 *   过渡期的例外：msime_
client_key 还没有"开一轮但不出声"�
�位，所以 Linux 的 IBus 和 Fcitx5 宿�
�只为会出按键音的打字键调用它�
�松开、单独的修饰键、带 Ctrl/Alt/S
uper 的组合键、录音中的键和 IBus �
��幕键盘面板送来的键都不开新的�
��轮，它们引起的派发（如组字中�
�� Ctrl 组合键发出的 MSIME_FINISH_COMPO
SITION 或 MSIME_CANCEL、单按 Shift 切换
中英文时的 MSIME_COMMIT_RAW）算在上�
��轮里。第 3 阶段按轮计 fuel 的脚�
��运行时不会因为这类没有先调 msi
me_client_key 的派发熔断；补上这个�
��之后，宿主再为这些按下也调用�
��，让它们各自成一轮。
 * 同一次
调用内的事件顺序：input（派发前�
��→ selection_retracted → candidate_selec
ted → before_commit → commit → composit
ion_end 或 composition_start → composition
_update → candidates_shown 或 candidates_h
idden，每次调用最多一个候选事件�
��focus 调用没有 input，focus_in/focus_o
ut 排在它自己的上屏和组字结束之
后；失焦上屏一律不可改写。destro
y 先发 composition_end{Destroyed}，最后�
��会话结束。组字状态只在调用边�
��前后各采样一次。一次 transition �
��时带着上屏和新组字时（韩文音�
��延续）只发 commit{continues}，不发 
end 和 start。apply_online_*、resolve_onli
ne_candidates、clear_online_candidates、app
ly_cloud_response、rerank_settled、apply_tr
anslations 这些旁路调用，以及 set_en
glish_mode、set_chinese_punctuation、set_pu
nctuation_lock、update_preferences、set_ai_
credential 这些可能在组字中途改动�
��字或候选的设置调用，只给核心�
��视图类事件（composition_end{Other}、
candidates_shown/hidden 等）。派发、foc
us 和上面这些调用报错时照样推导
事件：runtime 可能在报错前已经结�
��或改动了组字，start 和 end 始终�
�对。事件不带文本，也不写进任�
�逐键记录。
 * Windows TIP 会话的约�
��：TIP 在应用进程里自己的会话只
给 Server 转述，不调用以下任何一�
��函数；字段属性和激活由 Server �
�会话报告。
 * msime_client_key：key �
�位 0-6 是按键类别 0-3（同 key_sound�
��，位 7（MSIME_KEY_POINTER）是指针调
用，类别被忽略，不出声不画；位
 8（MSIME_KEY_REPEAT）是自动重复，画
但不计连击；位 10（MSIME_KEY_PASSTHRO
UGH）见上。其余位忽略，类别超出
 0-3 时什么都不做。返回值是 FEEDBA
CK 下这个键的打字特效，位布局同
 msime_client_typing_effect；没开 FEEDBACK
 时为 0。
 * msime_client_set_field_traits
：宿主在每个键之前比较一次字段
属性，变了就先调它，再调 msime_cl
ient_key。没有 MSIME_TRAIT_KNOWN 位时属
性按未知处理：上屏不可改写；但
 SECURE 和 FULLSCREEN 位本身仍然生效�
��声音、特效和音乐照样按上面的�
��则关闭或静音。返回现在生效的�
��性（不认识的位被丢掉）；会话�
��选择加入时返回 0。
 * msime_client_
set_active：输入法在这个会话里激�
�（true）或停用（false）。FEEDBACK �
�，true 让调用者成为背景音乐的所
有者，只有所有者能开关音乐，所
有者被销毁时音乐自动关闭；实际
的音乐状态是"是所有者、处于激�
�、字段既不是 SECURE 也不是 FULLSCRE
EN"。会话已选择加入时返回 true。 
*/
#define MSIME_LIFECYCLE_FEEDBACK 0x1u
#def
ine MSIME_LIFECYCLE_COMMIT_OBSERVE_ONLY 0x4u

#define MSIME_LIFECYCLE_HOST_HOLDS_PIECES 0x8
u
#define MSIME_KEY_POINTER 0x80u
#define MSI
ME_KEY_REPEAT 0x100u
#define MSIME_KEY_PASSTH
ROUGH 0x400u
#define MSIME_TRAIT_SECURE 0x01u

#define MSIME_TRAIT_PRIVATE 0x02u
#define MS
IME_TRAIT_NO_SPELLCHECK 0x04u
#define MSIME_T
RAIT_FULLSCREEN 0x08u
#define MSIME_TRAIT_VOI
CE_ACTIVE 0x10u
#define MSIME_TRAIT_HOST_ENGL
ISH 0x20u
#define MSIME_TRAIT_ELEVATED 0x40u

#define MSIME_TRAIT_KNOWN 0x80000000u
uint32_
t msime_client_set_lifecycle(uint64_t session
, uint32_t flags);
uint32_t msime_client_key(
uint64_t session, uint32_t key);
uint32_t msi
me_client_set_field_traits(uint64_t session, 
uint32_t traits);
bool msime_client_set_activ
e(uint64_t session, bool active);
/* 脚本�
�件（桌面）。msime_client_scripts_enabl
e 是进程级开关，返回开关的实际�
��态；任何线程都可以调用，不分�
��要释放的内存。只由 4 个桌面入�
��在启动时、第一个会话之前调用 
msime_client_scripts_enable(true)：macOS 输
入法进程、Windows Server、IBus 引擎�
�程和 Fcitx5 addon。Windows TIP（与 Serv
er 共用同一份库，被注入每个应用
进程）、设置应用和 MCP 从不调用�
��所以解释器的代码在它们的进程�
��，但从不执行。iOS、Android 和 Harm
onyOS 上恒返回 false。
 * 授权文件 <
state root>/plugins/script-grants.json 在用
户可写的目录下，不签名：同一用
户下的进程能伪造授权。三个桌面
平台的开发构建和 release 构建都运
行脚本，这一残余风险写在授权页
上：macOS 和 Linux 是共享设置页，Wi
ndows 是原生设置应用（经 msime_clien
t_plugins 的 script_page 和三项授权操�
��）；Windows 的提权窗口（MSIME_TRAIT
_ELEVATED）里从不运行脚本。
 * 脚�
�运行要同时满足：开关开着；会�
�经 msime_client_set_lifecycle 选择加入�
��字段属性带 MSIME_TRAIT_KNOWN，且不�
�� SECURE、PRIVATE、ELEVATED。属性变得
不满足时正在跑的实例当场丢弃。
每次 msime_client_focus(true) 之后属性�
��要重新确认：宿主在聚焦之前调�
�� msime_client_set_field_traits（属性没�
��也算），或者聚焦之后按了第一�
��不带 MSIME_KEY_PASSTHROUGH 的键（msime
_client_key；交给应用的键在 Windows �
��经 Aux 管道异步上报，和属性没�
�先后保证，不算确认）；确认之�
�不实例化、不投递，所以想让脚�
�收到 focus_in 的宿主要在 msime_client
_focus(true) 之前报告属性。观察类�
�件（focus_in、focus_out、input、selecti
on_retracted、candidate_selected、commit、
composition_start/update/end、candidates_sho
wn/hidden）来自派发类调用、focus 和
 destroy；msime_client_key 的按键、字�
�属性、激活和旁路调用的视图事�
�不投递。修改类钩子只在派发类�
�用里运行：candidates 在候选刷新和
展开之后，插件可以重排、隐藏、
注释候选，视图和选择 ID 按重排�
�后的顺序，被隐藏的行不出现在�
�图、分页和 msime_client_all_candidates 
里、任何选择都选不到，插件的注
释放在候选的 "script_annotation" 键里
（没有时不出现，不改 "annotation"�
�由宿主另行绘制）；设着 MSIME_LIFE
CYCLE_COMMIT_OBSERVE_ONLY 时 candidates 不�
��行也不重放；before_commit 在上屏�
�前，可改写时（选中候选、原样�
�屏或暂存段送出，本地模式 none �
�算打字，不是以词定字，没设 MSIM
E_LIFECYCLE_COMMIT_OBSERVE_ONLY，不是 MSIM
E_LIFECYCLE_HOST_HOLDS_PIECES 下宿主暂存
的一段）插件可以改写标点和顶字
尾巴之前的那一段，回复里的 commi
t 就是改写之后的文本，引擎学习�
�� AI 上文仍按原文。旁路调用不运
行脚本，只按候选身份重放最近一
次的重排、隐藏和注释。command 钩�
��给插件自己的 / 指令出候选：选�
��加入时和每次 msime_client_focus(true)
 时，会话把启用、授予了 provide_co
mmands 的插件在清单里声明的触发�
�交给引擎（用户指令表赢、插件�
�执行顺序先到先得）；用户在 / �
�令里打完某个触发词时，派发类�
�用只投递给第一个声明它的插件�
�次，事件带触发词、后面的字母�
� 64 字节系统随机数，插件给的行�
��来源 14 的候选（"script_annotation" �
��「标题 注释」），由引擎按预编
辑串缓存，旁路调用、移动高亮和
翻页都不再投递；运行条件不满足
或设着 MSIME_LIFECYCLE_COMMIT_OBSERVE_ONLY
 时只有原样的那一行。脚本只能�
�授权的 import 在钩子调用期间读文
本，读不到宿主的任何别的状态，
也不能回调宿主。每次 msime_client_f
ocus（两个方向）和 destroy 都丢弃�
�个会话的全部脚本实例。fuel 按 ms
ime_client_key 开启的轮次给：没有先
调 msime_client_key 的派发（修饰键组
合、快捷键、失焦命令）和上一次
按键共用那一轮的配额；本轮配额
用完时插件跳过这次投递，不算失
败；运行到一半被本轮配额截断的
插件在这次聚焦里不再运行，这一
次用掉的不到单个钩子自己上限的
一半时同样不算失败，达到一半就
和超出单个钩子自己的上限一样算
一次失败（candidates 的单次上限和�
��轮配额相等，同一轮里 input 先花
过 fuel 时它的死循环先撞上每轮配
额，这样也照样熔断）。脚本的失
败不影响上屏：一次失败只丢弃它
这次的结果，trap 之后这个插件在�
��次聚焦里不再运行，连续失败按�
��件熔断；被停用的插件和停用的�
��别（invalid、init、failed、slow，不�
��次数、具体原因和任何文本）在�
��焦时写进 <user_data>/script-health.json
。 */
bool msime_client_scripts_enable(bool 
on);
/* The typing effect of one key, for the
 host to draw beside the caret: the session's
 combo count and the resolved style: the sele
cted effect pack's (preferences.plugins.effec
t_pack) when one is set, preferences.plugins.
effect_style otherwise. Also for the key path
: integer arithmetic on the session's own sta
te, no lock, no allocation, no file, no free;
 it works on every platform, iOS, Android and
 HarmonyOS included. Call it once per key the
 host handles while effect_style is not "off"
, effect_pack is set or combo_counter is on, 
alongside key_sound, and once per commit; ski
p keys typed into a secure (password) field.

 * event bits 0-7: 0 any other key, 1 space, 
2 enter, 3 backspace (the key_sound classes),
 4 commit, 5 backspace by any other route (a 
delete that bypasses key_sound). 0-2 count on
e key; 3 and 5 end the combo; 4 counts nothin
g and only reports the state.
 * event bit 8 
(0x100): the key is an auto-repeat of a held 
key; drawn, not counted. Bit 9 (0x200): sound
s must stay quiet now (a full-screen foregrou
nd application on Windows); the tier-up sound
 is neither queued nor reported due. Other bi
ts are ignored.
 * Return value, 0 when the r
esolved style is off and combo_counter is off
, for an unknown session handle, a wrong thre
ad, an event code above 5, or after a panic: 
bits 0-15 the combo count (saturating at 6553
5, always 0 while combo_counter is off); bit 
16 this key moved the combo up a tier (it rea
ched 10, 25, 50 or 100 keys); bits 17-19 the 
style, 0 off, 1 flash, 2 sparks, 3 power_mode
; bit 20 the tier-up sound is due, set only w
ith combo_counter and combo_tier_sound on and
 bit 9 clear. On macOS, Windows and Linux thi
s library has already queued that sound (the 
key pack's commit sample raised 3 semitones p
er tier); a host that plays packs itself play
s it. The combo also ends after 3000 ms witho
ut a counted key, and carries over a change o
f preferences. */
uint32_t msime_client_typin
g_effect(uint64_t session, uint32_t event);
/
* The session's resolved typing effect, for d
rawing what msime_client_typing_effect report
s. Standard response; value {pack: id|null, i
ssue: string|null, style: "off"|"flash"|"spar
ks"|"power_mode", intensity: 0..100, colors: 
["#RRGGBB", 0..4 entries], duration_ms: 60..1
500|null, particles: 0..64|null, combo_counte
r: bool}. With no effect pack selected (pack 
null) style and intensity are preferences.plu
gins.effect_style and effect_intensity, color
s is empty and duration_ms and particles are 
null: the host's own defaults for the style. 
With one selected, the pack's parameters repl
ace them; a pack that cannot be loaded gives 
style "off" and issue says why, in Chinese fo
r a log line. colors, duration_ms and particl
es are hints a host may ignore when its style
 has no such thing. Linux draws no effect and
 shows only the combo count. Reads one small 
manifest only when the selection or the pack 
changed: call it after msime_client_update_pr
eferences and after a focus-in, not per key. 
Free with string_free. */
char *msime_client_
typing_effect_settings(uint64_t session);
/* 
The validated files of one sound pack, for a 
host that plays packs itself (HarmonyOS). Req
uest (<=65536 bytes): {state_root: absolute|n
ull, sound_packs: absolute|null, pack: id}; s
tate_root is the preferences directory holdin
g plugins/, sound_packs the bundle's built-in
 pack root. Built-in ids ("default", "twinkle
", "msime-typewriter", "msime-bubble", "msime
-8bit", "msime-woodblock", "msime-pentatonic"
, "msime-canon", "msime-ode-to-joy") always r
esolve from sound_packs; any other id needs s
tate_root and is refused without one. Value: 
{id, name, license, builtin, mode:"keys"|"seq
uence", sounds:{default, space, enter, backsp
ace, commit, achievement: absolute path|null}
, sequence:{sample: absolute path, semitones:
[-24..24], advance:"key"|"commit"}|null, max_
sample_millis, melody_idle_reset_millis}. Rea
ds the pack from disk: not for the key path. 
*/
char *msime_client_key_sound_pack(const ui
nt8_t *request, size_t length);
/* The valida
ted tracks of one music pack, for a host that
 streams music itself (HarmonyOS). Request (<
=65536 bytes): {state_root: absolute|null, so
und_packs: absolute|null, pack: id}, as for k
ey_sound_pack. Built-in ids ("msime-music-lof
i", "msime-music-ambient") resolve from sound
_packs, which holds them beside the built-in 
sound packs; any other id needs state_root an
d is refused without one. Value: {id, name, l
icense, tracks:[absolute path, in play order]
, max_track_seconds}; a host plays a track on
ly when its duration is within max_track_seco
nds, then the next, starting over after the l
ast. Reads the pack from disk: not for the ke
y path. */
char *msime_client_music_pack(cons
t uint8_t *request, size_t length);
/* 插件
页的包仓库和 @ 名单，给桌面外壳
以外的设置宿主：Windows 的原生设�
��应用、鸿蒙的插件页，以及 Androi
d、iOS 设置里导入辅助码表。Request
（<=2 MiB）为 {state_root: 绝对路径, s
ound_packs: 绝对路径|null, plugin_packs?:
 绝对路径|null, script_page?: bool, user_
data?: 绝对路径|null, action}；包和 me
ntions.json 放在 state_root/plugins 下，s
ound_packs 是安装包自带的内置音效�
��目录，plugin_packs 是桌面安装包自
带的官方插件包目录（<kind>/<id>/，
没给时取 sound_packs 旁边的 plugin-pac
ks，同宿主选项的 plugin_packs）。scr
ipt_page 为 true 表示宿主有脚本插件
的授权页（列出、授权、排序，并
写明同一用户下的其他程序能伪造
授权），缺省为 false：Android、Harmo
nyOS 和只导入辅助码表的宿主不带�
��，永远看不到脚本插件。user_data 
是输入法宿主选项里的 user_data 目�
��，只用于脚本授权的回答里的停�
��记录。action.operation：
 * "catalog"�
�value 为 {packages:[...], issues:[{kind, fo
lder, reason}]}，即全部已安装的包、
内置音效包和官方插件包（builtin �
�� true，默认停用：脚本包要授权�
�短语表要在偏好里启用），与桌�
�外壳列出的相同；官方包的 id 在�
��件目录里有同名的旧副本时，那�
��副本列在 issues 里，不会被使用�
�不带 script_page 时去掉脚本插件（k
ind 为 script）的包和问题行。脚本�
��另有 abi、module、hooks（声明的钩�
��名）、permissions（申请的能力名�
�、memory_pages 和 content_hash（64 位小
写十六进制的内容哈希，授权绑定
在它上面）。
 * "import" {source: 包�
�件夹或 .zip 文件的绝对路径}：安�
��它，整包替换已安装的同 id 包；
value 是装好的包。不带 script_page �
� release 构建拒绝脚本包（plugin_inva
lid，与移动端遇到 script 时的原因�
��同）；带 script_page 时接受，装好
的脚本包默认不运行，要经授权。

 * "import_helpcode_table" {file_name, text}
：把单个 .txt 辅助码表装成 kind 为
 helpcode 的包。file_name 是用户选的�
��件名，只用于报错、显示名和 id�
��text 是码表正文，宿主必须先按 U
TF-8 严格解码（解码失败就不要调�
��），原始字节不超过 1 MiB。码表�
��辅助码表包的严格规则校验（每�
��「字=码」，码是 1 到 2 个小写字
母，同一个字只能出现一次，1 到 
30000 条），不合格时是 plugin_invalid
，detail 写明文件名、原因和行号�
�显示名取开头的 # name: 注释，没�
�时用去掉扩展名的文件名；id 是 c
ustom-<小写文件名主干>，组不成合�
�� id 时是 custom-<12 位十六进制哈希
>，同名文件再次导入时整包替换�
�value 是装好的包，其中 entries 是�
�数。宿主再把 id 写进 preferences.plu
gins.helpcode_pack_quanpin 或 helpcode_pack_
shuangpin 来选用它。
 * "remove" {kind: 
"sound"|"music"|"command_table"|"effect"|"phr
ase_table"|"helpcode"|"wordbook"|"symbol_set"
, id}：value 为 null；没装的包视为�
�删除。内置音效包和背景音乐包�
� id 是 plugin_reserved；官方插件包的
 id 只删插件目录里的同名旧副本�
�内置的那一份和它的授权都不动�
�
 * "load_mentions"：value 为 [{text, key}
]，还没保存过名单时为空。
 * "sav
e_mentions" {entries:[{text, key, phone}]}：
替换名单；value 为 null。
 * 以下三
项只在桌面（macOS、Windows、Linux）�
��有，别处是 invalid；value 都是整�
�授权 {revision, plugins:[{id, hash, enable
d, granted:[能力名]}], health:[{id, hash, 
disabled: "invalid"|"init"|"failed"|"slow"}],
 commands:[{id, trigger, title, status: "acti
ve"|"shadowed_by_user_table"|"shadowed_by_plu
gin", conflict?, conflict_id?}]}，plugins �
�顺序就是执行顺序，health 是输入�
��停用了的插件（来自 user_data/scrip
t-health.json，没给 user_data 时为空）
，commands 是启用、授予了 provide_com
mands、清单能载入的插件声明的 / �
��令和状态（conflict 是冲突的触发�
��，conflict_id 是冲突的插件，用户�
��令表取 state_root 里偏好启用的指�
��表包），与桌面外壳的 script_grant
s 命令相同。
 * "script_grants"：读出
授权。
 * "set_script_grant" {id, hash, en
abled, granted:[能力名]}：授予、调整
或撤销一个脚本插件的授权。hash �
��页面展示给用户的内容哈希，包�
��这之后被换过时是 plugin_script_chan
ged；granted 必须是清单申请的能力�
��子集，并满足依赖和 rewrite_commit 
与 read_composition 互斥的规则；启用
时同时启用超过 8 个是 plugin_script_
limit，超出内存总账是 plugin_script_m
emory。撤销是 enabled 为 false、granted
 为空。
 * "reorder_scripts" {ids:[id]}：
ids 按给出的顺序排到最前，其余�
�原来的相对顺序；有重复或没有�
�权条目的 id 时整个拒绝。
 * 失败
时为 {ok:false, error: 失败码, detail?}�
��失败码与桌面外壳相同（invalid、
storage、plugin_invalid、plugin_unsupported
_source、plugin_archive、plugin_reserved、
plugin_storage、plugin_script_missing、plug
in_script_changed、plugin_script_grant_inval
id、plugin_script_limit、plugin_script_memo
ry、plugin_script_grants_format、mention_in
valid、mention_format、mention_storage）�
�detail 存在时是被拒的包或条目违�
��的规则，中文，给设置页显示。�
��读写文件，导入最多复制一个音�
��包大小的数据：宿主有工作线程�
��在工作线程上调用。 */
char *msime_
client_plugins(const uint8_t *request, size_t
 length);
/* 不带编码的常用语（<=4 M
iB）：{directory: 偏好目录的绝对路�
��, action:{operation:"load"|"add"{text}|"rem
ove"{id}|"replace"{id,text}|"move"{id,index}|
"install_pack"{resource}|"remove_pack"{id}}}�
��Value 是整份文档 {phrases:[{id,text,pa
ck}],packs:[{id,name,revision}],skipped?}；�
��误是固定的代码（common_phrases_io�
�_corrupt、_invalid、_duplicate、_limit、
_too_large、_not_found）。设置进程和�
��盘进程都会调用，调用时锁住文�
��。在工作线程上调用。 */
char *msi
me_client_common_phrases(const uint8_t *reque
st, size_t length);
/* 具名词库集合（<
=17 MiB）：{options: 带 preferences_direct
ory 的 HostOptions, action:{operation:"load"
|"create"|"rename"|"delete"|"set_enabled"|"ad
d_words"|"remove_words"|"import"|"install_com
munity"|"queue_words"{entries}|"flush"|"synch
ronize"|"third_party_matches"{entries}, ...}}
，格式见 client-services 的 dictionary::
collections；import 的 format 为 "scel"（
搜狗细胞词库）时文件按 base64 放�
�� bytes_base64（不超过 8 MiB），不带
 text，结构损坏时是 import_malformed�
�queue_words 把不属于任何集合的词�
�本地备份恢复的个人词库，每次�
�多 20000 个）排进待发送队列，valu
e 带 queued；third_party_matches 只读地�
��对最多 128 个准备发布到社区的�
�，value 的 third_party_matches 是其中�
�现在搜狗细胞词库导入的集合里�
�下标。词条经个人词库队列传过�
�，键盘在下一次会话时应用。synch
ronize 在 budget_millis（1–2000，默认 
2000）毫秒软预算内逐批应用个人�
�列并补充待发项，每批至多 128 项
并释放维护租约，返回 {synchronized,
pending_count,failed_count,snapshot_error,sen
t}，pending_count 包括未发送的集合�
�目；普通操作的 Value 是集合视图�
��错误是固定的代码（collections_*、
builtin_locked、unsupported_format、import_
*、personal_dictionary_*）。在工作线�
�上调用。 */
char *msime_client_dictionar
y_collections(const uint8_t *request, size_t 
length);
/* 诊断包（<=65536 字节）：{
state_root: 偏好目录, include:{crash_logs
,performance_logs,input_events,config_snapsho
t}, sources:{crash_logs,performance_logs,inpu
t_events: 绝对路径|null}, destination: .z
ip 的绝对路径|null}。输入事件和性
能记录只有恰好是 {t_ms, kind (key_dow
n|key_up|candidate_shown|candidate_selected|c
ommit|backspace|panel_open|panel_close|ime_st
art|ime_finish|input_failure_runtime_rejected
|input_failure_invalid_response|input_failure
_editor_rejected|input_failure_jni_link), dur
ation_ms} 时才保留，带其他任何键�
�text、key、candidate……）的整行都�
��掉并计数。崩溃记录只保留异常�
��型和栈帧：异常说明可能带着正�
��输入的文字，message 只留冒号前�
�异常类型，stack 的说明行同样处�
�，路径只留文件名。input_failure_* �
��供本地诊断，云端上传器须按服�
��端白名单过滤。配置快照把所有�
��据换成 "<redacted>"。给了 destination
 时写出 zip，value 是 {path,bytes,counts
}；没给时 value 是 {counts,sections}，s
ections 是上传请求体的对象（crash_l
ogs、perf_trace、input_events、config_snap
shot）。错误：diagnostics_invalid、diag
nostics_source、diagnostics_preferences、di
agnostics_write。在工作线程上调用。
 */
char *msime_client_diagnostic_bundle(cons
t uint8_t *request, size_t length);
/* 账号
设置文档导出（<=4 MiB）：{preference
s_directory, feedback?:{soundEnabled,hapticsE
nabled,hapticStrength}|null, custom_keyboard_
skins?: JSON 数组字符串|null, android_lo
cal?: {同步键: 值}|null, schema?: 偏好�
��段表|null, cloud?: {revision,settings}|nu
ll}。Value {settings, merged?}：settings �
�本版本过滤（给了 schema 时再按它
的字段过滤）；同时给了 schema 和 
cloud 时，merged 是带着云端 revision �
�� PUT 的整份文档。android_local 是 An
droid 本机设置文件里参与同步的值
（应用主题、单手模式、按键细节
、工具栏的常用语/方案/隐藏项、�
��写、离线语音），不认识的键和�
��法值不导出。凭据和诊断日志从�
��导出；隐私模式、开发者选项和�
��音贡献只在 Android 本机文件里，�
��不同步。 */
char *msime_client_account_
settings_export(const uint8_t *request, size_
t length);
/* 账号设置文档应用（<=4 
MiB）：{preferences_directory, cloud:{revis
ion,settings}, schema, feedback?: 宿主当�
�的按键反馈|null}。把文档应用到�
�机偏好，按读到的 revision 做比较�
��交换后保存。本机不认识或超出�
��围的取值只跳过那一个键；本机�
��着账号装不下的方案（粤拼、注�
��、越南文、藏文、笔画）时导出�
��写 input.schema，应用时也不拿云端
的方案盖掉本机的方案。Value {prefe
rences: 保存后的快照, feedback: 应用�
��的按键反馈|null（宿主自己存）, 
custom_keyboard_skins: 云端皮肤库 JSON|n
ull（宿主合并）, android_local:{同步�
��: 值}（宿主写进本机设置文件）,
 skipped:[键]}。 */
char *msime_client_acco
unt_settings_apply(const uint8_t *request, si
ze_t length);
/* iOS 的账号设置文档导
出（<=4 MiB），与 Tauri iOS 的 account_
preferences_upload 相同，但不联网、�
�读会话：{preferences_directory, ios_nati
ve, schema, cloud?}。ios_native 是 App Grou
p 里的键盘设置，字段与 Tauri 插件
的 loadKeyboardPreferences 相同（camelCas
e：inputScheme、traditionalChineseOutput、
soundEnabled、hapticsEnabled、hapticStrengt
h、englishSuggestions、candidatePaletteFoll
owsDesktop、inlinePreedit、hapticsAvailable
、tabletFullKeys?、tabletSplitKeyboard?、d
ictionaryLearning、globalTheme、customKeybo
ardSkin）；schema 是服务端的偏好字�
��表，必需，因为服务端只要有一�
��键不在表里就拒绝整份文档，plat
form.ios.global_theme 和 platform.ios.custom
_theme_base 目前不在表里，导出时会
被去掉。Value {settings, merged?}：setti
ngs 按 preferences_directory 所属的版本
过滤、只留字段表收录的键；给了
 cloud（{revision,settings}）时 merged 是
带着云端 revision 去 PUT 的整份文档
。一个键都没有时 error 是 account_un
available；ios_native 不合规是 account_s
torage，请求不合法是 account_invalid�
� */
char *msime_client_account_ios_settings_
export(const uint8_t *request, size_t length)
;
/* iOS 的账号设置文档应用（<=4 Mi
B），与 Tauri iOS 的 account_preferences_
apply 相同，分两步，不读会话也不
检查会话代次，下载文档以后账号
有没有变由宿主负责：{preferences_di
rectory, cloud:{revision,settings}, step:"pla
n"|"commit", ios_native?, saved_native?}。�
�导出不同，应用不带 schema、不按�
��段表过滤（与 Tauri iOS 的 apply 相�
��），请求里多出任何字段（包括 
schema）都是 account_invalid。cloud 先�
� preferences_directory 所属的版本过滤
，再整份校验，任何一个值不合规
都是 account_invalid。step 为 plan 时要
带 ios_native（App Group 里当前的键盘
设置），Value {ios_native: 要写进 App 
Group 的键盘设置}，什么也不写；io
s_native 的方案是账号装不下的粤拼
、注音、越南文、藏文或笔画时（
导出不写方案），要写的键盘设置
保留本机的方案。cloud 里有 input.wu
bi_schema 时总是把它写进共享偏好�
�五笔版本，不管 input.schema 是不是
 wubi（与 Android、桌面和鸿蒙的映�
�相同），不认识的取值与其余的�
�一样是 account_invalid；没有这个键�
��保留本机的五笔版本。宿主写进 
App Group 以后，用实际保存下来的�
�作 saved_native 调 step 为 commit：按�
�到的修订号比较并交换写回共享�
�好，Value {preferences: 保存后的 Prefe
rencesSnapshot}。commit 失败时宿主把 A
pp Group 恢复成旧值，顺序与 Tauri �
�同：先写 App Group，再写共享偏好�
��失败时回滚 App Group。写回失败时
 error 是 conflict（本机设置已在别�
�更新）或 storage，映射失败是 accou
nt_invalid 或 account_storage。 */
char *ms
ime_client_account_ios_settings_apply(const u
int8_t *request, size_t length);
/* 鸿蒙的
账号设置文档导出（<=4 MiB），原�
�是鸿蒙账号桥 preferences_upload 里 Ac
countPreferencePlan 的那部分，不联网�
��不读会话：{preferences_directory, feed
back?:{soundEnabled,hapticsEnabled,hapticStre
ngth}|null, schema, cloud?}。鸿蒙的本机
设置就是 preferences_directory 里的共�
��偏好，与 Android 是同一套映射，�
��台键放在 platform.harmony.* 下（不�
� platform.android.*，也不导出只有 And
roid 才有的工具栏、按键音包、语�
��语言、本地设置和皮肤库）；全�
�� 14 键不上传（云端保留原值），
鸿蒙不提供的全局主题按 system、�
�认得的振动强度按 medium 上传。fee
dback 是宿主反馈文件里的值，不带
时不导出那三个键。schema 是服务�
�的偏好字段表，必需，服务端收�
� platform.harmony.* 之前只有 input.* 留
下来。Value {settings, merged?}：settings
 按 preferences_directory 所属的版本过
滤、只留字段表收录的键；给了 cl
oud（{revision,settings}）时 merged 是带
着云端 revision 去 PUT 的整份文档，
别的设备写的键原样保留。一个键
都没有时 error 是 account_unavailable；
读不出共享偏好是 account_storage，�
�求不合法或合并时键不在字段表�
�、类型不对、超出字节上限是 acco
unt_invalid。 */
char *msime_client_account_
harmony_settings_export(const uint8_t *reques
t, size_t length);
/* 鸿蒙的账号设置�
�档应用（<=4 MiB），原来是鸿蒙账�
��桥 preferences_apply 里 AccountPreference
Plan 的那部分，不读会话也不检查�
��话代次，下载文档以后账号有没�
��变由宿主负责：{preferences_directory
, cloud:{revision,settings}, schema, feedback
?: 反馈文件里当前的值|null}。cloud 
先按 preferences_directory 所属的版本�
��滤，再整份校验：字段表声明了�
��键类型不对、文档或字段表本身�
��合规时整份拒绝；只读 input.* 和 
platform.harmony.*，本机不认识或超出
范围的取值只跳过那一个键。导出
不上传的两项应用时也不改：本机
是全拼 14 键时不应用 platform.harmony
.keyboard_layout，本机选着账号装不�
�的方案（粤拼、注音、越南文、�
�文、笔画）时不应用 input.schema，�
��则上传后写回、或下载时会被云�
��留着的旧值改回去。文档和字段�
��里都有按键反馈的键而没带 feedba
ck 时是 account_invalid。偏好有变化�
�按读到的修订号比较并交换写回�
�份快照。Value {preferences: 保存后的
 PreferencesSnapshot, feedback: 应用后的�
��键反馈|null（不为 null 时宿主写�
�反馈文件，失败时不回滚已经写�
�的偏好）, skipped:[键]}。写回失败�
�� error 是 conflict（本机设置已在别
处更新）或 storage，读不出共享偏�
��是 account_storage，映射失败是 accou
nt_invalid 或 account_unavailable（字段�
�不可信）。 */
char *msime_client_accoun
t_harmony_settings_apply(const uint8_t *reque
st, size_t length);
/* 多分类自动云同�
��（设置、Android 的自定义键盘皮�
�库、无编码常用语、个人词库）�
�编排，原来在 iOS 的 CloudSync 和 And
roid 的 home/CloudSync 各有一份。Reques
t 是 length 字节（最多 4 MiB）的 UTF-
8 JSON {directory?|session_store?, legacy_dir
ectory?, state_directory, preferences_directo
ry?, options?, snapshot_queue_directory?, act
ion}。会话的来源与 msime_client_accoun
t 相同（directory 和 session_store 恰好
给一个），只用登录账号；peek、ma
rk_dirty 和 import_legacy 只碰状态文件
（import_legacy 还只读快照激活队列�
��，可以不带会话来源，也从不按�
��话对齐。state_directory 是绝对路径
，状态在它下面的 cloud-sync/state.jso
n（版本 2，最多 256 KiB，同目录 sta
te.lock 用 flock 保护，基线和收不下
的常用语只存 SHA-256）和 pending.json
；iOS 传 App Group 的根目录，键盘扩
展和应用读写同一份。preferences_dir
ectory 是共享偏好和常用语所在的�
�好目录，run 和 commit 要带。options 
是 Engine 的宿主选项（与 msime_client
_dictionary 相同，要带 preferences_direc
tory），snapshot_queue_directory 是词库�
��照激活队列的目录（Android 是 file
s/bootstrap/state/dictionary-snapshots），�
��步词库时两个都要带；sign_out 带�
��队列目录时取消旧账号排队中的�
��照；import_legacy 导入 Android 旧状�
�时也要带上队列目录（只读）。st
atus、set_enabled 和 run 每次调用都按
会话里的用户 id 对齐：账号变了�
�或已经退出）就清空进度、关掉�
�关；同一个账号重新登录保留进�
�。会话读不出来（钥匙串不可用�
�时不对齐。所以只有持有登录的�
�个进程（iOS 和鸿蒙的应用、Android
 的主进程、桌面的设置程序）可�
�调 status、set_enabled、run、commit 和 
sign_out；会话来源里看不到登录的�
��程（键盘扩展、Android 的 :ime、注
入存储读出 0 字节的扩展）调 statu
s 会被当成已经退出，把共享的同�
��进度清空、开关关掉，这些进程�
��开关一律用 peek，记改动用 mark_di
rty。
 * action 是下列之一：
 * {opera
tion:"status"} → 状态 S：{enabled, disab
led_on_cloud_cleared, account_id|null, sectio
ns:{settings|skins|phrases|dictionary:{cursor
|null, dirty, cleared, last_outcome?:{outcome
,reason?}}}, last_attempt_ms, last_synced_ms,
 not_before_ms, pending_choice:"merge"|"use_c
loud"|null, running, failure:{section,reason,
status?,code?,message}|null, skins_trimmed, u
nheld_phrases, status_line}。status_line 是
「我的」页同步行的一句话，措辞
与两端原来的相同，只有两句是新
的：等「云端被清空」的选择时是
「云端数据已被清空，请选择合并
、使用云端或关闭同步」，用户在
那里选了「关闭同步」以后、再开
关同步之前是「云端数据已被清空
，已关闭同步，本机数据没有改动
」，这时 disabled_on_cloud_cleared 为 tr
ue（打开、关闭同步和换账号、退�
��都把它清成 false）。
 * {operation:"
peek"} → S：只读 state.json，不读会�
��、不对齐、不写任何东西（状态�
��录还不存在时是全新的默认值）�
��给不持有登录的进程看开关；请�
��可以不带会话来源，带了也不用�
��
 * {operation:"sign_out"} → S 加 snapsh
ots_cancelled：退出登录时调用，同�
�清空状态、代次加一，进行中的�
�一轮因此提交不了。它不读会话�
�但请求仍要带会话来源，按 session
_store 时要在注销这个存储之前调�
�，否则是 cloud_sync_invalid、状态不�
��。
 * {operation:"set_enabled", enabled} �
�� S：打开时要有登录的账号（否�
�是 cloud_sync_signed_out），并把全部�
��类标成待上传；关闭时保留游标�
��
 * {operation:"mark_dirty", section:"setti
ngs"|"skins"|"phrases"|"dictionary"} → {mar
ked}：只读写状态文件，不读会话�
�不联网，键盘扩展和 Android 的 :ime
 进程用户改了本机内容时调用；�
�关关着时不记（marked 为 false）。�
��步自己写本机不经过这里，宿主�
��不要为同步写的值再打标记。
 * 
{operation:"run", trigger:"active"|"enable"|"
choice"|"manual", choice?:"merge"|"use_cloud"
|"disable", capabilities:{settings,skins_libr
ary,phrases,dictionary}, host_inputs?:{ios_na
tive?, harmony?, feedback?, android_local?, c
ustom_keyboard_skins?, settings_locked?, excl
uded_phrase_texts?}} → R：{outcome:"skippe
d"|"needs_choice"|"done"|"pending_commit"|"di
sabled", reason?, run_id?, host_writes?:{ios_
native?, feedback?, android_local?, custom_ke
yboard_skins?, adopt_texts?}, sections?:{<分
类>:{outcome:"synced"|"unchanged"|"skipped"|
"failed"|"needs_choice", reason?}}（分类�
� settings、phrases、dictionary，皮肤库
没有动时还有 skins）, status: S}。act
ive 是回到前台，按节流跑（距上�
�满 5 分钟，参与的分类有待上传�
�记时满 30 秒）；enable、manual 和这
一次请求带了 choice 的不等节流，�
��态里记着的选择（另一轮在跑时�
��下的 pending_choice、词库合并没做�
��时留给词库的选择）不让下一轮�
��过节流；所有触发都尊重服务端�
�� Retry-After（记成 not_before_ms，期�
�是 skipped/backoff）：超过 24 小时的
按 24 小时记，离现在超过 24 小时�
�� not_before_ms（时钟往回拨以前记�
�的）不再挡、status 里是 null，所�
�手动同步最多被挡 24 小时。trigger
 为 choice 时必须带 choice。capabilitie
s 是宿主这一端同步哪几类：iOS 是
 {settings, phrases}，Android 四类都开�
�鸿蒙不开 skins_library；skins_library �
��和 settings 一起开，dictionary 要带 
options 和 snapshot_queue_directory，否则
是 cloud_sync_invalid。host_inputs 是宿�
�保管、Rust 读不到的本机值：带 io
s_native（App Group 里的键盘设置，字
段同 msime_client_account_ios_settings_expo
rt）时设置按 iOS 映射，harmony 为 tr
ue 时按鸿蒙映射（platform.harmony.*，
同 msime_client_account_harmony_settings_exp
ort，按键反馈放在 feedback，不能同
时带 ios_native、android_local、custom_ke
yboard_skins 或开 skins_library，否则是
 cloud_sync_invalid），否则按 Android �
�射（feedback、android_local、custom_keyb
oard_skins 同 msime_client_account_settings_
export）；custom_keyboard_skins 不是 JSON
 数组、或者云端的皮肤库读不懂�
�不是 JSON 数组，可能是更新的版�
�写的）时，上传的设置文档里皮�
�库原样用云端的值，不删也不拿�
�一份替换它，设置照常同步，secti
ons.skins 是 skipped，reason 为 skins_libr
ary_unreadable 或 cloud_skins_library_unread
able；settings_locked 为 true（iOS 社区�
��肤试用中）时这一轮不同步设置�
��excluded_phrase_texts 是 Android 本机预
置、还没被认领的示例常用语，不
上传，也不会被当成本机多出来的
删掉。
 * run 的 outcome：skipped 时 re
ason 是 signed_out、disabled、throttled、
backoff、busy（另一轮还在跑；这时�
��的 choice 记成 pending_choice，下一�
�用它，但下一轮照样等节流）、aw
aiting_choice（要问用户但 5 分钟内�
�问过）、session_unavailable、dictionary
_storage（同步词库时建不出 state_dir
ectory 下的 cloud-sync/work）或 stale_run
（跑到一半账号变了、开关被关或
另一处写了状态，这一轮作废）。
needs_choice 时 reason 是 first_run（有�
�类第一次同步而云端已有数据）�
� cloud_cleared（云端被清空，多半是
「删除云端数据」；写入因修订号
冲突而重读云端时读到被清空的云
端也算，不拿旧基线合并），整轮
什么都没写，宿主弹「合并 / 使用
云端」（cloud_cleared 时多一个「关�
��同步，保留本机」），用户选了�
��以 trigger:"choice" 调一次；choice 为
 disable 只回答 cloud_cleared，只能和 
trigger:"choice" 一起给（否则是 cloud_
sync_invalid）：它不联网、不跑一轮
，也不读写本机的设置、常用语和
词库（所以可以不带 preferences_direc
tory），只在状态锁里关掉这台设�
�的同步，R 是 {outcome:"disabled", statu
s: S}；不等节流、Retry-After 和另一�
��（在跑的那一轮因此提交不了，�
��在这之前写到云端的不撤回），�
��不记成 pending_choice，记着的选择�
��pending_choice、词库留下的选择）�
�起丢掉；清空的标记和游标留着�
�用户以后重新打开同步，第一轮�
�完云端照样先问，不会把本机传�
�被清空的云端。账号对齐与 run 相
同；没有登录、开关关着、或者没
有分类记着清空（包括首次同步的
提示）时是 skipped，reason 为 signed_o
ut、disabled 或 not_cloud_cleared，状态�
��变；会话读不出来是 skipped/session
_unavailable。「使用云端」遇到从没
上传过（revision 0）或为空的云端�
�而本机有数据时按合并处理，不�
�空本机；词库还没同步过时按云�
�词库现在的条数（GET /v1/users/me/dat
a/summary）判断空不空，不管改动历
史里有没有 reset，读不出条数时按
不空处理。词库另有一条硬性约束
：下载下来要整份替换本机的云端
快照一个词都没有（entry 减去标成
删除的 overlay）、而本机这一轮读�
��自己的词时，不管是「使用云端�
��、首次同步还是之后云端有改动�
��下载，都不替换，改按合并处理�
��两边的词都留着，sections.dictionary
 照常是 synced，游标推进到快照的�
��订号，词库再标一次待上传，下�
��轮把本机的词传回云端（读不出�
��数时选了「使用云端」、或者在�
��词库面板里把词删光，都走这一�
��）。done 时各类的结果在 sections�
�一类失败不挡其他类，第一条失�
�记在 status.failure；冲突重读才发�
�云端被清空、而排在前面的分类�
�一轮已经写过时，整轮照常是 done
 或 pending_commit，那一类在 sections �
��是 needs_choice（cloud_cleared），状�
�里记下清空（status 显示清空的提�
��），下一轮读完云端再问。设置�
��回本机时按这一轮读本机时共享�
��好的修订号比较并交换：期间另�
��个进程里用户改了设置时不覆盖�
��，sections.settings 是 skipped、reason �
�� settings_changed，不算失败，游标�
�基线不动、设置再标一次待上传�
�下一轮按三方合并（这时云端可�
�已经写过）。pending_commit 时宿主�
�把 host_writes 里的值写进自己的存�
��（ios_native 写进 App Group；feedback�
�Android 和鸿蒙）、android_local 写回�
��机；custom_keyboard_skins 按 id 和更�
�时间合并进皮肤库；adopt_texts 里�
�示例常用语认领为用户自己的）�
�再以 run_id 调 commit；游标、清标�
�和基线只在提交时推进，宿主不�
�交时 180 秒后租约过期，下一轮重
新下载，结果相同。
 * {operation:"co
mmit", run_id, host_results?:{saved_native?}}
 → S：提交 pending_commit 的那一轮�
�iOS 要带 saved_native（按 host_writes.io
s_native 写进 App Group 以后实际保存�
��来的值），据此把设置文档应用�
��共享偏好，和 run 写回设置一样�
�这一轮读本机时共享偏好的修订�
�比较并交换；那一步失败时 error �
�� conflict（共享偏好在这一轮读本�
��以后被别处改过，包括宿主写 App
 Group 期间用户在设置页改的）或 s
torage，这一轮作废，什么都不推进
、设置的待上传标记留着，宿主把
 App Group 恢复成旧值，下一轮按三�
��合并。期间有别的写入（sign_out�
�set_enabled、另一轮）或账号变了时
是 stale_run，什么都不推进。
 * {ope
ration:"import_legacy", ios_v1?: iOS 第 1 �
�的 CloudSync/state.json 对象, android?:{s
ync_v1: msime_sync_v1 偏好的键值, cloud_
sync_status: msime_cloud_sync_status 偏好�
�键值}} → {imported, status: S}：两者�
��好给一个。只在还没有任何进度�
��没有导入过、用户没有开关过同�
��、开关关着、没有游标）、而且�
��态没有绑定账号或绑定的就是旧�
��态里的账号时导入开关、账号、�
��标、改动代数、收不下的常用语�
��几个时间，免得迁移以后又弹首�
��选择，所以导入之前先调过 status
 或 run 也不要紧；导入过一次、调
过 set_enabled（打开或关闭）以后不
再导入，退出、换账号也不重新开
放，宿主可以每次启动都带着旧状
态调用，用户关掉的同步不会被旧
状态改回打开；在 set_enabled 之前�
�用。已经有进度、导入已经关上�
�绑定的是别的账号或读不懂时 impo
rted 为 false。Android 的词库游标要�
�照 snapshot_queue_directory：旧版本下�
��词库时先把快照交给激活队列、�
��时就把游标记成云端修订号，队�
��里还有这个账号没应用的快照请�
��（在等激活，或者冲突、失败、�
��消而结束）时，把它记成等激活�
��快照，第一轮按等激活或没激活�
��理（没激活就按合并补上云端的�
��，不上传）；没带队列目录或读�
��出队列时不导入词库的游标，词�
��下一轮按首次同步处理。
 * 合并
规则：设置按键三方合并（本机自
上次同步以来改过的键用本机的值
，其余用云端的），上传前按本轮
读到的字段表过滤，字段表外和隐
私相关的键既不上传也不写回本机
；常用语按正文三方合并，一侧删
掉的不会在另一侧复活，两边都有
的沿用云端 id，合并后 id 冲突的�
�新 id，超过 500 条（phrases_over_limit
）或请求体超过 256 KiB（phrases_too_l
arge）时这一类失败、两边都不写�
�词库是整份快照，快照里的词是 e
ntry 减去同一 (kind, code, word) 上标�
�删除的 overlay（不按 id 匹配：云�
�库面板改词时新旧两条 overlay 和�
�后的 entry 共用一个 id），合并时�
��本机还没有的云端词排进本机导�
��队列，等键盘应用以后下一轮再�
��份上传；只有云端的每个词都已�
��在本机（或本机排着删除、改掉�
��）时才算合并完，在这之前这一�
��失败，游标不推进、不上传，首�
��同步（或云端被清空以后）的选�
��只留给词库的下一轮（不进 pendin
g_choice，设置和常用语以后遇到云�
��被清空照样要问），下一轮跳过�
��经在本机或已在队列里的词接着�
��：词还在排队、或队列一次排不�
��（最多 128 个没应用完的请求）�
�是 dictionary_merge_pending，云端有本�
��导入不了的词（本机新词的规则�
��收，或键盘应用时拒绝了）时是 
dictionary_merge_rejected，被导入队列�
�别的失败请求挡住时是 dictionary_me
rge_blocked；键盘因为暂时拿不到词�
��而没应用的请求会重排，权重超�
��新词范围（1 到 100000000）的云端�
��收进范围后导入。合并没做完、�
��者键盘还没发布过本机词库版本�
��dictionary_unpublished，新装以后还没
打开过键盘就打开了同步，下载的
快照交不给激活队列）时，选择一
样留给词库（dictionary_unpublished 而�
��机这一轮还没有自己的词时留成 
merge：这时两个选择都是下载云端�
��照，留着 use_cloud 会在以后那一�
�拿快照整份替换键盘期间学到的�
�），词库这一类还要退避；本机�
�一侧读写词库失败（dictionary_storag
e、dictionary_invalid 这类，不是联网�
��败）时也一样退避，只是选择不�
��。退避第一次 5 分钟，之后每多�
��轮没有进展就翻倍，最多 6 小时�
��这一轮往导入队列里排进了新的�
��、或者云端快照里已经在本机的�
��比上一次记下的多（键盘应用了�
��着的词）时从 5 分钟重新数，只�
��排了键盘因为暂时拿不到词库而�
��应用的请求不算进展；退避期间 
active 跳过词库（sections 里词库是 s
kipped、reason 为 dictionary_backoff，这�
��轮不算同步成功，status.failure 留�
��上次的原因），词库的待上传标�
��也不把节流缩到 30 秒，manual、ena
ble 和带 choice 的不等退避，状态里
记着另一轮在跑时做的 pending_choice
 时这一轮词库也不等退避（选择�
�这一轮用掉，词库跳过就会丢掉�
�；它仍不让这一轮跳过节流）。�
�入队列没清空（个人词库导入队�
�里还有没应用的请求，或者具名�
�库集合的待发送队列里还有没送�
�去的增删，例如 Android 本地备份�
�复排下的大批词）、或下载的云�
�快照还在激活队列里等键盘激活�
�推迟上传（dictionary_pending），等�
�活期间词库这一类也不下载、不�
�并；下载的快照没有激活就结束�
�（冲突、失败或取消）时，下一�
�改按合并处理（本机没有词时重�
�下载），不上传。
 * 错误：请求�
��合法是 cloud_sync_invalid，状态文件
读写失败是 cloud_sync_storage，会话�
�不出来是 account_* 码。会联网（每
个请求最长约 30 秒，词库快照最�
� 10 分钟），在工作线程上调用；�
��要在主线程上调用。 */
char *msime_
client_cloud_sync(const uint8_t *request, siz
e_t length);
char *msime_client_destroy(uint6
4_t session);
/* Write the selection counts h
eld by every session on the calling thread an
d all queued personal-context learning, witho
ut ending any session. Call from the host's w
ill-terminate hook (e.g. NSApplicationWillTer
minateNotification) on the thread that owns t
he sessions; the C++ Engine did this from ate
xit. Returns null on success. */
char *msime_
client_flush_all(void);
/* value must be NULL
 or a still-owned pointer returned by this li
brary. */
void msime_client_string_free(char 
*value);

#ifdef __cplusplus
}
#endif
#endif


