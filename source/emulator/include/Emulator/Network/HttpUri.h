#ifndef EMULATOR_INCLUDE_EMULATOR_NETWORK_HTTPURI_H_
#define EMULATOR_INCLUDE_EMULATOR_NETWORK_HTTPURI_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Network::Http {

// ABI output for the URI parser. Every string points into the caller pool.
struct HttpUriElement
{
	bool     opaque;
	uint8_t  align[7];
	char*    scheme;
	char*    username;
	char*    password;
	char*    hostname;
	char*    path;
	char*    query;
	char*    fragment;
	uint16_t port;
	uint8_t  reserved[10];
};

static_assert(sizeof(HttpUriElement) == 80);
static_assert(offsetof(HttpUriElement, scheme) == 8);
static_assert(offsetof(HttpUriElement, hostname) == 32);
static_assert(offsetof(HttpUriElement, port) == 64);

// Query mode requires only required_size. Fill mode requires both out and pool.
int KYTY_SYSV_ABI HttpUriParse(HttpUriElement* out, const char* source, void* pool, uint64_t* required_size, size_t pool_size);

} // namespace Kyty::Libs::Network::Http

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_NETWORK_HTTPURI_H_ */
