// M20-E: automated coverage for the restored VertexMaskForgeComponentOverrideBridge (the "Send to Mesh
// Paint Texture" transient bridge). This is NEW test code proving the RESTORED behavior against real
// UE 5.8 public APIs -- it is not a byte-exact restoration of any former M20-C test file, whose original
// source did not survive (M20-C's files were untracked and were `rm`-deleted, uncommitted, by M20-D; see
// the M20-E final report's "M20-C Recovery"/"Automated Tests" sections for the honest accounting of what
// evidence this reconstruction relies on).
//
// SVertexMaskForgePanel itself is never constructed here (no test anywhere in this codebase does -- the
// same established boundary every prior K.6D/M20 checkpoint documents), so the panel-level button/
// tooltip/status-text wiring is NOT covered here and remains a manual-validation item.
//
// M20-E.1 added one further test (SessionIndependentCandidateEntryIsEligible, below) proving
// BuildTransferTargets accepts an entry shaped exactly like SVertexMaskForgePanel::CollectViewportSelection
// produces for the new session-independent MeshPaintTransferCandidateMeshes list -- i.e. WITHOUT ever
// calling ConfigureIdentity/InstallWorkingMesh (the Forge-session-only steps) -- as the actual, grounded
// proof that this bridge's own eligibility contract has no real dependency on an active Edit Vertex Mask
// session. The M20-E.1 panel-level lifecycle/layout correction itself (enablement predicate, live
// selection re-resolution, Accept/Cancel/Transfer button reorder) remains outside this file's boundary
// for the same reason as everything else panel-level -- see the manual validation checklist in the
// M20-E.1 final report.
//
// The real native UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors bake is not exercised
// here -- it touches the GPU baking pipeline and is not safe inside an automation fixture (this
// checkpoint's own Phase 8 instruction). Every test below that needs to prove the capture/install/
// restore contract uses TransferToMeshPaintTexture's InNativeImportOverride test seam, which runs the
// exact same production capture/install/restore code path around a lightweight stand-in for the native
// call.

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeComponentOverrideBridge.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

namespace
{
	/** Real, transient UObjects kept alive for the duration of a test -- same fixture recipe already
	 *  proven by VertexMaskForgeInstanceOverrideWriterTests.cpp / VertexMaskForgeDynamicAcceptTargetBuilderTests.cpp. */
	struct FBridgeFixtureAsset
	{
		TStrongObjectPtr<UStaticMeshComponent> Component;
		TStrongObjectPtr<UStaticMesh> Mesh;

		FBridgeFixtureAsset()
			: Component(NewObject<UStaticMeshComponent>())
			, Mesh(NewObject<UStaticMesh>())
		{
			Component->SetStaticMesh(Mesh.Get());
		}
	};

	/** Builds and commits a real, non-Nanite quad MeshDescription (2 triangles, 4 positions) via
	 *  UStaticMesh::BuildFromMeshDescriptions, optionally with per-vertex-instance colors baked in
	 *  (simulating a prior successful Forge Accept) -- reusing the same real-build recipe already
	 *  proven inside this codebase's other Accept-family test files. */
	void BuildQuadStaticMesh(UStaticMesh& Mesh, const TArray<FColor>* OptionalBakedColors)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();

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

		if (OptionalBakedColors != nullptr)
		{
			TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
			const int32 Num = FMath::Min(OptionalBakedColors->Num(), Description.VertexInstances().Num());
			int32 Index = 0;
			for (const FVertexInstanceID InstanceID : Description.VertexInstances().GetElementIDs())
			{
				if (Index >= Num)
				{
					break;
				}
				const FColor& C = (*OptionalBakedColors)[Index];
				InstanceColors.Set(InstanceID, FVector4f(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f));
				++Index;
			}
		}

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		BuildParams.bFastBuild = true;
		Mesh.BuildFromMeshDescriptions({ &Description }, BuildParams);
	}

	TArray<FColor> MakeGradientColors(const int32 Count, const uint8 AlphaBase = 200)
	{
		TArray<FColor> Colors;
		Colors.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			Colors.Add(FColor(
				static_cast<uint8>((i * 37) % 256),
				static_cast<uint8>((i * 71) % 256),
				static_cast<uint8>((i * 113) % 256),
				static_cast<uint8>((AlphaBase + i) % 256)));
		}
		return Colors;
	}
}

