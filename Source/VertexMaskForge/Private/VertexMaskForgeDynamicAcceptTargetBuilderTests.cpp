// M16-K.6D-7A: automated coverage for VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets
// -- the non-panel Dynamic Source-Topology accept-target construction seam. See
// VertexMaskForgeDynamicAcceptTargetBuilder.h for the full module contract this file verifies.
//
// No production file is touched by this checkpoint; SVertexMaskForgePanel is not wired to this builder;
// WriteSourceTopologyAcceptTargets/BuildModifiedMeshes are never called from here -- these tests exercise
// the pure validate+compose function in isolation, exactly as it exists today (no production caller).

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "VertexMaskForgeDynamicAcceptTargetBuilder.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeDynamicSourceTopologyComposition.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

namespace
{
	/** Real, transient UObjects kept alive for the duration of a test. */
	struct FDynamicAcceptFixtureAsset
	{
		TStrongObjectPtr<UStaticMeshComponent> Component;
		TStrongObjectPtr<UStaticMesh> Mesh;

		FDynamicAcceptFixtureAsset()
			: Component(NewObject<UStaticMeshComponent>())
			, Mesh(NewObject<UStaticMesh>())
		{
			Component->SetStaticMesh(Mesh.Get());
		}
	};

	/** Real quad WorkingMesh: two triangles, Tri0 -> Slot 0, Tri1 -> Slot 1 -- same shape as the
	 *  M16-K.5J Dynamic composition integration fixture, reused here for continuity with the
	 *  orchestrator's own existing coverage. */
	FVertexMaskForgeWorkingMesh MakeQuadWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		const int32 V0 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0));
		const int32 V3 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));
		const int32 Tri0 = WorkingMesh.Mesh->AppendTriangle(V0, V1, V2);
		const int32 Tri1 = WorkingMesh.Mesh->AppendTriangle(V0, V2, V3);

		WorkingMesh.bMaterialSlotResolutionValid = true;
		WorkingMesh.bRenderVertexMaterialSlotAmbiguous = false;
		WorkingMesh.MaterialSlotOptions.SetNum(2);
		WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
		WorkingMesh.DynamicTriangleToMaterialSlot[Tri0] = 0;
		WorkingMesh.DynamicTriangleToMaterialSlot[Tri1] = 1;

		WorkingMesh.TriIDMap.SetNum(WorkingMesh.Mesh->MaxTriangleID());
		WorkingMesh.TriIDMap[Tri0] = FTriangleID(0);
		WorkingMesh.TriIDMap[Tri1] = FTriangleID(1);

		return WorkingMesh;
	}

	/** Builds and commits a real FMeshDescription on Mesh matching MakeQuadWorkingMesh's own topology
	 *  (2 triangles, source FTriangleID 0 and 1, each with exactly 3 valid Vertex Instances) -- required
	 *  for VertexMaskForgeAcceptTargetBuilder::ValidateSourceTopologyCorrespondence to succeed. */
	void CommitQuadMeshDescription(UStaticMesh& Mesh)
	{
		FMeshDescription Description;
		FStaticMeshAttributes(Description).Register();

		FMeshDescriptionBuilder Builder;
		Builder.SetMeshDescription(&Description);
		Builder.EnablePolyGroups();
		Builder.SetNumUVLayers(1);

		const FPolygonGroupID PolyGroup = Builder.AppendPolygonGroup();
		const FVertexID V0 = Builder.AppendVertex(FVector(0.0, 0.0, 0.0));
		const FVertexID V1 = Builder.AppendVertex(FVector(1.0, 0.0, 0.0));
		const FVertexID V2 = Builder.AppendVertex(FVector(1.0, 1.0, 0.0));
		const FVertexID V3 = Builder.AppendVertex(FVector(0.0, 1.0, 0.0));

		Builder.AppendTriangle(V0, V1, V2, PolyGroup);
		Builder.AppendTriangle(V0, V2, V3, PolyGroup);

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		BuildParams.bFastBuild = true;
		Mesh.BuildFromMeshDescriptions({ &Description }, BuildParams);
	}

	/** Assembles one fully real, valid FVertexMaskForgeSelectedMesh entry: real Mesh/Component, real
	 *  committed MeshDescription, a real installed WorkingMesh, and one PreviewComponents element with
	 *  the given captured BaselineColors (6 corners, quad topology). Returns the owning Asset alongside
	 *  the Entry so the caller can keep the UObjects alive for the test's duration. */
	TSharedPtr<FVertexMaskForgeSelectedMesh> MakeValidEntry(FDynamicAcceptFixtureAsset& Asset, const TArray<FColor>& BaselineColors, FAutomationTestBase& Test)
	{
		CommitQuadMeshDescription(*Asset.Mesh);

		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
		Entry->Mesh = Asset.Mesh.Get();
		Entry->AssetName = Asset.Mesh->GetName();
		Entry->bUseSourceTopology = true;

		const bool bIdentityConfigured = Entry->MeshOwner->ConfigureIdentity(Asset.Mesh.Get(), 0, /*bUseSourceTopology=*/true);
		Test.TestTrue(TEXT("ConfigureIdentity succeeds"), bIdentityConfigured);

		const bool bInstalled = Entry->MeshOwner->InstallWorkingMesh(MakeQuadWorkingMesh(), /*ExpectedCardinality=*/6);
		Test.TestTrue(TEXT("InstallWorkingMesh succeeds"), bInstalled);

		TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
		const bool bTargetConfigured = StateOwner->ConfigureTarget(Asset.Component.Get());
		Test.TestTrue(TEXT("ConfigureTarget succeeds"), bTargetConfigured);
		const bool bAttached = StateOwner->AttachToMeshOwner(Entry->MeshOwner);
		Test.TestTrue(TEXT("AttachToMeshOwner succeeds"), bAttached);
		const bool bInitialized = StateOwner->InitializeColors(BaselineColors);
		Test.TestTrue(TEXT("InitializeColors succeeds"), bInitialized);

		Entry->PreviewComponents.Add(MoveTemp(StateOwner));
		return Entry;
	}

	TArray<FColor> MakeDefaultBaseline()
	{
		return {
			FColor(10, 20, 30, 100),
			FColor(11, 21, 31, 101),
			FColor(12, 22, 32, 102),
			FColor(13, 23, 33, 103),
			FColor(14, 24, 34, 104),
			FColor(15, 25, 35, 105),
		};
	}

	/** One enabled White/Copy@1.0/all-channels layer, masked by Material Slot 0 (selected) -- the same
	 *  configuration as the M16-K.5J composition integration test, reused here for parity. */
	FGuid AddEnabledMaterialSlotWhiteLayer(FVertexMaskForgeDynamicLayerStack& Stack, int32 SelectedSlotIndex = 0)
	{
		const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
		Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(LayerId, 1.0f);
		Stack.SetLayerChannelFilter(LayerId, true, true, true);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		if (MaskInstance)
		{
			FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
			FVertexMaskForgeMaterialSlotParams& SlotParams = NewParams.Get<FVertexMaskForgeMaterialSlotParams>();
			SlotParams.SelectedSlotIndex = SelectedSlotIndex;
			SlotParams.bInvert = false;
			Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);
		}
		return LayerId;
	}

	/** One enabled White/Copy@1.0/all-channels layer with Generator Type=None (Layer.Mask left unset). */
	FGuid AddEnabledNoneWhiteLayer(FVertexMaskForgeDynamicLayerStack& Stack)
	{
		const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
		Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(LayerId, 1.0f);
		Stack.SetLayerChannelFilter(LayerId, true, true, true);
		return LayerId;
	}

	// M16-K.6D-8G-E: same quad topology/Material-Slot/TriIDMap setup as MakeQuadWorkingMesh, PLUS a real
	// Normal Overlay (EnableAttributes()/PrimaryNormals(), one independent element per corner) --
	// Ambient Occlusion requires a Normal Overlay to report anything other than Unavailable, which
	// MakeQuadWorkingMesh deliberately never provides (the other five generators need none).
	FVertexMaskForgeWorkingMesh MakeQuadWorkingMeshWithNormalOverlay()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh = MakeQuadWorkingMesh();

		WorkingMesh.Mesh->EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();
		const FVector3f CornerNormals[6] = {
			FVector3f(0.0f, 0.0f, 1.0f),
			FVector3f(0.3f, 0.0f, 0.95f).GetSafeNormal(),
			FVector3f(0.0f, 0.3f, 0.95f).GetSafeNormal(),
			FVector3f(-0.3f, 0.0f, 0.95f).GetSafeNormal(),
			FVector3f(0.0f, -0.3f, 0.95f).GetSafeNormal(),
			FVector3f(0.2f, 0.2f, 0.96f).GetSafeNormal(),
		};
		int32 CornerIndex = 0;
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(CornerNormals[CornerIndex]);
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		}
		WorkingMesh.GeometryFingerprint = 1;
		return WorkingMesh;
	}

	/** Same contract as MakeValidEntry, but installs MakeQuadWorkingMeshWithNormalOverlay instead, so a
	 *  real Ambient Occlusion layer can be evaluated. */
	TSharedPtr<FVertexMaskForgeSelectedMesh> MakeValidEntryWithNormalOverlay(FDynamicAcceptFixtureAsset& Asset, const TArray<FColor>& BaselineColors, FAutomationTestBase& Test)
	{
		CommitQuadMeshDescription(*Asset.Mesh);

		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
		Entry->Mesh = Asset.Mesh.Get();
		Entry->AssetName = Asset.Mesh->GetName();
		Entry->bUseSourceTopology = true;

		const bool bIdentityConfigured = Entry->MeshOwner->ConfigureIdentity(Asset.Mesh.Get(), 0, /*bUseSourceTopology=*/true);
		Test.TestTrue(TEXT("ConfigureIdentity succeeds"), bIdentityConfigured);

		const bool bInstalled = Entry->MeshOwner->InstallWorkingMesh(MakeQuadWorkingMeshWithNormalOverlay(), /*ExpectedCardinality=*/6);
		Test.TestTrue(TEXT("InstallWorkingMesh succeeds"), bInstalled);

		TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
		const bool bTargetConfigured = StateOwner->ConfigureTarget(Asset.Component.Get());
		Test.TestTrue(TEXT("ConfigureTarget succeeds"), bTargetConfigured);
		const bool bAttached = StateOwner->AttachToMeshOwner(Entry->MeshOwner);
		Test.TestTrue(TEXT("AttachToMeshOwner succeeds"), bAttached);
		const bool bInitialized = StateOwner->InitializeColors(BaselineColors);
		Test.TestTrue(TEXT("InitializeColors succeeds"), bInitialized);

		Entry->PreviewComponents.Add(MoveTemp(StateOwner));
		return Entry;
	}

	/** One enabled White/Copy@1.0/all-channels layer, masked by Ambient Occlusion. */
	FGuid AddEnabledAmbientOcclusionWhiteLayer(FVertexMaskForgeDynamicLayerStack& Stack)
	{
		const FGuid LayerId = Stack.AddLayer(TEXT("AO Layer"));
		Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(LayerId, 1.0f);
		Stack.SetLayerChannelFilter(LayerId, true, true, true);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::AmbientOcclusion);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		if (MaskInstance)
		{
			FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::AmbientOcclusion);
			FVertexMaskForgeAmbientOcclusionParams& AOParams = NewParams.Get<FVertexMaskForgeAmbientOcclusionParams>();
			AOParams.Samples = 16;
			AOParams.MaxDistance = 100.0f;
			AOParams.Bias = 0.1f;
			Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);
		}
		return LayerId;
	}
}

