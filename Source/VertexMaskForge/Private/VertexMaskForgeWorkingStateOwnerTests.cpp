// M16-J.0A.1: owner lifecycle tests -- the ONLY tests that count as proof of the split headless
// mesh-owner/state-owner seam. Every test drives FVertexMaskForgeWorkingMeshOwner and
// FVertexMaskForgeWorkingStateOwner exclusively through their own public, production-ready APIs -- the
// exact same APIs a future M16-J.0B panel adapter will use. No test-only hooks, no friend-of-test, no
// setter for Generation/Provenance/Authority, no synthetic metadata fabrication.
//
// Structural proofs (no dedicated test, verified instead by source inspection / diff, per this
// checkpoint's own contract): neither .cpp/.h includes any generator header, no
// VertexMaskForgeRecipeInstanceGeneration/VertexMaskForgeRecipeEvaluation/VertexMaskForgeFillLayerResolution
// header, never call ComposeMaskStack/ToDisplayFColor, never touch InstanceResults, never touch
// SVertexMaskForgePanel, never apply colors visually, never persist an asset.

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeWorkingStateOwner.h"

namespace
{
	using EStatus = EVertexMaskForgeWorkingColorsPublicationValidationStatus;

	/** Keeps real, transient UObjects alive for the duration of one test. */
	struct FOwnerTestFixture
	{
		TStrongObjectPtr<UStaticMeshComponent> Component;
		TStrongObjectPtr<UStaticMesh> Mesh;

		FOwnerTestFixture()
			: Component(NewObject<UStaticMeshComponent>())
			, Mesh(NewObject<UStaticMesh>())
		{
		}
	};

	TArray<FColor> MakeCapturedColors(const int32 Count, const uint8 AlphaBase = 0)
	{
		TArray<FColor> Colors;
		Colors.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			Colors.Add(FColor(
				static_cast<uint8>((i * 17) % 256),
				static_cast<uint8>((i * 31) % 256),
				static_cast<uint8>((i * 53) % 256),
				static_cast<uint8>((AlphaBase + i * 7) % 256)));
		}
		return Colors;
	}

	/** Builds a ready-to-use mesh owner (identity configured + one Working Mesh installed). */
	TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MakeReadyMeshOwner(UStaticMesh* Mesh, const int32 Cardinality, const int32 LODIndex = 0, const bool bUseSourceTopology = false)
	{
		TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeShared<FVertexMaskForgeWorkingMeshOwner>();
		MeshOwner->ConfigureIdentity(Mesh, LODIndex, bUseSourceTopology);
		MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), Cardinality);
		return MeshOwner;
	}
}

// 18.1: owner real por componente -- construction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerConstructionTest, "VertexMaskForge.WorkingStateOwner.Construction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerConstructionTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingStateOwner Owner;

	TestFalse(TEXT("IsTargetConfigured() == false"), Owner.IsTargetConfigured());
	TestFalse(TEXT("IsAttached() == false"), Owner.IsAttached());
	TestFalse(TEXT("WorkingColorsAuthority not valid yet"), Owner.GetPreviewState().GetWorkingColorsAuthority().IsValid());
	TestFalse(TEXT("AreColorsInitialized() == false"), Owner.AreColorsInitialized());
	TestTrue(TEXT("Binding on a fresh owner does not validate"), Owner.ValidateBinding() != EStatus::Success);

	return true;
}

// ConfigureTarget: null rejected, no-op reconfigure, real reconfigure invalidates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerConfigureTargetTest, "VertexMaskForge.WorkingStateOwner.ConfigureTarget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerConfigureTargetTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	FVertexMaskForgeWorkingStateOwner Owner;

	TestFalse(TEXT("Null component rejected"), Owner.ConfigureTarget(nullptr));
	TestTrue(TEXT("Valid configure succeeds"), Owner.ConfigureTarget(Fixture.Component.Get()));
	TestTrue(TEXT("IsTargetConfigured() == true"), Owner.IsTargetConfigured());
	TestEqual(TEXT("SourceComponent comes from the owner"), Owner.GetPreviewState().GetSourceComponent().Get(), Fixture.Component.Get());
	TestTrue(TEXT("Repeated identical configure succeeds"), Owner.ConfigureTarget(Fixture.Component.Get()));

	return true;
}

