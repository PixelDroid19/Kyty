#include "Emulator/Libs/TrophyRules.h"

#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/String.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <set>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Trophies {

namespace {

constexpr uint32_t CONTAINER_MAGIC       = 0xb228c60au;
constexpr size_t   CONTAINER_HEADER_SIZE = 0x60;
constexpr size_t   CONTAINER_ENTRY_SIZE  = 0x40;
constexpr size_t   CONTAINER_NAME_SIZE   = 32;

uint64_t ReadBigEndian(const std::vector<uint8_t>& file, size_t offset, size_t bytes)
{
	uint64_t value = 0;
	for (size_t i = 0; i < bytes; i++)
	{
		value = (value << 8u) | file[offset + i];
	}
	return value;
}

bool ParseInteger(const std::string& text, int64_t* value)
{
	const auto* end    = text.data() + text.size();
	const auto  result = std::from_chars(text.data(), end, *value);
	return !text.empty() && result.ec == std::errc() && result.ptr == end;
}

std::string Utf8(const String& text)
{
	return text.utf8_str().GetData();
}

// The named member, or null when it is missing or JSON null (GetItem returns a shared null node, never nullptr).
const Core::Json* Member(const Core::Json& object, const char* name)
{
	const auto* item = object.GetItem(name);
	return item->IsNull() ? nullptr : item;
}

// A number written either as a JSON number or as a string of digits, as trophy and UDS files mix both.
bool JsonInteger(const Core::Json* item, int64_t* value)
{
	if (item == nullptr)
	{
		return false;
	}
	if (item->IsString())
	{
		return ParseInteger(Utf8(item->ToString()), value);
	}
	return item->IsNumber() && item->TryToInt(value);
}

using JsonDocument = std::unique_ptr<const Core::Json>;

JsonDocument ParseJson(const std::string& text)
{
	JsonDocument document(Core::Json::Create(String::FromUtf8(text.c_str())));
	return document != nullptr && document->IsObject() ? std::move(document) : nullptr;
}

const Core::Json::ListType* ArrayOf(const Core::Json& object, const char* name)
{
	const auto* item = Member(object, name);
	return item != nullptr && item->IsArray() ? &item->ToArray() : nullptr;
}

// "$._trophy_id" names the top-level property _trophy_id; nested paths are not used by trophy rules.
bool TopLevelProperty(const String& path, std::string* name)
{
	const auto text = Utf8(path);
	if (text.size() < 3 || text.compare(0, 2, "$.") != 0 || text.find('.', 2) != std::string::npos)
	{
		return false;
	}
	*name = text.substr(2);
	return true;
}

int64_t SaturatingIncrement(int64_t value)
{
	return value == std::numeric_limits<int64_t>::max() ? value : value + 1;
}

} // namespace

bool ReadContainer(const std::vector<uint8_t>& file, std::map<std::string, std::string>* entries)
{
	if (entries == nullptr || file.size() < CONTAINER_HEADER_SIZE || file.size() > CONTAINER_MAX_SIZE ||
	    ReadBigEndian(file, 0, 4) != CONTAINER_MAGIC || ReadBigEndian(file, 0x14, 4) != CONTAINER_ENTRY_SIZE)
	{
		return false;
	}
	const uint64_t count = ReadBigEndian(file, 0x10, 4);
	if (count > (file.size() - CONTAINER_HEADER_SIZE) / CONTAINER_ENTRY_SIZE)
	{
		return false;
	}
	entries->clear();
	uint64_t json_size = 0;
	for (uint64_t i = 0; i < count; i++)
	{
		const size_t   entry  = CONTAINER_HEADER_SIZE + i * CONTAINER_ENTRY_SIZE;
		const auto*    name   = reinterpret_cast<const char*>(file.data() + entry);
		const uint64_t offset = ReadBigEndian(file, entry + CONTAINER_NAME_SIZE, 8);
		const uint64_t size   = ReadBigEndian(file, entry + CONTAINER_NAME_SIZE + 8, 8);
		if (offset > file.size() || size > file.size() - offset)
		{
			return false;
		}
		const std::string entry_name(name, strnlen(name, CONTAINER_NAME_SIZE));
		if (entry_name.size() < 5 || entry_name.compare(entry_name.size() - 5, 5, ".json") != 0)
		{
			continue;
		}
		// Entries may share one payload, so the bound is on what is copied, not on the file.
		json_size += size;
		if (json_size > CONTAINER_MAX_JSON_SIZE)
		{
			return false;
		}
		(*entries)[entry_name] = std::string(reinterpret_cast<const char*>(file.data() + offset), size);
	}
	return true;
}

