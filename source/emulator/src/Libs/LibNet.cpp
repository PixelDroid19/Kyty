#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Log.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Network.h"
#include "Emulator/Network/HttpUri.h"

#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef KYTY_EMU_ENABLED

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NET_CALL(func)                                                                                                                     \
	[&]()                                                                                                                                  \
	{                                                                                                                                      \
		auto result = func;                                                                                                                \
		if (result < 0)                                                                                                                    \
		{                                                                                                                                  \
			*GetNetErrorAddr() = result;                                                                                                   \
		}                                                                                                                                  \
		return result;                                                                                                                     \
	}()

namespace Kyty::Libs {

namespace Network::Net {
struct NetEtherAddr;
} // namespace Network::Net

namespace LibNet {

LIB_VERSION("Net", 1, "Net", 1, 1);

static thread_local int g_net_errno = 0;
alignas(16) static constexpr uint8_t g_in6addr_any[16] {};

namespace Net = Network::Net;

KYTY_SYSV_ABI int* GetNetErrorAddr()
{
	return &g_net_errno;
}

static int KYTY_SYSV_ABI NetInit()
{
	return NET_CALL(Net::NetInit());
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags)
{
	return NET_CALL(Net::NetPoolCreate(name, size, flags));
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst)
{
	return NET_CALL(Net::NetInetPton(af, src, dst));
}

int KYTY_SYSV_ABI NetEtherNtostr(const Net::NetEtherAddr* n, char* str, size_t len)
{
	return NET_CALL(Net::NetEtherNtostr(n, str, len));
}

int KYTY_SYSV_ABI NetGetMacAddress(Net::NetEtherAddr* addr, int flags)
{
	return NET_CALL(Net::NetGetMacAddress(addr, flags));
}

static int KYTY_SYSV_ABI NetEpollCreate(const char* name, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name  = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t flags = %d\n", flags);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollDestroy(int epoll_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id = %d\n", epoll_id);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollControl(int epoll_id, int operation, int socket, const void* event)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id = %d\n", epoll_id);
	KYTY_LOG_DEBUG("\t operation = %d\n", operation);
	KYTY_LOG_DEBUG("\t socket    = %d\n", socket);
	KYTY_LOG_DEBUG("\t event     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(event));
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollWait(int epoll_id, void* events, int max_events, int timeout_ms)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id   = %d\n", epoll_id);
	KYTY_LOG_DEBUG("\t events     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(events));
	KYTY_LOG_DEBUG("\t max_events = %d\n", max_events);
	KYTY_LOG_DEBUG("\t timeout_ms = %d\n", timeout_ms);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetResolverStartNtoa(int resolver_id, const char* hostname, void* address, int timeout_ms, int retries,
                                              int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t resolver_id = %d\n", resolver_id);
	KYTY_LOG_DEBUG("\t hostname    = %s\n", hostname != nullptr ? hostname : "(null)");
	KYTY_LOG_DEBUG("\t address     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	KYTY_LOG_DEBUG("\t timeout_ms  = %d\n", timeout_ms);
	KYTY_LOG_DEBUG("\t retries     = %d\n", retries);
	KYTY_LOG_DEBUG("\t flags       = %d\n", flags);
	EXIT("Net resolver is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetResolverStartAton()
{
	PRINT_NAME();
	EXIT("Net address-to-name resolver is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetGetSockInfo(int socket, void* info, int info_size, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t socket    = %d\n", socket);
	KYTY_LOG_DEBUG("\t info      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));
	KYTY_LOG_DEBUG("\t info_size = %d\n", info_size);
	KYTY_LOG_DEBUG("\t flags     = %d\n", flags);
	EXIT("Net socket information is not implemented\n");
	return -1;
}

LIB_DEFINE(InitNet_1_Net)
{
	LIB_FUNC("Nlev7Lg8k3A", LibNet::NetInit);
	LIB_FUNC("cTGkc6-TBlI", Net::NetTerm);
	LIB_FUNC("dgJBaeJnGpo", LibNet::NetPoolCreate);
	LIB_FUNC("K7RlrTkI-mw", Net::NetPoolDestroy);
	LIB_FUNC("HQOwnfMGipQ", LibNet::GetNetErrorAddr);
	LIB_FUNC("Q4qBuN-c0ZM", Net::NetSocket);
	LIB_FUNC("45ggEzakPJQ", Net::NetSocketClose);
	LIB_FUNC("bErx49PgxyY", Net::NetBind);
	LIB_FUNC("kOj1HiAGE54", Net::NetListen);
	LIB_FUNC("PIWqhn9oSxc", Net::NetAccept);
	LIB_FUNC("2mKX2Spso7I", Net::NetSetsockopt);
	LIB_FUNC("xphrZusl78E", Net::NetGetsockopt);
	LIB_FUNC("9T2pDF2Ryqg", Net::NetHtonl);
	LIB_FUNC("iWQWrwiSt8A", Net::NetHtons);
	LIB_FUNC("pQGpHYopAIY", Net::NetNtohl);
	LIB_FUNC("Rbvt+5Y2iEw", Net::NetNtohs);
	LIB_FUNC("C4UgDHHPvdw", Net::NetResolverCreate);
	LIB_FUNC("kJlYH5uMAWI", Net::NetResolverDestroy);
	LIB_FUNC("J5i3hiLJMPk", Net::NetResolverGetError);
	LIB_FUNC("8Kcp5d-q1Uo", LibNet::NetInetPton);
	LIB_FUNC("v6M4txecCuo", LibNet::NetEtherNtostr);
	LIB_FUNC("6Oc0bLsIYe0", LibNet::NetGetMacAddress);
	LIB_FUNC("SF47kB2MNTo", LibNet::NetEpollCreate);
	LIB_FUNC("Inp1lfL+Jdw", LibNet::NetEpollDestroy);
	LIB_FUNC("ZVw46bsasAk", LibNet::NetEpollControl);
	LIB_FUNC("drjIbDbA7UQ", LibNet::NetEpollWait);
	LIB_FUNC("Nd91WaWmG2w", LibNet::NetResolverStartNtoa);
	LIB_FUNC("Apb4YDxKsRI", LibNet::NetResolverStartAton);
	LIB_FUNC("hLuXdjHnhiI", LibNet::NetGetSockInfo);
	LIB_OBJECT("ZRAJo-A-ukc", LibNet::g_in6addr_any);
}

} // namespace LibNet

namespace LibSsl {

LIB_VERSION("Ssl", 1, "Ssl", 2, 1);

namespace Ssl = Network::Ssl;

LIB_DEFINE(InitNet_1_Ssl)
{
	LIB_FUNC("hdpVEUDFW3s", Ssl::SslInit);
	LIB_FUNC("0K1yQ6Lv-Yc", Ssl::SslTerm);
	LIB_FUNC("viRXSHZYd0c", Ssl::SslClose);
}

} // namespace LibSsl

namespace LibHttp {

LIB_VERSION("Http", 1, "Http", 1, 1);

namespace Http = Network::Http;

LIB_DEFINE(InitNet_1_Http)
{
	LIB_FUNC("A9cVMUtEp4Y", Http::HttpInit);
	LIB_FUNC("Ik-KpLTlf7Q", Http::HttpTerm);
	LIB_FUNC("0gYjPTR-6cY", Http::HttpCreateTemplate);
	LIB_FUNC("4I8vEpuEhZ8", Http::HttpDeleteTemplate);
	LIB_FUNC("s2-NPIvz+iA", Http::HttpSetNonblock);
	LIB_FUNC("htyBOoWeS58", Http::HttpsSetSslCallback);
	LIB_FUNC("mSQCxzWTwVI", Http::HttpsDisableOption);
	LIB_FUNC("6381dWF+xsQ", Http::HttpCreateEpoll);
	LIB_FUNC("wYhXVfS2Et4", Http::HttpDestroyEpoll);
	LIB_FUNC("-xm7kZQNpHI", Http::HttpSetEpoll);
	LIB_FUNC("59tL1AQBb8U", Http::HttpUnsetEpoll);
	LIB_FUNC("qgxDBjorUxs", Http::HttpCreateConnectionWithURL);
	LIB_FUNC("P6A3ytpsiYc", Http::HttpDeleteConnection);
	LIB_FUNC("Cnp77podkCU", Http::HttpCreateRequestWithURL2);
	LIB_FUNC("qe7oZ+v4PWA", Http::HttpDeleteRequest);
	LIB_FUNC("EY28T2bkN7k", Http::HttpAddRequestHeader);
	LIB_FUNC("1e2BNwI-XzE", Http::HttpSendRequest);
	LIB_FUNC("Tc-hAYDKtQc", Http::HttpSetResolveTimeOut);
	LIB_FUNC("K1d1LqZRQHQ", Http::HttpSetResolveRetry);
	LIB_FUNC("0S9tTH0uqTU", Http::HttpSetConnectTimeOut);
	LIB_FUNC("xegFfZKBVlw", Http::HttpSetSendTimeOut);
	LIB_FUNC("yigr4V0-HTM", Http::HttpSetRecvTimeOut);
	LIB_FUNC("T-mGo9f3Pu4", Http::HttpSetAutoRedirect);
	LIB_FUNC("qFg2SuyTJJY", Http::HttpSetAuthEnabled);
	LIB_FUNC("IWalAn-guFs", Http::HttpUriParse);
}

} // namespace LibHttp

namespace LibNetCtl {

LIB_VERSION("NetCtl", 1, "NetCtl", 1, 1);

namespace NetCtl = Network::NetCtl;

LIB_DEFINE(InitNet_1_NetCtl)
{
	LIB_FUNC("gky0+oaNM4k", NetCtl::NetCtlInit);
	LIB_FUNC("JO4yuTuMoKI", NetCtl::NetCtlGetNatInfo);
	LIB_FUNC("iQw3iQPhvUQ", NetCtl::NetCtlCheckCallback);
	LIB_FUNC("uBPlr0lbuiI", NetCtl::NetCtlGetState);
	LIB_FUNC("UJ+Z7Q+4ck0", NetCtl::NetCtlRegisterCallback);
	LIB_FUNC("1NE9OWdBIww", NetCtl::NetCtlRegisterCallback);
	LIB_FUNC("obuxdTiwkF8", NetCtl::NetCtlGetInfo);
}

} // namespace LibNetCtl

namespace LibNpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace NpManager = Network::NpManager;

LIB_DEFINE(InitNet_1_NpManager)
{
	LIB_FUNC("3Zl8BePTh9Y", NpManager::NpCheckCallback);
	LIB_FUNC("Ec63y59l9tw", NpManager::NpSetNpTitleId);
	LIB_FUNC("A2CQ3kgSopQ", NpManager::NpSetContentRestriction);
	LIB_FUNC("VfRSmPmj8Q8", NpManager::NpRegisterStateCallback);
	LIB_FUNC("qQJfO8HAiaY", NpManager::NpRegisterStateCallback);
	LIB_FUNC("uFJpaKNBAj4", NpManager::NpRegisterGamePresenceCallback);
	LIB_FUNC("GImICnh+boA", NpManager::NpRegisterPlusEventCallback);
	LIB_FUNC("hw5KNqAAels", NpManager::NpRegisterNpReachabilityStateCallback);
	LIB_FUNC("p-o74CnoNzY", NpManager::NpGetNpId);
	LIB_FUNC("XDncXQIJUSk", NpManager::NpGetOnlineId);
	LIB_FUNC("eiqMCt9UshI", NpManager::NpCreateAsyncRequest);
	LIB_FUNC("S7QTn72PrDw", NpManager::NpDeleteRequest);
	LIB_FUNC("2rsFmlGWleQ", NpManager::NpCheckNpAvailability);
	LIB_FUNC("uqcPJLWL08M", NpManager::NpPollAsync);
	LIB_FUNC("eQH7nWPcAgc", NpManager::NpGetState);
}

} // namespace LibNpManager

namespace LibNpManagerForToolkit {

LIB_VERSION("NpManagerForToolkit", 1, "NpManager", 1, 1);

namespace NpManagerForToolkit = Network::NpManagerForToolkit;

LIB_DEFINE(InitNet_1_NpManagerForToolkit)
{
	LIB_FUNC("0c7HbXRKUt4", NpManagerForToolkit::NpRegisterStateCallbackForToolkit);
	LIB_FUNC("JELHf4xPufo", NpManagerForToolkit::NpCheckCallbackForLib);
}

} // namespace LibNpManagerForToolkit

namespace LibNpSessionSignaling {

LIB_VERSION("NpSessionSignaling", 1, "NpSessionSignaling", 1, 1);

static int KYTY_SYSV_ABI NpSessionSignalingInitialize(void* parameters)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t parameters = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(parameters));

	// The offline contract only establishes the library. Network signaling is
	// deliberately not fabricated until a caller requests a concrete operation.
	return OK;
}

LIB_DEFINE(InitNet_1_NpSessionSignaling)
{
	LIB_FUNC("ysmw6J-P8Ak", NpSessionSignalingInitialize);
}

} // namespace LibNpSessionSignaling

namespace LibNpTrophy {

LIB_VERSION("NpTrophy", 1, "NpTrophy", 1, 1);

namespace NpTrophy = Network::NpTrophy;

LIB_DEFINE(InitNet_1_NpTrophy)
{
	LIB_FUNC("q7U6tEAQf7c", NpTrophy::NpTrophyCreateHandle);
	LIB_FUNC("XbkjbobZlCY", NpTrophy::NpTrophyCreateContext);
	LIB_FUNC("TJCAxto9SEU", NpTrophy::NpTrophyRegisterContext);
	LIB_FUNC("GNcF4oidY0Y", NpTrophy::NpTrophyDestroyHandle);
	LIB_FUNC("LHuSmO3SLd8", NpTrophy::NpTrophyGetTrophyUnlockState);
}

} // namespace LibNpTrophy

namespace LibNpWebApi {

LIB_VERSION("NpWebApi", 1, "NpWebApi", 1, 1);

namespace NpWebApi = Network::NpWebApi;

LIB_DEFINE(InitNet_1_NpWebApi)
{
	LIB_FUNC("G3AnLNdRBjE", NpWebApi::NpWebApiInitialize);
}

} // namespace LibNpWebApi

namespace LibNpWebApi2 {

LIB_VERSION("NpWebApi2", 1, "NpWebApi2", 1, 1);

static constexpr int kNpWebApi2ErrorInvalidArgument = static_cast<int>(0x80553402u);
static constexpr int kNpWebApi2ErrorRequestNotFound = static_cast<int>(0x80553406u);

// Gen5 sceNpWebApi2Initialize (NID +o9816YQhqQ). Returns a positive library
// context id; no real NP traffic yet.
static int g_npwebapi2_next = 1;

static int KYTY_SYSV_ABI NpWebApi2Initialize(int lib_http_ctx_id, size_t pool_size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_http_ctx_id = %d\n", lib_http_ctx_id);
	KYTY_LOG_DEBUG("\t pool_size       = %" PRIu64 "\n", static_cast<uint64_t>(pool_size));
	return g_npwebapi2_next++;
}

static int KYTY_SYSV_ABI NpWebApi2Terminate(int lib_ctx_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_ctx_id = %d\n", lib_ctx_id);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2CreateUserContext(int lib_ctx_id, int user_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_ctx_id = %d\n", lib_ctx_id);
	KYTY_LOG_DEBUG("\t user_id    = %d\n", user_id);
	static int next = 1;
	return next++;
}

static int KYTY_SYSV_ABI NpWebApi2DeleteUserContext(int user_ctx_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t user_ctx_id = %d\n", user_ctx_id);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateHandle(int lib_ctx_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_ctx_id = %d\n", lib_ctx_id);
	static int handle = 1;
	return handle++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeleteHandle(int handle)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateFilter(int handle, const void* filter, size_t size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d size = %" PRIu64 "\n", handle, static_cast<uint64_t>(size));
	static int filter_id = 1;
	return filter_id++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventRegisterCallback(int handle, void* cb, void* user)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d cb = 0x%016" PRIx64 "\n", handle, reinterpret_cast<uint64_t>(cb));
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2CheckTimeout(int lib_ctx_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_ctx_id = %d\n", lib_ctx_id);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2GetHttpResponseHeaderValueLength(int64_t /*request_id*/, const char* field_name,
	                                                                  size_t* value_length)
{
	PRINT_NAME();
	if (field_name == nullptr || value_length == nullptr)
	{
		return kNpWebApi2ErrorInvalidArgument;
	}
	return kNpWebApi2ErrorRequestNotFound;
}

LIB_DEFINE(InitNet_1_NpWebApi2)
{
	LIB_FUNC("+o9816YQhqQ", LibNpWebApi2::NpWebApi2Initialize);
	LIB_FUNC("bEvXpcEk200", LibNpWebApi2::NpWebApi2Terminate);
	LIB_FUNC("sk54bi6FtYM", LibNpWebApi2::NpWebApi2CreateUserContext);
	LIB_FUNC("9X9+cneTGUU", LibNpWebApi2::NpWebApi2DeleteUserContext);
	LIB_FUNC("WV1GwM32NgY", LibNpWebApi2::NpWebApi2PushEventCreateHandle);
	LIB_FUNC("fIATVMo4Y1w", LibNpWebApi2::NpWebApi2PushEventDeleteHandle);
	LIB_FUNC("MsaFhR+lPE4", LibNpWebApi2::NpWebApi2PushEventCreateFilter);
	LIB_FUNC("fY3QqeNkF8k", LibNpWebApi2::NpWebApi2PushEventRegisterCallback);
	LIB_FUNC("3Tt9zL3tkoc", LibNpWebApi2::NpWebApi2CheckTimeout);
	LIB_FUNC("HwP3aM+c85c", LibNpWebApi2::NpWebApi2GetHttpResponseHeaderValueLength);
}

} // namespace LibNpWebApi2

namespace LibGameUpdate {

LIB_VERSION("GameUpdate", 1, "GameUpdate", 1, 1);

constexpr int GAME_UPDATE_ERROR_NOT_INITIALIZED   = static_cast<int>(0x80412801);
constexpr int GAME_UPDATE_ERROR_INVALID_ARG       = static_cast<int>(0x80412803);
constexpr int GAME_UPDATE_ERROR_INVALID_SIZE      = static_cast<int>(0x80412804);
constexpr int GAME_UPDATE_ERROR_REQUEST_NOT_FOUND = static_cast<int>(0x80412805);

struct GameUpdateCheckParam
{
	uint64_t size;
	uint32_t option;
	uint32_t reserved[9];
};

struct GameUpdateCheckResult
{
	uint64_t size;
	uint8_t  found;
	uint8_t  addcont_found;
	uint8_t  padding[2];
	char     content_version[11];
	uint8_t  padding2;
	uint32_t reserved[6];
};

struct GameUpdateAddcontVersionInfo
{
	uint64_t size;
	uint8_t  found;
	char     content_version[11];
	uint32_t reserved[6];
};

static std::mutex    g_game_update_mutex;
static bool          g_game_update_initialized = false;
static int           g_game_update_next_request = 1;
static std::map<int, bool> g_game_update_requests;

static int KYTY_SYSV_ABI GameUpdateInitialize()
{
	std::lock_guard lock(g_game_update_mutex);
	g_game_update_initialized = true;
	return 0;
}

static int KYTY_SYSV_ABI GameUpdateTerminate()
{
	std::lock_guard lock(g_game_update_mutex);
	g_game_update_initialized = false;
	g_game_update_requests.clear();
	return 0;
}

static int KYTY_SYSV_ABI GameUpdateCreateRequest()
{
	std::lock_guard lock(g_game_update_mutex);
	if (!g_game_update_initialized)
	{
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	const int request_id = g_game_update_next_request++;
	g_game_update_requests[request_id] = true;
	return request_id;
}

static int KYTY_SYSV_ABI GameUpdateCheck(int request_id, const GameUpdateCheckParam* param, GameUpdateCheckResult* result)
{
	std::lock_guard lock(g_game_update_mutex);
	if (!g_game_update_initialized)
	{
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.find(request_id) == g_game_update_requests.end())
	{
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}
	if (param == nullptr || result == nullptr)
	{
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (param->size < sizeof(GameUpdateCheckParam) || result->size < sizeof(GameUpdateCheckResult))
	{
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}
	const uint64_t result_size = result->size;
	*result = {};
	result->size = result_size;
	return 0;
}

static int KYTY_SYSV_ABI GameUpdateAbortRequest(int request_id)
{
	std::lock_guard lock(g_game_update_mutex);
	if (!g_game_update_initialized)
	{
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	return g_game_update_requests.find(request_id) != g_game_update_requests.end() ? 0 : GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
}

static int KYTY_SYSV_ABI GameUpdateDeleteRequest(int request_id)
{
	std::lock_guard lock(g_game_update_mutex);
	if (!g_game_update_initialized)
	{
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	return g_game_update_requests.erase(request_id) != 0 ? 0 : GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
}

static int KYTY_SYSV_ABI GameUpdateGetAddcontLatestVersion(uint32_t, const void*, GameUpdateAddcontVersionInfo* info)
{
	std::lock_guard lock(g_game_update_mutex);
	if (!g_game_update_initialized)
	{
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr)
	{
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (info->size < sizeof(GameUpdateAddcontVersionInfo))
	{
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}
	const uint64_t info_size = info->size;
	*info = {};
	info->size = info_size;
	return 0;
}

LIB_DEFINE(InitNet_1_GameUpdate)
{
	LIB_FUNC("YJtKLttI9fM", GameUpdateInitialize);
	LIB_FUNC("NSH-C-OmoNI", GameUpdateTerminate);
	LIB_FUNC("UvcvKaFvupA", GameUpdateCreateRequest);
	LIB_FUNC("LYVV9z8+owM", GameUpdateCheck);
	LIB_FUNC("d1CNGEOaK28", GameUpdateAbortRequest);
	LIB_FUNC("bcCyjHN5sn0", GameUpdateDeleteRequest);
	LIB_FUNC("0g0+Oq9xcI0", GameUpdateGetAddcontLatestVersion);
}

} // namespace LibGameUpdate

namespace LibHttp2 {

LIB_VERSION("Http2", 1, "Http2", 1, 1);

namespace {

constexpr int HTTP2_ERROR_INVALID_ID   = static_cast<int>(0x817b1100u);
constexpr int HTTP2_ERROR_BEFORE_SEND  = static_cast<int>(0x817b1065u);
constexpr int HTTP2_ERROR_TIMEOUT      = static_cast<int>(0x817b1068u);
constexpr int HTTP2_ERROR_NULL_POINTER = static_cast<int>(0x817b1225u);

struct Http2Options
{
	bool     auth_enabled {};
	bool     auto_redirect {};
	bool     inflate_gzip {};
	uint32_t ssl_options {};
	uint32_t resolve_timeout_us {};
	uint32_t connect_timeout_us {};
	uint32_t connection_wait_timeout_us {};
	uint32_t send_timeout_us {};
	uint32_t recv_timeout_us {};
	uint32_t timeout_us {};
	void*    ssl_callback {};
	void*    ssl_callback_arg {};
	void*    redirect_callback {};
	void*    redirect_callback_arg {};
};

struct Http2Context
{
	int    libnet_mem_id {};
	int    libssl_ctx_id {};
	size_t pool_size {};
	int    max_concurrent_requests {};
	Http2Options options {};
};

struct Http2Template
{
	int         context_id {};
	std::string user_agent;
	int         http_version {};
	bool        auto_proxy_configuration {};
	Http2Options options {};
};

struct Http2Request
{
	int         template_id {};
	std::string method;
	std::string url;
	uint64_t    content_length {};
	int         send_result = HTTP2_ERROR_BEFORE_SEND;
	int         async_result = HTTP2_ERROR_BEFORE_SEND;
	int         async_event {};
	int         status_code {};
	bool        aborted {};
	std::string response_headers;
	std::vector<uint8_t> response_body;
	size_t      read_offset {};
	Http2Options options {};
	struct Header
	{
		std::string name;
		std::string value;
		uint32_t    mode {};
	};
	std::vector<Header> headers;
};

struct Http2AsyncResult
{
	int      event_type {};
	int      request_id {};
	int      result {};
	uint8_t  reserved0[4] {};
	uint64_t reserved1 {};
};

struct Http2Registry
{
	std::mutex                   mutex;
	int                          next_context_id  = 1;
	int                          next_template_id = 1;
	int                          next_request_id  = 1;
	std::map<int, Http2Context>  contexts;
	std::map<int, Http2Template> templates;
	std::map<int, Http2Request>  requests;
};

Http2Registry g_http2_registry;

static Http2Options* FindOptions(int id)
{
	if (auto request = g_http2_registry.requests.find(id); request != g_http2_registry.requests.end())
	{
		return &request->second.options;
	}
	if (auto request_template = g_http2_registry.templates.find(id); request_template != g_http2_registry.templates.end())
	{
		return &request_template->second.options;
	}
	if (auto context = g_http2_registry.contexts.find(id); context != g_http2_registry.contexts.end())
	{
		return &context->second.options;
	}
	return nullptr;
}

} // namespace

static int KYTY_SYSV_ABI Http2Init(int libnet_mem_id, int libssl_ctx_id, size_t pool_size, int max_concurrent_request)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t libnet_mem_id          = %d\n", libnet_mem_id);
	KYTY_LOG_DEBUG("\t libssl_ctx_id          = %d\n", libssl_ctx_id);
	KYTY_LOG_DEBUG("\t pool_size              = %" PRIu64 "\n", static_cast<uint64_t>(pool_size));
	KYTY_LOG_DEBUG("\t max_concurrent_request = %d\n", max_concurrent_request);

	std::lock_guard lock(g_http2_registry.mutex);
	const int       context_id = g_http2_registry.next_context_id++;
	g_http2_registry.contexts.emplace(
	    context_id, Http2Context {
	                    .libnet_mem_id            = libnet_mem_id,
	                    .libssl_ctx_id            = libssl_ctx_id,
	                    .pool_size                = pool_size,
	                    .max_concurrent_requests = max_concurrent_request,
	                });
	return context_id;
}

static int KYTY_SYSV_ABI Http2CreateTemplate(int lib_http2_context_id, const char* user_agent, int http_version,
                                             int is_auto_proxy_configuration)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t lib_http2_context_id       = %d\n", lib_http2_context_id);
	KYTY_LOG_DEBUG("\t user_agent                 = %s\n", user_agent != nullptr ? user_agent : "(null)");
	KYTY_LOG_DEBUG("\t http_version               = %d\n", http_version);
	KYTY_LOG_DEBUG("\t auto_proxy_configuration   = %d\n", is_auto_proxy_configuration);

	if (user_agent == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	if (g_http2_registry.contexts.find(lib_http2_context_id) == g_http2_registry.contexts.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	const int template_id = g_http2_registry.next_template_id++;
	g_http2_registry.templates.emplace(
	    template_id, Http2Template {
	                     .context_id               = lib_http2_context_id,
	                     .user_agent               = user_agent,
	                     .http_version             = http_version,
	                     .auto_proxy_configuration = is_auto_proxy_configuration != 0,
	                 });
	return template_id;
}

static int KYTY_SYSV_ABI Http2CreateRequestWithUrl(int template_id, const char* method, const char* url, uint64_t content_length)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t template_id    = %d\n", template_id);
	KYTY_LOG_DEBUG("\t method         = %s\n", method != nullptr ? method : "(null)");
	KYTY_LOG_DEBUG("\t url            = %s\n", url != nullptr ? url : "(null)");
	KYTY_LOG_DEBUG("\t content_length = %" PRIu64 "\n", content_length);

	if (method == nullptr || url == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	if (g_http2_registry.templates.find(template_id) == g_http2_registry.templates.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	const int request_id = g_http2_registry.next_request_id++;
	g_http2_registry.requests.emplace(
	    request_id, Http2Request {.template_id = template_id, .method = method, .url = url, .content_length = content_length});
	return request_id;
}

static int KYTY_SYSV_ABI Http2AddRequestHeader(int request_id, const char* name, const char* value, uint32_t mode)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t request_id = %d\n", request_id);
	KYTY_LOG_DEBUG("\t name       = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t value      = %s\n", value != nullptr ? value : "(null)");
	KYTY_LOG_DEBUG("\t mode       = %" PRIu32 "\n", mode);

	if (name == nullptr || value == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.headers.push_back(Http2Request::Header {.name = name, .value = value, .mode = mode});
	return 0;
}

static int KYTY_SYSV_ABI Http2SetRequestContentLength(int request_id, uint64_t content_length)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t request_id     = %d\n", request_id);
	KYTY_LOG_DEBUG("\t content_length = %" PRIu64 "\n", content_length);

	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.content_length = content_length;
	return 0;
}

static int KYTY_SYSV_ABI Http2SetAuthEnabled(int id, int enabled)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id      = %d\n", id);
	KYTY_LOG_DEBUG("\t enabled = %d\n", enabled);

	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	options->auth_enabled = enabled != 0;
	return 0;
}

static int KYTY_SYSV_ABI Http2CookieFlush()
{
	PRINT_NAME();
	return 0;
}

static int KYTY_SYSV_ABI Http2DeleteTemplate(int template_id)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	if (g_http2_registry.templates.find(template_id) == g_http2_registry.templates.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	for (auto request = g_http2_registry.requests.begin(); request != g_http2_registry.requests.end();)
	{
		request = request->second.template_id == template_id ? g_http2_registry.requests.erase(request) : std::next(request);
	}
	g_http2_registry.templates.erase(template_id);
	return 0;
}

static int KYTY_SYSV_ABI Http2Term(int context_id)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	if (g_http2_registry.contexts.find(context_id) == g_http2_registry.contexts.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	for (auto request = g_http2_registry.requests.begin(); request != g_http2_registry.requests.end();)
	{
		const auto template_id = request->second.template_id;
		const auto template_it = g_http2_registry.templates.find(template_id);
		request = template_it != g_http2_registry.templates.end() && template_it->second.context_id == context_id
		              ? g_http2_registry.requests.erase(request)
		              : std::next(request);
	}
	for (auto request_template = g_http2_registry.templates.begin(); request_template != g_http2_registry.templates.end();)
	{
		request_template = request_template->second.context_id == context_id
		                      ? g_http2_registry.templates.erase(request_template)
		                      : std::next(request_template);
	}
	g_http2_registry.contexts.erase(context_id);
	return 0;
}

static int KYTY_SYSV_ABI Http2SetAutoRedirect(int id, int enabled)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->auto_redirect = enabled != 0;
	return 0;
}

static int KYTY_SYSV_ABI Http2SetInflateGzipEnabled(int id, int enabled)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->inflate_gzip = enabled != 0;
	return 0;
}

static int KYTY_SYSV_ABI Http2SslEnableOption(int id, uint32_t flags)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->ssl_options |= flags;
	return 0;
}

static int KYTY_SYSV_ABI Http2SslDisableOption(int id, uint32_t flags)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->ssl_options &= ~flags;
	return 0;
}

static int KYTY_SYSV_ABI Http2SetRedirectCallback(int id, void* callback, void* callback_arg)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->redirect_callback     = callback;
	options->redirect_callback_arg = callback_arg;
	return 0;
}

static int KYTY_SYSV_ABI Http2SetSslCallback(int id, void* callback, void* callback_arg)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->ssl_callback     = callback;
	options->ssl_callback_arg = callback_arg;
	return 0;
}

static int SetHttp2Timeout(int id, uint32_t value, uint32_t Http2Options::*field)
{
	std::lock_guard lock(g_http2_registry.mutex);
	auto*           options = FindOptions(id);
	if (options == nullptr)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	options->*field = value;
	return 0;
}

static int KYTY_SYSV_ABI Http2SetConnectTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::connect_timeout_us);
}

static int KYTY_SYSV_ABI Http2SetConnectionWaitTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::connection_wait_timeout_us);
}