// 18.2: lifecycle real de construção (configure -> attach -> install -> initialize -> valid binding).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerFullLifecycleTest, "VertexMaskForge.WorkingStateOwner.FullLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerFullLifecycleTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	FVertexMaskForgeWorkingStateOwner Owner;

	TestFalse(TEXT("Attach before mesh owner exists is impossible by construction -- attaching null rejected"), Owner.AttachToMeshOwner(nullptr));

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	TestFalse(TEXT("Initialize before ConfigureTarget rejected"), Owner.InitializeColors(MakeCapturedColors(4)));

	Owner.ConfigureTarget(Fixture.Component.Get());
	TestFalse(TEXT("Initialize before attach rejected"), Owner.InitializeColors(MakeCapturedColors(4)));

	TestTrue(TEXT("Attach succeeds"), Owner.AttachToMeshOwner(MeshOwner));
	TestTrue(TEXT("IsAttached() == true"), Owner.IsAttached());

	TestFalse(TEXT("Wrong cardinality rejected"), Owner.InitializeColors(MakeCapturedColors(3)));
	TestFalse(TEXT("Colors not initialized after rejected call"), Owner.AreColorsInitialized());

	const TArray<FColor> Captured = MakeCapturedColors(4);
	TestTrue(TEXT("Initialize succeeds"), Owner.InitializeColors(Captured));
	TestTrue(TEXT("AreColorsInitialized() == true"), Owner.AreColorsInitialized());

	for (int32 i = 0; i < Captured.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("WorkingColors[%d] byte-identical"), i), Owner.GetWorkingColors()[i], Captured[i]);
	}

	TestTrue(TEXT("Authority MeshOwnerId matches the attached mesh owner"),
		Owner.GetPreviewState().GetWorkingColorsAuthority().GetMeshOwnerId() == MeshOwner->GetWorkingMesh().Provenance.GetMeshOwnerId());
	TestEqual(TEXT("Authority revision == mesh owner generation"), Owner.GetPreviewState().GetWorkingColorsAuthority().GetRevision(), MeshOwner->GetGeneration());
	TestTrue(TEXT("Binding validates after full lifecycle"), Owner.ValidateBinding() == EStatus::Success);

	return true;
}

// 18.1 / M16-J.0A.1 test 1: uma mesh compartilhada, dois componentes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerTwoComponentsSharedMeshTest, "VertexMaskForge.WorkingStateOwner.TwoComponentsSharedMesh", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerTwoComponentsSharedMeshTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwnerA;
	StateOwnerA.ConfigureTarget(Fixture.Component.Get());
	StateOwnerA.AttachToMeshOwner(MeshOwner);
	StateOwnerA.InitializeColors(MakeCapturedColors(4, /*AlphaBase=*/0));

	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(OtherComponent.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner); // SAME mesh owner -- no copy of the Working Mesh.
	StateOwnerB.InitializeColors(MakeCapturedColors(4, /*AlphaBase=*/100));

	TestTrue(TEXT("StateOwnerA validates"), StateOwnerA.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("StateOwnerB validates"), StateOwnerB.ValidateBinding() == EStatus::Success);
	// M16-J.0B.1 corrective pass: the Binding no longer exposes a raw WorkingMesh pointer to compare
	// (see FVertexMaskForgeWorkingColorPublicationBinding's own doc comment) -- "no copy of the Working
	// Mesh happened" is proven structurally instead, by both owners sharing the exact same MeshOwner
	// TSharedPtr (asserted by construction above) and both Authorities carrying the SAME MeshOwnerId.
	TestEqual(TEXT("Both Authorities were stamped from the SAME MeshOwnerId (no copy)"),
		StateOwnerA.GetPreviewState().GetWorkingColorsAuthority().GetMeshOwnerId(),
		StateOwnerB.GetPreviewState().GetWorkingColorsAuthority().GetMeshOwnerId());

	TestNotEqual(TEXT("Buffers are independent"), StateOwnerA.GetWorkingColors()[0].A, StateOwnerB.GetWorkingColors()[0].A);
	StateOwnerA.FillBlack();
	TestTrue(TEXT("Mutating A's buffer does not affect B"), StateOwnerB.GetWorkingColors()[0].R != 0 || StateOwnerB.GetWorkingColors()[1].R != 0);

	return true;
}