// 1/2/3. One valid component produces one target, byte-exact against the direct orchestrator call, with
// Alpha matching Baseline Alpha.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderValidComponentTest, "VertexMaskForge.DynamicAcceptTargetBuilder.OneValidComponentMatchesOrchestratorByteExact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderValidComponentTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, /*SelectedSlotIndex=*/0);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1"), Targets.Num(), 1);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}

	TestEqual(TEXT("Target.Mesh matches"), Targets[0].Mesh.Get(), Asset.Mesh.Get());
	TestEqual(TEXT("Target.AssetName matches"), Targets[0].AssetName, Entry->AssetName);
	TestEqual(TEXT("Target.Entry matches"), Targets[0].Entry, Entry);

	// Direct orchestrator call, same inputs -- byte-exact comparison, no duplicated formula.
	TArray<FColor> ExpectedColors;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ExpectedColorsAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ExpectedColorsThicknessCaches;
	const bool bComposed = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		Entry->MeshOwner->GetWorkingMesh(), Stack, Baseline, FTransform::Identity, ExpectedColorsAOCaches, ExpectedColorsThicknessCaches, ExpectedColors);
	TestTrue(TEXT("Direct orchestrator call succeeds"), bComposed);

	TestEqual(TEXT("FinalColors.Num() == ExpectedColors.Num()"), Targets[0].FinalColors.Num(), ExpectedColors.Num());
	if (Targets[0].FinalColors.Num() != ExpectedColors.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < ExpectedColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] byte-exact against orchestrator"), Index), Targets[0].FinalColors[Index], ExpectedColors[Index]);
		TestEqual(*FString::Printf(TEXT("FinalColors[%d].A matches Baseline Alpha"), Index), Targets[0].FinalColors[Index].A, Baseline[Index].A);
	}
	return true;
}