bool TrophySet::Load(const std::string& tropconf, const std::string& stats_definition, const std::string& stats_extraction,
                     std::string* error)
{
	const auto  conf        = ParseJson(tropconf);
	const auto  definitions = ParseJson(stats_definition);
	const auto  extraction  = ParseJson(stats_extraction);
	const auto* trophies    = conf != nullptr ? ArrayOf(*conf, "trophies") : nullptr;
	const auto* stats       = definitions != nullptr ? ArrayOf(*definitions, "statDefinitionArray") : nullptr;
	const auto* rules       = extraction != nullptr ? ArrayOf(*extraction, "statsExtractionRuleArray") : nullptr;
	if (trophies == nullptr || stats == nullptr || rules == nullptr)
	{
		*error = "the trophy or UDS definitions are not valid JSON of the expected shape";
		return false;
	}

	m_trophies.clear();
	m_rules.clear();
	m_stats.clear();
	m_unsupported = 0;

	std::set<int> linked;
	for (const auto* item: *trophies)
	{
		Trophy  trophy;
		int64_t id = 0;
		if (!item->IsObject() || !JsonInteger(Member(*item, "id"), &id))
		{
			*error = "a trophy has no id";
			return false;
		}
		trophy.id       = static_cast<int>(id);
		trophy.platinum = item->GetString("grade") == U"P";
		int64_t group   = 0;
		if (JsonInteger(Member(*item, "platinumTrophyId"), &group))
		{
			trophy.group = static_cast<int>(group);
		}
		if (const auto* condition = Member(*item, "unlockCondition"); condition != nullptr && condition->IsObject())
		{
			int64_t stat = 0;
			if (condition->GetString("comparator") == U"ge" && JsonInteger(Member(*condition, "udsStatId"), &stat) &&
			    JsonInteger(Member(*condition, "targetValue"), &trophy.target))
			{
				trophy.stat = static_cast<int>(stat);
				linked.insert(trophy.stat);
			} else
			{
				m_unsupported++;
			}
		}
		m_trophies.push_back(trophy);
	}

	for (const auto* item: *stats)
	{
		int64_t id = 0;
		if (!item->IsObject() || !JsonInteger(Member(*item, "statId"), &id) || linked.count(static_cast<int>(id)) == 0)
		{
			continue;
		}
		Stat       stat;
		const auto aggregation = item->GetString("aggregation");
		if (aggregation == U"count")
		{
			stat.aggregation = Aggregation::Count;
		} else if (aggregation == U"max")
		{
			stat.aggregation = Aggregation::Max;
		} else if (aggregation == U"latest")
		{
			stat.aggregation = Aggregation::Latest;
		} else
		{
			m_unsupported++;
			continue;
		}
		(void)JsonInteger(Member(*item, "initialValue"), &stat.initial);
		m_stats[static_cast<int>(id)] = stat;
	}

	for (const auto* item: *rules)
	{
		const auto* condition = item->IsObject() ? Member(*item, "condition") : nullptr;
		const auto* action    = item->IsObject() ? Member(*item, "action") : nullptr;
		const auto* output    = action != nullptr && action->IsObject() ? Member(*action, "output") : nullptr;
		int64_t     stat      = 0;
		if (condition == nullptr || !condition->IsObject() || output == nullptr || !output->IsObject() ||
		    !JsonInteger(Member(*output, "statId"), &stat) || linked.count(static_cast<int>(stat)) == 0)
		{
			continue;
		}
		Rule rule;
		rule.event     = Utf8(condition->GetString("eventName"));
		rule.stat      = static_cast<int>(stat);
		bool supported = !rule.event.empty() && m_stats.count(rule.stat) != 0 && Member(*action, "convert") == nullptr;
		if (const auto* property = Member(*condition, "property"); supported && property != nullptr)
		{
			const auto compare = property->GetString("comparator");
			rule.compare       = compare == U"==" ? Compare::Equal : (compare == U">" ? Compare::Greater : Compare::None);
			rule.value         = Utf8(property->GetString("value"));
			supported          = rule.compare != Compare::None && TopLevelProperty(property->GetString("path"), &rule.property);
		}
		if (supported && Member(*action, "input") != nullptr)
		{
			supported = TopLevelProperty(action->GetString("input"), &rule.input);
		}
		// A count adds one per event; max and latest need the value they keep.
		supported = supported && (m_stats[rule.stat].aggregation == Aggregation::Count) == rule.input.empty();
		if (!supported)
		{
			m_unsupported++;
			continue;
		}
		m_rules.push_back(rule);
	}
	return true;
}