// M16-J.0A.1 test 2: rebuild compartilhado.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerSharedRebuildTest, "VertexMaskForge.WorkingStateOwner.SharedRebuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerSharedRebuildTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwnerA;
	StateOwnerA.ConfigureTarget(Fixture.Component.Get());
	StateOwnerA.AttachToMeshOwner(MeshOwner);
	StateOwnerA.InitializeColors(MakeCapturedColors(4));

	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(OtherComponent.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner);
	StateOwnerB.InitializeColors(MakeCapturedColors(4));

	TestTrue(TEXT("A valid before rebuild"), StateOwnerA.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("B valid before rebuild"), StateOwnerB.ValidateBinding() == EStatus::Success);
	const uint64 GenerationBefore = MeshOwner->GetGeneration();

	// Real rebuild via the mesh owner's own API -- ONLY its own generation advances; StateOwners are
	// never touched directly by this call.
	MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), 4);
	TestTrue(TEXT("MeshOwner generation advanced"), MeshOwner->GetGeneration() != GenerationBefore);
	TestTrue(TEXT("A's binding now fails (stale authority)"), StateOwnerA.ValidateBinding() != EStatus::Success);
	TestTrue(TEXT("B's binding now fails (stale authority)"), StateOwnerB.ValidateBinding() != EStatus::Success);

	const TArray<FColor> NewColorsA = MakeCapturedColors(4, /*AlphaBase=*/10);
	const TArray<FColor> NewColorsB = MakeCapturedColors(4, /*AlphaBase=*/200);
	StateOwnerA.InitializeColors(NewColorsA);
	StateOwnerB.InitializeColors(NewColorsB);

	TestTrue(TEXT("A valid again after reinitializing for the new generation"), StateOwnerA.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("B valid again after reinitializing for the new generation"), StateOwnerB.ValidateBinding() == EStatus::Success);
	TestEqual(TEXT("A's colors are A's own, not B's"), StateOwnerA.GetWorkingColors()[0].A, NewColorsA[0].A);
	TestEqual(TEXT("B's colors are B's own, not A's"), StateOwnerB.GetWorkingColors()[0].A, NewColorsB[0].A);

	return true;
}

// M16-J.0A.1 test 3: reset de um StateOwner afeta só ele mesmo.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerResetOneStateOwnerTest, "VertexMaskForge.WorkingStateOwner.ResetOneStateOwner", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerResetOneStateOwnerTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwnerA;
	StateOwnerA.ConfigureTarget(Fixture.Component.Get());
	StateOwnerA.AttachToMeshOwner(MeshOwner);
	StateOwnerA.InitializeColors(MakeCapturedColors(4));

	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(OtherComponent.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner);
	StateOwnerB.InitializeColors(MakeCapturedColors(4));

	TestTrue(TEXT("A.Reset() succeeds"), StateOwnerA.Reset());
	TestTrue(TEXT("A now invalid"), StateOwnerA.ValidateBinding() != EStatus::Success);
	TestTrue(TEXT("B (sibling) remains valid"), StateOwnerB.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("MeshOwner remains valid"), MeshOwner->GetWorkingMesh().Provenance.IsValid());

	return true;
}

