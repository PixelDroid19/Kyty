#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_HOST_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_HOST_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <memory>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

class HostAudio final
{
public:
	enum class Format
	{
		Unknown,
		Signed16bitMono,
		Signed16bitStereo,
		Signed16bit8Ch,
		FloatMono,
		FloatStereo,
		Float8Ch,
		Signed16bit8ChStd,
		Float8ChStd,
	};

	class Id
	{
	public:
		explicit Id(int id): m_id(id - 1) {}
		[[nodiscard]] int  ToInt() const { return m_id + 1; }
		[[nodiscard]] bool IsValid() const { return m_id >= 0; }

		friend class HostAudio;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int audio_id)
		{
			Id r;
			r.m_id = audio_id;
			return r;
		}
		[[nodiscard]] int GetId() const { return m_id; }

		int m_id = -1;
	};

	struct OutputParam
	{
		Id          handle;
		const void* data = nullptr;
	};

	static constexpr int OUT_PORTS_MAX = 32;
	static constexpr int IN_PORTS_MAX  = 8;

	static std::shared_ptr<HostAudio> Create(std::string* error);

	~HostAudio();

	KYTY_CLASS_NO_COPY(HostAudio);

	Id   AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
	bool AudioOutClose(Id handle);
	bool AudioOutValid(Id handle);
	bool AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume);
	bool AudioOutOutputs(const OutputParam* params, uint32_t num, uint32_t* samples_num);
	bool AudioOutGetStatus(Id handle, int* type, int* channels_num);
	void SetHostPaused(bool paused);

	Id       AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioInClose(Id handle);
	bool     AudioInValid(Id handle);
	uint32_t AudioInInput(Id handle, void* dest);

	void Shutdown();

private:
	class Impl;
	explicit HostAudio(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> m_impl;
};

} // namespace Kyty::Libs::Audio

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO_HOST_H_ */
