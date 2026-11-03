#include "Kyty/UnitTest.h"

#include "Emulator/Libs/TrophyRules.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

UT_BEGIN(EmulatorTrophies);

using Libs::Trophies::EventValue;
using Libs::Trophies::EventValues;
using Libs::Trophies::Progress;
using Libs::Trophies::TrophySet;

namespace {

void PutBigEndian(std::vector<uint8_t>* file, size_t offset, uint64_t value, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++)
	{
		(*file)[offset + i] = static_cast<uint8_t>(value >> (8u * (bytes - 1 - i)));
	}
}

// A container laid out as the title's .ucp files: big-endian header, 0x40-byte entries from 0x60, then the data.
std::vector<uint8_t> BuildContainer(const std::map<std::string, std::string>& files)
{
	const size_t         data_start = 0x60 + files.size() * 0x40;
	std::vector<uint8_t> file(data_start);
	PutBigEndian(&file, 0x00, 0xb228c60au, 4);
	PutBigEndian(&file, 0x04, 1, 4);
	PutBigEndian(&file, 0x10, files.size(), 4);
	PutBigEndian(&file, 0x14, 0x40, 4);
	size_t entry = 0x60;
	for (const auto& [name, data]: files)
	{
		std::copy(name.begin(), name.end(), file.begin() + static_cast<std::ptrdiff_t>(entry));
		PutBigEndian(&file, entry + 32, file.size(), 8);
		PutBigEndian(&file, entry + 40, data.size(), 8);
		file.insert(file.end(), data.begin(), data.end());
		entry += 0x40;
	}
	PutBigEndian(&file, 0x08, file.size(), 8);
	return file;
}

// Trophy 1 counts UnlockTrophy events for it, trophy 2 waits for a progress value of 5, trophy 3 for a level above 10,
// trophy 4 is unlocked by the server and so never here; trophy 0 is the platinum of 1, 2 and 3.
const char* const TROPCONF = R"({"trophies":[
	{"id":"000","grade":"P"},
	{"id":"001","grade":"B","platinumTrophyId":"0000","unlockCondition":{"comparator":"ge","udsStatId":"10","targetValue":"1"}},
	{"id":"002","grade":"S","platinumTrophyId":"0000","unlockCondition":{"comparator":"ge","udsStatId":"20","targetValue":"5"}},
	{"id":"003","grade":"G","platinumTrophyId":"0000","unlockCondition":{"comparator":"ge","udsStatId":"30","targetValue":"3"}}
]})";

const char* const STATS = R"({"statDefinitionArray":[
	{"statId":"10","aggregation":"count","initialValue":"0"},
	{"statId":"20","aggregation":"max","initialValue":"0"},
	{"statId":"30","aggregation":"latest","initialValue":"0"},
	{"statId":"99","aggregation":"count","initialValue":"0"}
]})";

const char* const RULES = R"({"statsExtractionRuleArray":[
	{"condition":{"eventName":"_UnlockTrophy","property":{"path":"$._trophy_id","comparator":"==","value":"1"}},
	 "action":{"output":{"statId":"10"}}},
	{"condition":{"eventName":"_UpdateTrophyProgress","property":{"path":"$._trophy_id","comparator":"==","value":"2"}},
	 "action":{"input":"$._trophy_progress","output":{"statId":"20"}}},
	{"condition":{"eventName":"COUNT__level","property":{"path":"$.level","comparator":">","value":"10"}},
	 "action":{"input":"$.counter","output":{"statId":"30"}}},
	{"condition":{"eventName":"Nested","property":{"path":"$.a.b","comparator":"==","value":"1"}},
	 "action":{"output":{"statId":"10"}}},
	{"condition":{"eventName":"Unlinked"},"action":{"output":{"statId":"99"}}}
]})";

EventValues Values(std::initializer_list<std::pair<const std::string, EventValue>> values)
{
	return EventValues(values);
}

EventValue Number(int64_t number)
{
	EventValue value;
	value.number = number;
	return value;
}

EventValue Text(const char* text)
{
	EventValue value;
	value.is_text = true;
	value.text    = text;
	return value;
}

TrophySet LoadSet()
{
	TrophySet   set;
	std::string error;
	EXPECT_TRUE(set.Load(TROPCONF, STATS, RULES, &error)) << error;
	return set;
}

} // namespace

TEST(EmulatorTrophies, ReadsContainerEntries)
{
	const std::map<std::string, std::string> files = {{"tropconf.json", "{}"}, {"icon.png", std::string("\x89PNG\0x", 6)}};
	std::map<std::string, std::string>       entries;
	ASSERT_TRUE(Libs::Trophies::ReadContainer(BuildContainer(files), &entries));
	EXPECT_EQ(entries, (std::map<std::string, std::string> {{"tropconf.json", "{}"}}));
}

