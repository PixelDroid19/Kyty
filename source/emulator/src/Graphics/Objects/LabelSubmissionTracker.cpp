#include "Emulator/Graphics/Objects/LabelSubmissionTracker.h"

namespace Kyty::Libs::Graphics {

LabelSubmissionResult LabelSubmissionTracker::Bind(uint64_t token, SubmissionId submission)
{
	if (token == 0 || submission.sequence == 0)
	{
		return LabelSubmissionResult::InvalidArgument;
	}
	if (IsBound(token))
	{
		return LabelSubmissionResult::AlreadyBound;
	}

	m_entries.Add({token, submission, false});
	return LabelSubmissionResult::Success;
}

LabelSubmissionResult LabelSubmissionTracker::MarkDeleted(uint64_t token)
{
	if (token == 0)
	{
		return LabelSubmissionResult::InvalidArgument;
	}
	for (auto& entry: m_entries)
	{
		if (entry.token == token)
		{
			entry.deleted = true;
			return LabelSubmissionResult::Success;
		}
	}
	return LabelSubmissionResult::UnknownLabel;
}

LabelSubmissionResult LabelSubmissionTracker::TakeCompleted(SubmissionId submission,
                                                            Vector<LabelSubmissionCompletion>* completed)
{
	if (submission.sequence == 0 || completed == nullptr)
	{
		return LabelSubmissionResult::InvalidArgument;
	}

	completed->Clear();
	for (int index = 0; index < static_cast<int>(m_entries.Size());)
	{
		const auto& entry = m_entries.At(index);
		if (entry.submission == submission)
		{
			completed->Add({entry.token, entry.deleted ? LabelSubmissionCompletionKind::Destroy
			                                          : LabelSubmissionCompletionKind::Keep});
			m_entries.RemoveAt(index);
			continue;
		}
		index++;
	}
	return LabelSubmissionResult::Success;
}

bool LabelSubmissionTracker::IsBound(uint64_t token) const
{
	for (const auto& entry: m_entries)
	{
		if (entry.token == token)
		{
			return true;
		}
	}
	return false;
}

} // namespace Kyty::Libs::Graphics
