#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SHAREDMAPPING_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SHAREDMAPPING_H_

#include "Kyty/DevTools/Protocol/Protocol.h"

#include <cstdint>
#include <memory>

namespace Kyty::DevTools {

enum class ProcessOperationError: uint8_t
{
	None               = 0,
	Unsupported        = 1,
	InvalidArgument    = 2,
	EntropyUnavailable = 3,
	MappingFailed      = 4,
	SpawnFailed        = 5,
	HandleFailed       = 6
};

class SharedMapping
{
public:
	SharedMapping() noexcept;
	SharedMapping(SharedMapping&&) noexcept;
	SharedMapping& operator=(SharedMapping&&) noexcept;
	~SharedMapping();
	SharedMapping(const SharedMapping&)            = delete;
	SharedMapping& operator=(const SharedMapping&) = delete;

	static ProcessOperationError CreateOwnerOnly(uint64_t size, SharedMapping* out) noexcept;

	[[nodiscard]] MutableMappingView MutableView() noexcept;
	[[nodiscard]] ConstMappingView   View() const noexcept;
	[[nodiscard]] uint64_t           InheritableHandle() const noexcept;
	[[nodiscard]] bool               IsValid() const noexcept;
	void                             Close() noexcept;

private:
	struct State;
	std::unique_ptr<State> state_;
};

// Fill 16 random bytes; no fallback entropy sources.
[[nodiscard]] ProcessOperationError SecureRandomFill(uint8_t* out, uint32_t size) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SHAREDMAPPING_H_ */
