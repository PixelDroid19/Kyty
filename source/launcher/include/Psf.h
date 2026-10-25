#ifndef LAUNCHER_INCLUDE_PSF_H_
#define LAUNCHER_INCLUDE_PSF_H_

#include "Common.h"

#include <QMap>
#include <QString>
#include <QVariant>

// Reads a param.sfo file. Malformed or truncated files leave the map empty.
class Psf
{
public:
	Psf()          = default;
	virtual ~Psf() = default;

	KYTY_QT_CLASS_NO_COPY(Psf);

	void Load(const QString& file_name);

	[[nodiscard]] const QMap<QString, QVariant>& GetMap() const { return m_map; }

private:
	QMap<QString, QVariant> m_map;
};

// What the library shows about a game, from its param.sfo or param.json. Fields are empty when missing.
struct ParamInfo
{
	QString title;
	QString serial;  /* title ID, e.g. PPSA01234 */
	QString region;  /* first two letters of the content ID: UP, EP, JP, HP, ... */
	QString version; /* application version */
};

ParamInfo ReadParamInfo(const QString& param_file);

#endif /* LAUNCHER_INCLUDE_PSF_H_ */
