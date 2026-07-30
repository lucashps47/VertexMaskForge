// M16-J.0A: validator-only unit tests for VertexMaskForgeWorkingColorsProvenance -- pure default-state
// branches of ValidateWorkingColorsPublicationBinding that do not require an authoritatively-populated
// Provenance/Authority. These are NOT proof of the owner lifecycle -- see
// VertexMaskForgeWorkingStateOwnerTests.cpp for that (per this checkpoint's own "Somente os testes do
// owner contam como prova da seam headless" requirement).
//
// AUDITED (M16-J.0A): the M16-J.0 version of this file (14 tests) constructed "valid" Provenance/
// Authority directly via the (now-removed) public MakeWorkingMeshProvenance/MakeWorkingColorsAuthority
// factories -- exactly the external-fabrication vector M16-J.0A's encapsulation closes (see
// VertexMaskForgeWorkingColorsProvenance.h's own module comment). Those tests are incompatible with the
// new encapsulation (FVertexMaskForgeWorkingMeshProvenance/FVertexMaskForgeWorkingColorsAuthority/
// FVertexMaskForgeWorkingColorPublicationBinding now have private fields, settable only by
// FVertexMaskForgeWorkingStateOwner) and have been removed rather than reworked around a test-only
// backdoor. Their branch coverage (Success, ComponentMismatch, MeshMismatch, LODMismatch, DomainMismatch,
// StaleWorkingMesh, CardinalityMismatch, AlphaSourceCardinalityMismatch, cross-owner rejection, move
// preservation) is now provided by VertexMaskForgeWorkingStateOwnerTests.cpp, exercised through the real
// production API instead of synthetic metadata.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeWorkingColorsProvenance.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	using EStatus = EVertexMaskForgeWorkingColorsPublicationValidationStatus;
}

// A default-constructed Binding (no MeshOwner) fails deterministically -- Target is a fresh, unrelated
// FVertexMaskForgePreviewComponentState, supplied only because ValidateWorkingColorsPublicationBinding's
// signature requires it (M16-J.0B.1 2nd corrective pass); step 1 (IsWellFormed()) rejects before Target is
// ever read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingColorsProvenanceInvalidOwnerTest, "VertexMaskForge.WorkingColorsProvenance.InvalidOwner", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingColorsProvenanceInvalidOwnerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingColorPublicationBinding EmptyBinding;
	TestFalse(TEXT("Empty binding is not well-formed"), EmptyBinding.IsWellFormed());
	const FVertexMaskForgePreviewComponentState UnrelatedTarget;
	const EStatus Status = VertexMaskForgeWorkingColorsProvenance::ValidateWorkingColorsPublicationBinding(EmptyBinding, UnrelatedTarget);
	TestTrue(TEXT("Status == InvalidOwner"), Status == EStatus::InvalidOwner);
	return true;
}

// A freshly default-constructed FVertexMaskForgeWorkingMesh carries no Provenance -- IsValid() == false,
// without needing any factory to prove it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingColorsProvenanceFreshWorkingMeshTest, "VertexMaskForge.WorkingColorsProvenance.FreshWorkingMeshHasNoProvenance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingColorsProvenanceFreshWorkingMeshTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh Fresh;
	TestFalse(TEXT("Fresh WorkingMesh.Provenance.IsValid() == false"), Fresh.Provenance.IsValid());
	return true;
}

// A freshly default-constructed FVertexMaskForgePreviewComponentState carries no alpha authority.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingColorsProvenanceFreshPreviewStateTest, "VertexMaskForge.WorkingColorsProvenance.FreshPreviewStateHasNoAuthority", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingColorsProvenanceFreshPreviewStateTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgePreviewComponentState Fresh;
	TestFalse(TEXT("Fresh PreviewState.WorkingColorsAuthority.IsValid() == false"), Fresh.GetWorkingColorsAuthority().IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
