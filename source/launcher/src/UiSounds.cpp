#include "UiSounds.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

constexpr int SAMPLE_RATE = 48000;
// How long the device stays open after the last sound, so a quick run of moves does not reopen it each time.
constexpr int IDLE_CLOSE_MS = 3000;
// Built-in sounds stay well under full scale, so the volume setting has room above them.
constexpr float TONE_LEVEL = 0.22F;
constexpr float ATTACK_S   = 0.003F;
constexpr float TWO_PI     = 6.28318531F;

namespace {

const SDL_AudioSpec CLIP_SPEC {SDL_AUDIO_F32, 1, SAMPLE_RATE};

const char* const SOUND_NAMES[] = {"move", "accept", "back", "start"};

struct Note
{
	float frequency;
	float start_s;
	float length_s;
};

// A sum of short sine notes, each with a click-free attack and an exponential fade.
QByteArray Tones(std::initializer_list<Note> notes)
{
	float end_s = 0;
	for (const auto& note: notes)
	{
		end_s = std::max(end_s, note.start_s + note.length_s);
	}
	const int  count = static_cast<int>(std::ceil(end_s * SAMPLE_RATE));
	QByteArray clip(static_cast<qsizetype>(count * sizeof(float)), '\0');
	auto*      samples = reinterpret_cast<float*>(clip.data());
	for (const auto& note: notes)
	{
		const int first  = static_cast<int>(note.start_s * SAMPLE_RATE);
		const int length = static_cast<int>(note.length_s * SAMPLE_RATE);
		for (int i = 0; i < length && first + i < count; i++)
		{
			const float t        = static_cast<float>(i) / SAMPLE_RATE;
			const float envelope = std::min(1.0F, t / ATTACK_S) * std::exp(-5.0F * t / note.length_s);
			samples[first + i] += TONE_LEVEL * envelope * std::sin(TWO_PI * note.frequency * t);
		}
	}
	return clip;
}

QByteArray OwnSound(UiSounds::Sound sound)
{
	switch (sound)
	{
		case UiSounds::Sound::Move: return Tones({{1480, 0, 0.035F}});
		case UiSounds::Sound::Accept: return Tones({{660, 0, 0.07F}, {990, 0.055F, 0.11F}});
		case UiSounds::Sound::Back: return Tones({{740, 0, 0.07F}, {494, 0.055F, 0.11F}});
		case UiSounds::Sound::Start: return Tones({{523, 0, 0.3F}, {659, 0.09F, 0.3F}, {784, 0.18F, 0.3F}, {1047, 0.27F, 0.5F}});
		case UiSounds::Sound::Count: break;
	}
	return {};
}

// The WAV file in the clips' format, or an empty array with error set.
QByteArray ReadWav(const QString& path, QString* error)
{
	SDL_AudioSpec spec {};
	Uint8*        data   = nullptr;
	Uint32        length = 0;
	if (!SDL_LoadWAV(path.toUtf8().constData(), &spec, &data, &length))
	{
		*error = QString::fromUtf8(SDL_GetError());
		return {};
	}
	Uint8* converted      = nullptr;
	int    converted_size = 0;
	const bool ok         = SDL_ConvertAudioSamples(&spec, data, static_cast<int>(length), &CLIP_SPEC, &converted, &converted_size);
	SDL_free(data);
	if (!ok)
	{
		*error = QString::fromUtf8(SDL_GetError());
		return {};
	}
	QByteArray clip(reinterpret_cast<const char*>(converted), converted_size);
	SDL_free(converted);
	return clip;
}

} // namespace

UiSounds::UiSounds(QObject* parent): QObject(parent)
{
	SetFiles({});
	m_idle.setSingleShot(true);
	connect(&m_idle, &QTimer::timeout, this, &UiSounds::CloseWhenDrained);
}

UiSounds::~UiSounds()
{
	Close();
}

QString UiSounds::SetFiles(const QHash<QString, QString>& files)
{
	for (int i = 0; i < static_cast<int>(Sound::Count); i++)
	{
		m_clips[i] = OwnSound(static_cast<Sound>(i));
	}
	for (int i = 0; i < static_cast<int>(Sound::Count); i++)
	{
		const auto path = files.value(QString::fromLatin1(SOUND_NAMES[i]));
		if (path.isEmpty())
		{
			continue;
		}
		QString error;
		auto    clip = ReadWav(path, &error);
		if (clip.isEmpty())
		{
			SetFiles({});
			return error.isEmpty() ? tr("%1 holds no sound.").arg(path) : error;
		}
		m_clips[i] = std::move(clip);
	}
	return {};
}

void UiSounds::SetGain(float gain)
{
	m_gain = gain;
	if (m_stream != nullptr)
	{
		SDL_SetAudioStreamGain(m_stream, m_gain);
	}
}

void UiSounds::Play(Sound sound)
{
	const auto& clip = m_clips[static_cast<int>(sound)];
	if (m_gain <= 0 || clip.isEmpty() || !Open())
	{
		return;
	}
	// A new sound cuts the one still playing, so quick moves never fall behind the focus.
	SDL_ClearAudioStream(m_stream);
	SDL_PutAudioStreamData(m_stream, clip.constData(), static_cast<int>(clip.size()));
	const auto clip_ms = clip.size() * 1000 / static_cast<qsizetype>(sizeof(float) * SAMPLE_RATE);
	m_idle.start(static_cast<int>(clip_ms) + IDLE_CLOSE_MS);
}

bool UiSounds::Open()
{
	if (m_stream != nullptr)
	{
		return true;
	}
	// A missing or busy audio device leaves the launcher silent; nothing else depends on it.
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		return false;
	}
	m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &CLIP_SPEC, nullptr, nullptr);
	if (m_stream == nullptr)
	{
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}
	SDL_SetAudioStreamGain(m_stream, m_gain);
	SDL_ResumeAudioStreamDevice(m_stream);
	return true;
}

void UiSounds::Close()
{
	m_idle.stop();
	if (m_stream == nullptr)
	{
		return;
	}
	SDL_DestroyAudioStream(m_stream);
	m_stream = nullptr;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void UiSounds::CloseWhenDrained()
{
	if (m_stream != nullptr && SDL_GetAudioStreamQueued(m_stream) > 0)
	{
		m_idle.start(IDLE_CLOSE_MS);
		return;
	}
	Close();
}