static int KYTY_SYSV_ABI Http2SetResolveTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::resolve_timeout_us);
}

static int KYTY_SYSV_ABI Http2SetTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::timeout_us);
}

static int KYTY_SYSV_ABI Http2SetSendTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::send_timeout_us);
}

static int KYTY_SYSV_ABI Http2SetRecvTimeout(int id, uint32_t value)
{
	PRINT_NAME();
	return SetHttp2Timeout(id, value, &Http2Options::recv_timeout_us);
}

static int KYTY_SYSV_ABI Http2SendRequest(int request_id, const void* post_data, size_t size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t request_id = %d\n", request_id);
	KYTY_LOG_DEBUG("\t post_data  = %p\n", post_data);
	KYTY_LOG_DEBUG("\t size       = %" PRIu64 "\n", static_cast<uint64_t>(size));

	if (post_data == nullptr && size != 0u)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.send_result = HTTP2_ERROR_TIMEOUT;
	return request->second.send_result;
}

static int KYTY_SYSV_ABI Http2SendRequestAsync(int request_id, const void* post_data, size_t size, void* kqueue_option,
	                                              void* option)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t request_id    = %d\n", request_id);
	KYTY_LOG_DEBUG("\t post_data     = %p\n", post_data);
	KYTY_LOG_DEBUG("\t size          = %" PRIu64 "\n", static_cast<uint64_t>(size));
	KYTY_LOG_DEBUG("\t kqueue_option = %p\n", kqueue_option);
	KYTY_LOG_DEBUG("\t option        = %p\n", option);

	if (post_data == nullptr && size != 0u)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.send_result  = HTTP2_ERROR_TIMEOUT;
	request->second.async_result = HTTP2_ERROR_TIMEOUT;
	request->second.async_event  = 0;
	return 0;
}

