// M16-K.3A: automation tests for FVertexMaskForgeDynamicLayerStack -- the pure, Slate-free dynamic layer
// domain. Nothing here constructs SVertexMaskForgePanel, touches the composition path
// (ComposeGeneratorLayersSequential/EvaluateFillLayers), or reads GeneratorLayerOrder -- GeneratorLayerOrder
// remains the sole production order owner; this stack is not wired into any production call site yet.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerStack.h"

#include <limits>
#include <type_traits>

// 1. MakeInitialStack: exactly one layer, "Base Layer", valid GUID, Fill=None, other defaults, stack valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackInitialTest, "VertexMaskForge.DynamicLayerStack.MakeInitialStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackInitialTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();

	TestEqual(TEXT("Exactly one layer"), Stack.Num(), 1);
	TestFalse(TEXT("Not empty"), Stack.IsEmpty());

	const FVertexMaskForgeLayer& Layer = Stack.GetLayers()[0];
	TestTrue(TEXT("LayerId is valid"), Layer.LayerId.IsValid());
	TestEqual(TEXT("Name is 'Base Layer'"), Layer.Name, FString(TEXT("Base Layer")));
	TestTrue(TEXT("bEnabled defaults true"), Layer.bEnabled);
	TestTrue(TEXT("Fill is None"), Layer.Fill == EVertexMaskForgeLayerFill::None);
	TestTrue(TEXT("BlendMode is Copy"), Layer.BlendMode == EVertexMaskForgeBlendMode::Copy);
	TestEqual(TEXT("Opacity is 1.0"), Layer.Opacity, 1.0f);
	TestTrue(TEXT("Red channel affected"), Layer.bAffectRed);
	TestTrue(TEXT("Green channel affected"), Layer.bAffectGreen);
	TestTrue(TEXT("Blue channel affected"), Layer.bAffectBlue);

	TestTrue(TEXT("Stack is valid"), Stack.IsValid());

	return true;
}

// 2. AddLayer: appends at the end, returns a valid GUID, Fill=None, preserves existing layers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackAddTest, "VertexMaskForge.DynamicLayerStack.AddLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackAddTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid FirstId = Stack.AddLayer(TEXT("First"));
	TestTrue(TEXT("First LayerId is valid"), FirstId.IsValid());
	TestEqual(TEXT("One layer after first add"), Stack.Num(), 1);

	const FGuid SecondId = Stack.AddLayer(TEXT("Second"));
	TestTrue(TEXT("Second LayerId is valid"), SecondId.IsValid());
	TestNotEqual(TEXT("Second LayerId differs from first"), SecondId, FirstId);

	TestEqual(TEXT("Two layers after second add"), Stack.Num(), 2);
	TestEqual(TEXT("First layer preserved at index 0"), Stack.GetLayers()[0].LayerId, FirstId);
	TestEqual(TEXT("Second layer appended at index 1"), Stack.GetLayers()[1].LayerId, SecondId);
	TestTrue(TEXT("New layer Fill is None"), Stack.GetLayers()[1].Fill == EVertexMaskForgeLayerFill::None);

	return true;
}

// 3. MultipleAddsHaveUniqueIds: many layers, no duplicate identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackUniqueIdsTest, "VertexMaskForge.DynamicLayerStack.MultipleAddsHaveUniqueIds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackUniqueIdsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	TSet<FGuid> Ids;
	for (int32 i = 0; i < 25; ++i)
	{
		const FGuid Id = Stack.AddLayer(FString::Printf(TEXT("Layer %d"), i));
		bool bAlreadyInSet = false;
		Ids.Add(Id, &bAlreadyInSet);
		TestFalse(FString::Printf(TEXT("Add %d produced a duplicate id"), i), bAlreadyInSet);
	}

	TestEqual(TEXT("25 layers added"), Stack.Num(), 25);
	TestEqual(TEXT("25 unique ids collected"), Ids.Num(), 25);
	TestTrue(TEXT("Stack stays valid"), Stack.IsValid());

	return true;
}

// 4. DuplicateNamesRemainDistinct: identical names are allowed; identity is still separate per layer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackDuplicateNamesTest, "VertexMaskForge.DynamicLayerStack.DuplicateNamesRemainDistinct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackDuplicateNamesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid FirstId = Stack.AddLayer(TEXT("Layer"));
	const FGuid SecondId = Stack.AddLayer(TEXT("Layer"));

	TestNotEqual(TEXT("Identity differs despite identical names"), FirstId, SecondId);
	TestEqual(TEXT("First layer name"), Stack.FindLayerById(FirstId)->Name, FString(TEXT("Layer")));
	TestEqual(TEXT("Second layer name"), Stack.FindLayerById(SecondId)->Name, FString(TEXT("Layer")));
	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 5. FindById: M16-K.3A corrective audit -- FindLayerById is const-only. Proves the correct layer is
