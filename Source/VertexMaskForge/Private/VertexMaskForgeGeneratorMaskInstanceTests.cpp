// M16-K.5B: automation tests for FVertexMaskForgeGeneratorMaskInstance identity/ownership and the three
// new FVertexMaskForgeDynamicLayerStack mutators (SetLayerMaskGeneratorType/ClearLayerMask/GetLayerMask).
// Nothing here constructs SVertexMaskForgePanel, touches VertexMaskForgeDynamicLayerEvaluator's real
// production behavior beyond the one dedicated inertness proof (EvaluatorIgnoresMaskInstance), or reads/
// writes any Params field -- editable parameters are explicitly out of scope, deferred to M16-K.5C.
// Independence between layers/instances is proven exclusively via distinct MaskInstanceIds, per-layer
// TOptional ownership, and assign/replace/clear on one layer having zero effect on another -- never via
// TVariant internal address comparison (per this checkpoint's own instruction).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"

// 1. NewLayerStartsWithNoMask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceNewLayerTest, "VertexMaskForge.GeneratorMaskInstance.NewLayerStartsWithNoMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceNewLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	TestNull(TEXT("GetLayerMask returns nullptr for a freshly added layer"), Stack.GetLayerMask(Id));

	const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
	TestNotNull(TEXT("Layer itself exists"), Layer);
	if (Layer)
	{
		TestFalse(TEXT("Layer.Mask is unset"), Layer->Mask.IsSet());
	}

	return true;
}

// 2. AssignMaskAffectsOnlyTargetLayer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceAssignScopeTest, "VertexMaskForge.GeneratorMaskInstance.AssignMaskAffectsOnlyTargetLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceAssignScopeTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));

	TestTrue(TEXT("Assign Curvature to A succeeds"), Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Curvature));

	TestNotNull(TEXT("A now has a mask"), Stack.GetLayerMask(A));
	TestNull(TEXT("B still has no mask"), Stack.GetLayerMask(B));

	return true;
}

// 3. SameGeneratorTypeCreatesIndependentInstancesAcrossLayers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceIndependentAcrossLayersTest, "VertexMaskForge.GeneratorMaskInstance.SameGeneratorTypeCreatesIndependentInstancesAcrossLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceIndependentAcrossLayersTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));

	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Curvature);
	Stack.SetLayerMaskGeneratorType(B, EVertexMaskForgeGeneratorType::Curvature);

	const FVertexMaskForgeGeneratorMaskInstance* MaskA = Stack.GetLayerMask(A);
	const FVertexMaskForgeGeneratorMaskInstance* MaskB = Stack.GetLayerMask(B);
	TestNotNull(TEXT("A has a mask"), MaskA);
	TestNotNull(TEXT("B has a mask"), MaskB);
	if (MaskA && MaskB)
	{
		TestTrue(TEXT("Both are Curvature"), MaskA->GeneratorType == EVertexMaskForgeGeneratorType::Curvature && MaskB->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
		TestNotEqual(TEXT("Distinct MaskInstanceIds despite same generator type"), MaskA->MaskInstanceId, MaskB->MaskInstanceId);
	}

	// Independence proof: replacing A's mask type must not affect B's.
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise);
	MaskB = Stack.GetLayerMask(B);
	TestNotNull(TEXT("B still has a mask after A's replace"), MaskB);
	if (MaskB)
	{
		TestTrue(TEXT("B is still Curvature, unaffected by A's replace"), MaskB->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
	}

	return true;
}

// 4. SameTypeAssignmentIsNoOp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceSameTypeNoOpTest, "VertexMaskForge.GeneratorMaskInstance.SameTypeAssignmentIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceSameTypeNoOpTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	TestTrue(TEXT("First assign succeeds"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::AmbientOcclusion));
	const FGuid OriginalInstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	TestTrue(TEXT("Re-requesting the same type succeeds"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::AmbientOcclusion));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present"), Mask);
	if (Mask)
	{
		TestEqual(TEXT("MaskInstanceId preserved -- not recreated"), Mask->MaskInstanceId, OriginalInstanceId);
		TestTrue(TEXT("GeneratorType unchanged"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::AmbientOcclusion);
	}

	return true;
}

