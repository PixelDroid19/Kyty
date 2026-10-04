#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/Objects/GpuWritebackPageCache.h"

#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct Label;
class CommandBuffer;
struct GraphicContext;

void LabelInit();

constexpr int LABEL_ARGS_MAX = 5;

enum class LabelFenceRegistrationStatus : uint8_t
{
	Inserted,
	AlreadyRegistered,
	InvalidArgument,
};

enum class LabelFenceReleaseStatus : uint8_t
{
	Released,
	NotFound,
	InvalidArgument,
	PartialOverlap,
};

constexpr uint64_t LabelDwordStoreSizeBytes(uint32_t dword_count)
{
	return static_cast<uint64_t>(dword_count) * sizeof(uint32_t);
}

class LabelFenceRegistry
{
public:
	[[nodiscard]] LabelFenceRegistrationStatus Register(uint64_t addr, uint64_t bytes);
	[[nodiscard]] LabelFenceReleaseStatus      ReleaseAllocation(uint64_t addr, uint64_t bytes);
	void Snapshot(Vector<uint64_t>* begin, Vector<uint64_t>* end) const;
	// Called after the real completion callback/store, never at event creation
	// or deletion. A completed version can be acquired by a subsequent upload.
	void Complete(uint64_t addr, uint64_t bytes);
	void SnapshotExcluded(uint64_t acquired_version, Vector<uint64_t>* begin, Vector<uint64_t>* end) const;
	[[nodiscard]] bool NeedsUpload(uint64_t addr, uint64_t bytes, uint64_t acquired_version) const;
	[[nodiscard]] uint64_t Version() const { return m_version; }

	[[nodiscard]] uint32_t Size() const { return m_begin.Size(); }

private:
	Vector<uint64_t> m_begin;
	Vector<uint64_t> m_end;
	Vector<uint64_t> m_completed_version;
	Vector<uint64_t> m_pending;
	uint64_t m_version = 0;
};

// Per-backing ownership acquired by a real guest->GPU upload. Excluded GPU
// bytes cannot become publishable just because a transient Label was deleted:
// a new upload must first reset the GPU snapshot from the completed guest data.
class LabelStoragePublication
{
public:
	void Upload(const LabelFenceRegistry& registry, void* gpu_dst, const void* guest_src, uint64_t size,
	            GpuWritebackPageCache* page_cache);
	[[nodiscard]] bool NeedsUpload(const LabelFenceRegistry& registry, uint64_t addr, uint64_t size) const;
	[[nodiscard]] GpuWritebackResult Copy(const LabelFenceRegistry& registry, void* guest_dst, const void* gpu_src, uint64_t size,
	                                    GpuWritebackPageCache* page_cache, GpuWritebackPageCache::NotifyWriteFunc notify_write,
	                                    void* notify_opaque);

private:
	struct FenceBaseline
	{
		uint64_t offset = 0;
		uint64_t word_size = sizeof(uint32_t);
		std::vector<uint8_t> bytes;
		bool whole_words = false;
	};
	uint64_t m_version = 0;
	std::vector<FenceBaseline> m_fences;
	bool m_cpu_conflict = false;
};

using LabelCallback = bool (*)(SubmissionId submission, const uint64_t* args);

class CommandProcessor;

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelCallback callback_1,
                     LabelCallback callback_2, const uint64_t* args);
Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, uint32_t dst_word_count,
                     LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args);
void   LabelDelete(Label* label);
void   LabelSet(CommandBuffer* buffer, Label* label);
void   LabelDrainCompleted();
// After the fence for an exact logical submission completes, force-complete
// only the Active and ActiveDeleted labels recorded into that submission.
// MoltenVK/host often never observes vkCmdSetEvent via vkGetEventStatus; relying
// only on event polling skips WriteBack/OnlyFlip SubmitFlip and leaves WaitRegMem
// spinning or Flip queues empty (guest ThreadFlag soft-lock).
void LabelCompleteSubmission(SubmissionId submission);
// Upload and writeback share label publication synchronization. Completion
// versions remain in the registry until unmap so old backings stay excluded.
void LabelStorageUpload(void* gpu_dst, const void* guest_src, uint64_t size, GpuWritebackPageCache* page_cache,
                        LabelStoragePublication* publication);
[[nodiscard]] bool LabelStorageNeedsUpload(uint64_t addr, uint64_t size, const LabelStoragePublication& publication);
[[nodiscard]] GpuWritebackResult LabelWriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size,
                                                   GpuWritebackPageCache* page_cache, LabelStoragePublication* publication);
// Called only after GPU submissions and host presentation have quiesced, while
// the guest VA is still mapped and before it can be reused.
void LabelReleaseMappedRange(uint64_t addr, uint64_t bytes);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_ */