static int KYTY_SYSV_ABI Http2WaitAsync(int request_id, Http2AsyncResult* result, uint32_t* timeout, void* option)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t request_id = %d\n", request_id);
	KYTY_LOG_DEBUG("\t result     = %p\n", static_cast<void*>(result));
	KYTY_LOG_DEBUG("\t timeout    = %p\n", static_cast<void*>(timeout));
	KYTY_LOG_DEBUG("\t option     = %p\n", option);

	if (result == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}

	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}

	*result = {.event_type = request->second.async_event, .request_id = request_id, .result = request->second.async_result};
	return 0;
}

static int KYTY_SYSV_ABI Http2AbortRequest(int request_id)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	request->second.aborted      = true;
	request->second.send_result  = HTTP2_ERROR_TIMEOUT;
	request->second.async_result = HTTP2_ERROR_TIMEOUT;
	request->second.async_event  = 1;
	return 0;
}

static int KYTY_SYSV_ABI Http2GetStatusCode(int request_id, int* status_code)
{
	if (status_code == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}
	*status_code = 0;
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	if (request->second.send_result != 0)
	{
		return request->second.send_result;
	}
	*status_code = request->second.status_code;
	return 0;
}

static int KYTY_SYSV_ABI Http2GetResponseContentLength(int request_id, int* result, uint64_t* content_length)
{
	if (result == nullptr || content_length == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}
	*result         = 0;
	*content_length = 0;
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	if (request->second.send_result != 0)
	{
		*result = -1;
		return request->second.send_result;
	}
	*content_length = request->second.response_body.size();
	return 0;
}