// M16-J.0A.1 test 4: reset do MeshOwner invalida todos os StateOwners associados.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerResetMeshOwnerTest, "VertexMaskForge.WorkingStateOwner.ResetMeshOwner", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerResetMeshOwnerTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwnerA;
	StateOwnerA.ConfigureTarget(Fixture.Component.Get());
	StateOwnerA.AttachToMeshOwner(MeshOwner);
	StateOwnerA.InitializeColors(MakeCapturedColors(4));

	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(OtherComponent.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner);
	StateOwnerB.InitializeColors(MakeCapturedColors(4));

	TestTrue(TEXT("MeshOwner.Reset() succeeds"), MeshOwner->Reset());
	TestTrue(TEXT("A now invalid (old authority does not authenticate the reconstructed geometry)"), StateOwnerA.ValidateBinding() != EStatus::Success);
	TestTrue(TEXT("B now invalid too"), StateOwnerB.ValidateBinding() != EStatus::Success);

	// Reinstall + reinitialize both -- proves recovery is possible and old authorities never revalidate.
	MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), 4);
	StateOwnerA.InitializeColors(MakeCapturedColors(4));
	StateOwnerB.InitializeColors(MakeCapturedColors(4));
	TestTrue(TEXT("A valid again"), StateOwnerA.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("B valid again"), StateOwnerB.ValidateBinding() == EStatus::Success);

	return true;
}

// M16-J.0A.1 test 5: reassociação.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerReassociationTest, "VertexMaskForge.WorkingStateOwner.Reassociation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerReassociationTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMesh> OtherMesh(NewObject<UStaticMesh>());

	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwnerA = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);
	// Same cardinality, DIFFERENT Static Mesh -- must not authenticate.
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwnerB = MakeReadyMeshOwner(OtherMesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwner;
	StateOwner.ConfigureTarget(Fixture.Component.Get());
	StateOwner.AttachToMeshOwner(MeshOwnerA);
	StateOwner.InitializeColors(MakeCapturedColors(4));
	TestTrue(TEXT("Valid against original mesh owner"), StateOwner.ValidateBinding() == EStatus::Success);

	TestTrue(TEXT("ValidateAgainst a different mesh owner fails despite matching cardinality"),
		StateOwner.ValidateAgainst(MeshOwnerB) != EStatus::Success);

	TestTrue(TEXT("Reassociating to MeshOwnerB succeeds"), StateOwner.AttachToMeshOwner(MeshOwnerB));
	TestTrue(TEXT("Binding against the OLD mesh owner's identity no longer trivially valid -- colors were invalidated"), !StateOwner.AreColorsInitialized());
	TestTrue(TEXT("Binding fails until colors are reinitialized against the new mesh owner"), StateOwner.ValidateBinding() != EStatus::Success);

	StateOwner.InitializeColors(MakeCapturedColors(4));
	TestTrue(TEXT("Valid again against the NEW mesh owner"), StateOwner.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("No longer valid against the OLD mesh owner"), StateOwner.ValidateAgainst(MeshOwnerA) != EStatus::Success);

	// LOD/domain difference, same cardinality, does not authenticate either.
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwnerLOD1 = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4, /*LODIndex=*/1, false);
	TestTrue(TEXT("Different LOD with matching cardinality fails"), StateOwner.ValidateAgainst(MeshOwnerLOD1) != EStatus::Success);
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwnerTopology = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4, /*LODIndex=*/0, /*bUseSourceTopology=*/true);
	TestTrue(TEXT("Different domain with matching cardinality fails"), StateOwner.ValidateAgainst(MeshOwnerTopology) != EStatus::Success);

	return true;
}

