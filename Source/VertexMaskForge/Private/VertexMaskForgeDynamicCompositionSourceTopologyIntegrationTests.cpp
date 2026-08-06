// M16-K.5J: real end-to-end proof that VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors produces
// byte-exact output when its BaseColors come from a real FVertexMaskForgeWorkingStateOwner's own
// SourceTopologyBaselineColors (via the domain-mirrored GetBaselineColors() accessor) and its ResultStore
// comes from a real FVertexMaskForgeWorkingMesh.InstanceResults populated by the real
// VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance production path --
// never a hand-built FVertexMaskForgeInstanceMaskResult, never a literal TArray<FColor> substituted for a
// real WorkingStateOwner's own baseline.
//
// Domain scope (deliberately limited, matching every existing GenerateStoredResultForMaterialSlotInstance
// test in this codebase): Source-Topology (triangle-corner) only, bUseSourceTopology=true, LOD0=nullptr.
// This test does NOT exercise the render-vertex domain -- no fixture for a real FStaticMeshLODResources
// exists anywhere in this codebase's test suite, so inventing one here was explicitly rejected (see the
// M16-K.5I-R2/R3 reports). No production file is touched by this checkpoint; WorkingColors is never
// written; ApplyComposedColorsRGB is never called; Dynamic is not wired to SVertexMaskForgePanel.

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerBatchCompositor.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeDynamicMaskGeneration.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

namespace
{
	/** Keeps real, transient UObjects alive for the duration of this test. SetStaticMesh is called
	 *  explicitly below (in the test body, not here) so the fixture represents a semantically coherent
	 *  Component <-> StaticMesh identity, even though M16-K.5I-R3 confirmed by direct source read that
	 *  neither ConfigureTarget nor AttachToMeshOwner actually requires or checks this association. */
	struct FSourceTopologyIntegrationFixture
	{
		TStrongObjectPtr<UStaticMeshComponent> Component;
		TStrongObjectPtr<UStaticMesh> Mesh;

		FSourceTopologyIntegrationFixture()
			: Component(NewObject<UStaticMeshComponent>())
			, Mesh(NewObject<UStaticMesh>())
		{
		}
	};
}

