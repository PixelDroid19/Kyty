#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kSpirvBinaryCacheSchemaVersion = 2;

struct ShaderModuleKey;

struct SpirvBinaryCacheLimits
{
	// Room for the translated working sets of several titles in the shared default root.
	size_t max_total_bytes       = 1024u * 1024u * 1024u;
	size_t max_entry_bytes       = 4u * 1024u * 1024u;
	size_t session_write_budget  = 256u * 1024u * 1024u;
	size_t max_pending_entries   = 64u;
	size_t max_pending_bytes     = 8u * 1024u * 1024u;
	size_t max_completed_entries = 1024u;
	// Bounds the entries a title used in earlier sessions that are read into memory ahead of use.
	size_t max_working_set_entries = 4096u;
	size_t max_working_set_bytes   = 256u * 1024u * 1024u;
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

enum class SpirvBinaryCacheQueueResult
{
	Queued,
	Coalesced,
	QueueFull,
	TooLarge,
	Failed
};

struct SpirvBinaryCacheAsyncStats
{
	size_t queued            = 0;
	size_t coalesced         = 0;
	size_t dropped           = 0;
	size_t written           = 0;
	size_t failed            = 0;
	size_t pending_entries   = 0;
	size_t pending_bytes     = 0;
	size_t completed_entries = 0;
};

class SpirvBinaryCacheStore final
{
public:
	using WriteHookForTesting = void (*)(void* opaque);

	explicit SpirvBinaryCacheStore(std::filesystem::path root, SpirvBinaryCacheLimits limits = {});
	~SpirvBinaryCacheStore();
	KYTY_CLASS_NO_COPY(SpirvBinaryCacheStore);

	[[nodiscard]] SpirvBinaryCacheLoadResult Load(const String8& source, uint32_t optimization, bool validation_enabled,
	                                              Vector<uint32_t>* binary);
	[[nodiscard]] SpirvBinaryCacheStoreResult Store(const String8& source, uint32_t optimization, bool validation_enabled,
	                                                const Vector<uint32_t>& binary);
	[[nodiscard]] SpirvBinaryCacheQueueResult QueueStore(const String8& source, uint32_t optimization, bool validation_enabled,
	                                                    const Vector<uint32_t>& binary);
	[[nodiscard]] SpirvBinaryCacheLoadResult  LoadModule(const ShaderModuleKey& key, bool validation_enabled,
	                                                     Vector<uint32_t>* binary);
	[[nodiscard]] SpirvBinaryCacheStoreResult StoreModule(const ShaderModuleKey& key, bool validation_enabled,
	                                                      const Vector<uint32_t>& binary);
	[[nodiscard]] SpirvBinaryCacheQueueResult QueueStoreModule(const ShaderModuleKey& key, bool validation_enabled,
	                                                          const Vector<uint32_t>& binary);
	[[nodiscard]] size_t                     DiskUsageBytes() const;
	[[nodiscard]] size_t                     SessionBytesAttempted() const;
	[[nodiscard]] SpirvBinaryCacheAsyncStats AsyncStats() const;
	void                                      Drain();
	void                                      SetWriteHookForTesting(WriteHookForTesting hook, void* opaque);
	// Records in `manifest` the entries this store loads, and reads the entries it
	// already lists into memory on a background thread: a load that runs on the
	// render path then does not wait on the disk. Only the first call takes effect.
	void StartWorkingSet(std::filesystem::path manifest);
	// Blocks until the background read of the working set has finished.
	void WaitForWorkingSet();

private:
	[[nodiscard]] SpirvBinaryCacheLoadResult LoadEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
	                                                   bool validation_enabled, uint64_t key_low, uint64_t key_high,
	                                                   const char* extension, Vector<uint32_t>* binary);
	[[nodiscard]] SpirvBinaryCacheStoreResult StoreEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
	                                                     bool validation_enabled, uint64_t key_low, uint64_t key_high,
	                                                     const char* extension, const Vector<uint32_t>& binary);
	[[nodiscard]] SpirvBinaryCacheQueueResult QueueEntry(const uint8_t* identity, size_t identity_size, uint32_t optimization,
	                                                    bool validation_enabled, uint64_t key_low, uint64_t key_high,
	                                                    const char* extension, const Vector<uint32_t>& binary);
	void WorkerMain();
	void PreloadMain();
	// Called with the queue lock held; empty when the set did not change since it was last written.
	[[nodiscard]] std::string TakeWorkingSetManifest();

	struct State;
	State* m_state = nullptr;
};

[[nodiscard]] std::filesystem::path SpirvBinaryCacheDefaultRoot();
SpirvBinaryCacheStore&              SpirvBinaryCacheDefaultStore();

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SPIRVBINARYCACHESTORE_H_ */