// 4. Empty stack succeeds with byte-exact Baseline passthrough.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderEmptyStackTest, "VertexMaskForge.DynamicAcceptTargetBuilder.EmptyStackProducesBaselinePassthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderEmptyStackTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack EmptyStack;
	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, EmptyStack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds for an empty stack"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1"), Targets.Num(), 1);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("FinalColors.Num() == Baseline.Num()"), Targets[0].FinalColors.Num(), Baseline.Num());
	for (int32 Index = 0; Index < Baseline.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] byte-exact Baseline passthrough"), Index), Targets[0].FinalColors[Index], Baseline[Index]);
	}
	return true;
}

// 5. All-disabled stack succeeds with byte-exact Baseline passthrough.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderAllDisabledStackTest, "VertexMaskForge.DynamicAcceptTargetBuilder.AllDisabledStackProducesBaselinePassthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderAllDisabledStackTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddEnabledMaterialSlotWhiteLayer(Stack, 0);
	const bool bDisabled = Stack.SetLayerEnabled(LayerId, false);
	TestTrue(TEXT("SetLayerEnabled(false) succeeds"), bDisabled);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds for an all-disabled stack"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1"), Targets.Num(), 1);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}
	for (int32 Index = 0; Index < Baseline.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] byte-exact Baseline passthrough"), Index), Targets[0].FinalColors[Index], Baseline[Index]);
	}
	return true;
}

