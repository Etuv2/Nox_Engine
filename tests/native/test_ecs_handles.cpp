// ECS checked handles (C6): incarnations, world tokens, Clear and reuse. Headless.
#include "TestFramework.h"

#include "ComponentManager.h"

NOX_TEST(EcsHandles, ValidUntilDestroyed)
{
	ComponentManager cm;
	const EntityID id = cm.CreateEntity("hero");
	const EntityHandle h = cm.MakeHandle(id);
	NOX_CHECK(h.world != 0);
	NOX_CHECK(cm.IsHandleValid(h));

	cm.DestroyEntity(id);
	NOX_CHECK(!cm.IsHandleValid(h));
	NOX_CHECK(cm.MakeHandle(id).world == 0);
}

NOX_TEST(EcsHandles, ReusedIdDoesNotRevivePreviousHandle)
{
	ComponentManager cm;
	const EntityID first = cm.CreateEntity("a");
	const EntityHandle oldHandle = cm.MakeHandle(first);
	cm.DestroyEntity(first);

	const EntityID second = cm.CreateEntity("b");
	NOX_CHECK_EQ(second, first);   // the ID is recycled
	const EntityHandle newHandle = cm.MakeHandle(second);
	NOX_CHECK(newHandle.incarnation != oldHandle.incarnation);
	NOX_CHECK(!cm.IsHandleValid(oldHandle));
	NOX_CHECK(cm.IsHandleValid(newHandle));
	NOX_CHECK(!(oldHandle == newHandle));
}

NOX_TEST(EcsHandles, IncarnationIncreasesAcrossRepeatedReuse)
{
	ComponentManager cm;
	uint32_t last = 0;
	for (int i = 0; i < 100; ++i) {
		const EntityID id = cm.CreateEntity("cycle");
		const EntityHandle h = cm.MakeHandle(id);
		NOX_CHECK(h.incarnation > last);
		last = h.incarnation;
		cm.DestroyEntity(id);
	}
}

NOX_TEST(EcsHandles, ClearInvalidatesEverythingAndKeepsIdentityHistory)
{
	ComponentManager cm;
	const EntityID id = cm.CreateEntity("keep");
	const EntityHandle before = cm.MakeHandle(id);
	const uint32_t worldBefore = cm.GetWorldToken();

	cm.Clear();
	NOX_CHECK(cm.GetWorldToken() != worldBefore);
	NOX_CHECK(!cm.IsHandleValid(before));

	// Clear restarts ID allocation, so the same ID comes back; the old handle must still not match.
	const EntityID again = cm.CreateEntity("keep");
	NOX_CHECK_EQ(again, id);
	const EntityHandle after = cm.MakeHandle(again);
	NOX_CHECK(after.world != before.world);
	NOX_CHECK(after.incarnation != before.incarnation);
	NOX_CHECK(!cm.IsHandleValid(before));
	NOX_CHECK(cm.IsHandleValid(after));
}

NOX_TEST(EcsHandles, NewWorldRejectsForeignHandles)
{
	ComponentManager a;
	ComponentManager b;
	const EntityID ida = a.CreateEntity("x");
	const EntityHandle fromA = a.MakeHandle(ida);
	const EntityID idb = b.CreateEntity("x");
	const EntityHandle fromB = b.MakeHandle(idb);
	NOX_CHECK(fromA.world != fromB.world);
	// Same numeric ID and incarnation in another world must not validate.
	NOX_CHECK(!b.IsHandleValid(fromA));
	NOX_CHECK(!a.IsHandleValid(fromB));
	NOX_CHECK(a.IsHandleValid(fromA));
	NOX_CHECK(b.IsHandleValid(fromB));
}

NOX_TEST(EcsHandles, MissingComponentsAreReportedNotCreated)
{
	ComponentManager cm;
	const EntityID id = cm.CreateEntity("plain");
	NOX_CHECK(cm.HasTransform(id));
	NOX_CHECK(!cm.HasRenderable(id));
	NOX_CHECK(cm.GetRenderable(id) == nullptr);
	NOX_CHECK(!cm.HasPhysics(id));
	NOX_CHECK(cm.GetPhysics(id) == nullptr);
	NOX_CHECK(cm.IsHandleValid(cm.MakeHandle(id)));
}

NOX_TEST(EcsHandles, InvalidHandlesAreRejected)
{
	ComponentManager cm;
	EntityHandle none;
	NOX_CHECK(!cm.IsHandleValid(none));
	EntityHandle garbage;
	garbage.world = cm.GetWorldToken();
	garbage.id = 999;
	garbage.incarnation = 1;
	NOX_CHECK(!cm.IsHandleValid(garbage));
	garbage.id = INVALID_ENTITY;
	NOX_CHECK(!cm.IsHandleValid(garbage));
}
