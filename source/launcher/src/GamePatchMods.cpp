#include "GamePatchMods.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace {

// The mods array of a patch file whose mods are all objects.
bool ReadMods(const QString& file, QJsonObject* root, QString* error)
{
	QFile in(file);
	if (!in.open(QIODevice::ReadOnly))
	{
		*error = QCoreApplication::translate("GamePatchMods", "The game patch file cannot be read.");
		return false;
	}
	QJsonParseError parse {};
	const auto      document = QJsonDocument::fromJson(in.readAll(), &parse);
	*root                    = document.object();
	if (parse.error != QJsonParseError::NoError || !document.isObject() || !root->value("mods").isArray())
	{
		*error = QCoreApplication::translate("GamePatchMods", "The game patch file is not a patch file.");
		return false;
	}
	const auto mods = root->value("mods").toArray();
	if (!std::all_of(mods.begin(), mods.end(), [](const QJsonValue& mod) { return mod.isObject(); }))
	{
		*error = QCoreApplication::translate("GamePatchMods", "The game patch file is not a patch file.");
		return false;
	}
	return true;
}

} // namespace

QVariantList ReadPatchMods(const QString& file)
{
	QJsonObject root;
	QString     error;
	if (file.isEmpty() || !ReadMods(file, &root, &error))
	{
		return {};
	}
	QVariantList entries;
	for (const auto& value: root.value("mods").toArray())
	{
		const auto mod = value.toObject();
		// The emulator applies a mod that has no enabled flag.
		entries << QVariantMap {{"path", mod.value("name").toString()}, {"present", true}, {"selected", mod.value("enabled").toBool(true)}};
	}
	return entries;
}

QString WritePatchMods(const QString& file, const QVariantList& mods)
{
	QJsonObject root;
	QString     error;
	if (!ReadMods(file, &root, &error))
	{
		return error;
	}
	auto list = root.value("mods").toArray();
	if (list.size() != mods.size())
	{
		return QCoreApplication::translate("GamePatchMods", "The game patch file changed after its mods were shown. Reload the mods and save again.");
	}
	bool changed = false;
	for (qsizetype i = 0; i < list.size(); i++)
	{
		auto       mod   = list.at(i).toObject();
		const auto entry = mods.at(i).toMap();
		if (mod.value("name").toString() != entry.value("path").toString())
		{
			return QCoreApplication::translate("GamePatchMods", "The game patch file changed after its mods were shown. Reload the mods and save again.");
		}
		const bool enabled = entry.value("selected").toBool();
		if (mod.value("enabled").toBool(true) != enabled)
		{
			mod["enabled"] = enabled;
			list[i]        = mod;
			changed        = true;
		}
	}
	if (!changed)
	{
		return {};
	}
	root["mods"] = list;
	QSaveFile out(file);
	if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(root).toJson()) < 0 || !out.commit())
	{
		return QCoreApplication::translate("GamePatchMods", "The game patch file cannot be written.");
	}
	return {};
}
