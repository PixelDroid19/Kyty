#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kSpirvBinaryCacheSchemaVersion = 1;

struct SpirvBinaryCacheLimits
{
	size_t max_total_bytes      = 64u * 1024u * 1024u;
	size_t max_entry_bytes      = 4u * 1024u * 1024u;
	size_t session_write_budget = 16u * 1024u * 1024u;
};

enum class SpirvBinaryCacheLoadResult
{
	Hit,
	Miss,
	Corrupt
};

enum class SpirvBinaryCacheStoreResult
{
	Written,
	BudgetExceeded,
	TooLarge,
	Failed
};

class SpirvBinaryCacheStore final
{
public:
	explicit SpirvBinaryCacheStore(std::filesystem::path root, SpirvBinaryCacheLimits limits = {});
	~SpirvBinaryCacheStore();
	KYTY_CLASS_NO_COPY(SpirvBinaryCacheStore);

	[[nodiscard]] SpirvBinaryCacheLoadResult Load(const String8& source, uint32_t optimization, bool validation_enabled,
	                                              Vector<uint32_t>* binary);
	[[nodiscard]] SpirvBinaryCacheStoreResult Store(const String8& source, uint32_t optimization, bool validation_enabled,
	                                                const Vector<uint32_t>& binary);
	[[nodiscard]] size_t                     DiskUsageBytes() const;
	[[nodiscard]] size_t                     SessionBytesAttempted() const;

private:
	struct State;
	State* m_state = nullptr;
};

[[nodiscard]] std::filesystem::path SpirvBinaryCacheDefaultRoot();
SpirvBinaryCacheStore&              SpirvBinaryCacheDefaultStore();

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_ */