// 5. DifferentTypeReplacementMintsNewInstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceReplaceMintsNewIdTest, "VertexMaskForge.GeneratorMaskInstance.DifferentTypeReplacementMintsNewInstanceId", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceReplaceMintsNewIdTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Curvature);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	TestTrue(TEXT("Replace with a different type succeeds"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present"), Mask);
	if (Mask)
	{
		TestNotEqual(TEXT("MaskInstanceId changed on type replacement"), Mask->MaskInstanceId, OriginalInstanceId);
		TestTrue(TEXT("GeneratorType is now Noise"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
	}

	return true;
}

// 6. ReplacementCreatesMatchingDefaultParamsAlternative.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceParamsAlternativeMatchesTypeTest, "VertexMaskForge.GeneratorMaskInstance.ReplacementCreatesMatchingDefaultParamsAlternative", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceParamsAlternativeMatchesTypeTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Thickness);
	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask present"), Mask);
	if (Mask)
	{
		TestTrue(TEXT("Params holds the Thickness alternative"), Mask->Params.IsType<FVertexMaskForgeThicknessParams>());
		TestFalse(TEXT("Params does NOT hold an unrelated alternative"), Mask->Params.IsType<FVertexMaskForgeCurvatureParams>());
	}

	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::MaterialSlot);
	Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present after replace"), Mask);
	if (Mask)
	{
		TestTrue(TEXT("Params now holds the MaterialSlot alternative"), Mask->Params.IsType<FVertexMaskForgeMaterialSlotParams>());
	}

	return true;
}

// 7. RenamePreservesMaskInstance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceRenamePreservesTest, "VertexMaskForge.GeneratorMaskInstance.RenamePreservesMaskInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceRenamePreservesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Original"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::BoundingBox);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	TestTrue(TEXT("Rename succeeds"), Stack.RenameLayer(Id, TEXT("Renamed")));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask survives rename"), Mask);
	if (Mask)
	{
		TestEqual(TEXT("MaskInstanceId unchanged"), Mask->MaskInstanceId, OriginalInstanceId);
	}

	return true;
}

// 8. ReorderPreservesMaskInstance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceReorderPreservesTest, "VertexMaskForge.GeneratorMaskInstance.ReorderPreservesMaskInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceReorderPreservesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::DirectionalNormal);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(A)->MaskInstanceId;

	TestTrue(TEXT("MoveLayerUp(B) succeeds"), Stack.MoveLayerUp(B));
	TestEqual(TEXT("B now first"), Stack.GetLayers()[0].LayerId, B);

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(A);
	TestNotNull(TEXT("A's mask survives the reorder"), Mask);
	if (Mask)
	{
		TestEqual(TEXT("MaskInstanceId unchanged"), Mask->MaskInstanceId, OriginalInstanceId);
		TestTrue(TEXT("GeneratorType unchanged"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::DirectionalNormal);
	}

	return true;
}

// 9. RemovingOtherLayerDoesNotTransferMask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceRemoveOtherTest, "VertexMaskForge.GeneratorMaskInstance.RemovingOtherLayerDoesNotTransferMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceRemoveOtherTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::AmbientOcclusion);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(A)->MaskInstanceId;
	// B intentionally left without a mask.

	Stack.RemoveLayer(B);

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(A);
	TestNotNull(TEXT("A's mask unaffected by B's removal"), Mask);
	if (Mask)
	{
		TestEqual(TEXT("MaskInstanceId unchanged"), Mask->MaskInstanceId, OriginalInstanceId);
	}
	TestEqual(TEXT("Only one layer remains"), Stack.Num(), 1);

	return true;
}

// 10. RemovingMaskedLayerEndsOwnership.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceRemoveOwnTest, "VertexMaskForge.GeneratorMaskInstance.RemovingMaskedLayerEndsOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceRemoveOwnTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::MaterialSlot);

	const FVertexMaskForgeDynamicLayerStack::FRemoveResult Result = Stack.RemoveLayer(A);
	TestTrue(TEXT("Removed"), Result.bRemoved);
	TestNull(TEXT("Layer itself is gone"), Stack.FindLayerById(A));
	TestNull(TEXT("GetLayerMask on the removed id returns nullptr"), Stack.GetLayerMask(A));
	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 11. UnknownGuidMaskOperationsFailSafely.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceUnknownGuidTest, "VertexMaskForge.GeneratorMaskInstance.UnknownGuidMaskOperationsFailSafely", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceUnknownGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Real = Stack.AddLayer(TEXT("Real"));
	const FGuid Unknown = FGuid::NewGuid();

	TestFalse(TEXT("SetLayerMaskGeneratorType on unknown id fails"), Stack.SetLayerMaskGeneratorType(Unknown, EVertexMaskForgeGeneratorType::Curvature));
	TestFalse(TEXT("ClearLayerMask on unknown id fails"), Stack.ClearLayerMask(Unknown));
	TestNull(TEXT("GetLayerMask on unknown id returns nullptr"), Stack.GetLayerMask(Unknown));

	TestNull(TEXT("Real layer still has no mask (untouched)"), Stack.GetLayerMask(Real));
	TestEqual(TEXT("Only one layer exists"), Stack.Num(), 1);

	return true;
}

