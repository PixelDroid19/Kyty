#ifndef INCLUDE_KYTY_CORE_DOMAINRESULT_H_
#define INCLUDE_KYTY_CORE_DOMAINRESULT_H_

#include "Kyty/Core/Common.h"

#include <cstddef>
#include <cstring>

namespace Kyty::Core::Domain {

// Thin typed results for validate → resolve/policy → execute → diagnostic.
// Not a generic framework: domains compose short sequences of pure rules.

enum class Status : uint8_t
{
	Ok    = 0,
	Error = 1,
};

// Stable error shape required by the validation architecture.
struct Error
{
	char code[64]      = {};
	char subsystem[32] = {};
	char operation[48] = {};
	char reason[128]   = {};
	// Sanitized context only (no private guest paths required).
	char context[192] = {};
};

struct ValidationResult
{
	Status status = Status::Ok;
	Error  error {};

	[[nodiscard]] bool Ok() const noexcept { return status == Status::Ok; }
};

// Resolve and operation results share the same shape so callers stay uniform
// without introducing a mega-framework.
using ResolveResult    = ValidationResult;
using OperationResult  = ValidationResult;

inline void CopyField(char* dst, std::size_t cap, const char* src) noexcept
{
	if (dst == nullptr || cap == 0)
	{
		return;
	}
	if (src == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	std::size_t i = 0;
	for (; i + 1 < cap && src[i] != '\0'; ++i)
	{
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

[[nodiscard]] inline ValidationResult Ok() noexcept
{
	return ValidationResult {};
}

[[nodiscard]] inline ValidationResult Fail(const char* code, const char* subsystem, const char* operation,
                                           const char* reason, const char* context = nullptr) noexcept
{
	ValidationResult r {};
	r.status = Status::Error;
	CopyField(r.error.code, sizeof(r.error.code), code);
	CopyField(r.error.subsystem, sizeof(r.error.subsystem), subsystem);
	CopyField(r.error.operation, sizeof(r.error.operation), operation);
	CopyField(r.error.reason, sizeof(r.error.reason), reason);
	CopyField(r.error.context, sizeof(r.error.context), context != nullptr ? context : "");
	return r;
}

// Run pure rules in order; first failure wins. Rules are callables returning
// ValidationResult. No logging, mutation, or process exit.
template <typename Rule, typename... Rest>
[[nodiscard]] ValidationResult RunRules(Rule&& rule, Rest&&... rest) noexcept
{
	ValidationResult r = rule();
	if (!r.Ok())
	{
		return r;
	}
	if constexpr (sizeof...(rest) == 0)
	{
		return r;
	} else
	{
		return RunRules(static_cast<Rest&&>(rest)...);
	}
}

} // namespace Kyty::Core::Domain

#endif /* INCLUDE_KYTY_CORE_DOMAINRESULT_H_ */
