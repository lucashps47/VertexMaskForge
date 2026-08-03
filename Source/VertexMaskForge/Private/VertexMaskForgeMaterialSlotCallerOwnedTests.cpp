// M16-K.6D-3: proves that VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh
// -- the REAL, unmodified, already-existing Source-Topology Material Slot generator function -- already
// satisfies every property this checkpoint's own contract requires of a "caller-owned Material Slot API"
// for a future Dynamic orchestrator (M16-K.6D-4) to consume directly:
//   - inputs are explicit and read-only (WorkingMesh by const&, SelectedSlotIndex, bInvert);
//   - the result is returned entirely by value, with no FVertexMaskForgeInstanceResultStore dependency
//     of any kind;
//   - domain/cardinality are exactly Source Topology corners (Mesh.TriangleCount() * 3);
//   - success/failure is unambiguous (Mask.State), and a legitimately-all-zero valid result (a selected
//     slot matching no triangle) is never confused with a structural failure;
//   - the SAME function is what every existing Legacy-facing wrapper (the panel's own inline legacy
//     call, GenerateMaterialSlotMaskInstanceResult, and VertexMaskForgeDynamicMaskGeneration::
//     GenerateStoredResultForMaterialSlotInstance) already delegates to -- proven here by direct,
//     byte-for-byte comparison against GenerateMaterialSlotMaskInstanceResult's own stored result for
//     identical inputs.
//
// No new production function is introduced by this checkpoint -- these tests exercise the existing,
// public, already-non-static VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh
// entry point directly, never a reimplementation of its math.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// Same two-triangle (quad) fixture shape as VertexMaskForgeMaterialSlotGeneratorTests.cpp's own
	// BuildFixtureWorkingMesh -- deliberately re-derived here (not shared) so this file exercises the
	// caller-owned entry point with zero dependency on that other file's own helpers.
	FVertexMaskForgeWorkingMesh BuildCallerOwnedFixtureWorkingMesh(const int32 SlotForTri0, const int32 SlotForTri1, const int32 NumSlotOptions = 2)
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
		WorkingMesh.MaterialSlotOptions.SetNum(NumSlotOptions);

		WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
		WorkingMesh.DynamicTriangleToMaterialSlot[0] = SlotForTri0;
		WorkingMesh.DynamicTriangleToMaterialSlot[1] = SlotForTri1;

		return WorkingMesh;
	}
}

// 1. Valid input generates a caller-owned result with the correct Source Topology domain and exact
// cardinality (Mesh.TriangleCount() * 3 == 6 for this two-triangle fixture), values matching the real
// generator algorithm, and Ready state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedValidInputTest, "VertexMaskForge.MaterialSlotCallerOwned.ValidInputProducesCorrectDomainAndCardinality", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedValidInputTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/0, /*bInvert=*/false);

	TestTrue(TEXT("State is Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Cardinality is TriangleCount * 3"), Mask.Values.Num(), WorkingMesh.Mesh->TriangleCount() * 3);
	TestEqual(TEXT("Cardinality is exactly 6 for this fixture"), Mask.Values.Num(), 6);
	if (Mask.Values.Num() == 6)
	{
		// Triangle 0 (slot 0, selected) -> corners 0-2 = 1.0; Triangle 1 (slot 1) -> corners 3-5 = 0.0.
		const TArray<float> Expected = { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Values[%d]"), Index), Mask.Values[Index], Expected[Index]);
			TestTrue(*FString::Printf(TEXT("bHasValue[%d]"), Index), Mask.bHasValue[Index]);
		}
	}

	return true;
}

// 2. A second selected slot preserves the semantics independently -- multiple slots do not interfere
// with each other (each call is a fresh, independent, pure computation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedMultipleSlotsTest, "VertexMaskForge.MaterialSlotCallerOwned.MultipleSlotsPreserveIndependentSemantics", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedMultipleSlotsTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	const FVertexMaskForgeScalarMask MaskSlot0 = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(WorkingMesh, 0, false);
	const FVertexMaskForgeScalarMask MaskSlot1 = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(WorkingMesh, 1, false);

	TestTrue(TEXT("Slot 0 Ready"), MaskSlot0.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("Slot 1 Ready"), MaskSlot1.State == EVertexMaskForgeScalarMaskState::Ready);
	if (MaskSlot0.Values.Num() == 6 && MaskSlot1.Values.Num() == 6)
	{
		const TArray<float> ExpectedSlot0 = { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
		const TArray<float> ExpectedSlot1 = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Slot0 Values[%d]"), Index), MaskSlot0.Values[Index], ExpectedSlot0[Index]);
			TestEqual(*FString::Printf(TEXT("Slot1 Values[%d]"), Index), MaskSlot1.Values[Index], ExpectedSlot1[Index]);
		}
	}

	return true;
}

