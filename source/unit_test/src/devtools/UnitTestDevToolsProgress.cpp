#include "Kyty/DevTools/Telemetry/Progress.h"
#include "Kyty/UnitTest.h"

#include <memory>

UT_BEGIN(DevToolsProgress);

using namespace Kyty::DevTools;

TEST(DevToolsProgress, ProgressRefRoundTripsDomainAnd56BitInstance)
{
	ProgressKey key {Domain::Hle, 7};
	uint64_t    wire = 0;
	ASSERT_TRUE(key.TryPack(&wire));
	ProgressKey out {};
	ASSERT_TRUE(ProgressKey::TryUnpack(wire, &out));
	EXPECT_EQ(out.domain, Domain::Hle);
	EXPECT_EQ(out.instance, 7u);

	// Same instance different domains are distinct.
	ProgressKey g {Domain::GuestThread, 7};
	uint64_t    wire_g = 0;
	ASSERT_TRUE(g.TryPack(&wire_g));
	EXPECT_NE(wire, wire_g);

	ProgressKey bad {Domain::Unknown, 1};
	EXPECT_FALSE(bad.TryPack(&wire));
	ProgressKey zero {Domain::Hle, 0};
	EXPECT_FALSE(zero.TryPack(&wire));
	ProgressKey huge {Domain::Hle, (uint64_t {1} << 56u)};
	EXPECT_FALSE(huge.TryPack(&wire));
	EXPECT_FALSE(ProgressKey::TryUnpack(0, &out));
}

TEST(DevToolsProgress, KeepsConcurrentInstancesDistinct)
{
	auto reg = std::make_unique<ProgressRegistry>();
	ProgressToken hle_a {};
	ProgressToken hle_b {};
	ProgressToken gpu_a {};
	ProgressToken gpu_b {};
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::Hle, 1}, &hle_a));
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::Hle, 2}, &hle_b));
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::GpuQueue, 1}, &gpu_a));
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::GpuQueue, 2}, &gpu_b));

	ProgressUpdate sub {};
	sub.epoch        = 1;
	sub.operation    = OperationCode::HleCall;
	sub.state        = ProgressState::Active;
	sub.flags        = static_cast<uint16_t>(ProgressFlag::OperationValid);
	sub.monotonic_ns = 100;
	ASSERT_TRUE(reg->Submit(hle_a, sub));

	sub.operation = OperationCode::QueueSubmit;
	sub.epoch     = 5;
	ASSERT_TRUE(reg->Submit(gpu_a, sub));

	ProgressRecord ra {};
	ProgressRecord rb {};
	ProgressRecord ga {};
	ASSERT_TRUE(reg->Snapshot(hle_a, &ra));
	ASSERT_TRUE(reg->Snapshot(hle_b, &rb));
	ASSERT_TRUE(reg->Snapshot(gpu_a, &ga));
	EXPECT_EQ(ra.submitted, 1u);
	EXPECT_EQ(ra.operation, static_cast<uint32_t>(OperationCode::HleCall));
	EXPECT_EQ(rb.submitted, 0u); // untouched
	EXPECT_EQ(ga.submitted, 5u);
	EXPECT_EQ(ga.operation, static_cast<uint32_t>(OperationCode::QueueSubmit));

	// Advancing hle_a does not replace hle_b or gpu.
	ProgressUpdate adv = sub;
	adv.operation      = OperationCode::HleCall;
	adv.epoch          = 1;
	adv.state          = ProgressState::Waiting;
	adv.monotonic_ns   = 200;
	ASSERT_TRUE(reg->Advance(hle_a, adv));
	ASSERT_TRUE(reg->Snapshot(hle_a, &ra));
	ASSERT_TRUE(reg->Snapshot(hle_b, &rb));
	ASSERT_TRUE(reg->Snapshot(gpu_a, &ga));
	EXPECT_EQ(ra.state, static_cast<uint16_t>(ProgressState::Waiting));
	EXPECT_EQ(rb.submitted, 0u);
	EXPECT_EQ(ga.submitted, 5u);
}