// 6. Enabled Generator Type=None Fill produces the expected (full-coverage) target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderNoneGeneratorTest, "VertexMaskForge.DynamicAcceptTargetBuilder.EnabledNoneGeneratorFillProducesExpectedTarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderNoneGeneratorTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledNoneWhiteLayer(Stack);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1"), Targets.Num(), 1);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}

	TArray<FColor> ExpectedColors;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ExpectedColorsAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ExpectedColorsThicknessCaches;
	VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		Entry->MeshOwner->GetWorkingMesh(), Stack, Baseline, FTransform::Identity, ExpectedColorsAOCaches, ExpectedColorsThicknessCaches, ExpectedColors);
	for (int32 Index = 0; Index < ExpectedColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] matches expected None-generator (full coverage) White"), Index), Targets[0].FinalColors[Index], ExpectedColors[Index]);
		TestEqual(*FString::Printf(TEXT("FinalColors[%d].RGB == 255 (full White, unmasked)"), Index), Targets[0].FinalColors[Index].R, uint8(255));
	}
	return true;
}

// 7. Enabled Material Slot masked Fill produces the expected target (covered corners White, uncovered
// corners unchanged from Baseline -- same shape as the M16-K.5J integration test's own expectation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderMaterialSlotMaskedTest, "VertexMaskForge.DynamicAcceptTargetBuilder.EnabledMaterialSlotMaskedFillProducesExpectedTarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderMaterialSlotMaskedTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, /*SelectedSlotIndex=*/0); // Tri0 (corners 0-2) selected, Tri1 (corners 3-5) not.

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds"), bBuilt);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}

	// Copy blend at Opacity 1.0 fully overwrites RGB with PaintValue = Fill * EffectiveMask, even where
	// EffectiveMask is 0 (uncovered corners become black, not a Baseline-RGB pass-through) -- matching
	// VertexMaskForgeDynamicSourceTopologyComposition's own documented per-corner fold, and byte-identical
	// to the M16-K.5J composition integration test's own ExpectedOut for this same layer configuration.
	// Alpha always passes through from Baseline, uncovered or not.
	const FColor ExpectedOut[6] = {
		FColor(255, 255, 255, 100),
		FColor(255, 255, 255, 101),
		FColor(255, 255, 255, 102),
		FColor(0, 0, 0, 103),
		FColor(0, 0, 0, 104),
		FColor(0, 0, 0, 105),
	};
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] matches expected masked composite"), Index), Targets[0].FinalColors[Index], ExpectedOut[Index]);
	}
	return true;
}