// 1. RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicCompositionSourceTopologyIntegrationTest, "VertexMaskForge.DynamicCompositionSourceTopologyIntegration.RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicCompositionSourceTopologyIntegrationTest::RunTest(const FString& Parameters)
{
	FSourceTopologyIntegrationFixture Fixture;
	TestTrue(TEXT("Fixture.Component is valid"), IsValid(Fixture.Component.Get()));
	TestTrue(TEXT("Fixture.Mesh is valid"), IsValid(Fixture.Mesh.Get()));
	if (!IsValid(Fixture.Component.Get()) || !IsValid(Fixture.Mesh.Get()))
	{
		return false;
	}

	// Explicit, semantically coherent Component <-> StaticMesh association -- not required by
	// ConfigureTarget/AttachToMeshOwner's own validation (confirmed by direct source read in
	// M16-K.5I-R3), but the fixture should not represent an artificially inconsistent identity.
	Fixture.Component->SetStaticMesh(Fixture.Mesh.Get());
	const bool bStaticMeshAssociated = Fixture.Component->GetStaticMesh().Get() == Fixture.Mesh.Get();
	TestTrue(TEXT("Component's StaticMesh matches Fixture.Mesh after SetStaticMesh"), bStaticMeshAssociated);
	if (!bStaticMeshAssociated)
	{
		return false;
	}

	// --- Real Dynamic Layer stack, one layer, White Copy@1, all channels affected. ---
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("LayerId is valid"), LayerId.IsValid());
	if (!LayerId.IsValid())
	{
		return false;
	}

	const bool bFillSet = Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	TestTrue(TEXT("SetLayerFill(White) succeeds"), bFillSet);
	if (!bFillSet)
	{
		return false;
	}

	const bool bBlendModeSet = Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	TestTrue(TEXT("SetLayerBlendMode(Copy) succeeds"), bBlendModeSet);
	if (!bBlendModeSet)
	{
		return false;
	}

	const bool bOpacitySet = Stack.SetLayerOpacity(LayerId, 1.0f);
	TestTrue(TEXT("SetLayerOpacity(1.0) succeeds"), bOpacitySet);
	if (!bOpacitySet)
	{
		return false;
	}

	const bool bChannelFilterSet = Stack.SetLayerChannelFilter(LayerId, true, true, true, false);
	TestTrue(TEXT("SetLayerChannelFilter(true,true,true,false) succeeds"), bChannelFilterSet);
	if (!bChannelFilterSet)
	{
		return false;
	}

	const bool bGeneratorTypeSet = Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot);
	TestTrue(TEXT("SetLayerMaskGeneratorType(MaterialSlot) succeeds"), bGeneratorTypeSet);
	if (!bGeneratorTypeSet)
	{
		return false;
	}

	// bEnabled=tuue is AddLayeu's own confiumed default (M16-K.5I-R3); no public accessou exists to uead
	// it back without going through FindLayerById's returned FVertexMaskForgeLayer pointer, which is
	// already the layer this test configures below -- the composed output's own byte-exact match is the
	// functional proof this layer participated (a disabled layer would leave Out untouched at BaseColor).
	const FVertexMaskForgeLayer* LayerForEnabledCheck = Stack.FindLayerById(LayerId);
	TestNotNull(TEXT("FindLayerById returns the layer just configured"), LayerForEnabledCheck);
	if (!LayerForEnabledCheck)
	{
		return false;
	}
	TestTrue(TEXT("Layer bEnabled is true (AddLayer's own default, never explicitly disabled)"), LayerForEnabledCheck->bEnabled);

	const FVertexMaskForgeGeneratorMaskInstance* MaskInstanceBeforeParams = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("GetLayerMask non-null after SetLayerMaskGeneratorType"), MaskInstanceBeforeParams);
	if (!MaskInstanceBeforeParams)
	{
		return false;
	}
	const FGuid MaskInstanceId = MaskInstanceBeforeParams->MaskInstanceId;
	TestTrue(TEXT("MaskInstanceId is valid"), MaskInstanceId.IsValid());
	if (!MaskInstanceId.IsValid())
	{
		return false;
	}

	// --- Explicit Material Slot params, applied via the controlled mutator. ---
	FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
	FVertexMaskForgeMaterialSlotParams& SlotParams = NewParams.Get<FVertexMaskForgeMaterialSlotParams>();
	SlotParams.SelectedSlotIndex = 0;
	SlotParams.bInvert = false;

	const bool bMaskParamsSet = Stack.SetLayerMaskParams(LayerId, MaskInstanceId, NewParams);
	TestTrue(TEXT("SetLayerMaskParams succeeds"), bMaskParamsSet);
	if (!bMaskParamsSet)
	{
		return false;
	}

	const FVertexMaskForgeGeneratorMaskInstance* GeneratorMaskInstance = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("GeneratorMaskInstance present after SetLayerMaskParams"), GeneratorMaskInstance);
	if (!GeneratorMaskInstance)
	{
		return false;
	}
	TestEqual(TEXT("MaskInstanceId preserved by SetLayerMaskParams"), GeneratorMaskInstance->MaskInstanceId, MaskInstanceId);
	if (GeneratorMaskInstance->MaskInstanceId != MaskInstanceId)
	{
		return false;
	}

	// --- Real, standalone WorkingMesh: two triangles, Tri0 -> Slot 0 (selected), Tri1 -> Slot 1 (not
	// selected). Generated into a LOCAL mesh (never the mesh owner's own installed WorkingMesh --
	// FVertexMaskForgeWorkingMeshOwner::GetWorkingMesh() has no mutable overload) and only afterward moved
	// into the owner via InstallWorkingMesh. ---
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

	// --- Real generation -- Source-Topology domain, exactly once, AFTER MaskInstanceId was obtained. ---
	const bool bGenerated = VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance(
		*GeneratorMaskInstance, WorkingMesh, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr);
	TestTrue(TEXT("GenerateStoredResultForMaterialSlotInstance returns true"), bGenerated);
	if (!bGenerated)
	{
		return false;
	}

	TestTrue(TEXT("Local WorkingMesh.InstanceResults contains MaskInstanceId"), WorkingMesh.InstanceResults.Contains(MaskInstanceId));
	const FVertexMaskForgeInstanceMaskResult* LocalResult = WorkingMesh.InstanceResults.Find(MaskInstanceId);
	TestNotNull(TEXT("Local stored result is non-null"), LocalResult);
	if (!LocalResult)
	{
		return false;
	}
	TestEqual(TEXT("Local Values.Num() == 6"), LocalResult->Values.Num(), 6);
	TestEqual(TEXT("Local bHasValue.Num() == 6"), LocalResult->bHasValue.Num(), 6);
	if (LocalResult->Values.Num() != 6 || LocalResult->bHasValue.Num() != 6)
	{
		return false;
	}

	// --- Real mesh owner: identity configured for Source-Topology, then the generated WorkingMesh moved
	// in wholesale via InstallWorkingMesh -- InstanceResults travels with the move untouched. ---
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeShared<FVertexMaskForgeWorkingMeshOwner>();

	const bool bIdentityConfigured = MeshOwner->ConfigureIdentity(Fixture.Mesh.Get(), 0, /*bUseSourceTopology=*/true);
	TestTrue(TEXT("ConfigureIdentity succeeds"), bIdentityConfigured);
	if (!bIdentityConfigured)
	{
		return false;
	}

	const bool bInstalled = MeshOwner->InstallWorkingMesh(MoveTemp(WorkingMesh), /*ExpectedCardinality=*/6);
	TestTrue(TEXT("InstallWorkingMesh succeeds"), bInstalled);
	if (!bInstalled)
	{
		return false;
	}

	// --- Prove the store survived the move, read exclusively through the owner's own real accessor. ---
	TestEqual(TEXT("MeshOwner->GetWorkingMesh().InstanceResults.Num() == 1"), MeshOwner->GetWorkingMesh().InstanceResults.Num(), 1);
	TestTrue(TEXT("MeshOwner's InstanceResults contains MaskInstanceId after the move"), MeshOwner->GetWorkingMesh().InstanceResults.Contains(MaskInstanceId));
	const FVertexMaskForgeInstanceMaskResult* MovedResult = MeshOwner->GetWorkingMesh().InstanceResults.Find(MaskInstanceId);
	TestNotNull(TEXT("Moved stored result is non-null"), MovedResult);
	if (!MovedResult)
	{
		return false;
	}
	TestEqual(TEXT("Moved Values.Num() == 6"), MovedResult->Values.Num(), 6);
	TestEqual(TEXT("Moved bHasValue.Num() == 6"), MovedResult->bHasValue.Num(), 6);
	if (MovedResult->Values.Num() != 6 || MovedResult->bHasValue.Num() != 6)
	{
		return false;
	}

	const float ExpectedValues[6] = { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	const bool ExpectedHasValue[6] = { true, true, true, true, true, true };
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Moved bHasValue[%d] matches expected"), Index), MovedResult->bHasValue[Index], ExpectedHasValue[Index]);
		TestEqual(*FString::Printf(TEXT("Moved Values[%d] matches expected coverage"), Index), MovedResult->Values[Index], ExpectedValues[Index]);
	}

	// --- Real WorkingStateOwner: configure target, attach to the same mesh owner, initialize with an
	// explicit, known 6-element SourceTopologyBaselineColors array. ---
	FVertexMaskForgeWorkingStateOwner Owner;

	const bool bTargetConfigured = Owner.ConfigureTarget(Fixture.Component.Get());
	TestTrue(TEXT("ConfigureTarget succeeds"), bTargetConfigured);
	if (!bTargetConfigured)
	{
		return false;
	}

	const bool bAttached = Owner.AttachToMeshOwner(MeshOwner);
	TestTrue(TEXT("AttachToMeshOwner succeeds"), bAttached);
	if (!bAttached)
	{
		return false;
	}

	const TArray<FColor> Captured = {
		FColor(10, 20, 30, 100),
		FColor(11, 21, 31, 101),
		FColor(12, 22, 32, 102),
		FColor(13, 23, 33, 103),
		FColor(14, 24, 34, 104),
		FColor(15, 25, 35, 105),
	};

	const bool bColorsInitialized = Owner.InitializeColors(Captured);
	TestTrue(TEXT("InitializeColors succeeds"), bColorsInitialized);
	if (!bColorsInitialized)
	{
		return false;
	}
	TestTrue(TEXT("AreColorsInitialized() true"), Owner.AreColorsInitialized());
	if (!Owner.AreColorsInitialized())
	{
		return false;
	}

	TestEqual(TEXT("GetBaselineColors().Num() == 6"), Owner.GetBaselineColors().Num(), 6);
	TestEqual(TEXT("GetWorkingColors().Num() == 6"), Owner.GetWorkingColors().Num(), 6);
	if (Owner.GetBaselineColors().Num() != 6 || Owner.GetWorkingColors().Num() != 6)
	{
		return false;
	}

	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("BaselineColors[%d] byte-identical to Captured, before composition"), Index), Owner.GetBaselineColors()[Index], Captured[Index]);
		TestEqual(*FString::Printf(TEXT("WorkingColors[%d] byte-identical to Captured, before composition"), Index), Owner.GetWorkingColors()[Index], Captured[Index]);
	}

	// --- Real batch composition: real domain-matched baseline, real stack, real moved store. ---
	TArray<FColor> Out;
	VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors(Owner.GetBaselineColors(), Stack, MeshOwner->GetWorkingMesh().InstanceResults, Out);

	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (Out.Num() != 6)
	{
		return false;
	}

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
		TestEqual(*FString::Printf(TEXT("Out[%d] matches expected composed color"), Index), Out[Index], ExpectedOut[Index]);
	}

	// --- Non-mutation proofs: the ENTIRE BaselineColors and WorkingColors arrays, not spot-checked. ---
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("BaselineColors[%d] still byte-exact equal to Captured after composition"), Index), Owner.GetBaselineColors()[Index], Captured[Index]);
		TestEqual(*FString::Printf(TEXT("WorkingColors[%d] still byte-exact equal to Captured (never touched by ComposeColors)"), Index), Owner.GetWorkingColors()[Index], Captured[Index]);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