TEST(DevToolsProgress, ProgressEpochTransitionsAreExact)
{
	auto reg = std::make_unique<ProgressRegistry>();
	ProgressToken token {};
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::CommandProcessor, 9}, &token));

	ProgressRecord idle {};
	ASSERT_TRUE(reg->Snapshot(token, &idle));
	EXPECT_EQ(idle.submitted, 0u);
	EXPECT_EQ(idle.completed, 0u);
	EXPECT_EQ(idle.state, static_cast<uint16_t>(ProgressState::Idle));

	ProgressUpdate sub {};
	sub.epoch        = 3;
	sub.operation    = OperationCode::CommandSubmit;
	sub.state        = ProgressState::Active;
	sub.flags        = static_cast<uint16_t>(ProgressFlag::OperationValid);
	sub.monotonic_ns = 10;
	ASSERT_TRUE(reg->Submit(token, sub));

	// Same-epoch advance ok.
	ProgressUpdate adv = sub;
	adv.state          = ProgressState::Waiting;
	adv.monotonic_ns   = 11;
	ASSERT_TRUE(reg->Advance(token, adv));

	ASSERT_TRUE(reg->Complete(token, 3, 12));
	ProgressRecord done {};
	ASSERT_TRUE(reg->Snapshot(token, &done));
	EXPECT_EQ(done.completed, 3u);
	EXPECT_EQ(done.submitted, 3u);
	EXPECT_EQ(done.state, static_cast<uint16_t>(ProgressState::Idle));

	// Double complete rejected.
	EXPECT_FALSE(reg->Complete(token, 3, 13));
	// Regression submit rejected.
	sub.epoch = 2;
	EXPECT_FALSE(reg->Submit(token, sub));
	// Later submit ok.
	sub.epoch = 4;
	ASSERT_TRUE(reg->Submit(token, sub));
}

TEST(DevToolsProgress, RejectsInvalidProgressSchemaWithoutMutatingEndpoint)
{
	auto reg = std::make_unique<ProgressRegistry>();
	ProgressToken token {};
	ASSERT_TRUE(reg->Register(ProgressKey {Domain::Hle, 11}, &token));

	ProgressUpdate bad {};
	bad.epoch        = 1;
	bad.operation    = OperationCode::CommandPacket; // wrong domain
	bad.state        = ProgressState::Active;
	bad.flags        = static_cast<uint16_t>(ProgressFlag::OperationValid);
	bad.monotonic_ns = 1;
	EXPECT_FALSE(reg->Submit(token, bad));

	ProgressRecord r {};
	ASSERT_TRUE(reg->Snapshot(token, &r));
	EXPECT_EQ(r.submitted, 0u);

	bad.operation = OperationCode::HleCall;
	bad.flags     = 0; // missing OperationValid
	EXPECT_FALSE(reg->Submit(token, bad));
	ASSERT_TRUE(reg->Snapshot(token, &r));
	EXPECT_EQ(r.submitted, 0u);

	const auto loss = reg->SnapshotLoss();
	EXPECT_GE(loss.rejected_update[static_cast<uint32_t>(Domain::Hle)].total, 2u);
}

TEST(DevToolsProgress, ProgressWireIdentityIsCanonical)
{
	ProgressKey a {Domain::Renderer, 100};
	uint64_t    wire = 0;
	ASSERT_TRUE(a.TryPack(&wire));
	auto reg = std::make_unique<ProgressRegistry>();
	ProgressToken token {};
	ASSERT_TRUE(reg->Register(a, &token));
	ProgressRecord r {};
	ASSERT_TRUE(reg->Snapshot(token, &r));
	EXPECT_EQ(r.instance_key, wire);

	// Duplicate live key rejected.
	ProgressToken dup {};
	EXPECT_FALSE(reg->Register(a, &dup));
}

UT_END();