// --- BuildTransferTargets --------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeUsesAcceptedAssetColorsTest,
	"VertexMaskForge.ComponentOverrideBridge.UsesAcceptedAssetColorsNotWorkingColors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeUsesAcceptedAssetColorsTest::RunTest(const FString& Parameters)
{
	FBridgeFixtureAsset Fixture;
	const TArray<FColor> BakedColors = MakeGradientColors(6, 100);
	BuildQuadStaticMesh(*Fixture.Mesh, &BakedColors);

	const FStaticMeshLODResources& LOD0 = Fixture.Mesh->GetRenderData()->LODResources[0];
	TestTrue(TEXT("LOD0 has an accepted (non-empty) ColorVertexBuffer"), LOD0.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0);

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;
	TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
	Entry->Mesh = Fixture.Mesh.Get();
	Entry->AssetName = TEXT("BridgeTestQuad");
	Entry->bUseSourceTopology = false;

	const int32 RenderVertexCount = static_cast<int32>(LOD0.GetNumVertices());
	TestTrue(TEXT("ConfigureIdentity succeeds"), Entry->MeshOwner->ConfigureIdentity(Fixture.Mesh.Get(), 0, /*bUseSourceTopology=*/false));
	TestTrue(TEXT("InstallWorkingMesh succeeds"), Entry->MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), RenderVertexCount));

	TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("ConfigureTarget succeeds"), StateOwner->ConfigureTarget(Fixture.Component.Get()));
	TestTrue(TEXT("AttachToMeshOwner succeeds"), StateOwner->AttachToMeshOwner(Entry->MeshOwner));
	Entry->PreviewComponents.Add(MoveTemp(StateOwner));
	SelectedMeshes.Add(Entry);

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	TArray<FText> IneligibleReasons;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeComponentOverrideBridge::BuildTransferTargets(SelectedMeshes, Targets, IneligibleReasons, ErrorText);

	TestTrue(TEXT("BuildTransferTargets returns true (pure validation never fails structurally here)"), bBuilt);
	TestEqual(TEXT("One eligible target"), Targets.Num(), 1);
	if (Targets.Num() == 1)
	{
		TestEqual(TEXT("Render vertex count matches LOD0 authority"), Targets[0].AcceptedColors.Num(), static_cast<int32>(LOD0.GetNumVertices()));

		// The exact assertion this test exists for: the gathered colors are the ASSET's own rebuilt
		// ColorVertexBuffer, read back byte-for-byte -- never some other, unaccepted source.
		bool bMatchesAssetBuffer = true;
		for (int32 i = 0; i < Targets[0].AcceptedColors.Num(); ++i)
		{
			if (Targets[0].AcceptedColors[i] != LOD0.VertexBuffers.ColorVertexBuffer.VertexColor(i))
			{
				bMatchesAssetBuffer = false;
				break;
			}
		}
		TestTrue(TEXT("Gathered colors are byte-exact against LOD0.VertexBuffers.ColorVertexBuffer"), bMatchesAssetBuffer);

		// Alpha specifically, since Alpha is easy to accidentally drop.
		bool bAlphaPreserved = true;
		for (int32 i = 0; i < Targets[0].AcceptedColors.Num(); ++i)
		{
			if (Targets[0].AcceptedColors[i].A != LOD0.VertexBuffers.ColorVertexBuffer.VertexColor(i).A)
			{
				bAlphaPreserved = false;
				break;
			}
		}
		TestTrue(TEXT("Alpha preserved byte-for-byte"), bAlphaPreserved);
	}

	return true;
}

