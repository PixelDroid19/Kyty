#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRULES_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRULES_H_

#include "Emulator/Common.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Trophies {

// One property a title set on a posted UDS event: integers of every width and bools as numbers, strings as text.
struct EventValue
{
	bool        is_text = false;
	int64_t     number  = 0;
	std::string text;
};
using EventValues = std::unordered_map<std::string, EventValue>;

// What one user has earned in one title: UDS stat values by stat id, unlock times (Unix seconds) by trophy id.
struct Progress
{
	std::map<int, int64_t> stats;
	std::map<int, int64_t> unlocked;
};

// Real containers stay far below these: 29 MB for the largest file and 450 KB of JSON for the largest set checked.
constexpr size_t CONTAINER_MAX_SIZE      = 256u << 20u;
constexpr size_t CONTAINER_MAX_JSON_SIZE = 16u << 20u;

// The JSON files of a PS5 .ucp container (trophy00.ucp, uds00.ucp) by name; images and other entries are skipped.
// False when the container is malformed, larger than CONTAINER_MAX_SIZE, or holds more than CONTAINER_MAX_JSON_SIZE
// of JSON in total.
bool ReadContainer(const std::vector<uint8_t>& file, std::map<std::string, std::string>* entries);

// A title's trophies and the UDS rules that unlock them: tropconf.json from the trophy container and
// stats_definition.json and stats_extraction.json from the UDS container. Only rules that feed a stat a trophy
// waits for are kept.
class TrophySet
{
public:
	bool Load(const std::string& tropconf, const std::string& stats_definition, const std::string& stats_extraction, std::string* error);

	// Applies one posted event to the stats and returns the trophies it unlocked, platinum included.
	std::vector<int> Post(const std::string& event, const EventValues& values, int64_t now, Progress* progress) const;

	[[nodiscard]] int Unsupported() const { return m_unsupported; }
	[[nodiscard]] int Count() const { return static_cast<int>(m_trophies.size()); }

private:
	enum class Compare
	{
		None,
		Equal,
		Greater
	};
	enum class Aggregation
	{
		Count,
		Max,
		Latest
	};
	struct Rule
	{
		std::string event;
		std::string property;
		Compare     compare = Compare::None;
		std::string value;
		std::string input;
		int         stat = 0;
	};
	struct Stat
	{
		Aggregation aggregation = Aggregation::Count;
		int64_t     initial     = 0;
	};
	struct Trophy
	{
		int     id       = 0;
		bool    platinum = false;
		int     group    = -1;
		int     stat     = -1;
		int64_t target   = 0;
	};

	[[nodiscard]] bool    Matches(const Rule& rule, const EventValues& values) const;
	[[nodiscard]] int64_t StatValue(int stat, const Progress& progress) const;

	std::vector<Trophy> m_trophies;
	std::vector<Rule>   m_rules;
	std::map<int, Stat> m_stats;
	int                 m_unsupported = 0;
};

// The progress file: {"stats":{"<id>":<value>},"unlocked":{"<id>":<time>}}.
std::string ProgressToJson(const Progress& progress);
bool        ProgressFromJson(const std::string& json, Progress* progress);

} // namespace Kyty::Libs::Trophies

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_TROPHYRULES_H_ */
