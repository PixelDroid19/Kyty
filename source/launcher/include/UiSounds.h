#ifndef LAUNCHER_INCLUDE_UI_SOUNDS_H_
#define LAUNCHER_INCLUDE_UI_SOUNDS_H_

#include "Common.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

struct SDL_AudioStream;

// The interface's short sounds: moving the focus, accepting, going back and starting a game. The launcher's own
// sounds are synthesised; a theme may replace any of them with a WAV file. Plays through SDL 3 audio, and closes
// the audio device a few seconds after the last sound, so it holds nothing while a game runs.
class UiSounds: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(UiSounds);

public:
	enum class Sound
	{
		Move,
		Accept,
		Back,
		Start,
		Count
	};

	explicit UiSounds(QObject* parent = nullptr);
	~UiSounds() override;

	// Theme files by sound name (move, accept, back, start); the others keep the launcher's own sound. Returns
	// why the first file that cannot be read failed, and then keeps every own sound.
	QString SetFiles(const QHash<QString, QString>& files);
	// 0 mutes.
	void SetGain(float gain);
	void Play(Sound sound);

private:
	bool Open();
	void Close();
	void CloseWhenDrained();

	QByteArray       m_clips[static_cast<int>(Sound::Count)];
	SDL_AudioStream* m_stream = nullptr;
	float            m_gain   = 0;
	QTimer           m_idle;
};

#endif /* LAUNCHER_INCLUDE_UI_SOUNDS_H_ */
