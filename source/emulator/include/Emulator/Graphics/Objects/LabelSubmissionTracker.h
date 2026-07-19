#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABELSUBMISSIONTRACKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABELSUBMISSIONTRACKER_H_

#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

enum class LabelSubmissionResult : uint8_t
{
	Success,
	InvalidArgument,
	AlreadyBound,
	UnknownLabel,
};

enum class LabelSubmissionCompletionKind : uint8_t
{
	Keep,
	Destroy,
};

struct LabelSubmissionCompletion
{
	uint64_t                      token = 0;
	LabelSubmissionCompletionKind kind = LabelSubmissionCompletionKind::Keep;
};

// Records the exact logical submission containing each Label's vkCmdSetEvent.
// The caller owns synchronization and translates opaque tokens back to Labels.
class LabelSubmissionTracker
{
public:
	LabelSubmissionResult Bind(uint64_t token, SubmissionId submission);
	LabelSubmissionResult MarkDeleted(uint64_t token);
	LabelSubmissionResult TakeCompleted(SubmissionId submission, Vector<LabelSubmissionCompletion>* completed);

	[[nodiscard]] bool IsBound(uint64_t token) const;

private:
	struct Entry
	{
		uint64_t     token = 0;
		SubmissionId submission;
		bool         deleted = false;
	};

	Vector<Entry> m_entries;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABELSUBMISSIONTRACKER_H_ */