// found, an unknown id resolves to nullptr, read access to every field is available through the const
// pointer, and (via decltype, no metaprogramming beyond a single compile-time type comparison) that the
// public overload set contains no mutable pointer overload -- i.e. the API cannot hand out a way to
// reassign LayerId or replace an element wholesale.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackFindByIdTest, "VertexMaskForge.DynamicLayerStack.FindById", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackFindByIdTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	// Compile-time proof that FindLayerById returns a pointer to CONST FVertexMaskForgeLayer -- if a
	// mutable overload were ever reintroduced, this line would still compile (overload resolution would
	// simply prefer it for a non-const Stack), so the real guarantee is the static_assert below, not the
	// mere fact that this call compiles.
	const FVertexMaskForgeLayer* Found = Stack.FindLayerById(Id);
	static_assert(std::is_same_v<decltype(Stack.FindLayerById(Id)), const FVertexMaskForgeLayer*>,
		"FindLayerById must return a const pointer -- no mutable element access may be exposed.");

	TestNotNull(TEXT("Find succeeds for present id"), Found);
	if (Found)
	{
		TestEqual(TEXT("Resolves the correct layer"), Found->LayerId, Id);
		TestEqual(TEXT("Read access to Name works through the const pointer"), Found->Name, FString(TEXT("Layer")));
	}

	const FGuid UnknownId = FGuid::NewGuid();
	TestNull(TEXT("Find returns null for unknown id"), Stack.FindLayerById(UnknownId));
	TestEqual(TEXT("FindLayerIndexById returns INDEX_NONE for unknown id"), Stack.FindLayerIndexById(UnknownId), (int32)INDEX_NONE);

	return true;
}

// 6. RenamePreservesIdentity: name changes, LayerId and position stay the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackRenameTest, "VertexMaskForge.DynamicLayerStack.RenamePreservesIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackRenameTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Original"));
	const int32 IndexBefore = Stack.FindLayerIndexById(Id);

	const bool bResult = Stack.RenameLayer(Id, TEXT("Renamed"));
	TestTrue(TEXT("RenameLayer returns true"), bResult);
	TestEqual(TEXT("Name updated"), Stack.FindLayerById(Id)->Name, FString(TEXT("Renamed")));
	TestEqual(TEXT("LayerId unchanged"), Stack.FindLayerById(Id)->LayerId, Id);
	TestEqual(TEXT("Position unchanged"), Stack.FindLayerIndexById(Id), IndexBefore);

	// Empty name is explicitly allowed.
	const bool bEmptyResult = Stack.RenameLayer(Id, TEXT(""));
	TestTrue(TEXT("RenameLayer to empty string is allowed"), bEmptyResult);
	TestEqual(TEXT("Name is now empty"), Stack.FindLayerById(Id)->Name, FString(TEXT("")));

	// Unknown id is a no-op.
	const bool bUnknownResult = Stack.RenameLayer(FGuid::NewGuid(), TEXT("Ghost"));
	TestFalse(TEXT("RenameLayer on unknown id returns false"), bUnknownResult);

	return true;
}

// 7. RemoveMiddle: removes only the target, preserves relative order of the rest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackRemoveMiddleTest, "VertexMaskForge.DynamicLayerStack.RemoveMiddle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackRemoveMiddleTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	const FGuid C = Stack.AddLayer(TEXT("C"));

	const FVertexMaskForgeDynamicLayerStack::FRemoveResult Result = Stack.RemoveLayer(B);
	TestTrue(TEXT("Removed"), Result.bRemoved);
	TestEqual(TEXT("Removed index was 1"), Result.RemovedIndex, 1);
	TestEqual(TEXT("Two layers remain"), Result.RemainingNum, 2);

	TestEqual(TEXT("Stack now has 2 layers"), Stack.Num(), 2);
	TestEqual(TEXT("A still at index 0"), Stack.GetLayers()[0].LayerId, A);
	TestEqual(TEXT("C now at index 1"), Stack.GetLayers()[1].LayerId, C);
	TestNull(TEXT("B is gone"), Stack.FindLayerById(B));

	return true;
}

// 8. RemoveLastRemainingLayer: stack becomes empty and stays valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackRemoveLastTest, "VertexMaskForge.DynamicLayerStack.RemoveLastRemainingLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackRemoveLastTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Only"));

	const FVertexMaskForgeDynamicLayerStack::FRemoveResult Result = Stack.RemoveLayer(Id);
	TestTrue(TEXT("Removed"), Result.bRemoved);
	TestEqual(TEXT("Remaining count is 0"), Result.RemainingNum, 0);

	TestTrue(TEXT("Stack is empty"), Stack.IsEmpty());
	TestEqual(TEXT("Num is 0"), Stack.Num(), 0);
	TestTrue(TEXT("Empty stack is valid"), Stack.IsValid());

	return true;
}