// AUDITED (M20-E, real-engine-behavior correction): an earlier version of this test asserted that a
// quad built with NO explicit vertex-instance colors produces ZERO eligible targets (i.e. that
// GetNumVertices()==0 reliably means "never accepted"). Running against the real, installed UE 5.8
// UStaticMesh::BuildFromMeshDescriptions path proved that assumption FALSE: FStaticMeshAttributes::
// Register() always registers a VertexInstanceColor attribute defaulted to opaque white, so ANY real
// build -- accepted or not -- produces a non-empty, fully-populated LOD0 ColorVertexBuffer (uniform
// white when nothing was ever explicitly painted/accepted). BuildTransferTargets' GetNumVertices()==0
// eligibility guard is therefore a genuine structural sanity check (protects against the true zero-
// buffer edge case, e.g. a LOD0 built without ANY color attribute at all), NOT a reliable "was this
// asset ever Accepted" detector -- exactly the honest limitation Phase 5 of the M20-E checkpoint spec
// asks to be documented rather than papered over with an invented, unproven detection mechanism. This
// test now proves the REAL, grounded behavior: a mesh with no explicit colors is still eligible, with
// its default-white buffer read back verbatim (never rejected, never silently treated as "no colors").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeDefaultWhiteAssetIsStillEligibleTest,
	"VertexMaskForge.ComponentOverrideBridge.AssetWithNoExplicitColorsIsStillEligibleDefaultWhite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeDefaultWhiteAssetIsStillEligibleTest::RunTest(const FString& Parameters)
{
	FBridgeFixtureAsset Fixture;
	BuildQuadStaticMesh(*Fixture.Mesh, /*OptionalBakedColors=*/nullptr);

	const FStaticMeshLODResources& LOD0 = Fixture.Mesh->GetRenderData()->LODResources[0];
	const int32 RenderVertexCount = static_cast<int32>(LOD0.GetNumVertices());
	TestTrue(TEXT("LOD0's ColorVertexBuffer is non-empty even with no explicit colors (default white)"), LOD0.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0);

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;
	TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
	Entry->Mesh = Fixture.Mesh.Get();
	Entry->AssetName = TEXT("BridgeTestQuadDefaultWhite");
	Entry->bUseSourceTopology = false;

	TestTrue(TEXT("ConfigureIdentity succeeds"), Entry->MeshOwner->ConfigureIdentity(Fixture.Mesh.Get(), 0, /*bUseSourceTopology=*/false));
	TestTrue(TEXT("InstallWorkingMesh succeeds"), Entry->MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), RenderVertexCount));

	TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("ConfigureTarget succeeds"), StateOwner->ConfigureTarget(Fixture.Component.Get()));
	TestTrue(TEXT("AttachToMeshOwner succeeds"), StateOwner->AttachToMeshOwner(Entry->MeshOwner));
	Entry->PreviewComponents.Add(MoveTemp(StateOwner));
	SelectedMeshes.Add(Entry);

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	TArray<FText> IneligibleReasons;
	FText ErrorText;
	VertexMaskForgeComponentOverrideBridge::BuildTransferTargets(SelectedMeshes, Targets, IneligibleReasons, ErrorText);

	TestEqual(TEXT("One eligible target -- this bridge cannot distinguish default-white from an intentional Accept"), Targets.Num(), 1);
	if (Targets.Num() == 1)
	{
		bool bAllWhite = true;
		for (const FColor& C : Targets[0].AcceptedColors)
		{
			if (C != FColor::White)
			{
				bAllWhite = false;
				break;
			}
		}
		TestTrue(TEXT("Gathered colors are the real default-white buffer, read back verbatim"), bAllWhite);
	}

	return true;
}

