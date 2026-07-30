// M16-E: automation tests for VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult
// -- the first real integration between a FVertexMaskForgeMaskInstance (M16-A) and
// FVertexMaskForgeInstanceResultStore (M16-C), scoped to Material Slot only. Every expected value below
// is derived directly from the real generator algorithm (VertexMaskForgeMaterialSlotGenerator.cpp), not
// assumed.
//
// Fixtures use the Source-Topology (Dynamic Mesh) domain exclusively: a FDynamicMesh3 is a plain CPU
// data structure buildable in-process (AppendVertex/AppendTriangle), unlike the render-vertex domain's
// FStaticMeshLODResources, which is real engine render data (FPositionVertexBuffer/
// FStaticMeshVertexBuffer) with no lightweight in-test construction path -- consistent with there being
// no pre-existing automation test for the legacy Material Slot generator's render-vertex path either.
// The render-vertex domain's own validation branch (LOD0 == nullptr rejection) is covered directly; the
// domain's happy path is proven structurally instead -- see the M16-E report's own "Render Vertex vs
// Source Topology" section for the full justification of this scope limitation.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	/**
	 * A minimal two-triangle (quad) Source-Topology working mesh: Triangle 0 = corners {0,1,2},
	 * Triangle 1 = corners {0,2,3} (6 corners total), with DynamicTriangleToMaterialSlot resolving
	 * Triangle 0 to SlotForTri0 and Triangle 1 to SlotForTri1. MaterialSlotOptions is sized to 2 so
	 * SelectedSlotIndex 0 and 1 are both valid indices (IsValidIndex is checked against
	 * MaterialSlotOptions, not against the resolved slot values themselves -- see
	 * GenerateMaterialSlotMaskFromDynamicMesh's own guard clause).
	 */
	FVertexMaskForgeWorkingMesh BuildFixtureWorkingMesh(const int32 SlotForTri0, const int32 SlotForTri1)
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;

		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		const int32 V0 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0));
		const int32 V3 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));
		WorkingMesh.Mesh->AppendTriangle(V0, V1, V2);
		WorkingMesh.Mesh->AppendTriangle(V0, V2, V3);

		WorkingMesh.bMaterialSlotResolutionValid = true;
		WorkingMesh.bRenderVertexMaterialSlotAmbiguous = false;
		WorkingMesh.MaterialSlotOptions.SetNum(2);

		WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
		WorkingMesh.DynamicTriangleToMaterialSlot[0] = SlotForTri0;
		WorkingMesh.DynamicTriangleToMaterialSlot[1] = SlotForTri1;

		return WorkingMesh;
	}

	FVertexMaskForgeMaskInstance MakeMaterialSlotInstance(const int32 SelectedSlotIndex, const bool bInvert = false)
	{
		FVertexMaskForgeMaskInstance Instance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
		FVertexMaskForgeMaterialSlotParams& Params = Instance.Params.Get<FVertexMaskForgeMaterialSlotParams>();
		Params.SelectedSlotIndex = SelectedSlotIndex;
		Params.bInvert = bInvert;
		return Instance;
	}

	bool ValuesMatch(const FVertexMaskForgeInstanceMaskResult& Result, const TArray<float>& Expected, FAutomationTestBase& Test)
	{
		if (Result.Values.Num() != Expected.Num())
		{
			Test.AddError(FString::Printf(TEXT("Values.Num() mismatch: got %d, expected %d"), Result.Values.Num(), Expected.Num()));
			return false;
		}
		bool bAllMatch = true;
		for (int32 i = 0; i < Expected.Num(); ++i)
		{
			float Value = 0.0f;
			if (!Result.TryGetValue(i, Value) || !FMath::IsNearlyEqual(Value, Expected[i]))
			{
				Test.AddError(FString::Printf(TEXT("Values[%d] mismatch: expected %f"), i, Expected[i]));
				bAllMatch = false;
			}
		}
		return bAllMatch;
	}
}

// A. One Material Slot Instance generates a keyed result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceBasicTest, "VertexMaskForge.MaterialSlotInstanceExecution.BasicGeneration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceBasicTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	const bool bSuccess = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		A, WorkingMesh, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr);

	TestTrue(TEXT("Generation succeeds"), bSuccess);
	TestEqual(TEXT("Num after one instance"), WorkingMesh.InstanceResults.Num(), 1);
	TestTrue(TEXT("Contains A"), WorkingMesh.InstanceResults.Contains(A.InstanceId));

	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Find A is non-null"), Found);
	if (Found)
	{
		// Triangle 0 (slot 0, selected) -> corners 0-2 = 1.0; Triangle 1 (slot 1) -> corners 3-5 = 0.0.
		ValuesMatch(*Found, { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }, *this);
	}

	return true;
}