// 9. RemoveUnknownIdIsNoOp: failure reported, content and order untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackRemoveUnknownTest, "VertexMaskForge.DynamicLayerStack.RemoveUnknownIdIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackRemoveUnknownTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));

	const FVertexMaskForgeDynamicLayerStack::FRemoveResult Result = Stack.RemoveLayer(FGuid::NewGuid());
	TestFalse(TEXT("bRemoved is false"), Result.bRemoved);
	TestEqual(TEXT("RemovedIndex is INDEX_NONE"), Result.RemovedIndex, (int32)INDEX_NONE);
	TestEqual(TEXT("RemainingNum reports current count"), Result.RemainingNum, 2);

	TestEqual(TEXT("Stack still has 2 layers"), Stack.Num(), 2);
	TestEqual(TEXT("A still at index 0"), Stack.GetLayers()[0].LayerId, A);
	TestEqual(TEXT("B still at index 1"), Stack.GetLayers()[1].LayerId, B);

	return true;
}

// 10/11/12. MoveUp, MoveDown, and boundary no-ops in one pass over a 3-layer stack -- mirrors
// VertexMaskForgeLayerOrderTests.cpp's MoveUpMiddle/MoveDownMiddle/boundary-rejection coverage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackMoveUpDownTest, "VertexMaskForge.DynamicLayerStack.MoveUpAndMoveDown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackMoveUpDownTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	const FGuid C = Stack.AddLayer(TEXT("C"));
	// Order: [A, B, C]

	// Middle up: B moves before A.
	TestTrue(TEXT("MoveLayerUp(B) succeeds"), Stack.MoveLayerUp(B));
	TestEqual(TEXT("B now first"), Stack.GetLayers()[0].LayerId, B);
	TestEqual(TEXT("A now second"), Stack.GetLayers()[1].LayerId, A);
	TestEqual(TEXT("C unchanged at index 2"), Stack.GetLayers()[2].LayerId, C);
	TestTrue(TEXT("Stack valid after MoveUp"), Stack.IsValid());
	// Order: [B, A, C]

	// Middle down: A moves after C.
	TestTrue(TEXT("MoveLayerDown(A) succeeds"), Stack.MoveLayerDown(A));
	TestEqual(TEXT("B unchanged at index 0"), Stack.GetLayers()[0].LayerId, B);
	TestEqual(TEXT("C now index 1"), Stack.GetLayers()[1].LayerId, C);
	TestEqual(TEXT("A now last"), Stack.GetLayers()[2].LayerId, A);
	TestTrue(TEXT("Stack valid after MoveDown"), Stack.IsValid());
	// Order: [B, C, A]

	// First up is a boundary no-op.
	const TArray<FVertexMaskForgeLayer> BeforeFirstUp = Stack.GetLayers();
	TestFalse(TEXT("MoveLayerUp on first element returns false"), Stack.MoveLayerUp(B));
	TestEqual(TEXT("No mutation after boundary MoveUp"), Stack.GetLayers().Num(), BeforeFirstUp.Num());
	for (int32 i = 0; i < BeforeFirstUp.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("Layer %d unchanged after boundary MoveUp"), i), Stack.GetLayers()[i].LayerId, BeforeFirstUp[i].LayerId);
	}

	// Last down is a boundary no-op.
	TestFalse(TEXT("MoveLayerDown on last element returns false"), Stack.MoveLayerDown(A));
	for (int32 i = 0; i < BeforeFirstUp.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("Layer %d unchanged after boundary MoveDown"), i), Stack.GetLayers()[i].LayerId, BeforeFirstUp[i].LayerId);
	}

	// Unknown id.
	TestFalse(TEXT("MoveLayerUp on unknown id returns false"), Stack.MoveLayerUp(FGuid::NewGuid()));
	TestFalse(TEXT("MoveLayerDown on unknown id returns false"), Stack.MoveLayerDown(FGuid::NewGuid()));

	return true;
}