// M20-E.1: proves BuildTransferTargets requires NO Forge editing-session/Working-Mesh state at all --
// only ConfigureTarget (sets PreviewState.SourceComponent) + AttachToMeshOwner (requires a non-expired
// mesh owner, which FVertexMaskForgeSelectedMesh's constructor always provides). Deliberately does NOT
// call MeshOwner->ConfigureIdentity/InstallWorkingMesh (the session-only steps RefreshSelection/
// BuildWorkingMeshes perform) -- this is the EXACT shape of entry SVertexMaskForgePanel::
// CollectViewportSelection/AddOrUpdateSelectedMesh produces for SVertexMaskForgePanel::
// MeshPaintTransferCandidateMeshes (the M20-E.1 session-independent candidate list backing
// CanSendToMeshPaintTexture()/OnSendToMeshPaintTextureClicked()), never a byte-exact simulation of the
// Slate panel itself (which this test file's own header comment already establishes is out of scope).
// If BuildTransferTargets ever silently started depending on WorkingMesh/session-only state, this test
// would fail even though the higher-level bridge tests above (which DO call ConfigureIdentity/
// InstallWorkingMesh, matching a real completed session) would not catch the regression.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeSessionIndependentEntryIsEligibleTest,
	"VertexMaskForge.ComponentOverrideBridge.SessionIndependentCandidateEntryIsEligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeSessionIndependentEntryIsEligibleTest::RunTest(const FString& Parameters)
{
	FBridgeFixtureAsset Fixture;
	const TArray<FColor> BakedColors = MakeGradientColors(6, 150);
	BuildQuadStaticMesh(*Fixture.Mesh, &BakedColors);

	const FStaticMeshLODResources& LOD0 = Fixture.Mesh->GetRenderData()->LODResources[0];

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> Candidates;
	TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
	Entry->Mesh = Fixture.Mesh.Get();
	Entry->AssetName = TEXT("SessionIndependentQuad");
	Entry->bUseSourceTopology = false;
	// Deliberately NOT calling Entry->MeshOwner->ConfigureIdentity/InstallWorkingMesh -- no Forge
	// session/Working Mesh ever existed for this entry, exactly like a component that was merely
	// selected in the Editor and never had "Edit Vertex Mask" clicked.

	TUniquePtr<FVertexMaskForgeWorkingStateOwner> StateOwner = MakeUnique<FVertexMaskForgeWorkingStateOwner>();
	TestTrue(TEXT("ConfigureTarget succeeds with no session/WorkingMesh state"), StateOwner->ConfigureTarget(Fixture.Component.Get()));
	TestTrue(TEXT("AttachToMeshOwner succeeds against a freshly-constructed, still-unconfigured MeshOwner"), StateOwner->AttachToMeshOwner(Entry->MeshOwner));
	Entry->PreviewComponents.Add(MoveTemp(StateOwner));
	Candidates.Add(Entry);

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	TArray<FText> IneligibleReasons;
	FText ErrorText;
	const bool bBuilt = VertexMaskForgeComponentOverrideBridge::BuildTransferTargets(Candidates, Targets, IneligibleReasons, ErrorText);

	TestTrue(TEXT("BuildTransferTargets returns true"), bBuilt);
	TestEqual(TEXT("One eligible target from a session-free candidate entry"), Targets.Num(), 1);
	if (Targets.Num() == 1)
	{
		TestEqual(TEXT("Render vertex count matches LOD0 authority"), Targets[0].AcceptedColors.Num(), static_cast<int32>(LOD0.GetNumVertices()));
		bool bMatchesAssetBuffer = true;
		for (int32 i = 0; i < Targets[0].AcceptedColors.Num(); ++i)
		{
			if (Targets[0].AcceptedColors[i] != LOD0.VertexBuffers.ColorVertexBuffer.VertexColor(i))
			{
				bMatchesAssetBuffer = false;
				break;
			}
		}
		TestTrue(TEXT("Gathered colors are byte-exact against LOD0.VertexBuffers.ColorVertexBuffer, even with no session"), bMatchesAssetBuffer);
	}

	return true;
}

// --- TransferToMeshPaintTexture: capture/install/restore contract, via the test injection seam ------

namespace
{
	/** Shared setup for the restore-contract tests below: a real quad mesh with accepted colors, and a
	 *  real component optionally pre-seeded with an existing LOD0 override + PaintedVertices. */
	struct FRestoreContractFixture
	{
		FBridgeFixtureAsset Fixture;
		TArray<FColor> AcceptedColors;

		explicit FRestoreContractFixture(const bool bSeedExistingOverride)
		{
			AcceptedColors = MakeGradientColors(6, 50);
			BuildQuadStaticMesh(*Fixture.Mesh, &AcceptedColors);

			if (bSeedExistingOverride)
			{
				UStaticMeshComponent* Component = Fixture.Component.Get();
				Component->SetLODDataCount(1, 1);
				FStaticMeshComponentLODInfo& LODInfo = Component->LODData[0];
				LODInfo.OverrideVertexColors = new FColorVertexBuffer;
				const TArray<FColor> ExistingColors = MakeGradientColors(6, 9);
				LODInfo.OverrideVertexColors->InitFromColorArray(ExistingColors);
				LODInfo.PaintedVertices.Reserve(6);
				for (int32 i = 0; i < 6; ++i)
				{
					LODInfo.PaintedVertices.Add(FPaintedVertex());
				}
			}
		}
	};