// 8. Blend, Opacity, Invert, and reorder are represented purely through orchestrator output -- proven by
// a direct byte-exact comparison against the orchestrator for a non-trivial multi-layer stack, without
// any duplicated blend formula in this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderMultiLayerParityTest, "VertexMaskForge.DynamicAcceptTargetBuilder.MultiLayerBlendOpacityInvertReorderMatchesOrchestrator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderMultiLayerParityTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	// Layer A: Material Slot 0, White, Multiply, Opacity 0.5, Invert=false.
	const FGuid LayerA = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerFill(LayerA, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerA, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(LayerA, 0.5f);
	Stack.SetLayerChannelFilter(LayerA, true, true, false);
	Stack.SetLayerMaskGeneratorType(LayerA, EVertexMaskForgeGeneratorType::MaterialSlot);
	if (const FVertexMaskForgeGeneratorMaskInstance* MaskA = Stack.GetLayerMask(LayerA))
	{
		FVertexMaskForgeGeneratorParams ParamsA = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
		ParamsA.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1;
		ParamsA.Get<FVertexMaskForgeMaterialSlotParams>().bInvert = true; // Invert exercised here.
		Stack.SetLayerMaskParams(LayerA, MaskA->MaskInstanceId, ParamsA);
	}

	// Layer B: Generator None, Black, Overlay, Opacity 0.25, Blue channel only.
	const FGuid LayerB = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerFill(LayerB, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(LayerB, EVertexMaskForgeBlendMode::Overlay);
	Stack.SetLayerOpacity(LayerB, 0.25f);
	Stack.SetLayerChannelFilter(LayerB, false, false, true);

	// Reorder: B before A.
	const bool bMoved = Stack.MoveLayer(LayerB, 0);
	TestTrue(TEXT("MoveLayer succeeds"), bMoved);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds"), bBuilt);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}

	TArray<FColor> ExpectedColors;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ExpectedColorsAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ExpectedColorsThicknessCaches;
	const bool bComposed = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		Entry->MeshOwner->GetWorkingMesh(), Stack, Baseline, FTransform::Identity, ExpectedColorsAOCaches, ExpectedColorsThicknessCaches, ExpectedColors);
	TestTrue(TEXT("Direct orchestrator call succeeds"), bComposed);
	for (int32 Index = 0; Index < ExpectedColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] byte-exact against orchestrator for multi-layer stack"), Index), Targets[0].FinalColors[Index], ExpectedColors[Index]);
	}
	return true;
}

// 9. Two components sharing one asset with identical output coalesce safely into a single target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderAgreeingComponentsTest, "VertexMaskForge.DynamicAcceptTargetBuilder.TwoComponentsSameAssetAgreeingOutputCoalesce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderAgreeingComponentsTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	// A second component on the SAME asset/mesh owner, with an identical captured Baseline.
	TStrongObjectPtr<UStaticMeshComponent> SecondComponent(NewObject<UStaticMeshComponent>());
	SecondComponent->SetStaticMesh(Asset.Mesh.Get());
	TUniquePtr<FVertexMaskForgeWorkingStateOwner> SecondOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("Second ConfigureTarget succeeds"), SecondOwner->ConfigureTarget(SecondComponent.Get()));
	TestTrue(TEXT("Second AttachToMeshOwner succeeds"), SecondOwner->AttachToMeshOwner(Entry->MeshOwner));
	TestTrue(TEXT("Second InitializeColors succeeds"), SecondOwner->InitializeColors(Baseline));
	Entry->PreviewComponents.Add(MoveTemp(SecondOwner));

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, 0);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds with two agreeing components"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1 (coalesced, not duplicated)"), Targets.Num(), 1);
	return bBuilt && Targets.Num() == 1;
}

// 10. Two components sharing one asset with divergent output reject the entire operation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderDivergentComponentsTest, "VertexMaskForge.DynamicAcceptTargetBuilder.TwoComponentsSameAssetDivergentOutputRejectsWhole", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderDivergentComponentsTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	// A second component on the SAME asset, but with a DIFFERENT captured Baseline -- diverges once
	// composed. The configured layer below (Copy blend, Opacity 1.0, all channels) fully overwrites RGB
	// with PaintValue regardless of Baseline RGB, so an RGB-only divergence would NOT survive
	// composition; Alpha is the one channel the orchestrator always passes through from Baseline
	// unmodified, so it is the divergence this fixture must use to produce different FINAL composed
	// output (see ComputeComposedColorsRGBSourceTopology's own `FinalColor.W = BaseColor.W` contract).
	TArray<FColor> DivergentBaseline = Baseline;
	DivergentBaseline[0].A = 250;

	TStrongObjectPtr<UStaticMeshComponent> SecondComponent(NewObject<UStaticMeshComponent>());
	SecondComponent->SetStaticMesh(Asset.Mesh.Get());
	TUniquePtr<FVertexMaskForgeWorkingStateOwner> SecondOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("Second ConfigureTarget succeeds"), SecondOwner->ConfigureTarget(SecondComponent.Get()));
	TestTrue(TEXT("Second AttachToMeshOwner succeeds"), SecondOwner->AttachToMeshOwner(Entry->MeshOwner));
	TestTrue(TEXT("Second InitializeColors succeeds"), SecondOwner->InitializeColors(DivergentBaseline));
	Entry->PreviewComponents.Add(MoveTemp(SecondOwner));

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, 0);

	// Sentinel: pre-populated with a value that must survive the rejected call untouched.
	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget Sentinel;
	Sentinel.AssetName = TEXT("__SENTINEL__");
	Targets.Add(Sentinel);

	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestFalse(TEXT("BuildSourceTopologyAcceptTargets fails on divergent same-asset components"), bBuilt);
	TestFalse(TEXT("OutErrorText is non-empty on failure"), ErrorText.IsEmpty());
	TestEqual(TEXT("OutTargets left completely unchanged (sentinel preserved) on failure"), Targets.Num(), 1);
	if (Targets.Num() == 1)
	{
		TestEqual(TEXT("Sentinel AssetName unchanged"), Targets[0].AssetName, FString(TEXT("__SENTINEL__")));
	}
	return !bBuilt;
}