// 13. MoveToExplicitIndex: MoveLayer's "final desired index" contract, first<->last, plus rejected
// out-of-range and same-index-is-a-successful-no-op cases.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackMoveToIndexTest, "VertexMaskForge.DynamicLayerStack.MoveToExplicitIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackMoveToIndexTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	const FGuid C = Stack.AddLayer(TEXT("C"));
	// Order: [A, B, C]

	// First -> last.
	TestTrue(TEXT("MoveLayer(A, 2) succeeds"), Stack.MoveLayer(A, 2));
	TestEqual(TEXT("B now first"), Stack.GetLayers()[0].LayerId, B);
	TestEqual(TEXT("C now second"), Stack.GetLayers()[1].LayerId, C);
	TestEqual(TEXT("A now last"), Stack.GetLayers()[2].LayerId, A);
	// Order: [B, C, A]

	// Last -> first.
	TestTrue(TEXT("MoveLayer(A, 0) succeeds"), Stack.MoveLayer(A, 0));
	TestEqual(TEXT("A now first"), Stack.GetLayers()[0].LayerId, A);
	TestEqual(TEXT("B now second"), Stack.GetLayers()[1].LayerId, B);
	TestEqual(TEXT("C now last"), Stack.GetLayers()[2].LayerId, C);
	// Order: [A, B, C]

	// Same index is a successful no-op.
	TestTrue(TEXT("MoveLayer(B, 1) (already at 1) returns true"), Stack.MoveLayer(B, 1));
	TestEqual(TEXT("Order unchanged"), Stack.GetLayers()[1].LayerId, B);

	// Out-of-range NewIndex is rejected outright, not clamped.
	const TArray<FVertexMaskForgeLayer> Before = Stack.GetLayers();
	TestFalse(TEXT("MoveLayer with NewIndex == Num() is rejected"), Stack.MoveLayer(A, Stack.Num()));
	TestFalse(TEXT("MoveLayer with negative NewIndex is rejected"), Stack.MoveLayer(A, -1));
	for (int32 i = 0; i < Before.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("Layer %d unchanged after rejected MoveLayer"), i), Stack.GetLayers()[i].LayerId, Before[i].LayerId);
	}

	// Unknown id.
	TestFalse(TEXT("MoveLayer on unknown id returns false"), Stack.MoveLayer(FGuid::NewGuid(), 0));

	return true;
}

// 14. RepeatedMovesPreservePermutation: a sequence of valid moves loses/duplicates nothing, same IDs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackRepeatedMovesTest, "VertexMaskForge.DynamicLayerStack.RepeatedMovesPreservePermutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackRepeatedMovesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	TArray<FGuid> Ids;
	for (int32 i = 0; i < 5; ++i)
	{
		Ids.Add(Stack.AddLayer(FString::Printf(TEXT("Layer %d"), i)));
	}

	TestTrue(TEXT("Move 1"), Stack.MoveLayerDown(Ids[0]));
	TestTrue(TEXT("Move 2"), Stack.MoveLayerUp(Ids[4]));
	TestTrue(TEXT("Move 3"), Stack.MoveLayer(Ids[2], 0));
	TestTrue(TEXT("Move 4"), Stack.MoveLayerDown(Ids[1]));
	TestTrue(TEXT("Move 5"), Stack.MoveLayer(Ids[3], Stack.Num() - 1));

	TestTrue(TEXT("Stack still valid"), Stack.IsValid());
	TestEqual(TEXT("Still exactly 5 layers"), Stack.Num(), 5);

	TSet<FGuid> RemainingIds;
	for (const FVertexMaskForgeLayer& Layer : Stack.GetLayers())
	{
		RemainingIds.Add(Layer.LayerId);
	}
	for (const FGuid& Id : Ids)
	{
		TestTrue(TEXT("Original id still present exactly once"), RemainingIds.Contains(Id));
	}
	TestEqual(TEXT("No id created or lost"), RemainingIds.Num(), 5);

	return true;
}

// 15. EmptyStackOperations: find/remove/move on an empty stack are all safe, predictable no-ops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackEmptyOpsTest, "VertexMaskForge.DynamicLayerStack.EmptyStackOperations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackEmptyOpsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	TestTrue(TEXT("New stack is empty"), Stack.IsEmpty());
	TestTrue(TEXT("Empty stack is valid"), Stack.IsValid());

	const FGuid RandomId = FGuid::NewGuid();
	TestNull(TEXT("FindLayerById on empty stack returns null"), Stack.FindLayerById(RandomId));
	TestEqual(TEXT("FindLayerIndexById on empty stack returns INDEX_NONE"), Stack.FindLayerIndexById(RandomId), (int32)INDEX_NONE);

	const FVertexMaskForgeDynamicLayerStack::FRemoveResult RemoveResult = Stack.RemoveLayer(RandomId);
	TestFalse(TEXT("RemoveLayer on empty stack returns bRemoved=false"), RemoveResult.bRemoved);
	TestEqual(TEXT("RemainingNum is 0"), RemoveResult.RemainingNum, 0);

	TestFalse(TEXT("MoveLayerUp on empty stack returns false"), Stack.MoveLayerUp(RandomId));
	TestFalse(TEXT("MoveLayerDown on empty stack returns false"), Stack.MoveLayerDown(RandomId));
	TestFalse(TEXT("MoveLayer on empty stack returns false"), Stack.MoveLayer(RandomId, 0));
	TestFalse(TEXT("RenameLayer on empty stack returns false"), Stack.RenameLayer(RandomId, TEXT("Ghost")));

	TestTrue(TEXT("Stack still empty and valid after the no-ops"), Stack.IsEmpty() && Stack.IsValid());

	return true;
}

