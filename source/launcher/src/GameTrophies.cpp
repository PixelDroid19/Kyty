#include "GameTrophies.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QtEndian>

namespace {

constexpr quint32 CONTAINER_MAGIC = 0xb228c60au;
constexpr qint64  HEADER_SIZE     = 0x60;
constexpr qint64  ENTRY_SIZE      = 0x40;
constexpr int     ENTRY_NAME_SIZE = 32;

// The files of a .ucp container by name: a big-endian header with the entry count at 0x10, then 0x40-byte entries of
// a name and a 64-bit offset and size. Empty when the container is malformed.
QMap<QString, QByteArray> ReadContainer(const QString& file)
{
	QFile in(file);
	if (!in.open(QIODevice::ReadOnly))
	{
		return {};
	}
	const auto  bytes = in.readAll();
	const auto* data  = reinterpret_cast<const uchar*>(bytes.constData());
	if (bytes.size() < HEADER_SIZE || qFromBigEndian<quint32>(data) != CONTAINER_MAGIC ||
	    qFromBigEndian<quint32>(data + 0x14) != ENTRY_SIZE)
	{
		return {};
	}
	const qint64 count = qFromBigEndian<quint32>(data + 0x10);
	if (count > (bytes.size() - HEADER_SIZE) / ENTRY_SIZE)
	{
		return {};
	}
	QMap<QString, QByteArray> entries;
	for (qint64 i = 0; i < count; i++)
	{
		const auto* entry  = data + HEADER_SIZE + i * ENTRY_SIZE;
		const auto  offset = qFromBigEndian<quint64>(entry + ENTRY_NAME_SIZE);
		const auto  size   = qFromBigEndian<quint64>(entry + ENTRY_NAME_SIZE + 8);
		if (offset > static_cast<quint64>(bytes.size()) || size > static_cast<quint64>(bytes.size()) - offset)
		{
			return {};
		}
		const auto name = QString::fromLatin1(reinterpret_cast<const char*>(entry),
		                                      static_cast<int>(qstrnlen(reinterpret_cast<const char*>(entry), ENTRY_NAME_SIZE)));
		entries[name]   = bytes.mid(static_cast<qsizetype>(offset), static_cast<qsizetype>(size));
	}
	return entries;
}

QJsonObject ParseObject(const QByteArray& json)
{
	return QJsonDocument::fromJson(json).object();
}

// Trophy and UDS files write numbers both as JSON numbers and as strings of digits.
bool ToInteger(const QJsonValue& value, qint64* number)
{
	if (value.isDouble())
	{
		*number = value.toInteger();
		return true;
	}
	bool ok = false;
	*number = value.toString().toLongLong(&ok);
	return ok;
}

// The set's language for locale: the same language and region, else the same language, else the set's default.
QString PickLanguage(const QJsonObject& conf, const QLocale& locale)
{
	const auto languages = conf.value("languages").toArray();
	const auto wanted    = locale.bcp47Name();
	const auto primary   = wanted.section('-', 0, 0);
	QString    same_language;
	for (const auto& value: languages)
	{
		const auto language = value.toString();
		if (language.compare(wanted, Qt::CaseInsensitive) == 0)
		{
			return language;
		}
		if (same_language.isEmpty() && language.section('-', 0, 0).compare(primary, Qt::CaseInsensitive) == 0)
		{
			same_language = language;
		}
	}
	return same_language.isEmpty() ? conf.value("defaultLanguage").toString() : same_language;
}

QMap<qint64, QJsonObject> ReadMetadata(const QMap<QString, QByteArray>& files, const QString& language)
{
	QMap<qint64, QJsonObject> metadata;
	const auto                list =
	    ParseObject(files.value("tropmeta_" + language + ".json")).value("metadata").toObject().value("trophyMetadata").toArray();
	for (const auto& value: list)
	{
		const auto item = value.toObject();
		qint64     id   = 0;
		if (ToInteger(item.value("id"), &id))
		{
			metadata[id] = item;
		}
	}
	return metadata;
}

} // namespace

QVector<GameTrophy> ReadGameTrophies(const QString& app_dir, const QString& progress_file, const QLocale& locale)
{
	const auto files = ReadContainer(QDir(app_dir).filePath("sce_sys/trophy2/trophy00.ucp"));
	const auto conf  = ParseObject(files.value("tropconf.json"));
	auto       names = ReadMetadata(files, PickLanguage(conf, locale));
	if (names.isEmpty())
	{
		names = ReadMetadata(files, conf.value("defaultLanguage").toString());
	}

	QJsonObject progress;
	if (QFile in(progress_file); !progress_file.isEmpty() && in.open(QIODevice::ReadOnly))
	{
		progress = ParseObject(in.readAll());
	}
	const auto stats    = progress.value("stats").toObject();
	const auto unlocked = progress.value("unlocked").toObject();

	QVector<GameTrophy> trophies;
	for (const auto& value: conf.value("trophies").toArray())
	{
		const auto item = value.toObject();
		qint64     id   = 0;
		if (!ToInteger(item.value("id"), &id))
		{
			continue;
		}
		const auto key = QString::number(id);
		GameTrophy trophy;
		const auto grade = item.value("grade").toString();
		trophy.grade     = grade.isEmpty() ? QChar() : grade.front();
		trophy.name      = names.value(id).value("name").toString();
		trophy.detail    = names.value(id).value("detail").toString();
		trophy.hidden    = item.value("hidden").toBool();
		(void)ToInteger(unlocked.value(key), &trophy.unlocked);
		const auto condition = item.value("unlockCondition").toObject();
		qint64     stat      = 0;
		if (condition.value("progressive").toBool() && ToInteger(condition.value("udsStatId"), &stat) &&
		    ToInteger(condition.value("targetValue"), &trophy.target))
		{
			qint64 recorded = 0;
			if (ToInteger(stats.value(QString::number(stat)), &recorded))
			{
				trophy.progress = recorded;
			}
		}
		trophies << trophy;
	}
	return trophies;
}

QString TrophyProgressFile(const QString& save_root, const QString& title_id)
{
	if (save_root.isEmpty() || !QDir::isAbsolutePath(save_root))
	{
		return {};
	}
	// The emulator's save-data folder name for a title id.
	QString folder;
	for (const QChar c: title_id.left(64))
	{
		const bool ascii_alnum = c.unicode() < 0x80 && c.isLetterOrNumber();
		folder += ascii_alnum ? c.toUpper() : (c == '-' || c == '_' ? c : QChar('_'));
	}
	if (folder.isEmpty())
	{
		folder = QStringLiteral("UNKNOWN");
	}
	return QDir(save_root).filePath(".kyty-trophies/" + folder + "/user-1.json");
}