// 12. ClearMaskPreservesLayerState.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceClearPreservesLayerTest, "VertexMaskForge.GeneratorMaskInstance.ClearMaskPreservesLayerState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceClearPreservesLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerOpacity(Id, 0.42f);
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);

	TestTrue(TEXT("ClearLayerMask succeeds"), Stack.ClearLayerMask(Id));
	TestNull(TEXT("Mask is gone"), Stack.GetLayerMask(Id));

	const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
	TestNotNull(TEXT("Layer still exists"), Layer);
	if (Layer)
	{
		TestEqual(TEXT("LayerId unaffected"), Layer->LayerId, Id);
		TestTrue(TEXT("Fill unaffected"), Layer->Fill == EVertexMaskForgeLayerFill::White);
		TestEqual(TEXT("Opacity unaffected"), Layer->Opacity, 0.42f);
		TestFalse(TEXT("Layer.Mask is now unset"), Layer->Mask.IsSet());
	}

	return true;
}

// 13. ClearAlreadyEmptyMaskIsIdempotent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceClearIdempotentTest, "VertexMaskForge.GeneratorMaskInstance.ClearAlreadyEmptyMaskIsIdempotent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceClearIdempotentTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	TestTrue(TEXT("Clear on a layer with no mask still succeeds"), Stack.ClearLayerMask(Id));
	TestTrue(TEXT("Clear again is still a harmless success"), Stack.ClearLayerMask(Id));
	TestNull(TEXT("Still no mask"), Stack.GetLayerMask(Id));

	return true;
}

// 14. ClearThenReassignMintsNewInstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceClearThenReassignTest, "VertexMaskForge.GeneratorMaskInstance.ClearThenReassignMintsNewInstanceId", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceClearThenReassignTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Curvature);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	Stack.ClearLayerMask(Id);
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Curvature); // Same type as before clearing.

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask present after reassign"), Mask);
	if (Mask)
	{
		TestNotEqual(TEXT("Fresh MaskInstanceId, never reused across a Clear boundary"), Mask->MaskInstanceId, OriginalInstanceId);
	}

	return true;
}

// 15. LayerAndMaskIdsRemainDistinctAndValid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceIdsDistinctTest, "VertexMaskForge.GeneratorMaskInstance.LayerAndMaskIdsRemainDistinctAndValid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceIdsDistinctTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	TSet<FGuid> AllIdsEverSeen;

	auto RecordId = [&AllIdsEverSeen](const FGuid& Id) -> bool
	{
		if (!Id.IsValid())
		{
			return false;
		}
		bool bAlready = false;
		AllIdsEverSeen.Add(Id, &bAlready);
		return !bAlready;
	};

	const FGuid A = Stack.AddLayer(TEXT("A"));
	TestTrue(TEXT("LayerId A valid and unique"), RecordId(A));

	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Curvature);
	TestTrue(TEXT("MaskInstanceId (Curvature) valid and unique"), RecordId(Stack.GetLayerMask(A)->MaskInstanceId));

	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise); // Replace -> new id.
	TestTrue(TEXT("MaskInstanceId (Noise, replaced) valid and unique"), RecordId(Stack.GetLayerMask(A)->MaskInstanceId));

	Stack.ClearLayerMask(A);
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise); // Reassign after clear -> new id.
	TestTrue(TEXT("MaskInstanceId (Noise, reassigned after clear) valid and unique"), RecordId(Stack.GetLayerMask(A)->MaskInstanceId));

	const FGuid B = Stack.AddLayer(TEXT("B"));
	TestTrue(TEXT("LayerId B valid and unique, distinct from every mask id seen so far"), RecordId(B));

	TestEqual(TEXT("Five distinct valid ids collected total"), AllIdsEverSeen.Num(), 5);

	return true;
}

// 16. CopyAndMovePreserveValueSemantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceCopyMoveTest, "VertexMaskForge.GeneratorMaskInstance.CopyAndMovePreserveValueSemantics", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceCopyMoveTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Thickness);
	const FGuid OriginalInstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	// Explicit copy of the FVertexMaskForgeLayer VALUE (not through the stack) -- proves
	// FVertexMaskForgeGeneratorMaskInstance/TOptional/TVariant all copy correctly, with the SAME
	// MaskInstanceId preserved (copying is not "duplicate layer", per this checkpoint's own semantics).
	const FVertexMaskForgeLayer* OriginalLayer = Stack.FindLayerById(Id);
	TestNotNull(TEXT("Original layer found"), OriginalLayer);
	if (!OriginalLayer)
	{
		return false;
	}

	FVertexMaskForgeLayer CopiedLayer = *OriginalLayer; // Copy-construct.
	TestTrue(TEXT("Copy has a mask"), CopiedLayer.Mask.IsSet());
	if (CopiedLayer.Mask.IsSet())
	{
		TestEqual(TEXT("Copy preserves the same MaskInstanceId"), CopiedLayer.Mask->MaskInstanceId, OriginalInstanceId);
		TestTrue(TEXT("Copy preserves GeneratorType"), CopiedLayer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::Thickness);
	}

	// Move-construct from the copy -- moved-from state is irrelevant (about to be discarded); the moved-TO
	// value must be intact.
	FVertexMaskForgeLayer MovedLayer = MoveTemp(CopiedLayer);
	TestTrue(TEXT("Moved-to layer has a mask"), MovedLayer.Mask.IsSet());
	if (MovedLayer.Mask.IsSet())
	{
		TestEqual(TEXT("Move preserves the same MaskInstanceId"), MovedLayer.Mask->MaskInstanceId, OriginalInstanceId);
	}

	// The ORIGINAL, still owned by the stack, must be completely unaffected by any of the above.
	const FVertexMaskForgeGeneratorMaskInstance* StillOriginal = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Stack's own original mask instance untouched"), StillOriginal);
	if (StillOriginal)
	{
		TestEqual(TEXT("Stack's original MaskInstanceId unchanged"), StillOriginal->MaskInstanceId, OriginalInstanceId);
	}

	return true;
}

// 17. BaseLayerUsesSameMaskRules.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceBaseLayerTest, "VertexMaskForge.GeneratorMaskInstance.BaseLayerUsesSameMaskRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceBaseLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();
	const FGuid BaseLayerId = Stack.GetLayers()[0].LayerId;

	TestNull(TEXT("Base Layer starts with no mask"), Stack.GetLayerMask(BaseLayerId));

	TestTrue(TEXT("Base Layer can receive a mask"), Stack.SetLayerMaskGeneratorType(BaseLayerId, EVertexMaskForgeGeneratorType::BoundingBox));
	TestNotNull(TEXT("Base Layer now has a mask"), Stack.GetLayerMask(BaseLayerId));

	TestTrue(TEXT("Base Layer's mask can be replaced"), Stack.SetLayerMaskGeneratorType(BaseLayerId, EVertexMaskForgeGeneratorType::AmbientOcclusion));
	TestTrue(TEXT("Base Layer's mask is now AmbientOcclusion"), Stack.GetLayerMask(BaseLayerId)->GeneratorType == EVertexMaskForgeGeneratorType::AmbientOcclusion);

	TestTrue(TEXT("Base Layer's mask can be cleared"), Stack.ClearLayerMask(BaseLayerId));
	TestNull(TEXT("Base Layer has no mask again"), Stack.GetLayerMask(BaseLayerId));

	// No special restriction anywhere -- the Base Layer is still present and otherwise ordinary.
	TestNotNull(TEXT("Base Layer itself still exists"), Stack.FindLayerById(BaseLayerId));
	TestEqual(TEXT("Still exactly one layer"), Stack.Num(), 1);

	return true;
}