// M16-J.0A.1 test 6: lifetime -- detach, no stale pointer, sibling independence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerLifetimeTest, "VertexMaskForge.WorkingStateOwner.Lifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerLifetimeTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());

	TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 4);

	FVertexMaskForgeWorkingStateOwner StateOwnerA;
	StateOwnerA.ConfigureTarget(Fixture.Component.Get());
	StateOwnerA.AttachToMeshOwner(MeshOwner);
	StateOwnerA.InitializeColors(MakeCapturedColors(4));

	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(OtherComponent.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner);
	StateOwnerB.InitializeColors(MakeCapturedColors(4));

	// Explicit detach before destruction (disciplined cleanup order) -- A no longer references MeshOwner.
	StateOwnerA.DetachFromMeshOwner();
	TestFalse(TEXT("A detached"), StateOwnerA.IsAttached());
	TestTrue(TEXT("A's binding fails after detach"), StateOwnerA.ValidateBinding() != EStatus::Success);

	// Now destroy the mesh owner entirely (simulates entry teardown) while B is STILL attached, without
	// B ever calling DetachFromMeshOwner -- proves TWeakPtr safety regardless of destruction order: B's
	// weak attachment correctly reports itself as gone (IsAttached() == false, TWeakPtr::Pin() fails
	// cleanly) rather than dangling or crashing.
	MeshOwner.Reset();
	TestFalse(TEXT("B's weak attachment correctly reports expired after the mesh owner's destruction"), StateOwnerB.IsAttached());
	const FVertexMaskForgeWorkingColorPublicationBinding BindingAfterDestruction = StateOwnerB.CreateBinding();
	TestFalse(TEXT("B's binding is not well-formed once the mesh owner is gone (Pin() failed cleanly)"), BindingAfterDestruction.IsWellFormed());
	TestTrue(TEXT("B's ValidateBinding fails safely, no crash, no dangling read"), StateOwnerB.ValidateBinding() != EStatus::Success);

	return true;
}

