#include "Psf.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr uint32_t PSF_MAGIC         = 0x46535000; /* "\0PSF" read as little-endian */
constexpr uint32_t PSF_VERSION       = 0x00000101;
constexpr uint16_t PSF_TYPE_TEXT     = 0x0204;
constexpr uint16_t PSF_TYPE_UINT32   = 0x0404;
constexpr qint64   PSF_MAX_FILE_SIZE = 4 * 1024 * 1024;
constexpr qsizetype PSF_MAX_TEXT_SIZE = 64 * 1024;
constexpr qsizetype PSF_HEADER_SIZE  = 20;
constexpr qsizetype PSF_ENTRY_SIZE   = 16;

// Reads a little-endian integer; fails when it would run past the buffer.
template <class T>
bool ReadLe(const QByteArray& data, qsizetype offset, T* value)
{
	if (offset < 0 || offset + static_cast<qsizetype>(sizeof(T)) > data.size())
	{
		return false;
	}
	T result = 0;
	for (size_t i = 0; i < sizeof(T); i++)
	{
		auto byte = static_cast<T>(static_cast<uint8_t>(data.at(offset + static_cast<qsizetype>(i))));
		result |= static_cast<T>(byte << (8 * i));
	}
	*value = result;
	return true;
}

// Key names live in [begin, end) and are NUL-terminated.
QString ReadKey(const QByteArray& data, qsizetype begin, qsizetype end)
{
	if (begin < 0 || begin >= end || end > data.size())
	{
		return {};
	}
	QByteArray raw = data.mid(begin, end - begin);
	const auto terminator = raw.indexOf('\0');
	if (terminator >= 0)
	{
		raw.truncate(terminator);
	}
	return QString::fromUtf8(raw);
}

} // namespace

void Psf::Load(const QString& file_name)
{
	m_map.clear();

	QFile file(file_name);
	if (!file.open(QIODevice::ReadOnly) || file.size() > PSF_MAX_FILE_SIZE)
	{
		return;
	}
	const QByteArray data = file.readAll();

	uint32_t magic      = 0;
	uint32_t version    = 0;
	uint32_t key_offset = 0;
	uint32_t data_start = 0;
	uint32_t count      = 0;
	if (!ReadLe(data, 0, &magic) || !ReadLe(data, 4, &version) || !ReadLe(data, 8, &key_offset) || !ReadLe(data, 12, &data_start) ||
	    !ReadLe(data, 16, &count))
	{
		return;
	}
	if (magic != PSF_MAGIC || version != PSF_VERSION || key_offset == 0 || data_start <= key_offset || count == 0)
	{
		return;
	}
	if (static_cast<qsizetype>(count) > (data.size() - PSF_HEADER_SIZE) / PSF_ENTRY_SIZE)
	{
		return;
	}

	const auto key_begin  = static_cast<qsizetype>(key_offset);
	const auto data_begin = static_cast<qsizetype>(data_start);

	for (qsizetype i = 0; i < static_cast<qsizetype>(count); i++)
	{
		const qsizetype entry = PSF_HEADER_SIZE + i * PSF_ENTRY_SIZE;

		uint16_t name_offset  = 0;
		uint16_t type         = 0;
		uint32_t size         = 0;
		uint32_t value_offset = 0;
		if (!ReadLe(data, entry, &name_offset) || !ReadLe(data, entry + 2, &type) || !ReadLe(data, entry + 4, &size) ||
		    !ReadLe(data, entry + 12, &value_offset))
		{
			break;
		}

		const QString name = ReadKey(data, key_begin + name_offset, data_begin);
		if (name.isEmpty())
		{
			continue;
		}

		const qsizetype value_pos = data_begin + static_cast<qsizetype>(value_offset);

		if (type == PSF_TYPE_TEXT)
		{
			const auto text_size = static_cast<qsizetype>(size);
			if (text_size > PSF_MAX_TEXT_SIZE || value_pos + text_size > data.size())
			{
				continue;
			}
			QByteArray raw = data.mid(value_pos, text_size);
			const auto terminator = raw.indexOf('\0');
			if (terminator >= 0)
			{
				raw.truncate(terminator);
			}
			m_map.insert(name, QString::fromUtf8(raw));
		} else if (type == PSF_TYPE_UINT32)
		{
			uint32_t value = 0;
			if (ReadLe(data, value_pos, &value))
			{
				m_map.insert(name, value);
			}
		}
	}
}

namespace {

// A content ID starts with the store region, e.g. EP9000-PPSA01234_00-...
QString RegionOf(const QString& content_id)
{
	return content_id.size() > 2 && content_id.at(0).isLetter() && content_id.at(1).isLetter() ? content_id.left(2) : QString();
}

} // namespace

ParamInfo ReadParamInfo(const QString& param_file)
{
	ParamInfo info;
	if (!param_file.endsWith(".json", Qt::CaseInsensitive))
	{
		Psf psf;
		psf.Load(param_file);
		const auto& map = psf.GetMap();
		info.title      = map.value("TITLE").toString();
		info.serial     = map.value("TITLE_ID").toString();
		info.region     = RegionOf(map.value("CONTENT_ID").toString());
		info.version    = map.value("APP_VER").toString();
		return info;
	}

	QFile file(param_file);
	if (!file.open(QIODevice::ReadOnly) || file.size() > PSF_MAX_FILE_SIZE)
	{
		return info;
	}
	const auto json      = QJsonDocument::fromJson(file.readAll()).object();
	const auto localized = json.value("localizedParameters").toObject();
	const auto language  = localized.value("defaultLanguage").toString();
	info.title           = localized.value(language).toObject().value("titleName").toString();
	info.serial          = json.value("titleId").toString();
	info.region          = RegionOf(json.value("contentId").toString());
	info.version         = json.value("contentVersion").toString();
	return info;
}