// 18. EvaluatorIgnoresMaskInstance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceEvaluatorInertTest, "VertexMaskForge.GeneratorMaskInstance.EvaluatorIgnoresMaskInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceEvaluatorInertTest::RunTest(const FString& Parameters)
{
	// Two stacks, built to be identical in every field EvaluateColor is documented to read, EXCEPT one
	// layer additionally carries a Mask Instance -- the evaluator itself is never modified to achieve
	// this; the proof is that its real, unmodified production function produces byte-identical output.
	FVertexMaskForgeDynamicLayerStack StackWithoutMask;
	const FGuid IdA = StackWithoutMask.AddLayer(TEXT("Layer"));
	StackWithoutMask.SetLayerFill(IdA, EVertexMaskForgeLayerFill::White);
	StackWithoutMask.SetLayerBlendMode(IdA, EVertexMaskForgeBlendMode::Overlay);
	StackWithoutMask.SetLayerOpacity(IdA, 0.6f);
	StackWithoutMask.SetLayerChannelFilter(IdA, true, false, true);

	FVertexMaskForgeDynamicLayerStack StackWithMask;
	const FGuid IdB = StackWithMask.AddLayer(TEXT("Layer"));
	StackWithMask.SetLayerFill(IdB, EVertexMaskForgeLayerFill::White);
	StackWithMask.SetLayerBlendMode(IdB, EVertexMaskForgeBlendMode::Overlay);
	StackWithMask.SetLayerOpacity(IdB, 0.6f);
	StackWithMask.SetLayerChannelFilter(IdB, true, false, true);
	StackWithMask.SetLayerMaskGeneratorType(IdB, EVertexMaskForgeGeneratorType::Curvature); // The only difference.

	const FVector4f Base(0.3f, 0.4f, 0.5f, 0.9f);
	const FVector4f ResultWithoutMask = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, StackWithoutMask);
	const FVector4f ResultWithMask = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, StackWithMask);

	TestEqual(TEXT("R identical regardless of Mask Instance presence"), ResultWithMask.X, ResultWithoutMask.X);
	TestEqual(TEXT("G identical regardless of Mask Instance presence"), ResultWithMask.Y, ResultWithoutMask.Y);
	TestEqual(TEXT("B identical regardless of Mask Instance presence"), ResultWithMask.Z, ResultWithoutMask.Z);
	TestEqual(TEXT("Alpha identical regardless of Mask Instance presence"), ResultWithMask.W, ResultWithoutMask.W);

	return true;
}

// --- M16-K.5B.1 corrective: SetLayerMaskGeneratorType must reject an unrecognized GeneratorType ---------
// Confirmed gap: MakeVertexMaskForgeGeneratorParams (VertexMaskForgeRecipeTypes.h) is NOT a validator --
// its own switch's `default:` case is shared with MaterialSlot, so an out-of-range GeneratorType value
// used to silently produce a MaterialSlot Params alternative with no failure signal, while GeneratorType
// itself was stored verbatim as the invalid value -- a real, publicly-reachable inconsistency (GeneratorType
// not equal to any real enumerator, yet Params looking like MaterialSlot's). The four tests below prove the
// fix via observable state only (GetLayerMask/FindLayerById/Params.IsType<T>()) -- no instrumentation, no
// GUID hooking, no padding/memcmp comparison.

// 19. InvalidGeneratorTypeOnEmptyMaskFailsWithoutMutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceInvalidTypeEmptyMaskTest, "VertexMaskForge.GeneratorMaskInstance.InvalidGeneratorTypeOnEmptyMaskFailsWithoutMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceInvalidTypeEmptyMaskTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	TestNull(TEXT("Starts with no mask"), Stack.GetLayerMask(Id));

	const bool bResult = Stack.SetLayerMaskGeneratorType(Id, static_cast<EVertexMaskForgeGeneratorType>(255));
	TestFalse(TEXT("Invalid GeneratorType is rejected"), bResult);

	// Observable proof that no Mask Instance (and therefore no MaskInstanceId) was ever created.
	TestNull(TEXT("Still no mask after the rejected call"), Stack.GetLayerMask(Id));
	TestNotNull(TEXT("The layer itself still exists, untouched"), Stack.FindLayerById(Id));

	return true;
}