// M16-J.0B (2nd rejection-corrective pass), section 5.A: composição controlada via
// EnsureBaselineCaptured/ApplyComposedColorsRGB -- replaces the removed GetComposedColorArraysForRealPipeline/
// CommitComposedColors (which still handed out mutable pointers into owner storage). Neither new method
// ever exposes a pointer or reference into BaselineColors/CommittedColors/WorkingColors; FinalRGB is
// passed BY VALUE and the caller retains nothing after the call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerComposedColorsControlledTest, "VertexMaskForge.WorkingStateOwner.ComposedColorsControlled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerComposedColorsControlledTest::RunTest(const FString& Parameters)
{
	using EMode = FVertexMaskForgeWorkingStateOwner::EColorCommitMode;

	FOwnerTestFixture Fixture;
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner Owner;

	// ApplyComposedColorsRGB before any baseline capture is rejected outright.
	TArray<FColor> EarlyRGB = MakeCapturedColors(3);
	TestFalse(TEXT("ApplyComposedColorsRGB before EnsureBaselineCaptured rejected"), Owner.ApplyComposedColorsRGB(EarlyRGB, EMode::PreviewOnly));

	Owner.ConfigureTarget(Fixture.Component.Get());
	Owner.AttachToMeshOwner(MeshOwner);
	const uint64 GenerationBefore = MeshOwner->GetGeneration();

	const TArray<FColor> Captured = MakeCapturedColors(3, /*AlphaBase=*/17);
	TestTrue(TEXT("EnsureBaselineCaptured succeeds"), Owner.EnsureBaselineCaptured(Captured));
	TestTrue(TEXT("AreColorsInitialized() true"), Owner.AreColorsInitialized());
	for (int32 i = 0; i < Captured.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("Baseline[%d] byte-identical to captured"), i), Owner.GetBaselineColors()[i], Captured[i]);
	}
	// AUDITED: EnsureBaselineCaptured (via InitializeColors) already seeds Baseline == Committed ==
	// Working and stamps Authority -- this is itself a valid, self-consistent state (WorkingColors
	// correctly reflects "no layer composed yet"), so the binding already validates here.
	TestTrue(TEXT("Binding already valid right after baseline capture"), Owner.ValidateBinding() == EStatus::Success);

	// Idempotency: a second EnsureBaselineCaptured with DIFFERENT content is a no-op -- Baseline only
	// ever changes via this one gated operation, never silently on every call.
	const TArray<FColor> DifferentColors = MakeCapturedColors(3, /*AlphaBase=*/99);
	TestTrue(TEXT("Second EnsureBaselineCaptured succeeds (no-op)"), Owner.EnsureBaselineCaptured(DifferentColors));
	for (int32 i = 0; i < Captured.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("Baseline[%d] unchanged by second capture"), i), Owner.GetBaselineColors()[i], Captured[i]);
	}

	// Cardinality mismatch rejected outright -- no partial write.
	TArray<FColor> WrongCardinality;
	WrongCardinality.Add(FColor::Black);
	TestFalse(TEXT("Wrong cardinality rejected"), Owner.ApplyComposedColorsRGB(WrongCardinality, EMode::PreviewOnly));

	// The caller deliberately supplies a WRONG alpha (0) to prove the owner does NOT trust it -- this is
	// the exact gap the previous mutate-then-reseal API could not close (a cooperative caller was
	// required to preserve alpha manually; here it is structurally impossible for the caller to affect it).
	TArray<FColor> FinalRGB;
	FinalRGB.Add(FColor(10, 20, 30, 0));
	FinalRGB.Add(FColor(40, 50, 60, 0));
	FinalRGB.Add(FColor(70, 80, 90, 0));
	TestTrue(TEXT("ApplyComposedColorsRGB (PreviewOnly) succeeds"), Owner.ApplyComposedColorsRGB(FinalRGB, EMode::PreviewOnly));

	for (int32 i = 0; i < FinalRGB.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("Working[%d].R matches submitted RGB"), i), Owner.GetWorkingColors()[i].R, FinalRGB[i].R);
		TestNotEqual(*FString::Printf(TEXT("Working[%d].A did NOT take the caller's (wrong) alpha"), i), Owner.GetWorkingColors()[i].A, (uint8)0);
		TestEqual(*FString::Printf(TEXT("Working[%d].A forced from Baseline instead"), i), Owner.GetWorkingColors()[i].A, Captured[i].A);
	}
	// PreviewOnly must NOT touch Committed.
	for (int32 i = 0; i < Captured.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("Committed[%d] unchanged by PreviewOnly"), i), Owner.GetCommittedColors()[i], Captured[i]);
	}
	TestTrue(TEXT("Binding validates after PreviewOnly application"), Owner.ValidateBinding() == EStatus::Success);
	TestEqual(TEXT("Generation unaffected by composition"), MeshOwner->GetGeneration(), GenerationBefore);

	// Consolidate DOES promote Committed to match Working.
	TArray<FColor> SecondRGB;
	SecondRGB.Add(FColor(1, 2, 3, 255));
	SecondRGB.Add(FColor(4, 5, 6, 255));
	SecondRGB.Add(FColor(7, 8, 9, 255));
	TestTrue(TEXT("ApplyComposedColorsRGB (Consolidate) succeeds"), Owner.ApplyComposedColorsRGB(SecondRGB, EMode::Consolidate));
	for (int32 i = 0; i < SecondRGB.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("Committed[%d].R promoted to match Working after Consolidate"), i), Owner.GetCommittedColors()[i].R, SecondRGB[i].R);
		TestEqual(*FString::Printf(TEXT("Committed[%d].A still forced from Baseline"), i), Owner.GetCommittedColors()[i].A, Captured[i].A);
	}

	TestEqual(TEXT("Target component unaffected by composition"), Owner.GetPreviewState().GetSourceComponent().Get(), Fixture.Component.Get());
	TestEqual(TEXT("InstanceResults untouched by composition"), Owner.GetPreviewState().GetInstanceResults().Num(), 0);

	return true;
}