// 11. Topology/cardinality mismatch fails (Baseline captured for a different corner count than the
// current WorkingMesh).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderCardinalityMismatchTest, "VertexMaskForge.DynamicAcceptTargetBuilder.CardinalityMismatchFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderCardinalityMismatchTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	CommitQuadMeshDescription(*Asset.Mesh);

	TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
	Entry->Mesh = Asset.Mesh.Get();
	Entry->AssetName = Asset.Mesh->GetName();
	Entry->bUseSourceTopology = true;
	TestTrue(TEXT("ConfigureIdentity succeeds"), Entry->MeshOwner->ConfigureIdentity(Asset.Mesh.Get(), 0, true));
	TestTrue(TEXT("InstallWorkingMesh succeeds"), Entry->MeshOwner->InstallWorkingMesh(MakeQuadWorkingMesh(), 6));

	TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("ConfigureTarget succeeds"), StateOwner->ConfigureTarget(Asset.Component.Get()));
	TestTrue(TEXT("AttachToMeshOwner succeeds"), StateOwner->AttachToMeshOwner(Entry->MeshOwner));
	// Wrong cardinality: 4 colors captured for a 6-corner quad WorkingMesh.
	const TArray<FColor> WrongCardinalityBaseline = { FColor::Red, FColor::Green, FColor::Blue, FColor::White };
	const bool bInitialized = StateOwner->InitializeColors(WrongCardinalityBaseline);
	Entry->PreviewComponents.Add(MoveTemp(StateOwner));

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, 0);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);

	if (bInitialized)
	{
		// If InitializeColors itself accepted the mismatched array, the orchestrator's own
		// BaseColors.Num() != ExpectedCornerCount guard must reject the whole build instead.
		TestFalse(TEXT("BuildSourceTopologyAcceptTargets fails on cardinality mismatch"), bBuilt);
		TestEqual(TEXT("Targets left empty on failure"), Targets.Num(), 0);
	}
	else
	{
		// If InitializeColors itself rejected the mismatched array, colors were never initialized, so
		// this component is skipped (not fatal) and the entry has no eligible component -> no target.
		TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds (component skipped, none eligible)"), bBuilt);
		TestEqual(TEXT("Targets.Num() == 0 (nothing eligible for this entry)"), Targets.Num(), 0);
	}
	return true;
}

// 12. Unsupported/failed mask generation fails (a masked layer whose GeneratorType is not MaterialSlot
// is explicitly unsupported by the orchestrator's own Pass 1 -- see
// VertexMaskForgeDynamicSourceTopologyComposition.cpp).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderUnsupportedGeneratorTest, "VertexMaskForge.DynamicAcceptTargetBuilder.UnsupportedMaskGeneratorFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderUnsupportedGeneratorTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Unsupported"));
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(LayerId, 1.0f);
	Stack.SetLayerChannelFilter(LayerId, true, true, true);
	Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::BoundingBox);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget Sentinel;
	Sentinel.AssetName = TEXT("__SENTINEL__");
	Targets.Add(Sentinel);

	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestFalse(TEXT("BuildSourceTopologyAcceptTargets fails for an unsupported masked generator type"), bBuilt);
	TestFalse(TEXT("OutErrorText is non-empty on failure"), ErrorText.IsEmpty());
	TestEqual(TEXT("OutTargets left completely unchanged (sentinel preserved) on failure"), Targets.Num(), 1);
	return !bBuilt;
}

