#include "Kyty/UnitTest.h"

#include <string>
#include <vector>

UT_BEGIN(CoreSubsystems);

namespace {

class ProbeSubsystem final : public Core::Subsystem
{
public:
	ProbeSubsystem(const char* id, std::vector<std::string>* events): m_id(id), m_events(events) {}

	const char* Id() override { return m_id; }

	void Init(Core::SubsystemsList* /*parent*/) override { m_events->emplace_back("init:").append(m_id); }

	void Destroy(Core::SubsystemsList* /*parent*/) override { m_events->emplace_back("destroy:").append(m_id); }

	void UnexpectedShutdown(Core::SubsystemsList* /*parent*/) override
	{
		m_events->emplace_back("shutdown:").append(m_id);
	}

private:
	const char*             m_id;
	std::vector<std::string>* m_events;
};

} // namespace

TEST(CoreSubsystems, RejectsMissingDependencies)
{
	std::vector<std::string> events;
	ProbeSubsystem missing("missing", &events);
	ProbeSubsystem consumer("consumer", &events);
	Core::SubsystemsList list;

	list.Add(&consumer, {&missing});

	EXPECT_FALSE(list.InitAll());
	EXPECT_TRUE(events.empty());
	EXPECT_STREQ(list.GetFailName(), "consumer");
	EXPECT_NE(std::string(list.GetFailMsg()).find("missing dependency 'missing'"), std::string::npos);

	list.Add(&missing, {});
	EXPECT_TRUE(list.InitAll());
	EXPECT_EQ(events, (std::vector<std::string> {"init:missing", "init:consumer"}));
	EXPECT_EQ(list.GetFailName(), nullptr);
	EXPECT_EQ(list.GetFailMsg(), nullptr);
}

TEST(CoreSubsystems, RejectsDependencyCycles)
{
	std::vector<std::string> events;
	ProbeSubsystem first("first", &events);
	ProbeSubsystem second("second", &events);
	Core::SubsystemsList list;

	list.Add(&first, {&second});
	list.Add(&second, {&first});

	EXPECT_FALSE(list.InitAll());
	EXPECT_TRUE(events.empty());
	EXPECT_NE(std::string(list.GetFailMsg()).find("dependency cycle or blocked dependency"), std::string::npos);
}

TEST(CoreSubsystems, ReportsTransitiveMissingDependenciesAndRollsBack)
{
	std::vector<std::string> events;
	ProbeSubsystem missing("missing", &events);
	ProbeSubsystem ready("ready", &events);
	ProbeSubsystem producer("producer", &events);
	ProbeSubsystem consumer("consumer", &events);
	Core::SubsystemsList list;

	list.Add(&ready, {});
	list.Add(&producer, {&missing});
	list.Add(&consumer, {&producer});

	EXPECT_FALSE(list.InitAll());
	EXPECT_TRUE(events.empty());
	EXPECT_STREQ(list.GetFailName(), "producer");
	EXPECT_NE(std::string(list.GetFailMsg()).find("missing dependency 'missing'"), std::string::npos);
}

TEST(CoreSubsystems, InitializesAndDestroysInDependencyOrder)
{
	std::vector<std::string> events;
	ProbeSubsystem core("core", &events);
	ProbeSubsystem graphics("graphics", &events);
	ProbeSubsystem window("window", &events);
	Core::SubsystemsList list;

	list.Add(&core, {});
	list.Add(&graphics, {&core});
	list.Add(&window, {&graphics});

	EXPECT_TRUE(list.InitAll());
	EXPECT_EQ(events, (std::vector<std::string> {"init:core", "init:graphics", "init:window"}));

	list.DestroyAll();
	EXPECT_EQ(events, (std::vector<std::string> {"init:core", "init:graphics", "init:window", "destroy:window", "destroy:graphics", "destroy:core"}));
}

UT_END();
