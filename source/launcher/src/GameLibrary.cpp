#include "GameLibrary.h"

#include "Configuration.h"
#include "Psf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>

constexpr char LIBRARY_SECTION[] = "ConfigurationList";

namespace {

// Artwork lives in sce_sys next to param.sfo, or in the base directory when no param file is set.
QString ExistingFile(const QString& dir, const char* name)
{
	const auto path = QDir(dir).filePath(name);
	return QFile::exists(path) ? path : QString();
}

} // namespace

GameLibrary::~GameLibrary()
{
	for (const auto& game: m_games)
	{
		delete game.config;
	}
}

GameLibrary::Game GameLibrary::MakeGame(Kyty::Configuration* config)
{
	Game game;
	game.config = config;

	const auto sce_dir = config->param_file.isEmpty() ? QDir(config->basedir).filePath("sce_sys") : QFileInfo(config->param_file).absolutePath();

	game.icon    = ExistingFile(sce_dir, "icon0.png");
	game.picture = ExistingFile(sce_dir, "pic0.png");

	if (!config->param_file.isEmpty())
	{
		game.param = ReadParamInfo(config->param_file);
	}
	return game;
}

void GameLibrary::Read(QSettings* s)
{
	const int size = s->beginReadArray(LIBRARY_SECTION);
	for (int i = 0; i < size; i++)
	{
		s->setArrayIndex(i);
		auto* config = new Kyty::Configuration;
		config->ReadSettings(s);
		m_games.append(MakeGame(config));
	}
	s->endArray();
}

void GameLibrary::Write(QSettings* s) const
{
	s->remove(LIBRARY_SECTION);
	s->beginWriteArray(LIBRARY_SECTION);
	for (int i = 0; i < m_games.size(); i++)
	{
		s->setArrayIndex(i);
		m_games.at(i).config->WriteSettings(s);
	}
	s->endArray();
}

QStringList GameLibrary::Sections() const
{
	QSet<QString> names;
	for (const auto& game: m_games)
	{
		for (const auto& section: game.config->sections)
		{
			names.insert(section);
		}
	}
	QStringList list(names.begin(), names.end());
	list.sort(Qt::CaseInsensitive);
	return list;
}

int GameLibrary::Add(Kyty::Configuration* config)
{
	m_games.append(MakeGame(config));
	return m_games.size() - 1;
}

void GameLibrary::Refresh(int row)
{
	m_games[row] = MakeGame(m_games.at(row).config);
}

void GameLibrary::Remove(int row)
{
	delete m_games.at(row).config;
	m_games.removeAt(row);
}

void GameLibrary::Move(int row, int delta)
{
	const int target = row + delta;
	if (target < 0 || target >= m_games.size())
	{
		return;
	}
	m_games.move(row, target);
}
