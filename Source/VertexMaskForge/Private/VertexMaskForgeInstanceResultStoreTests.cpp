// M16-C: lifecycle and cardinality automation tests for FVertexMaskForgeInstanceResultStore. Every
// expected value below is derived directly from the store's own real implementation
// (VertexMaskForgeInstanceResultStore.cpp), not assumed.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeInstanceResultStore.h"

namespace
{
	FVertexMaskForgeInstanceMaskResult MakeResult(const TArray<float>& Values)
	{
		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = Values;
		Result.bHasValue.Init(true, Values.Num());
		return Result;
	}
}

// A. Store and find.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreBasicTest, "VertexMaskForge.InstanceResultStore.StoreAndFind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreBasicTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();

	TestTrue(TEXT("Store A succeeds"), Store.StoreOrReplace(A, MakeResult({ 0.25f, 0.50f, 0.75f })));
	TestEqual(TEXT("Num after one store"), Store.Num(), 1);
	TestTrue(TEXT("Contains A"), Store.Contains(A));

	const FVertexMaskForgeInstanceMaskResult* Found = Store.Find(A);
	TestNotNull(TEXT("Find A is non-null"), Found);
	if (Found)
	{
		float Value = 0.0f;
		TestTrue(TEXT("TryGetValue(0)"), Found->TryGetValue(0, Value)); TestEqual(TEXT("Value[0]"), Value, 0.25f);
		TestTrue(TEXT("TryGetValue(1)"), Found->TryGetValue(1, Value)); TestEqual(TEXT("Value[1]"), Value, 0.50f);
		TestTrue(TEXT("TryGetValue(2)"), Found->TryGetValue(2, Value)); TestEqual(TEXT("Value[2]"), Value, 0.75f);
	}

	return true;
}

// B. Replace the same InstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreReplaceTest, "VertexMaskForge.InstanceResultStore.Replace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreReplaceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();

	Store.StoreOrReplace(A, MakeResult({ 0.25f }));
	Store.StoreOrReplace(A, MakeResult({ 0.75f, 1.0f }));

	TestEqual(TEXT("Num stays 1 after replace"), Store.Num(), 1);
	const FVertexMaskForgeInstanceMaskResult* Found = Store.Find(A);
	TestNotNull(TEXT("Find A after replace"), Found);
	if (Found)
	{
		TestEqual(TEXT("Replaced payload size"), Found->Values.Num(), 2);
		TestEqual(TEXT("Replaced payload[0]"), Found->Values[0], 0.75f);
		TestEqual(TEXT("Replaced payload[1]"), Found->Values[1], 1.0f);
	}

	return true;
}

// C. Scrubbing: 100 stores to the same InstanceId must never grow cardinality.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreScrubbingTest, "VertexMaskForge.InstanceResultStore.ScrubbingNoGrowth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreScrubbingTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();

	constexpr int32 NumIterations = 100;
	for (int32 i = 0; i < NumIterations; ++i)
	{
		Store.StoreOrReplace(A, MakeResult({ static_cast<float>(i) }));
	}

	TestEqual(TEXT("Num stays 1 after 100 stores"), Store.Num(), 1);
	const FVertexMaskForgeInstanceMaskResult* Found = Store.Find(A);
	TestNotNull(TEXT("Find A after scrubbing"), Found);
	if (Found)
	{
		TestEqual(TEXT("Final payload is the last iteration's value"), Found->Values[0], static_cast<float>(NumIterations - 1));
	}

	return true;
}

// D. Independent identities.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreIndependentIdentitiesTest, "VertexMaskForge.InstanceResultStore.IndependentIdentities", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreIndependentIdentitiesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();

	Store.StoreOrReplace(A, MakeResult({ 0.2f }));
	Store.StoreOrReplace(B, MakeResult({ 0.8f }));

	TestEqual(TEXT("Num with two identities"), Store.Num(), 2);
	TestEqual(TEXT("A value"), Store.Find(A)->Values[0], 0.2f);
	TestEqual(TEXT("B value"), Store.Find(B)->Values[0], 0.8f);

	return true;
}

// E. Selective remove.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreSelectiveRemoveTest, "VertexMaskForge.InstanceResultStore.SelectiveRemove", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreSelectiveRemoveTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	Store.StoreOrReplace(A, MakeResult({ 0.2f }));
	Store.StoreOrReplace(B, MakeResult({ 0.8f }));

	TestTrue(TEXT("Remove A returns true"), Store.Remove(A));
	TestFalse(TEXT("A absent after remove"), Store.Contains(A));
	TestTrue(TEXT("B preserved after removing A"), Store.Contains(B));
	TestEqual(TEXT("Num after selective remove"), Store.Num(), 1);

	return true;
}