// 20. InvalidGeneratorTypeDoesNotReplaceExistingMask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceInvalidTypeExistingMaskTest, "VertexMaskForge.GeneratorMaskInstance.InvalidGeneratorTypeDoesNotReplaceExistingMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceInvalidTypeExistingMaskTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("Assign a valid Curvature mask"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Curvature));

	// Capture observable state before the rejected call.
	const FVertexMaskForgeGeneratorMaskInstance* Before = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask present before"), Before);
	if (!Before)
	{
		return false;
	}
	const FGuid OriginalInstanceId = Before->MaskInstanceId;
	const bool bOriginalIsCurvature = Before->Params.IsType<FVertexMaskForgeCurvatureParams>();
	TestTrue(TEXT("Original Params alternative is Curvature"), bOriginalIsCurvature);

	const bool bResult = Stack.SetLayerMaskGeneratorType(Id, static_cast<EVertexMaskForgeGeneratorType>(255));
	TestFalse(TEXT("Invalid GeneratorType is rejected"), bResult);

	// Observable proof the existing instance was left completely alone -- same id, same type, same
	// Params alternative.
	const FVertexMaskForgeGeneratorMaskInstance* After = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present after the rejected call"), After);
	if (After)
	{
		TestEqual(TEXT("MaskInstanceId unchanged"), After->MaskInstanceId, OriginalInstanceId);
		TestTrue(TEXT("GeneratorType still Curvature"), After->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
		TestTrue(TEXT("Params alternative still Curvature"), After->Params.IsType<FVertexMaskForgeCurvatureParams>());
	}

	return true;
}

// 21. InvalidGeneratorTypeOnBaseLayerFailsWithoutMutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceInvalidTypeBaseLayerTest, "VertexMaskForge.GeneratorMaskInstance.InvalidGeneratorTypeOnBaseLayerFailsWithoutMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceInvalidTypeBaseLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();
	const FGuid BaseLayerId = Stack.GetLayers()[0].LayerId;
	TestNull(TEXT("Base Layer starts with no mask"), Stack.GetLayerMask(BaseLayerId));

	const bool bResult = Stack.SetLayerMaskGeneratorType(BaseLayerId, static_cast<EVertexMaskForgeGeneratorType>(255));
	TestFalse(TEXT("Invalid GeneratorType is rejected on the Base Layer, same as any other layer"), bResult);

	TestNull(TEXT("Base Layer still has no mask"), Stack.GetLayerMask(BaseLayerId));
	TestNotNull(TEXT("Base Layer itself still exists"), Stack.FindLayerById(BaseLayerId));
	TestEqual(TEXT("Still exactly one layer"), Stack.Num(), 1);

	return true;
}

// 22. ValidGeneratorTypesStillCreateMatchingPayloadAlternatives -- regression proof that the new guard
// does not affect any of the 7 real generator types.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeGeneratorMaskInstanceAllValidTypesStillWorkTest, "VertexMaskForge.GeneratorMaskInstance.ValidGeneratorTypesStillCreateMatchingPayloadAlternatives", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeGeneratorMaskInstanceAllValidTypesStillWorkTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;

	auto CheckType = [this, &Stack](const EVertexMaskForgeGeneratorType Type, const TCHAR* Label, TFunctionRef<bool(const FVertexMaskForgeGeneratorParams&)> IsExpectedAlternative)
	{
		const FGuid Id = Stack.AddLayer(Label);
		const bool bResult = Stack.SetLayerMaskGeneratorType(Id, Type);
		TestTrue(FString::Printf(TEXT("%s: SetLayerMaskGeneratorType returns true"), Label), bResult);

		const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
		TestNotNull(FString::Printf(TEXT("%s: mask instance is non-null"), Label), Mask);
		if (Mask)
		{
			TestTrue(FString::Printf(TEXT("%s: MaskInstanceId is valid"), Label), Mask->MaskInstanceId.IsValid());
			TestTrue(FString::Printf(TEXT("%s: GeneratorType is exactly the requested type"), Label), Mask->GeneratorType == Type);
			TestTrue(FString::Printf(TEXT("%s: Params holds the expected alternative"), Label), IsExpectedAlternative(Mask->Params));
		}
	};

	CheckType(EVertexMaskForgeGeneratorType::BoundingBox, TEXT("BoundingBox"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeBoundingBoxParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::AmbientOcclusion, TEXT("AmbientOcclusion"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeAmbientOcclusionParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::Curvature, TEXT("Curvature"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeCurvatureParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::Noise, TEXT("Noise"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeNoiseParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::DirectionalNormal, TEXT("DirectionalNormal"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeDirectionalNormalParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::Thickness, TEXT("Thickness"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeThicknessParams>(); });
	CheckType(EVertexMaskForgeGeneratorType::MaterialSlot, TEXT("MaterialSlot"), [](const FVertexMaskForgeGeneratorParams& P) { return P.IsType<FVertexMaskForgeMaterialSlotParams>(); });

	TestEqual(TEXT("Seven independent layers created"), Stack.Num(), 7);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