TEST(EmulatorTrophies, RejectsMalformedContainers)
{
	std::map<std::string, std::string> entries;
	auto                               bad_magic = BuildContainer({{"tropconf.json", "{}"}});
	bad_magic[0] ^= 0xffu;
	EXPECT_FALSE(Libs::Trophies::ReadContainer(bad_magic, &entries));

	auto past_end = BuildContainer({{"tropconf.json", "{}"}});
	PutBigEndian(&past_end, 0x60 + 40, past_end.size(), 8);
	EXPECT_FALSE(Libs::Trophies::ReadContainer(past_end, &entries));

	auto too_many = BuildContainer({{"tropconf.json", "{}"}});
	PutBigEndian(&too_many, 0x10, 1000, 4);
	EXPECT_FALSE(Libs::Trophies::ReadContainer(too_many, &entries));

	EXPECT_FALSE(Libs::Trophies::ReadContainer(std::vector<uint8_t>(0x20), &entries));

	// Entries that all point at one payload add up past the JSON bound although the file is small.
	const std::string payload(Libs::Trophies::CONTAINER_MAX_JSON_SIZE / 4 + 1, ' ');
	auto              shared = BuildContainer({{"a.json", payload}, {"b.json", "{}"}, {"c.json", "{}"}, {"d.json", "{}"}});
	const auto        first  = 0x60 + 32;
	for (size_t entry = 1; entry < 4; entry++)
	{
		for (size_t byte = 0; byte < 16; byte++)
		{
			shared[first + entry * 0x40 + byte] = shared[first + byte];
		}
	}
	EXPECT_LT(shared.size(), Libs::Trophies::CONTAINER_MAX_JSON_SIZE);
	EXPECT_FALSE(Libs::Trophies::ReadContainer(shared, &entries));
}

TEST(EmulatorTrophies, CountsUnreadableRules)
{
	const auto set = LoadSet();
	EXPECT_EQ(set.Count(), 4);
	// The nested path; the rule on the stat no trophy waits for is dropped, not counted.
	EXPECT_EQ(set.Unsupported(), 1);
}

TEST(EmulatorTrophies, UnlocksOnMatchingEvents)
{
	const auto set = LoadSet();
	Progress   progress;

	EXPECT_TRUE(set.Post("_UnlockTrophy", Values({{"_trophy_id", Text("2")}}), 100, &progress).empty());
	EXPECT_TRUE(progress.stats.empty());

	EXPECT_EQ(set.Post("_UnlockTrophy", Values({{"_trophy_id", Text("1")}}), 100, &progress), std::vector<int>({1}));
	EXPECT_EQ(progress.unlocked.at(1), 100);
	// An unlocked trophy is never reported twice.
	EXPECT_TRUE(set.Post("_UnlockTrophy", Values({{"_trophy_id", Text("1")}}), 200, &progress).empty());
	EXPECT_EQ(progress.stats.at(10), 2);
	EXPECT_EQ(progress.unlocked.at(1), 100);
}

TEST(EmulatorTrophies, KeepsMaxAndLatestValues)
{
	const auto set = LoadSet();
	Progress   progress;

	const auto progress_of = [](int64_t value) { return Values({{"_trophy_id", Text("2")}, {"_trophy_progress", Number(value)}}); };
	EXPECT_TRUE(set.Post("_UpdateTrophyProgress", progress_of(4), 1, &progress).empty());
	EXPECT_TRUE(set.Post("_UpdateTrophyProgress", progress_of(2), 1, &progress).empty());
	EXPECT_EQ(progress.stats.at(20), 4);
	EXPECT_EQ(set.Post("_UpdateTrophyProgress", progress_of(5), 2, &progress), std::vector<int>({2}));

	const auto level = [](int64_t level, int64_t counter) { return Values({{"level", Number(level)}, {"counter", Number(counter)}}); };
	EXPECT_TRUE(set.Post("COUNT__level", level(10, 7), 3, &progress).empty());
	EXPECT_EQ(progress.stats.count(30), 0u);
	EXPECT_TRUE(set.Post("COUNT__level", level(11, 2), 3, &progress).empty());
	// Latest replaces the value, so a reset lowers it.
	EXPECT_TRUE(set.Post("COUNT__level", level(11, 0), 3, &progress).empty());
	EXPECT_EQ(progress.stats.at(30), 0);
	EXPECT_EQ(set.Post("COUNT__level", level(12, 3), 4, &progress), std::vector<int>({3}));
}

TEST(EmulatorTrophies, UnlocksPlatinumAfterItsSet)
{
	const auto set = LoadSet();
	Progress   progress;
	progress.stats    = {{20, 5}, {30, 3}};
	progress.unlocked = {{2, 1}, {3, 1}};

	EXPECT_EQ(set.Post("_UnlockTrophy", Values({{"_trophy_id", Text("1")}}), 9, &progress), std::vector<int>({1, 0}));
	EXPECT_EQ(progress.unlocked.at(0), 9);
}

TEST(EmulatorTrophies, RoundTripsProgress)
{
	Progress progress;
	progress.stats    = {{10, 3}, {20, -1}};
	progress.unlocked = {{1, 1762128000}};

	Progress read;
	ASSERT_TRUE(Libs::Trophies::ProgressFromJson(Libs::Trophies::ProgressToJson(progress), &read));
	EXPECT_EQ(read.stats, progress.stats);
	EXPECT_EQ(read.unlocked, progress.unlocked);

	Progress empty;
	ASSERT_TRUE(Libs::Trophies::ProgressFromJson(Libs::Trophies::ProgressToJson(empty), &read));
	EXPECT_TRUE(read.stats.empty() && read.unlocked.empty());

	EXPECT_FALSE(Libs::Trophies::ProgressFromJson(R"({"stats":{"x":1},"unlocked":{}})", &read));
	EXPECT_FALSE(Libs::Trophies::ProgressFromJson(R"({"stats":{}})", &read));
	EXPECT_FALSE(Libs::Trophies::ProgressFromJson("not json", &read));
}

UT_END();