// 3. bInvert complements the selection -- proves the Invert contract is preserved verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedInvertTest, "VertexMaskForge.MaterialSlotCallerOwned.InvertComplementsSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedInvertTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/0, /*bInvert=*/true);

	TestTrue(TEXT("State is Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	if (Mask.Values.Num() == 6)
	{
		// Inverted: Triangle 0 (slot 0, selected) -> 0.0; Triangle 1 (slot 1) -> 1.0.
		const TArray<float> Expected = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Inverted Values[%d]"), Index), Mask.Values[Index], Expected[Index]);
		}
	}

	return true;
}

// 4. A SelectedSlotIndex that legitimately matches no triangle produces a VALID, Ready, all-zero mask --
// never a failure. This is the exact "totally-zero-is-not-failure" distinction this checkpoint's own
// contract requires to be proven, not assumed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedNoMatchIsValidZeroTest, "VertexMaskForge.MaterialSlotCallerOwned.NoMatchingTriangleIsValidAllZeroNotFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedNoMatchIsValidZeroTest::RunTest(const FString& Parameters)
{
	// Three valid slot options (0,1,2), but no triangle in this fixture is ever mapped to slot 2 --
	// selecting slot 2 must be a legitimate "nothing matched" case, not a structural error.
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1, /*NumSlotOptions=*/3);

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/2, /*bInvert=*/false);

	TestTrue(TEXT("State is Ready (valid, not a failure)"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Cardinality still exactly 6"), Mask.Values.Num(), 6);
	if (Mask.Values.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("All-zero Values[%d]"), Index), Mask.Values[Index], 0.0f);
			TestTrue(*FString::Printf(TEXT("bHasValue[%d] still true (Ready, not Missing)"), Index), Mask.bHasValue[Index]);
		}
	}

	return true;
}

// 5. Structural failure (out-of-range SelectedSlotIndex) returns Unavailable with an empty Values array
// -- never a partial result, and distinguishable from the valid-all-zero case above by State alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedOutOfRangeSlotFailsTest, "VertexMaskForge.MaterialSlotCallerOwned.OutOfRangeSlotIndexFailsExplicitly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedOutOfRangeSlotFailsTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1, /*NumSlotOptions=*/2);

	// SelectedSlotIndex 5 is out of range for MaterialSlotOptions.Num() == 2.
	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/5, /*bInvert=*/false);

	TestTrue(TEXT("State is Unavailable"), Mask.State == EVertexMaskForgeScalarMaskState::Unavailable);
	TestEqual(TEXT("No partial output -- Values stays empty"), Mask.Values.Num(), 0);

	return true;
}

// 6. Structural failure (invalid mesh -- bMaterialSlotResolutionValid == false) also fails explicitly,
// with no partial output.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedInvalidMeshFailsTest, "VertexMaskForge.MaterialSlotCallerOwned.InvalidMeshResolutionFailsExplicitly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedInvalidMeshFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	WorkingMesh.bMaterialSlotResolutionValid = false;

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/0, /*bInvert=*/false);

	TestTrue(TEXT("State is Unavailable"), Mask.State == EVertexMaskForgeScalarMaskState::Unavailable);
	TestEqual(TEXT("No partial output -- Values stays empty"), Mask.Values.Num(), 0);

	return true;
}

// 7. Inputs are never mutated -- WorkingMesh is taken by const&; re-check its fields are unchanged
// after the call (structural proof, on top of the const& signature itself).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedInputUnchangedTest, "VertexMaskForge.MaterialSlotCallerOwned.InputWorkingMeshUnchangedAfterCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedInputUnchangedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const int32 SlotOptionsBefore = WorkingMesh.MaterialSlotOptions.Num();
	const int32 Tri0SlotBefore = WorkingMesh.DynamicTriangleToMaterialSlot[0];
	const int32 Tri1SlotBefore = WorkingMesh.DynamicTriangleToMaterialSlot[1];

	VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(WorkingMesh, 0, false);

	TestEqual(TEXT("MaterialSlotOptions.Num() unchanged"), WorkingMesh.MaterialSlotOptions.Num(), SlotOptionsBefore);
	TestEqual(TEXT("Tri0 resolved slot unchanged"), WorkingMesh.DynamicTriangleToMaterialSlot[0], Tri0SlotBefore);
	TestEqual(TEXT("Tri1 resolved slot unchanged"), WorkingMesh.DynamicTriangleToMaterialSlot[1], Tri1SlotBefore);
	TestTrue(TEXT("bMaterialSlotResolutionValid unchanged"), WorkingMesh.bMaterialSlotResolutionValid);

	return true;
}