// 13. On every failure path exercised above, OutTargets is left completely unchanged -- consolidated
// proof across the divergent-component and unsupported-generator failure paths (both already assert this
// individually); this test adds one more explicit failure path: an entry whose Mesh cannot be resolved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderInvalidMeshSentinelTest, "VertexMaskForge.DynamicAcceptTargetBuilder.InvalidMeshFailureLeavesOutTargetsUnchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderInvalidMeshSentinelTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);
	// Invalidate the soft reference after fixture setup -- LoadSynchronous() will fail to resolve.
	Entry->Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/DoesNotExist.DoesNotExist")));

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledMaterialSlotWhiteLayer(Stack, 0);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget Sentinel;
	Sentinel.AssetName = TEXT("__SENTINEL__");
	Targets.Add(Sentinel);

	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestFalse(TEXT("BuildSourceTopologyAcceptTargets fails for an unresolvable Mesh"), bBuilt);
	TestEqual(TEXT("OutTargets left completely unchanged (sentinel preserved) on failure"), Targets.Num(), 1);
	if (Targets.Num() == 1)
	{
		TestEqual(TEXT("Sentinel AssetName unchanged"), Targets[0].AssetName, FString(TEXT("__SENTINEL__")));
	}
	return !bBuilt;
}

// 14. The builder does not mutate DynamicLayerStack, Baseline colors, Legacy WorkingColors,
// Legacy SourceTopologyWorkingColors, or CommittedColors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderNonMutationTest, "VertexMaskForge.DynamicAcceptTargetBuilder.DoesNotMutateStackOrLegacyColorState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderNonMutationTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntry(Asset, Baseline, *this);

	FVertexMaskForgeWorkingStateOwner& StateOwner = *Entry->PreviewComponents[0];
	const TArray<FColor> BaselineBefore = StateOwner.GetBaselineColors();
	const TArray<FColor> WorkingBefore = StateOwner.GetWorkingColors();
	const TArray<FColor> CommittedBefore = StateOwner.GetCommittedColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddEnabledMaterialSlotWhiteLayer(Stack, 0);
	const int32 LayerCountBefore = Stack.GetLayers().Num();
	const FVertexMaskForgeLayer* LayerBefore = Stack.FindLayerById(LayerId);
	TestNotNull(TEXT("Layer exists before build"), LayerBefore);
	const EVertexMaskForgeLayerFill FillBefore = LayerBefore ? LayerBefore->Fill : EVertexMaskForgeLayerFill::None;
	const float OpacityBefore = LayerBefore ? LayerBefore->Opacity : -1.0f;

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds"), bBuilt);

	TestEqual(TEXT("Stack.GetLayers().Num() unchanged"), Stack.GetLayers().Num(), LayerCountBefore);
	const FVertexMaskForgeLayer* LayerAfter = Stack.FindLayerById(LayerId);
	TestNotNull(TEXT("Layer still exists after build"), LayerAfter);
	if (LayerAfter)
	{
		TestEqual(TEXT("Layer.Fill unchanged"), LayerAfter->Fill, FillBefore);
		TestEqual(TEXT("Layer.Opacity unchanged"), LayerAfter->Opacity, OpacityBefore);
	}

	TestEqual(TEXT("BaselineColors.Num() unchanged"), StateOwner.GetBaselineColors().Num(), BaselineBefore.Num());
	TestEqual(TEXT("WorkingColors.Num() unchanged"), StateOwner.GetWorkingColors().Num(), WorkingBefore.Num());
	TestEqual(TEXT("CommittedColors.Num() unchanged"), StateOwner.GetCommittedColors().Num(), CommittedBefore.Num());
	for (int32 Index = 0; Index < BaselineBefore.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("BaselineColors[%d] byte-exact unchanged"), Index), StateOwner.GetBaselineColors()[Index], BaselineBefore[Index]);
	}
	for (int32 Index = 0; Index < WorkingBefore.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("WorkingColors[%d] byte-exact unchanged (never written by this builder)"), Index), StateOwner.GetWorkingColors()[Index], WorkingBefore[Index]);
	}
	for (int32 Index = 0; Index < CommittedBefore.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("CommittedColors[%d] byte-exact unchanged (never written by this builder)"), Index), StateOwner.GetCommittedColors()[Index], CommittedBefore[Index]);
	}
	return true;
}