// 16. ValidStacksProducedByPublicAPIRemainValid: every stack reachable through AddLayer/RemoveLayer/
// MoveLayer/RenameLayer/SetLayer* is IsValid() by construction -- AddLayer always assigns a unique,
// freshly-generated FGuid, every FVertexMaskForgeLayer default already satisfies IsValid()'s full
// contract, and the M16-K.3B SetLayer* mutators (see below) reject any value that would violate it. There
// is still no legitimate public seam to construct a DUPLICATE or otherwise-invalid IDENTITY (FindLayerById
// is const-only; AddLayer always mints a fresh unique FGuid; there is no way to insert a caller-
// constructed FVertexMaskForgeLayer into the stack's private array at all) -- that gap is unchanged from
// M16-K.3A and still deliberately not closed by exposing a mutable-array accessor. Fill/BlendMode/Opacity
// rejection, however, NOW HAS a legitimate seam (SetLayerFill/SetLayerBlendMode/SetLayerOpacity) and is
// exercised directly by InvalidOpacityMutationRejected and InvalidEnumMutationRejected below, not just
// implicitly here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackValidityInvariantTest, "VertexMaskForge.DynamicLayerStack.ValidStacksProducedByPublicAPIRemainValid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackValidityInvariantTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();
	TestTrue(TEXT("Valid after MakeInitialStack"), Stack.IsValid());

	const FGuid A = Stack.GetLayers()[0].LayerId;
	const FGuid B = Stack.AddLayer(TEXT("B"));
	const FGuid C = Stack.AddLayer(TEXT("C"));
	TestTrue(TEXT("Valid after Adds"), Stack.IsValid());

	Stack.RemoveLayer(B);
	TestTrue(TEXT("Valid after Remove"), Stack.IsValid());

	Stack.MoveLayerDown(A);
	Stack.MoveLayer(C, 0);
	TestTrue(TEXT("Valid after Moves"), Stack.IsValid());

	return true;
}

// 17. LayerDefaultsAreNeutralData: a default-constructed FVertexMaskForgeLayer is neutral data only --
// Fill=None, no evaluation performed. This is NOT an evaluator/composition test (that is M16-K.3B's
// concern) -- it only confirms the struct's own default state matches the approved contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackDefaultsAreNeutralTest, "VertexMaskForge.DynamicLayerStack.LayerDefaultsAreNeutralData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackDefaultsAreNeutralTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeLayer DefaultLayer;

	TestTrue(TEXT("Fill defaults to None, not White"), DefaultLayer.Fill == EVertexMaskForgeLayerFill::None);
	TestTrue(TEXT("bEnabled defaults true (not a substitute for Fill=None)"), DefaultLayer.bEnabled);
	TestEqual(TEXT("Opacity defaults to 1.0 (not used as a neutrality trick)"), DefaultLayer.Opacity, 1.0f);
	TestTrue(TEXT("BlendMode defaults to Copy"), DefaultLayer.BlendMode == EVertexMaskForgeBlendMode::Copy);
	TestTrue(TEXT("All channels affected by default"), DefaultLayer.bAffectRed && DefaultLayer.bAffectGreen && DefaultLayer.bAffectBlue);
	TestFalse(TEXT("LayerId is NOT auto-generated by the struct itself (only by AddLayer)"), DefaultLayer.LayerId.IsValid());

	return true;
}

// M16-K.3B: regression coverage for the new controlled mutations (SetLayerFill/BlendMode/Opacity/
// Enabled/ChannelFilter) added this checkpoint to make composition fields testable without reopening the
// mutable-element seam the M16-K.3A corrective audit closed.

// 26. UnknownLayerMutationIsNoOp: every SetLayer* mutator returns false and leaves the stack untouched
// for an unknown LayerId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackUnknownMutationTest, "VertexMaskForge.DynamicLayerStack.UnknownLayerMutationIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackUnknownMutationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	Stack.AddLayer(TEXT("Layer"));
	const TArray<FVertexMaskForgeLayer> Before = Stack.GetLayers();
	const FGuid UnknownId = FGuid::NewGuid();

	TestFalse(TEXT("SetLayerFill on unknown id"), Stack.SetLayerFill(UnknownId, EVertexMaskForgeLayerFill::White));
	TestFalse(TEXT("SetLayerBlendMode on unknown id"), Stack.SetLayerBlendMode(UnknownId, EVertexMaskForgeBlendMode::Add));
	TestFalse(TEXT("SetLayerOpacity on unknown id"), Stack.SetLayerOpacity(UnknownId, 0.5f));
	TestFalse(TEXT("SetLayerEnabled on unknown id"), Stack.SetLayerEnabled(UnknownId, false));
	TestFalse(TEXT("SetLayerChannelFilter on unknown id"), Stack.SetLayerChannelFilter(UnknownId, false, false, false));

	const TArray<FVertexMaskForgeLayer>& After = Stack.GetLayers();
	TestEqual(TEXT("Layer count unchanged"), After.Num(), Before.Num());
	TestEqual(TEXT("Layer name unchanged"), After[0].Name, Before[0].Name);
	TestTrue(TEXT("Layer Fill unchanged"), After[0].Fill == Before[0].Fill);
	TestTrue(TEXT("Layer BlendMode unchanged"), After[0].BlendMode == Before[0].BlendMode);
	TestEqual(TEXT("Layer Opacity unchanged"), After[0].Opacity, Before[0].Opacity);

	return true;
}