// M16-J.0B (2nd rejection-corrective pass): a second component (same MeshOwner) is completely unaffected
// by the first component's composition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerComposedColorsSiblingUnaffectedTest, "VertexMaskForge.WorkingStateOwner.ComposedColorsSiblingUnaffected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerComposedColorsSiblingUnaffectedTest::RunTest(const FString& Parameters)
{
	using EMode = FVertexMaskForgeWorkingStateOwner::EColorCommitMode;

	FOwnerTestFixture Fixture;
	TStrongObjectPtr<UStaticMeshComponent> OtherComponent(NewObject<UStaticMeshComponent>());
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner OwnerA;
	OwnerA.ConfigureTarget(Fixture.Component.Get());
	OwnerA.AttachToMeshOwner(MeshOwner);
	OwnerA.EnsureBaselineCaptured(MakeCapturedColors(3, 0));

	FVertexMaskForgeWorkingStateOwner OwnerB;
	OwnerB.ConfigureTarget(OtherComponent.Get());
	OwnerB.AttachToMeshOwner(MeshOwner);
	const TArray<FColor> CapturedB = MakeCapturedColors(3, 200);
	OwnerB.EnsureBaselineCaptured(CapturedB);

	TArray<FColor> FinalRGB;
	FinalRGB.Add(FColor(1, 1, 1, 255));
	FinalRGB.Add(FColor(2, 2, 2, 255));
	FinalRGB.Add(FColor(3, 3, 3, 255));
	OwnerA.ApplyComposedColorsRGB(FinalRGB, EMode::Consolidate);

	for (int32 i = 0; i < CapturedB.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("OwnerB Baseline[%d] untouched"), i), OwnerB.GetBaselineColors()[i], CapturedB[i]);
	}
	// AUDITED: OwnerB's own EnsureBaselineCaptured already stamped its own Authority (unaffected by
	// OwnerA's separate ApplyComposedColorsRGB call) -- OwnerB's binding is valid on its own, independent
	// lifecycle, which is exactly the point being proven: siblings never interfere with each other.
	TestTrue(TEXT("OwnerB's own binding is valid, on its own independent lifecycle"), OwnerB.ValidateBinding() == EStatus::Success);
	TestEqual(TEXT("OwnerB's WorkingColors alpha is its own Baseline's, not OwnerA's"), OwnerB.GetWorkingColors()[0].A, CapturedB[0].A);

	return true;
}

// 18.5: composição RGB existente pode ser escrita através de UpdateWorkingColorsRGB.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerUpdateWorkingColorsRGBTest, "VertexMaskForge.WorkingStateOwner.UpdateWorkingColorsRGB", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerUpdateWorkingColorsRGBTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner Owner;
	Owner.ConfigureTarget(Fixture.Component.Get());
	Owner.AttachToMeshOwner(MeshOwner);
	const uint64 GenerationBefore = MeshOwner->GetGeneration();
	Owner.InitializeColors(MakeCapturedColors(3));

	TArray<uint8> AlphasBefore;
	for (const FColor& C : Owner.GetWorkingColors())
	{
		AlphasBefore.Add(C.A);
	}

	TArray<FColor> ExpectedRGB;
	ExpectedRGB.Add(FColor(10, 20, 30, 255));
	ExpectedRGB.Add(FColor(40, 50, 60, 255));
	ExpectedRGB.Add(FColor(70, 80, 90, 255));

	TestFalse(TEXT("Wrong cardinality rejected"), Owner.UpdateWorkingColorsRGB(TArrayView<const FColor>(ExpectedRGB.GetData(), 2)));
	TestTrue(TEXT("UpdateWorkingColorsRGB succeeds"), Owner.UpdateWorkingColorsRGB(ExpectedRGB));

	for (int32 i = 0; i < ExpectedRGB.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("RGB[%d] matches expected"), i), Owner.GetWorkingColors()[i].R, ExpectedRGB[i].R);
		TestEqual(*FString::Printf(TEXT("Alpha[%d] preserved (not the input's own Alpha)"), i), Owner.GetWorkingColors()[i].A, AlphasBefore[i]);
	}
	TestEqual(TEXT("Generation unchanged"), MeshOwner->GetGeneration(), GenerationBefore);
	TestTrue(TEXT("Authority still valid"), Owner.GetPreviewState().GetWorkingColorsAuthority().IsValid());
	TestTrue(TEXT("Binding still validates"), Owner.ValidateBinding() == EStatus::Success);

	return true;
}