// M16-K.6D-8G-E: a real, enabled Ambient Occlusion layer through Accept -- cold cache, byte-exact against
// a direct orchestrator call using an independent (empty) cache map. Proves Accept recomposes from
// Baseline (never a preview shortcut), passes the real component transform (Identity here, since a
// freshly-constructed, unattached UStaticMeshComponent's own transform IS Identity -- never substituted),
// and passes a valid cache map for the AO branch to consume on a genuine cold cache.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderAOColdCacheTest, "VertexMaskForge.DynamicAcceptTargetBuilder.AmbientOcclusionColdCacheMatchesDirectOrchestrator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderAOColdCacheTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntryWithNormalOverlay(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledAmbientOcclusionWhiteLayer(Stack);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> Targets;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, Targets, ErrorText);
	TestTrue(TEXT("BuildSourceTopologyAcceptTargets succeeds for a real AO layer (cold cache)"), bBuilt);
	TestEqual(TEXT("Targets.Num() == 1"), Targets.Num(), 1);
	if (!bBuilt || Targets.Num() != 1)
	{
		return false;
	}

	// Direct orchestrator call, same Baseline/stack/mesh/transform, its own INDEPENDENT (empty, cold)
	// cache map -- AO cache hit/miss never changes numeric output, only performance, so this remains a
	// valid byte-exact oracle regardless of which map either side used.
	TArray<FColor> ExpectedColors;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ExpectedColorsAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ExpectedColorsThicknessCaches;
	const bool bComposed = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		Entry->MeshOwner->GetWorkingMesh(), Stack, Baseline, FTransform::Identity, ExpectedColorsAOCaches, ExpectedColorsThicknessCaches, ExpectedColors);
	TestTrue(TEXT("Direct orchestrator call succeeds"), bComposed);

	TestEqual(TEXT("FinalColors.Num() == ExpectedColors.Num()"), Targets[0].FinalColors.Num(), ExpectedColors.Num());
	if (Targets[0].FinalColors.Num() != ExpectedColors.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < ExpectedColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("FinalColors[%d] byte-exact against orchestrator (AO, cold cache)"), Index), Targets[0].FinalColors[Index], ExpectedColors[Index]);
		TestEqual(*FString::Printf(TEXT("FinalColors[%d].A matches Baseline Alpha"), Index), Targets[0].FinalColors[Index].A, Baseline[Index].A);
	}
	return true;
}

// M16-K.6D-8G-E: the SAME entry's persistent component-owned AO cache map (StateOwner->
// GetVisualSessionStateMutable().DynamicSourceTopologyAOCachesByLayerId) is warm on a second Accept call
// -- cold and warm Accept results must be byte-identical, proving Accept's own cache consumption (wired
// in M16-K.6D-8G-D) works correctly on reuse, never just on a fresh/cold map.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicAcceptTargetBuilderAOWarmCacheTest, "VertexMaskForge.DynamicAcceptTargetBuilder.AmbientOcclusionWarmCacheMatchesColdCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicAcceptTargetBuilderAOWarmCacheTest::RunTest(const FString& Parameters)
{
	FDynamicAcceptFixtureAsset Asset;
	const TArray<FColor> Baseline = MakeDefaultBaseline();
	const TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeValidEntryWithNormalOverlay(Asset, Baseline, *this);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddEnabledAmbientOcclusionWhiteLayer(Stack);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> ColdTargets;
	FText ColdErrorText;
	const bool bColdBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, ColdTargets, ColdErrorText);
	TestTrue(TEXT("Cold Accept build succeeds"), bColdBuilt);
	TestEqual(TEXT("Cold Targets.Num() == 1"), ColdTargets.Num(), 1);

	TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget> WarmTargets;
	FText WarmErrorText;
	const bool bWarmBuilt = VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets({ Entry }, Stack, WarmTargets, WarmErrorText);
	TestTrue(TEXT("Warm Accept build (same Entry, same persistent AO cache map) succeeds"), bWarmBuilt);
	TestEqual(TEXT("Warm Targets.Num() == 1"), WarmTargets.Num(), 1);

	if (!bColdBuilt || !bWarmBuilt || ColdTargets.Num() != 1 || WarmTargets.Num() != 1)
	{
		return false;
	}

	TestEqual(TEXT("Cold and warm FinalColors counts match"), ColdTargets[0].FinalColors.Num(), WarmTargets[0].FinalColors.Num());
	for (int32 Index = 0; Index < FMath::Min(ColdTargets[0].FinalColors.Num(), WarmTargets[0].FinalColors.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("ColdTargets[0].FinalColors[%d] == WarmTargets[0].FinalColors[%d]"), Index, Index), ColdTargets[0].FinalColors[Index], WarmTargets[0].FinalColors[Index]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