// F. Remove absent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreRemoveAbsentTest, "VertexMaskForge.InstanceResultStore.RemoveAbsent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreRemoveAbsentTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();
	const FGuid C = FGuid::NewGuid(); // Never stored.
	Store.StoreOrReplace(A, MakeResult({ 1.0f }));

	TestFalse(TEXT("Remove absent GUID returns false"), Store.Remove(C));
	TestEqual(TEXT("Num unchanged after removing absent GUID"), Store.Num(), 1);
	TestTrue(TEXT("A still present"), Store.Contains(A));
	TestEqual(TEXT("A payload unchanged"), Store.Find(A)->Values[0], 1.0f);

	return true;
}

// G. Prune.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStorePruneTest, "VertexMaskForge.InstanceResultStore.Prune", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStorePruneTest::RunTest(const FString& Parameters)
{
	// Prune preserving a subset.
	{
		FVertexMaskForgeInstanceResultStore Store;
		const FGuid A = FGuid::NewGuid();
		const FGuid B = FGuid::NewGuid();
		const FGuid C = FGuid::NewGuid();
		Store.StoreOrReplace(A, MakeResult({ 1.0f }));
		Store.StoreOrReplace(B, MakeResult({ 2.0f }));
		Store.StoreOrReplace(C, MakeResult({ 3.0f }));

		const TSet<FGuid> Live = { A, C };
		const int32 Removed = Store.PruneToInstanceIds(Live);

		TestEqual(TEXT("Prune removed count"), Removed, 1);
		TestTrue(TEXT("A preserved"), Store.Contains(A));
		TestFalse(TEXT("B removed"), Store.Contains(B));
		TestTrue(TEXT("C preserved"), Store.Contains(C));
		TestEqual(TEXT("Num after prune"), Store.Num(), 2);
	}

	// Prune with an empty live set removes everything.
	{
		FVertexMaskForgeInstanceResultStore Store;
		const FGuid A = FGuid::NewGuid();
		const FGuid B = FGuid::NewGuid();
		Store.StoreOrReplace(A, MakeResult({ 1.0f }));
		Store.StoreOrReplace(B, MakeResult({ 2.0f }));

		const int32 Removed = Store.PruneToInstanceIds(TSet<FGuid>());
		TestEqual(TEXT("Prune-to-empty removed count"), Removed, 2);
		TestEqual(TEXT("Num after prune-to-empty"), Store.Num(), 0);
	}

	return true;
}

// H. Clear/reset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreResetTest, "VertexMaskForge.InstanceResultStore.Reset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreResetTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	const FGuid C = FGuid::NewGuid();
	Store.StoreOrReplace(A, MakeResult({ 1.0f }));
	Store.StoreOrReplace(B, MakeResult({ 2.0f }));
	Store.StoreOrReplace(C, MakeResult({ 3.0f }));

	Store.Reset();

	TestEqual(TEXT("Num after Reset"), Store.Num(), 0);
	TestNull(TEXT("A absent after Reset"), Store.Find(A));
	TestNull(TEXT("B absent after Reset"), Store.Find(B));
	TestNull(TEXT("C absent after Reset"), Store.Find(C));

	return true;
}

// I. Invalid GUID policy: rejected without a slot being created and without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreInvalidGuidTest, "VertexMaskForge.InstanceResultStore.InvalidGuid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreInvalidGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FGuid Invalid; // Default-constructed FGuid is all-zero, IsValid() == false.
	TestFalse(TEXT("Precondition: default FGuid is invalid"), Invalid.IsValid());

	// The store's own StoreOrReplace deliberately reports this via a quiet Warning-level UE_LOG (never
	// ensure()/check()), so no AddExpectedError() suppression is needed here -- a Warning does not
	// fail an automation test.
	const bool bStored = Store.StoreOrReplace(Invalid, MakeResult({ 0.5f }));

	TestFalse(TEXT("StoreOrReplace with invalid GUID returns false"), bStored);
	TestEqual(TEXT("Num unchanged after rejected store"), Store.Num(), 0);

	return true;
}

// J. Separation across independent owners (proves InstanceId alone does not make results global).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeInstanceResultStoreOwnerSeparationTest, "VertexMaskForge.InstanceResultStore.OwnerSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeInstanceResultStoreOwnerSeparationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Owner1;
	FVertexMaskForgeInstanceResultStore Owner2;
	const FGuid A = FGuid::NewGuid();

	Owner1.StoreOrReplace(A, MakeResult({ 0.2f }));
	Owner2.StoreOrReplace(A, MakeResult({ 0.8f }));

	TestEqual(TEXT("Owner1 Num"), Owner1.Num(), 1);
	TestEqual(TEXT("Owner2 Num"), Owner2.Num(), 1);
	TestEqual(TEXT("Owner1 value for A"), Owner1.Find(A)->Values[0], 0.2f);
	TestEqual(TEXT("Owner2 value for A"), Owner2.Find(A)->Values[0], 0.8f);

	Owner1.Remove(A);
	TestFalse(TEXT("A removed from Owner1"), Owner1.Contains(A));
	TestTrue(TEXT("A still present in Owner2 after Owner1 removal"), Owner2.Contains(A));
	TestEqual(TEXT("Owner2 value for A unaffected by Owner1 removal"), Owner2.Find(A)->Values[0], 0.8f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