bool TrophySet::Matches(const Rule& rule, const EventValues& values) const
{
	if (rule.compare == Compare::None)
	{
		return true;
	}
	const auto found = values.find(rule.property);
	if (found == values.end())
	{
		return false;
	}
	const auto& value = found->second;
	if (value.is_text)
	{
		return rule.compare == Compare::Equal && value.text == rule.value;
	}
	int64_t expected = 0;
	if (!ParseInteger(rule.value, &expected))
	{
		return false;
	}
	return rule.compare == Compare::Equal ? value.number == expected : value.number > expected;
}

int64_t TrophySet::StatValue(int stat, const Progress& progress) const
{
	const auto found = progress.stats.find(stat);
	return found != progress.stats.end() ? found->second : m_stats.at(stat).initial;
}

std::vector<int> TrophySet::Post(const std::string& event, const EventValues& values, int64_t now, Progress* progress) const
{
	std::vector<int> unlocked;
	bool             changed = false;
	for (const auto& rule: m_rules)
	{
		if (rule.event != event || !Matches(rule, values))
		{
			continue;
		}
		const auto    aggregation = m_stats.at(rule.stat).aggregation;
		const int64_t current     = StatValue(rule.stat, *progress);
		int64_t       next        = SaturatingIncrement(current);
		if (aggregation != Aggregation::Count)
		{
			const auto input = values.find(rule.input);
			if (input == values.end() || input->second.is_text)
			{
				continue;
			}
			next = aggregation == Aggregation::Max ? std::max(current, input->second.number) : input->second.number;
		}
		progress->stats[rule.stat] = next;
		changed                    = true;
	}
	if (!changed)
	{
		return unlocked;
	}

	for (const auto& trophy: m_trophies)
	{
		if (m_stats.count(trophy.stat) != 0 && progress->unlocked.count(trophy.id) == 0 &&
		    StatValue(trophy.stat, *progress) >= trophy.target)
		{
			progress->unlocked[trophy.id] = now;
			unlocked.push_back(trophy.id);
		}
	}
	// The platinum comes once every trophy of its set is unlocked.
	for (const auto& platinum: m_trophies)
	{
		if (!platinum.platinum || progress->unlocked.count(platinum.id) != 0)
		{
			continue;
		}
		const auto in_set  = [&](const Trophy& trophy) { return trophy.group == platinum.id; };
		const auto earned  = [&](const Trophy& trophy) { return !in_set(trophy) || progress->unlocked.count(trophy.id) != 0; };
		const bool has_set = std::any_of(m_trophies.begin(), m_trophies.end(), in_set);
		if (has_set && std::all_of(m_trophies.begin(), m_trophies.end(), earned))
		{
			progress->unlocked[platinum.id] = now;
			unlocked.push_back(platinum.id);
		}
	}
	return unlocked;
}

std::string ProgressToJson(const Progress& progress)
{
	const auto members = [](const std::map<int, int64_t>& values)
	{
		std::string text;
		for (const auto& [id, value]: values)
		{
			text += (text.empty() ? "\"" : ",\"") + std::to_string(id) + "\":" + std::to_string(value);
		}
		return text;
	};
	return "{\"stats\":{" + members(progress.stats) + "},\"unlocked\":{" + members(progress.unlocked) + "}}\n";
}

bool ProgressFromJson(const std::string& json, Progress* progress)
{
	const auto document = ParseJson(json);
	if (document == nullptr)
	{
		return false;
	}
	Progress   read;
	const auto members = [](const Core::Json* object, std::map<int, int64_t>* values)
	{
		if (object == nullptr || !object->IsObject())
		{
			return false;
		}
		for (const auto* item: object->ToArray())
		{
			int64_t id    = 0;
			int64_t value = 0;
			if (!ParseInteger(Utf8(item->GetName()), &id) || !JsonInteger(item, &value) || id < 0 || id > std::numeric_limits<int>::max())
			{
				return false;
			}
			(*values)[static_cast<int>(id)] = value;
		}
		return true;
	};
	if (!members(Member(*document, "stats"), &read.stats) || !members(Member(*document, "unlocked"), &read.unlocked))
	{
		return false;
	}
	*progress = std::move(read);
	return true;
}

} // namespace Kyty::Libs::Trophies

#endif // KYTY_EMU_ENABLED