// 18.6: Fill Black / Fill White pelo caminho do owner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerFillTest, "VertexMaskForge.WorkingStateOwner.FillBlackWhite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerFillTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner Owner;
	Owner.ConfigureTarget(Fixture.Component.Get());
	Owner.AttachToMeshOwner(MeshOwner);

	TestFalse(TEXT("Fill before colors initialized rejected"), Owner.FillBlack());

	Owner.InitializeColors(MakeCapturedColors(3));
	TArray<uint8> AlphasBefore;
	for (const FColor& C : Owner.GetWorkingColors())
	{
		AlphasBefore.Add(C.A);
	}

	TestTrue(TEXT("FillBlack succeeds"), Owner.FillBlack());
	for (int32 i = 0; i < Owner.GetWorkingColors().Num(); ++i)
	{
		const FColor& C = Owner.GetWorkingColors()[i];
		TestEqual(*FString::Printf(TEXT("FillBlack R[%d]==0"), i), C.R, (uint8)0);
		TestEqual(*FString::Printf(TEXT("FillBlack alpha[%d] preserved"), i), C.A, AlphasBefore[i]);
		TestEqual(*FString::Printf(TEXT("FillBlack committed[%d] matches working"), i), Owner.GetCommittedColors()[i], C);
	}
	TestTrue(TEXT("Binding still validates"), Owner.ValidateBinding() == EStatus::Success);

	TestTrue(TEXT("FillWhite succeeds"), Owner.FillWhite());
	for (int32 i = 0; i < Owner.GetWorkingColors().Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("FillWhite R[%d]==255"), i), Owner.GetWorkingColors()[i].R, (uint8)255);
		TestEqual(*FString::Printf(TEXT("FillWhite alpha[%d] preserved"), i), Owner.GetWorkingColors()[i].A, AlphasBefore[i]);
	}

	return true;
}

// 18.6: Restore.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerRestoreTest, "VertexMaskForge.WorkingStateOwner.Restore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerRestoreTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner Owner;
	Owner.ConfigureTarget(Fixture.Component.Get());
	Owner.AttachToMeshOwner(MeshOwner);
	const TArray<FColor> Baseline = MakeCapturedColors(3);
	Owner.InitializeColors(Baseline);

	Owner.FillBlack();
	const TArray<FColor> AfterFillBlack = Owner.GetCommittedColors();

	TestTrue(TEXT("RestoreFromBaseline succeeds"), Owner.RestoreFromBaseline());
	for (int32 i = 0; i < Baseline.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("RestoreFromBaseline WorkingColors[%d] == Baseline"), i), Owner.GetWorkingColors()[i], Baseline[i]);
	}
	for (int32 i = 0; i < AfterFillBlack.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("RestoreFromBaseline did not touch CommittedColors[%d]"), i), Owner.GetCommittedColors()[i], AfterFillBlack[i]);
	}
	TestTrue(TEXT("Binding validates after RestoreFromBaseline"), Owner.ValidateBinding() == EStatus::Success);

	TestTrue(TEXT("RestoreFromCommitted succeeds"), Owner.RestoreFromCommitted());
	for (int32 i = 0; i < AfterFillBlack.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("RestoreFromCommitted WorkingColors[%d] == last committed (post-FillBlack)"), i), Owner.GetWorkingColors()[i], AfterFillBlack[i]);
	}

	return true;
}

// 18.8 / M16-J.0A.1 test 7: ausência de efeitos colaterais / preservação de estado.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingStateOwnerNoSideEffectsTest, "VertexMaskForge.WorkingStateOwner.NoSideEffects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingStateOwnerNoSideEffectsTest::RunTest(const FString& Parameters)
{
	FOwnerTestFixture Fixture;
	const TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeReadyMeshOwner(Fixture.Mesh.Get(), 3);

	FVertexMaskForgeWorkingStateOwner Owner;
	Owner.ConfigureTarget(Fixture.Component.Get());
	Owner.AttachToMeshOwner(MeshOwner);
	Owner.InitializeColors(MakeCapturedColors(3));
	Owner.FillBlack();
	Owner.FillWhite();
	Owner.RestoreFromBaseline();
	Owner.Reset();
	Owner.ValidateBinding();

	TestEqual(TEXT("MeshOwner's WorkingMesh.InstanceResults untouched throughout"), MeshOwner->GetWorkingMesh().InstanceResults.Num(), 0);
	TestEqual(TEXT("PreviewState.InstanceResults untouched throughout"), Owner.GetPreviewState().GetInstanceResults().Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