	VertexMaskForgeComponentOverrideBridge::FTransferTarget MakeTarget(UStaticMeshComponent* Component, const FString& AssetName, const TArray<FColor>& Colors)
	{
		VertexMaskForgeComponentOverrideBridge::FTransferTarget Target;
		Target.Component = Component;
		Target.AssetName = AssetName;
		Target.AcceptedColors = Colors;
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeNoPreexistingOverrideRestoredToNoneTest,
	"VertexMaskForge.ComponentOverrideBridge.NoPreexistingOverrideRestoredToNone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeNoPreexistingOverrideRestoredToNoneTest::RunTest(const FString& Parameters)
{
	FRestoreContractFixture Contract(/*bSeedExistingOverride=*/false);
	UStaticMeshComponent* Component = Contract.Fixture.Component.Get();

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	Targets.Add(MakeTarget(Component, TEXT("NoOverride"), Contract.AcceptedColors));

	TArray<UStaticMeshComponent*> Succeeded;
	FText ErrorText;
	const bool bResult = VertexMaskForgeComponentOverrideBridge::TransferToMeshPaintTexture(
		Targets, Succeeded, ErrorText,
		[](UStaticMeshComponent&) { return true; });

	TestTrue(TEXT("Simulated native import reported success"), bResult);
	TestEqual(TEXT("One succeeded component"), Succeeded.Num(), 1);

	const bool bHasOverrideAfter = Component->LODData.IsValidIndex(0)
		&& Component->LODData[0].OverrideVertexColors != nullptr
		&& Component->LODData[0].OverrideVertexColors->GetNumVertices() > 0;
	TestFalse(TEXT("No persistent override remains after a component with no initial override"), bHasOverrideAfter);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeExistingOverrideRestoredByteForByteTest,
	"VertexMaskForge.ComponentOverrideBridge.ExistingOverrideRestoredByteForByte",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeExistingOverrideRestoredByteForByteTest::RunTest(const FString& Parameters)
{
	FRestoreContractFixture Contract(/*bSeedExistingOverride=*/true);
	UStaticMeshComponent* Component = Contract.Fixture.Component.Get();

	// Record the exact pre-call state.
	TArray<FColor> ColorsBefore;
	const uint32 CountBefore = Component->LODData[0].OverrideVertexColors->GetNumVertices();
	for (uint32 i = 0; i < CountBefore; ++i)
	{
		ColorsBefore.Add(Component->LODData[0].OverrideVertexColors->VertexColor(i));
	}
	const int32 PaintedVerticesCountBefore = Component->LODData[0].PaintedVertices.Num();

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	Targets.Add(MakeTarget(Component, TEXT("ExistingOverride"), Contract.AcceptedColors));

	TArray<UStaticMeshComponent*> Succeeded;
	FText ErrorText;
	VertexMaskForgeComponentOverrideBridge::TransferToMeshPaintTexture(
		Targets, Succeeded, ErrorText,
		[](UStaticMeshComponent&) { return true; });

	TestTrue(TEXT("Override still present after restoration"), Component->LODData[0].OverrideVertexColors != nullptr);
	if (Component->LODData[0].OverrideVertexColors != nullptr)
	{
		TestEqual(TEXT("Vertex count restored exactly"), static_cast<int32>(Component->LODData[0].OverrideVertexColors->GetNumVertices()), static_cast<int32>(CountBefore));
		bool bBytesMatch = true;
		for (uint32 i = 0; i < CountBefore; ++i)
		{
			if (Component->LODData[0].OverrideVertexColors->VertexColor(i) != ColorsBefore[i])
			{
				bBytesMatch = false;
				break;
			}
		}
		TestTrue(TEXT("Existing override restored byte-for-byte"), bBytesMatch);
	}
	TestEqual(TEXT("PaintedVertices count restored exactly"), Component->LODData[0].PaintedVertices.Num(), PaintedVerticesCountBefore);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeFailureBeforeNativeCallRestoresStateTest,
	"VertexMaskForge.ComponentOverrideBridge.FailureBeforeNativeCallRestoresState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeFailureBeforeNativeCallRestoresStateTest::RunTest(const FString& Parameters)
{
	FRestoreContractFixture Contract(/*bSeedExistingOverride=*/true);
	UStaticMeshComponent* Component = Contract.Fixture.Component.Get();
	const int32 CountBefore = static_cast<int32>(Component->LODData[0].OverrideVertexColors->GetNumVertices());

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	Targets.Add(MakeTarget(Component, TEXT("FailBeforeImport"), Contract.AcceptedColors));

	TArray<UStaticMeshComponent*> Succeeded;
	FText ErrorText;
	const bool bResult = VertexMaskForgeComponentOverrideBridge::TransferToMeshPaintTexture(
		Targets, Succeeded, ErrorText,
		// Simulates the native import "failing" (early-returning without producing a texture).
		[](UStaticMeshComponent&) { return false; });

	TestFalse(TEXT("Overall result reports failure"), bResult);
	TestEqual(TEXT("No succeeded components"), Succeeded.Num(), 0);
	TestFalse(TEXT("OutErrorText is non-empty on failure"), ErrorText.IsEmpty());
	TestTrue(TEXT("Override still present (restored) after a simulated failure"), Component->LODData[0].OverrideVertexColors != nullptr);
	if (Component->LODData[0].OverrideVertexColors != nullptr)
	{
		TestEqual(TEXT("Vertex count restored exactly even on failure"), static_cast<int32>(Component->LODData[0].OverrideVertexColors->GetNumVertices()), CountBefore);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeComponentOverrideBridgeMultipleComponentsDontShareBuffersTest,
	"VertexMaskForge.ComponentOverrideBridge.MultipleComponentsDoNotShareBuffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVertexMaskForgeComponentOverrideBridgeMultipleComponentsDontShareBuffersTest::RunTest(const FString& Parameters)
{
	FRestoreContractFixture ContractA(/*bSeedExistingOverride=*/false);
	FRestoreContractFixture ContractB(/*bSeedExistingOverride=*/true);
	UStaticMeshComponent* ComponentA = ContractA.Fixture.Component.Get();
	UStaticMeshComponent* ComponentB = ContractB.Fixture.Component.Get();

	TArray<VertexMaskForgeComponentOverrideBridge::FTransferTarget> Targets;
	Targets.Add(MakeTarget(ComponentA, TEXT("MultiA"), ContractA.AcceptedColors));
	Targets.Add(MakeTarget(ComponentB, TEXT("MultiB"), ContractB.AcceptedColors));

	TArray<FColor*> ObservedBuffersDuringImport;
	TArray<UStaticMeshComponent*> Succeeded;
	FText ErrorText;
	VertexMaskForgeComponentOverrideBridge::TransferToMeshPaintTexture(
		Targets, Succeeded, ErrorText,
		[&ObservedBuffersDuringImport](UStaticMeshComponent& Component)
		{
			// Record the raw CPU buffer pointer this component's temporary override currently uses.
			if (Component.LODData.IsValidIndex(0) && Component.LODData[0].OverrideVertexColors != nullptr)
			{
				ObservedBuffersDuringImport.Add(&Component.LODData[0].OverrideVertexColors->VertexColor(0));
			}
			return true;
		});

	TestEqual(TEXT("Both components succeeded"), Succeeded.Num(), 2);
	TestEqual(TEXT("Two independent temporary buffers were observed"), ObservedBuffersDuringImport.Num(), 2);
	if (ObservedBuffersDuringImport.Num() == 2)
	{
		TestNotEqual(TEXT("The two components' temporary buffers are not the same allocation"), ObservedBuffersDuringImport[0], ObservedBuffersDuringImport[1]);
	}

	// Component A had no initial override -- must have none afterward.
	const bool bAHasOverride = ComponentA->LODData.IsValidIndex(0) && ComponentA->LODData[0].OverrideVertexColors != nullptr;
	TestFalse(TEXT("Component A (no initial override) restored to none"), bAHasOverride);

	// Component B had an initial override -- must still have one afterward.
	const bool bBHasOverride = ComponentB->LODData.IsValidIndex(0) && ComponentB->LODData[0].OverrideVertexColors != nullptr;
	TestTrue(TEXT("Component B (had an initial override) restored to present"), bBHasOverride);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
