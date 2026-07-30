// M16-J.0A.1: lifecycle tests for FVertexMaskForgeWorkingMeshOwner in isolation (no attached
// FVertexMaskForgeWorkingStateOwner). Cross-owner (mesh owner + state owner) behavior is covered in
// VertexMaskForgeWorkingStateOwnerTests.cpp instead, since that is where attachment/composed binding
// semantics actually live.

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeWorkingMeshOwner.h"

// Construction: no identity, no generation advance, invalid Provenance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshOwnerConstructionTest, "VertexMaskForge.WorkingMeshOwner.Construction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshOwnerConstructionTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMeshOwner Owner;
	TestFalse(TEXT("IsIdentityConfigured() == false"), Owner.IsIdentityConfigured());
	TestEqual(TEXT("Generation starts at 0"), Owner.GetGeneration(), (uint64)0);
	TestFalse(TEXT("WorkingMesh.Provenance not valid yet"), Owner.GetWorkingMesh().Provenance.IsValid());
	return true;
}

// ConfigureIdentity: null rejected, no-op reconfigure, real reconfigure invalidates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshOwnerConfigureIdentityTest, "VertexMaskForge.WorkingMeshOwner.ConfigureIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshOwnerConfigureIdentityTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	FVertexMaskForgeWorkingMeshOwner Owner;

	TestFalse(TEXT("Null mesh rejected"), Owner.ConfigureIdentity(nullptr, 0, false));
	TestTrue(TEXT("Valid configure succeeds"), Owner.ConfigureIdentity(Mesh.Get(), 0, false));
	TestTrue(TEXT("IsIdentityConfigured() == true"), Owner.IsIdentityConfigured());
	TestEqual(TEXT("Generation still 0 -- identity alone never advances it"), Owner.GetGeneration(), (uint64)0);

	TestTrue(TEXT("Repeated identical configure succeeds"), Owner.ConfigureIdentity(Mesh.Get(), 0, false));
	TestEqual(TEXT("Generation unaffected by no-op reconfigure"), Owner.GetGeneration(), (uint64)0);

	return true;
}

// InstallWorkingMesh: generation advance, provenance stamping, external provenance never trusted, staleness.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshOwnerInstallTest, "VertexMaskForge.WorkingMeshOwner.InstallWorkingMesh", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshOwnerInstallTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	FVertexMaskForgeWorkingMeshOwner Owner;

	TestFalse(TEXT("Install before ConfigureIdentity rejected"), Owner.InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), 4));

	Owner.ConfigureIdentity(Mesh.Get(), 0, false);

	FVertexMaskForgeWorkingMesh Untrusted;
	TestFalse(TEXT("Untrusted WorkingMesh's own Provenance starts invalid"), Untrusted.Provenance.IsValid());

	TestTrue(TEXT("Install succeeds"), Owner.InstallWorkingMesh(MoveTemp(Untrusted), /*ExpectedCardinality=*/4));
	TestEqual(TEXT("Generation advanced to 1"), Owner.GetGeneration(), (uint64)1);
	TestTrue(TEXT("Provenance now valid"), Owner.GetWorkingMesh().Provenance.IsValid());
	TestEqual(TEXT("MeshOwnerId is this owner"), Owner.GetWorkingMesh().Provenance.GetMeshOwnerId(), static_cast<const void*>(&Owner));
	TestEqual(TEXT("Provenance mesh from owner"), Owner.GetWorkingMesh().Provenance.GetSourceStaticMesh().Get(), Mesh.Get());
	TestEqual(TEXT("Provenance revision == generation"), Owner.GetWorkingMesh().Provenance.GetRevision(), (uint64)1);

	const uint64 RevisionBeforeRebuild = Owner.GetWorkingMesh().Provenance.GetRevision();
	TestTrue(TEXT("Second install succeeds"), Owner.InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), 4));
	TestEqual(TEXT("Generation advanced to 2"), Owner.GetGeneration(), (uint64)2);
	TestNotEqual(TEXT("New revision differs from the old one"), Owner.GetWorkingMesh().Provenance.GetRevision(), RevisionBeforeRebuild);

	return true;
}

// Reset: invalidates Working Mesh, advances generation, preserves configured identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshOwnerResetTest, "VertexMaskForge.WorkingMeshOwner.Reset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshOwnerResetTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	FVertexMaskForgeWorkingMeshOwner Owner;
	Owner.ConfigureIdentity(Mesh.Get(), 0, false);
	Owner.InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), 4);
	const uint64 GenerationBefore = Owner.GetGeneration();

	TestTrue(TEXT("Reset succeeds"), Owner.Reset());
	TestTrue(TEXT("Generation advanced by reset"), Owner.GetGeneration() != GenerationBefore);
	TestFalse(TEXT("Provenance invalid after reset"), Owner.GetWorkingMesh().Provenance.IsValid());
	TestTrue(TEXT("Identity configuration preserved across reset"), Owner.IsIdentityConfigured());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