// B. Two Instances of the same generator (different slots) coexist -- generator type is not identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceCoexistTest, "VertexMaskForge.MaterialSlotInstanceExecution.TwoInstancesCoexist", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceCoexistTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);
	const FVertexMaskForgeMaskInstance B = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/1);

	TestTrue(TEXT("A succeeds"), VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr));
	TestTrue(TEXT("B succeeds"), VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(B, WorkingMesh, true, nullptr));

	TestEqual(TEXT("Num after two instances"), WorkingMesh.InstanceResults.Num(), 2);

	const FVertexMaskForgeInstanceMaskResult* FoundA = WorkingMesh.InstanceResults.Find(A.InstanceId);
	const FVertexMaskForgeInstanceMaskResult* FoundB = WorkingMesh.InstanceResults.Find(B.InstanceId);
	TestNotNull(TEXT("Find A"), FoundA);
	TestNotNull(TEXT("Find B"), FoundB);
	if (FoundA) { ValuesMatch(*FoundA, { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }, *this); }
	if (FoundB) { ValuesMatch(*FoundB, { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f }, *this); }

	return true;
}

// C. Same slot, different GUIDs -- still two independent results.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceSameSlotTest, "VertexMaskForge.MaterialSlotInstanceExecution.SameSlotDifferentGuids", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceSameSlotTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);
	const FVertexMaskForgeMaskInstance B = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	TestNotEqual(TEXT("A and B have different InstanceIds"), A.InstanceId, B.InstanceId);

	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr);
	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(B, WorkingMesh, true, nullptr);

	TestEqual(TEXT("Num after two same-slot instances"), WorkingMesh.InstanceResults.Num(), 2);
	TestTrue(TEXT("Contains A"), WorkingMesh.InstanceResults.Contains(A.InstanceId));
	TestTrue(TEXT("Contains B"), WorkingMesh.InstanceResults.Contains(B.InstanceId));

	return true;
}

// D. Replacement without growth: reconfiguring the same InstanceId overwrites in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceReplaceTest, "VertexMaskForge.MaterialSlotInstanceExecution.ReplaceNoGrowth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceReplaceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr);
	TestEqual(TEXT("Num after first store"), WorkingMesh.InstanceResults.Num(), 1);

	A.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1;
	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr);

	TestEqual(TEXT("Num after replace (same InstanceId)"), WorkingMesh.InstanceResults.Num(), 1);
	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Find A after replace"), Found);
	if (Found)
	{
		// Now selecting slot 1: Triangle 0 (slot 0) -> 0.0; Triangle 1 (slot 1) -> 1.0. The old
		// slot-0 payload (1,1,1,0,0,0) must be gone.
		ValuesMatch(*Found, { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f }, *this);
	}

	return true;
}

// E. Scrubbing: 100 alternating reconfigurations of the same InstanceId never grow the store.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceScrubbingTest, "VertexMaskForge.MaterialSlotInstanceExecution.ScrubbingNoGrowth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceScrubbingTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	int32 LastSlot = 0;
	for (int32 i = 0; i < 100; ++i)
	{
		LastSlot = i % 2;
		A.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = LastSlot;
		VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr);
		TestEqual(TEXT("Num stays 1 during scrubbing"), WorkingMesh.InstanceResults.Num(), 1);
	}

	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Find A after scrubbing"), Found);
	if (Found)
	{
		const TArray<float> Expected = (LastSlot == 0)
			? TArray<float>{ 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }
			: TArray<float>{ 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
		ValuesMatch(*Found, Expected, *this);
	}

	return true;
}

// F. Invalid GUID is rejected deterministically -- no crash, no store mutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceInvalidGuidTest, "VertexMaskForge.MaterialSlotInstanceExecution.InvalidGuid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceInvalidGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	FVertexMaskForgeMaskInstance Invalid;
	Invalid.GeneratorType = EVertexMaskForgeGeneratorType::MaterialSlot;
	Invalid.Params = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
	// Invalid.InstanceId left default-constructed (zero, invalid) -- deliberate.
	TestFalse(TEXT("Default FGuid is invalid"), Invalid.InstanceId.IsValid());

	const bool bSuccess = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		Invalid, WorkingMesh, true, nullptr);

	TestFalse(TEXT("Generation with invalid GUID fails"), bSuccess);
	TestEqual(TEXT("Store stays empty"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// G. Incompatible generator type is rejected -- no other generator is executed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceWrongTypeTest, "VertexMaskForge.MaterialSlotInstanceExecution.WrongGeneratorType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceWrongTypeTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance BoundingBoxInstance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::BoundingBox);

	const bool bSuccess = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		BoundingBoxInstance, WorkingMesh, true, nullptr);

	TestFalse(TEXT("Generation with wrong GeneratorType fails"), bSuccess);
	TestEqual(TEXT("Store stays empty"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// H. A failed re-execution preserves the previous valid result for the same InstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstancePreservesOnFailureTest, "VertexMaskForge.MaterialSlotInstanceExecution.FailurePreservesPrevious", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstancePreservesOnFailureTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	TestTrue(TEXT("First (valid) generation succeeds"),
		VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr));
	TestEqual(TEXT("Num after valid store"), WorkingMesh.InstanceResults.Num(), 1);

	// Simulate the working mesh becoming invalid for Material Slot resolution.
	WorkingMesh.bMaterialSlotResolutionValid = false;

	const bool bSecondSucceeds = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		A, WorkingMesh, true, nullptr);
	TestFalse(TEXT("Second (invalid-mesh) generation fails"), bSecondSucceeds);

	TestEqual(TEXT("Num unchanged after failed re-execution"), WorkingMesh.InstanceResults.Num(), 1);
	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Previous result for A still present"), Found);
	if (Found)
	{
		ValuesMatch(*Found, { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }, *this);
	}

	return true;
}