// 27. InvalidOpacityMutationRejected: negative, >1, NaN, and Infinity are all rejected without mutation;
// the previous value and stack validity are preserved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackInvalidOpacityTest, "VertexMaskForge.DynamicLayerStack.InvalidOpacityMutationRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackInvalidOpacityTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("Baseline SetLayerOpacity(0.75) succeeds"), Stack.SetLayerOpacity(Id, 0.75f));

	// Runtime-constructed (never compile-time-folded) Infinity/NaN via std::numeric_limits -- avoids MSVC's
	// constant-arithmetic-overflow/divide-by-zero errors that a literal 0.0f/0.0f or Max()*2.0f would hit.
	const float PositiveInfinity = std::numeric_limits<float>::infinity();
	const float NegativeInfinity = -std::numeric_limits<float>::infinity();
	const float NaNValue = std::numeric_limits<float>::quiet_NaN();

	const float InvalidValues[] = { -0.1f, 1.1f, PositiveInfinity, NegativeInfinity };
	for (const float InvalidValue : InvalidValues)
	{
		const bool bResult = Stack.SetLayerOpacity(Id, InvalidValue);
		TestFalse(FString::Printf(TEXT("SetLayerOpacity(%f) rejected"), InvalidValue), bResult);
		TestEqual(TEXT("Opacity remains the last valid value"), Stack.FindLayerById(Id)->Opacity, 0.75f);
	}

	// NaN separately -- NaN != NaN, so it cannot appear in an initializer list compared meaningfully.
	TestFalse(TEXT("SetLayerOpacity(NaN) rejected"), Stack.SetLayerOpacity(Id, NaNValue));
	TestEqual(TEXT("Opacity remains the last valid value after NaN"), Stack.FindLayerById(Id)->Opacity, 0.75f);

	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 28. InvalidEnumMutationRejected: an out-of-range cast Fill/BlendMode value is rejected without storing
// any invalid state -- via the legitimate SetLayerFill/SetLayerBlendMode seam (no private-access hack).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackInvalidEnumTest, "VertexMaskForge.DynamicLayerStack.InvalidEnumMutationRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackInvalidEnumTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	const bool bFillResult = Stack.SetLayerFill(Id, static_cast<EVertexMaskForgeLayerFill>(255));
	TestFalse(TEXT("SetLayerFill rejects an out-of-range cast"), bFillResult);
	TestTrue(TEXT("Fill remains the default (None)"), Stack.FindLayerById(Id)->Fill == EVertexMaskForgeLayerFill::None);

	const bool bBlendResult = Stack.SetLayerBlendMode(Id, static_cast<EVertexMaskForgeBlendMode>(255));
	TestFalse(TEXT("SetLayerBlendMode rejects an out-of-range cast"), bBlendResult);
	TestTrue(TEXT("BlendMode remains the default (Copy)"), Stack.FindLayerById(Id)->BlendMode == EVertexMaskForgeBlendMode::Copy);

	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// 29. ValidMutationsPreserveIdentityAndOrder: Fill/BlendMode/Opacity/Enabled/channels all change, but
// LayerId and position do not, and the stack stays valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackValidMutationsTest, "VertexMaskForge.DynamicLayerStack.ValidMutationsPreserveIdentityAndOrder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackValidMutationsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));

	TestTrue(TEXT("SetLayerFill"), Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White));
	TestTrue(TEXT("SetLayerBlendMode"), Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Overlay));
	TestTrue(TEXT("SetLayerOpacity"), Stack.SetLayerOpacity(A, 0.42f));
	TestTrue(TEXT("SetLayerEnabled"), Stack.SetLayerEnabled(A, false));
	TestTrue(TEXT("SetLayerChannelFilter"), Stack.SetLayerChannelFilter(A, true, false, true));

	TestEqual(TEXT("Two layers still present"), Stack.Num(), 2);
	TestEqual(TEXT("A still at index 0"), Stack.GetLayers()[0].LayerId, A);
	TestEqual(TEXT("B still at index 1"), Stack.GetLayers()[1].LayerId, B);

	const FVertexMaskForgeLayer* FoundA = Stack.FindLayerById(A);
	TestNotNull(TEXT("A still found"), FoundA);
	if (FoundA)
	{
		TestTrue(TEXT("Fill applied"), FoundA->Fill == EVertexMaskForgeLayerFill::White);
		TestTrue(TEXT("BlendMode applied"), FoundA->BlendMode == EVertexMaskForgeBlendMode::Overlay);
		TestEqual(TEXT("Opacity applied"), FoundA->Opacity, 0.42f);
		TestFalse(TEXT("Enabled applied"), FoundA->bEnabled);
		TestTrue(TEXT("Channel filter applied"), FoundA->bAffectRed && !FoundA->bAffectGreen && FoundA->bAffectBlue);
	}

	TestTrue(TEXT("Stack remains valid"), Stack.IsValid());

	return true;
}