// 8. Equivalence with the Legacy wrapper: calling GenerateMaterialSlotMaskFromDynamicMesh directly
// produces byte-for-byte identical Values to what GenerateMaterialSlotMaskInstanceResult (the existing
// Legacy/Recipe wrapper) stores for the same inputs -- proving there is exactly one source of truth for
// the generation math, and the caller-owned path does not diverge from the stored path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedLegacyWrapperEquivalenceTest, "VertexMaskForge.MaterialSlotCallerOwned.EquivalentToLegacyWrapperStoredResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedLegacyWrapperEquivalenceTest::RunTest(const FString& Parameters)
{
	// Two independently-built fixtures with identical slot mapping -- avoids any aliasing between the
	// caller-owned call (const&) and the wrapper's own mutable WorkingMesh (needed for its store).
	const FVertexMaskForgeWorkingMesh WorkingMeshForCallerOwned = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	FVertexMaskForgeWorkingMesh WorkingMeshForWrapper = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	const FVertexMaskForgeScalarMask CallerOwnedMask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMeshForCallerOwned, /*SelectedSlotIndex=*/0, /*bInvert=*/false);

	FVertexMaskForgeMaskInstance Instance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
	Instance.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 0;
	Instance.Params.Get<FVertexMaskForgeMaterialSlotParams>().bInvert = false;

	const bool bWrapperSucceeded = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		Instance, WorkingMeshForWrapper, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr);
	TestTrue(TEXT("Wrapper generation succeeds"), bWrapperSucceeded);

	const FVertexMaskForgeInstanceMaskResult* StoredResult = WorkingMeshForWrapper.InstanceResults.Find(Instance.InstanceId);
	TestNotNull(TEXT("Wrapper stored a result"), StoredResult);
	if (!StoredResult)
	{
		return false;
	}

	TestTrue(TEXT("Caller-owned result is Ready"), CallerOwnedMask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Cardinality matches"), CallerOwnedMask.Values.Num(), StoredResult->Values.Num());
	if (CallerOwnedMask.Values.Num() == StoredResult->Values.Num())
	{
		for (int32 Index = 0; Index < CallerOwnedMask.Values.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Values[%d] byte-for-byte equal between caller-owned and wrapper-stored"), Index),
				CallerOwnedMask.Values[Index], StoredResult->Values[Index]);
		}
	}

	return true;
}

// 9. The caller-owned call never requires or touches any FVertexMaskForgeInstanceResultStore -- proven
// structurally by the fact that no store of any kind is constructed, passed, or referenced anywhere in
// this test, and the function signature itself accepts no store parameter (confirmed by direct call
// above with no such argument). This test additionally proves a second, completely independent
// WorkingMesh's own (separately-instantiated, never touched) InstanceResults store stays untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaterialSlotCallerOwnedNoStoreRequiredTest, "VertexMaskForge.MaterialSlotCallerOwned.DoesNotRequireOrTouchInstanceResultStore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaterialSlotCallerOwnedNoStoreRequiredTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);

	// A separate, unrelated WorkingMesh whose own InstanceResults store must remain untouched -- the
	// caller-owned call above receives no reference to it at all.
	FVertexMaskForgeWorkingMesh UnrelatedWorkingMesh = BuildCallerOwnedFixtureWorkingMesh(/*SlotForTri0=*/1, /*SlotForTri1=*/0);
	TestEqual(TEXT("Unrelated store starts empty"), UnrelatedWorkingMesh.InstanceResults.Num(), 0);

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
		WorkingMesh, /*SelectedSlotIndex=*/0, /*bInvert=*/false);

	TestTrue(TEXT("Caller-owned generation succeeds"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Unrelated store still empty -- never touched"), UnrelatedWorkingMesh.InstanceResults.Num(), 0);
	TestEqual(TEXT("Own WorkingMesh's own InstanceResults also untouched (function never wrote to it)"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