// I. The named legacy MaterialSlotMask field is never touched by the new keyed path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceLegacyPreservedTest, "VertexMaskForge.MaterialSlotInstanceExecution.LegacyResultPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceLegacyPreservedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	// M16-J.0B.1 (WorkingMesh Domain Split): the legacy named result (MaterialSlotMask) no longer lives on
	// FVertexMaskForgeWorkingMesh at all -- it moved to FVertexMaskForgeGeneratorState, a sibling struct
	// GenerateMaterialSlotMaskInstanceResult is never even given a reference to. This local GeneratorState
	// stands in for that sibling: since the function under test only ever touches WorkingMesh, proving it
	// leaves this sentinel value alone is the exact same proof the test always made, just against the
	// field's new home.
	FVertexMaskForgeGeneratorState GeneratorState;

	// Sentinel legacy result, deliberately unrelated to what the new path would compute.
	GeneratorState.MaterialSlotMask.State = EVertexMaskForgeScalarMaskState::Ready;
	GeneratorState.MaterialSlotMask.Values = { 0.42f };
	GeneratorState.MaterialSlotMask.bHasValue.Init(true, 1);
	GeneratorState.MaterialSlotMask.NumValidValues = 1;

	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);
	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(A, WorkingMesh, true, nullptr);

	TestTrue(TEXT("Legacy MaterialSlotMask.State unchanged"), GeneratorState.MaterialSlotMask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Legacy MaterialSlotMask.Values.Num() unchanged"), GeneratorState.MaterialSlotMask.Values.Num(), 1);
	if (GeneratorState.MaterialSlotMask.Values.Num() == 1)
	{
		TestEqual(TEXT("Legacy MaterialSlotMask sentinel value unchanged"), GeneratorState.MaterialSlotMask.Values[0], 0.42f);
	}
	TestEqual(TEXT("Keyed result was still created"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// K. Independent owners: two separate WorkingMesh instances never share results, even for the same InstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceOwnerSeparationTest, "VertexMaskForge.MaterialSlotInstanceExecution.OwnerSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceOwnerSeparationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMeshOne = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	FVertexMaskForgeWorkingMesh WorkingMeshTwo = BuildFixtureWorkingMesh(/*SlotForTri0=*/1, /*SlotForTri1=*/0);

	FVertexMaskForgeMaskInstance Shared = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(Shared, WorkingMeshOne, true, nullptr);
	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(Shared, WorkingMeshTwo, true, nullptr);

	TestEqual(TEXT("WorkingMeshOne holds exactly its own result"), WorkingMeshOne.InstanceResults.Num(), 1);
	TestEqual(TEXT("WorkingMeshTwo holds exactly its own result"), WorkingMeshTwo.InstanceResults.Num(), 1);

	const FVertexMaskForgeInstanceMaskResult* FoundOne = WorkingMeshOne.InstanceResults.Find(Shared.InstanceId);
	const FVertexMaskForgeInstanceMaskResult* FoundTwo = WorkingMeshTwo.InstanceResults.Find(Shared.InstanceId);
	TestNotNull(TEXT("Find in WorkingMeshOne"), FoundOne);
	TestNotNull(TEXT("Find in WorkingMeshTwo"), FoundTwo);
	// Same InstanceId, opposite slot-to-triangle mapping fixtures -> opposite payloads.
	if (FoundOne) { ValuesMatch(*FoundOne, { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f }, *this); }
	if (FoundTwo) { ValuesMatch(*FoundTwo, { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f }, *this); }

	return true;
}

// L. Render-vertex domain's own validation guard: LOD0 == nullptr is rejected deterministically. The
// render-vertex happy path itself is not exercised here -- see this file's header comment for why
// (FStaticMeshLODResources has no lightweight in-test construction path).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotInstanceRenderVertexRequiresLOD0Test, "VertexMaskForge.MaterialSlotInstanceExecution.RenderVertexRequiresLOD0", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotInstanceRenderVertexRequiresLOD0Test::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	const bool bSuccess = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		A, WorkingMesh, /*bUseSourceTopology=*/false, /*LOD0=*/nullptr);

	TestFalse(TEXT("Render-vertex domain without LOD0 fails"), bSuccess);
	TestEqual(TEXT("Store stays empty"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