// M16-K.6D-7B: HasAnyEnabledLayer() is the real decision primitive SVertexMaskForgePanel::
// RecomputeOperationState()'s Dynamic branch calls to decide UI Accept-eligibility -- covered here
// directly (a real, non-Slate production seam) rather than by any panel-level test, since exercising
// RecomputeOperationState()/CanAcceptChanges()/AcceptPendingChanges() themselves requires a live
// SVertexMaskForgePanel instance with real viewport selection, which has no automatable seam in this
// codebase (see that checkpoint's own report for the manual-validation deferral).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerEmptyTest, "VertexMaskForge.DynamicLayerStack.HasAnyEnabledLayerFalseOnEmptyStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerEmptyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	TestTrue(TEXT("Stack starts empty"), Stack.IsEmpty());
	TestFalse(TEXT("HasAnyEnabledLayer() false on an empty stack"), Stack.HasAnyEnabledLayer());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerAllDisabledTest, "VertexMaskForge.DynamicLayerStack.HasAnyEnabledLayerFalseWhenAllDisabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerAllDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	TestTrue(TEXT("SetLayerEnabled(A, false)"), Stack.SetLayerEnabled(A, false));
	TestTrue(TEXT("SetLayerEnabled(B, false)"), Stack.SetLayerEnabled(B, false));

	TestFalse(TEXT("Stack is non-empty"), Stack.IsEmpty());
	TestFalse(TEXT("HasAnyEnabledLayer() false when every layer is disabled -- deliberately NOT the same as !IsEmpty()"), Stack.HasAnyEnabledLayer());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerOneEnabledTest, "VertexMaskForge.DynamicLayerStack.HasAnyEnabledLayerTrueWithOneEnabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackHasAnyEnabledLayerOneEnabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	TestTrue(TEXT("SetLayerEnabled(A, false)"), Stack.SetLayerEnabled(A, false));
	// B is left at AddLayer's own default (bEnabled = true).
	(void)B;

	TestTrue(TEXT("HasAnyEnabledLayer() true with exactly one enabled layer"), Stack.HasAnyEnabledLayer());
	return true;
}