static int KYTY_SYSV_ABI Http2GetAllResponseHeaders(int request_id, char** headers, size_t* header_size)
{
	if (headers == nullptr || header_size == nullptr)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}
	*headers     = nullptr;
	*header_size = 0;
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	if (request->second.send_result != 0)
	{
		return request->second.send_result;
	}
	return 0;
}

static int KYTY_SYSV_ABI Http2ReadData(int request_id, void* data, size_t size)
{
	if (data == nullptr && size != 0u)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	if (request->second.send_result != 0)
	{
		return request->second.send_result;
	}
	const auto remaining = request->second.read_offset < request->second.response_body.size()
	                           ? request->second.response_body.size() - request->second.read_offset
	                           : 0;
	const auto to_copy = std::min(size, remaining);
	if (to_copy != 0)
	{
		std::memcpy(data, request->second.response_body.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}
	return static_cast<int>(to_copy);
}

static int KYTY_SYSV_ABI Http2ReadDataAsync(int request_id, void* data, size_t size, void* kqueue_option, void* option)
{
	(void) kqueue_option;
	(void) option;
	if (data == nullptr && size != 0u)
	{
		return HTTP2_ERROR_NULL_POINTER;
	}
	std::lock_guard lock(g_http2_registry.mutex);
	auto            request = g_http2_registry.requests.find(request_id);
	if (request == g_http2_registry.requests.end())
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	if (request->second.send_result != 0)
	{
		request->second.async_result = request->second.send_result;
		request->second.async_event  = 1;
		return 0;
	}
	const auto remaining = request->second.read_offset < request->second.response_body.size()
	                           ? request->second.response_body.size() - request->second.read_offset
	                           : 0;
	const auto to_copy = std::min(size, remaining);
	if (to_copy != 0)
	{
		std::memcpy(data, request->second.response_body.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}
	request->second.async_result = static_cast<int>(to_copy);
	request->second.async_event  = 1;
	return 0;
}

static int KYTY_SYSV_ABI Http2DeleteRequest(int request_id)
{
	PRINT_NAME();
	std::lock_guard lock(g_http2_registry.mutex);
	if (g_http2_registry.requests.erase(request_id) == 0)
	{
		return HTTP2_ERROR_INVALID_ID;
	}
	return 0;
}

LIB_DEFINE(InitNet_1_Http2)
{
	LIB_FUNC("3JCe3lCbQ8A", LibHttp2::Http2Init);
	LIB_FUNC("YiBUtz-pGkc", LibHttp2::Http2Term);
	LIB_FUNC("+wCt7fCijgk", LibHttp2::Http2CreateTemplate);
	LIB_FUNC("pDom5-078DA", LibHttp2::Http2DeleteTemplate);
	LIB_FUNC("mmyOCxQMVYQ", LibHttp2::Http2CreateRequestWithUrl);
	LIB_FUNC("nrPfOE8TQu0", LibHttp2::Http2AddRequestHeader);
	LIB_FUNC("FSAFOzi0FpM", LibHttp2::Http2SetRequestContentLength);
	LIB_FUNC("jjFahkBPCYs", LibHttp2::Http2SetAuthEnabled);
	LIB_FUNC("5VlQSzXW-SQ", LibHttp2::Http2CookieFlush);
	LIB_FUNC("b9AvoIaOuHI", LibHttp2::Http2SetAutoRedirect);
	LIB_FUNC("uRosf8GQbHQ", LibHttp2::Http2SetInflateGzipEnabled);
	LIB_FUNC("B37SruheQ5Y", LibHttp2::Http2SslDisableOption);
	LIB_FUNC("EWcwMpbr5F8", LibHttp2::Http2SslEnableOption);
	LIB_FUNC("BJgi0CH7al4", LibHttp2::Http2SetRedirectCallback);
	LIB_FUNC("izvHhqgDt44", LibHttp2::Http2SetRecvTimeout);
	LIB_FUNC("XPtW45xiLHk", LibHttp2::Http2SetSendTimeout);
	LIB_FUNC("-HIO4VT87v8", LibHttp2::Http2SetConnectTimeout);
	LIB_FUNC("n8hMLe31OPA", LibHttp2::Http2SetConnectionWaitTimeout);
	LIB_FUNC("ACjtE27aErY", LibHttp2::Http2SetResolveTimeout);
	LIB_FUNC("VYMxTcBqSE0", LibHttp2::Http2SetTimeout);
	LIB_FUNC("YrWX+DhPHQY", LibHttp2::Http2SetSslCallback);
	LIB_FUNC("IZ-qjhRqvjk", LibHttp2::Http2AbortRequest);
	LIB_FUNC("rbqZig38AT8", LibHttp2::Http2SendRequest);
	LIB_FUNC("A+NVAFu4eCg", LibHttp2::Http2SendRequestAsync);
	LIB_FUNC("MOp-AUhdfi8", LibHttp2::Http2WaitAsync);
	LIB_FUNC("9XYJwCf3lEA", LibHttp2::Http2GetStatusCode);
	LIB_FUNC("o0DBQpFE13o", LibHttp2::Http2GetResponseContentLength);
	LIB_FUNC("-rdXUi2XW90", LibHttp2::Http2GetAllResponseHeaders);
	LIB_FUNC("QygCNNmbGss", LibHttp2::Http2ReadData);
	LIB_FUNC("bGN-6zbo7ms", LibHttp2::Http2ReadDataAsync);
	LIB_FUNC("c8D9qIjo8EY", LibHttp2::Http2DeleteRequest);
}

} // namespace LibHttp2

LIB_DEFINE(InitNet_1)
{
	LibNet::InitNet_1_Net(s);
	LibSsl::InitNet_1_Ssl(s);
	LibHttp::InitNet_1_Http(s);
	LibHttp2::InitNet_1_Http2(s);
	LibNetCtl::InitNet_1_NetCtl(s);
	LibNpManager::InitNet_1_NpManager(s);
	LibNpManagerForToolkit::InitNet_1_NpManagerForToolkit(s);
	LibNpSessionSignaling::InitNet_1_NpSessionSignaling(s);
	LibNpTrophy::InitNet_1_NpTrophy(s);
	LibNpWebApi::InitNet_1_NpWebApi(s);
	LibNpWebApi2::InitNet_1_NpWebApi2(s);
	LibGameUpdate::InitNet_1_GameUpdate(s);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
