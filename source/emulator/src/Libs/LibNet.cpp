#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Network.h"

#include <cinttypes>
#include <cstddef>

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
	printf("\t name  = %s\n", name != nullptr ? name : "(null)");
	printf("\t flags = %d\n", flags);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollDestroy(int epoll_id)
{
	PRINT_NAME();
	printf("\t epoll_id = %d\n", epoll_id);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollControl(int epoll_id, int operation, int socket, const void* event)
{
	PRINT_NAME();
	printf("\t epoll_id = %d\n", epoll_id);
	printf("\t operation = %d\n", operation);
	printf("\t socket    = %d\n", socket);
	printf("\t event     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(event));
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetEpollWait(int epoll_id, void* events, int max_events, int timeout_ms)
{
	PRINT_NAME();
	printf("\t epoll_id   = %d\n", epoll_id);
	printf("\t events     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(events));
	printf("\t max_events = %d\n", max_events);
	printf("\t timeout_ms = %d\n", timeout_ms);
	EXIT("Net epoll is not implemented\n");
	return -1;
}

static int KYTY_SYSV_ABI NetResolverStartNtoa(int resolver_id, const char* hostname, void* address, int timeout_ms, int retries,
                                              int flags)
{
	PRINT_NAME();
	printf("\t resolver_id = %d\n", resolver_id);
	printf("\t hostname    = %s\n", hostname != nullptr ? hostname : "(null)");
	printf("\t address     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t timeout_ms  = %d\n", timeout_ms);
	printf("\t retries     = %d\n", retries);
	printf("\t flags       = %d\n", flags);
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
	printf("\t socket    = %d\n", socket);
	printf("\t info      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));
	printf("\t info_size = %d\n", info_size);
	printf("\t flags     = %d\n", flags);
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
}

} // namespace LibNet

namespace LibSsl {

LIB_VERSION("Ssl", 1, "Ssl", 1, 1);

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
	printf("\t parameters = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(parameters));

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

// Gen5 sceNpWebApi2Initialize (NID +o9816YQhqQ). Returns a positive library
// context id; no real NP traffic yet.
static int g_npwebapi2_next = 1;

static int KYTY_SYSV_ABI NpWebApi2Initialize(int lib_http_ctx_id, size_t pool_size)
{
	PRINT_NAME();
	printf("\t lib_http_ctx_id = %d\n", lib_http_ctx_id);
	printf("\t pool_size       = %" PRIu64 "\n", static_cast<uint64_t>(pool_size));
	return g_npwebapi2_next++;
}

static int KYTY_SYSV_ABI NpWebApi2Terminate(int lib_ctx_id)
{
	PRINT_NAME();
	printf("\t lib_ctx_id = %d\n", lib_ctx_id);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2CreateUserContext(int lib_ctx_id, int user_id)
{
	PRINT_NAME();
	printf("\t lib_ctx_id = %d\n", lib_ctx_id);
	printf("\t user_id    = %d\n", user_id);
	static int next = 1;
	return next++;
}

static int KYTY_SYSV_ABI NpWebApi2DeleteUserContext(int user_ctx_id)
{
	PRINT_NAME();
	printf("\t user_ctx_id = %d\n", user_ctx_id);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateHandle(int lib_ctx_id)
{
	PRINT_NAME();
	printf("\t lib_ctx_id = %d\n", lib_ctx_id);
	static int handle = 1;
	return handle++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeleteHandle(int handle)
{
	PRINT_NAME();
	printf("\t handle = %d\n", handle);
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateFilter(int handle, const void* filter, size_t size)
{
	PRINT_NAME();
	printf("\t handle = %d size = %" PRIu64 "\n", handle, static_cast<uint64_t>(size));
	static int filter_id = 1;
	return filter_id++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventRegisterCallback(int handle, void* cb, void* user)
{
	PRINT_NAME();
	printf("\t handle = %d cb = 0x%016" PRIx64 "\n", handle, reinterpret_cast<uint64_t>(cb));
	return OK;
}

static int KYTY_SYSV_ABI NpWebApi2CheckTimeout(int lib_ctx_id)
{
	PRINT_NAME();
	printf("\t lib_ctx_id = %d\n", lib_ctx_id);
	return OK;
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
}

} // namespace LibNpWebApi2

namespace LibHttp2 {

LIB_VERSION("Http2", 1, "Http2", 1, 1);

// Gen5 sceHttp2Init (NID 3JCe3lCbQ8A). Returns a positive context id; no real HTTP yet.
static int g_http2_next_ctx = 1;

static int KYTY_SYSV_ABI Http2Init(int libnet_mem_id, int libssl_ctx_id, size_t pool_size, int max_concurrent_request)
{
	PRINT_NAME();
	printf("\t libnet_mem_id          = %d\n", libnet_mem_id);
	printf("\t libssl_ctx_id          = %d\n", libssl_ctx_id);
	printf("\t pool_size              = %" PRIu64 "\n", static_cast<uint64_t>(pool_size));
	printf("\t max_concurrent_request = %d\n", max_concurrent_request);
	return g_http2_next_ctx++;
}

LIB_DEFINE(InitNet_1_Http2)
{
	LIB_FUNC("3JCe3lCbQ8A", LibHttp2::Http2Init);
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
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