// M17-TH-DL-B, 1-4: Thickness is a valid generator type at the stack/data-model level (this was already
// true before this checkpoint -- EVertexMaskForgeGeneratorType::Thickness and FVertexMaskForgeThicknessParams
// have existed since before M17-TH-DL-B; this checkpoint only wired the UI/evaluation/cache sides). Proves:
// assignment succeeds and creates the exact default FVertexMaskForgeThicknessParams; a parameter update
// preserves GeneratorType/Params coherence; replacing the generator entirely mints a fresh MaskInstanceId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerStackThicknessGeneratorTest, "VertexMaskForge.DynamicLayerStack.ThicknessAcceptedWithDefaultsAndCoherence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerStackThicknessGeneratorTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Thickness Layer"));

	// 1. Thickness is accepted as a valid generator type.
	TestTrue(TEXT("SetLayerMaskGeneratorType(Thickness) succeeds"), Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Thickness));

	// 2/3. Assigning it creates FVertexMaskForgeThicknessParams with exactly the authoritative defaults.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("Layer has a mask"), Mask);
	if (!Mask) { return false; }
	TestTrue(TEXT("GeneratorType == Thickness"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::Thickness);
	TestTrue(TEXT("Params holds FVertexMaskForgeThicknessParams"), Mask->Params.IsType<FVertexMaskForgeThicknessParams>());
	const FVertexMaskForgeThicknessParams* ThicknessParams = Mask->Params.TryGet<FVertexMaskForgeThicknessParams>();
	TestNotNull(TEXT("TryGet<FVertexMaskForgeThicknessParams> succeeds"), ThicknessParams);
	if (ThicknessParams)
	{
		TestEqual(TEXT("Default MinThickness == 0.0"), ThicknessParams->MinThickness, 0.0f);
		TestEqual(TEXT("Default MaxThickness == 50.0"), ThicknessParams->MaxThickness, 50.0f);
		TestEqual(TEXT("Default SearchDistance == 100.0"), ThicknessParams->SearchDistance, 100.0f);
		TestEqual(TEXT("Default Bias == 0.01"), ThicknessParams->Bias, 0.01f);
		TestEqual(TEXT("Default Blur == 0.0"), ThicknessParams->Blur, 0.0f);
		TestFalse(TEXT("Default bInvert == false"), ThicknessParams->bInvert);
	}
	const FGuid FirstMaskInstanceId = Mask->MaskInstanceId;

	// 4. Updating Thickness params (via SetLayerMaskParams, the same seam SVertexMaskForgePanel::
	// MutateDynamicThicknessParam uses) preserves GeneratorType/Params coherence -- the layer stays a
	// coherent Thickness mask, MaskInstanceId unchanged (SetLayerMaskParams never mints a new one).
	{
		FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
		NewParams.Get<FVertexMaskForgeThicknessParams>().MinThickness = 5.0f;
		NewParams.Get<FVertexMaskForgeThicknessParams>().MaxThickness = 30.0f;
		NewParams.Get<FVertexMaskForgeThicknessParams>().SearchDistance = 60.0f;
		NewParams.Get<FVertexMaskForgeThicknessParams>().bInvert = true;
		TestTrue(TEXT("SetLayerMaskParams succeeds"), Stack.SetLayerMaskParams(LayerId, FirstMaskInstanceId, NewParams));

		const FVertexMaskForgeGeneratorMaskInstance* MaskAfterUpdate = Stack.GetLayerMask(LayerId);
		TestNotNull(TEXT("Layer still has a mask after update"), MaskAfterUpdate);
		if (MaskAfterUpdate)
		{
			TestTrue(TEXT("GeneratorType still Thickness after update"), MaskAfterUpdate->GeneratorType == EVertexMaskForgeGeneratorType::Thickness);
			TestTrue(TEXT("Params still FVertexMaskForgeThicknessParams after update"), MaskAfterUpdate->Params.IsType<FVertexMaskForgeThicknessParams>());
			TestEqual(TEXT("MaskInstanceId unchanged by a params-only update"), MaskAfterUpdate->MaskInstanceId, FirstMaskInstanceId);
			const FVertexMaskForgeThicknessParams* UpdatedParams = MaskAfterUpdate->Params.TryGet<FVertexMaskForgeThicknessParams>();
			if (UpdatedParams)
			{
				TestEqual(TEXT("MinThickness updated"), UpdatedParams->MinThickness, 5.0f);
				TestEqual(TEXT("MaxThickness updated"), UpdatedParams->MaxThickness, 30.0f);
				TestEqual(TEXT("SearchDistance updated"), UpdatedParams->SearchDistance, 60.0f);
				TestTrue(TEXT("bInvert updated"), UpdatedParams->bInvert);
			}
		}
	}

	// 5. Replacing the generator entirely (Thickness -> Curvature) mints a FRESH MaskInstanceId and resets
	// Params to Curvature's own defaults -- the old Thickness MaskInstanceId can never be reused to target
	// the replacement (mirrors SetLayerMaskGeneratorType's own documented "previous instance discarded
	// outright" contract), so a stale Thickness-editor widget captured against FirstMaskInstanceId can never
	// mutate the replacement layer.
	TestTrue(TEXT("SetLayerMaskGeneratorType(Curvature) succeeds"), Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Curvature));
	const FVertexMaskForgeGeneratorMaskInstance* MaskAfterReplace = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("Layer has a mask after replacement"), MaskAfterReplace);
	if (MaskAfterReplace)
	{
		TestTrue(TEXT("GeneratorType now Curvature"), MaskAfterReplace->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
		TestTrue(TEXT("Params now FVertexMaskForgeCurvatureParams"), MaskAfterReplace->Params.IsType<FVertexMaskForgeCurvatureParams>());
		TestNotEqual(TEXT("MaskInstanceId changed by generator replacement"), MaskAfterReplace->MaskInstanceId, FirstMaskInstanceId);

		// A stale mutation attempt against the OLD (now-orphaned) Thickness MaskInstanceId must be a no-op
		// -- it must never reach across to mutate the replacement (Curvature) mask.
		FVertexMaskForgeGeneratorParams StaleThicknessParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Thickness);
		StaleThicknessParams.Get<FVertexMaskForgeThicknessParams>().MinThickness = 999.0f;
		TestFalse(TEXT("SetLayerMaskParams with the STALE (pre-replacement) MaskInstanceId fails"),
			Stack.SetLayerMaskParams(LayerId, FirstMaskInstanceId, StaleThicknessParams));
		const FVertexMaskForgeGeneratorMaskInstance* MaskAfterStaleAttempt = Stack.GetLayerMask(LayerId);
		if (MaskAfterStaleAttempt)
		{
			TestTrue(TEXT("Replacement layer is STILL Curvature -- stale Thickness mutation never crossed over"), MaskAfterStaleAttempt->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
