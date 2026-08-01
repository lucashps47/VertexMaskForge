// M16-K.5H: integration proof for VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors --
// FColor array -> ToLinearColorF -> EvaluateColor per index -> ToDisplayFColor -> FColor array. All four
// tests below prove ONLY the batch's own array/indexing/aliasing mechanics -- the underlying scalar math
// (BlendMode formulas, sequential layer order, Enabled handling, Missing/index-absent/FailedAfterReady
// semantics, clamp, RoundToInt, byte round-trip) is already frozen and tested by
// VertexMaskForgeDynamicLayerEvaluatorTests.cpp, VertexMaskForgeGeneratorMaskInstanceTests.cpp,
// VertexMaskForgeColorConversionTests.cpp, and VertexMaskForgeDynamicMaskGenerationTests.cpp -- none of
// that is duplicated here.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerBatchCompositor.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeInstanceResultStore.h"

// 1. EmptyInputProducesEmptyOutput.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerBatchCompositorEmptyInputTest, "VertexMaskForge.DynamicLayerBatchCompositor.EmptyInputProducesEmptyOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerBatchCompositorEmptyInputTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeDynamicLayerStack Stack;
	const FVertexMaskForgeInstanceResultStore ResultStore;
	const TConstArrayView<FColor> BaseColors;

	// OutColors starts with stale, non-empty content -- proves the output is wholesale SUBSTITUTED, not
	// merely left untouched.
	TArray<FColor> OutColors;
	OutColors.Add(FColor(1, 2, 3, 4));

	VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors(BaseColors, Stack, ResultStore, OutColors);

	TestEqual(TEXT("OutColors.Num() == 0"), OutColors.Num(), 0);

	return true;
}

// 2. EmptyStackPreservesAllBaseColorsByteExact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerBatchCompositorEmptyStackTest, "VertexMaskForge.DynamicLayerBatchCompositor.EmptyStackPreservesAllBaseColorsByteExact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerBatchCompositorEmptyStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeDynamicLayerStack Stack;
	const FVertexMaskForgeInstanceResultStore ResultStore;

	const TArray<FColor> BaseColors = {
		FColor(0, 1, 127, 2),
		FColor(10, 20, 30, 100),
		FColor(255, 128, 64, 200),
		FColor(17, 99, 231, 255),
	};

	TArray<FColor> OutColors;
	OutColors.Add(FColor(9, 9, 9, 9));

	VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors(BaseColors, Stack, ResultStore, OutColors);

	TestEqual(TEXT("OutColors.Num() == 4"), OutColors.Num(), 4);
	TestTrue(TEXT("OutColors is byte-exact equal to BaseColors (empty stack passthrough)"), OutColors == BaseColors);

	return true;
}

// 3. MultipleIndicesWithDistinctCoveragesAndMissingIndexProduceExpectedColors.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerBatchCompositorMultiIndexTest, "VertexMaskForge.DynamicLayerBatchCompositor.MultipleIndicesWithDistinctCoveragesAndMissingIndexProduceExpectedColors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerBatchCompositorMultiIndexTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("Enabled by default"), Stack.FindLayerById(LayerId)->bEnabled);
	TestTrue(TEXT("SetLayerFill(White) succeeds"), Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White));
	TestTrue(TEXT("SetLayerBlendMode(Copy) succeeds"), Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy));
	TestTrue(TEXT("SetLayerOpacity(1.0) succeeds"), Stack.SetLayerOpacity(LayerId, 1.0f));
	TestTrue(TEXT("SetLayerMaskGeneratorType(MaterialSlot) succeeds"), Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot));

	// MaskInstanceId obtained from the layer/stack itself -- never a disconnected/fabricated GUID.
	const FGuid MaskInstanceId = Stack.GetLayerMask(LayerId)->MaskInstanceId;

	// A real result IS stored under MaskInstanceId, but with only TWO elements -- index 2 is therefore
	// "index out-of-range for a PRESENT instance", never "MaskInstanceId Missing from the store". These
	// are the two distinct contracts EvaluateColor already treats identically (zero coverage), but this
	// fixture deliberately exercises the out-of-range one, not the Missing one.
	FVertexMaskForgeInstanceMaskResult Result;
	Result.Values = { 1.0f, 0.5f };
	Result.bHasValue.Init(true, 2);

	FVertexMaskForgeInstanceResultStore ResultStore;
	TestTrue(TEXT("StoreOrReplace succeeds"), ResultStore.StoreOrReplace(MaskInstanceId, MoveTemp(Result)));

	// Confirm the fixture is exactly what it claims to be: present, but short.
	const FVertexMaskForgeInstanceMaskResult* Found = ResultStore.Find(MaskInstanceId);
	TestNotNull(TEXT("Find(MaskInstanceId) is NOT nullptr -- this is NOT the Missing case"), Found);
	if (!Found)
	{
		return false;
	}
	TestEqual(TEXT("Values.Num() == 2"), Found->Values.Num(), 2);
	TestEqual(TEXT("bHasValue.Num() == 2"), Found->bHasValue.Num(), 2);
	float Unused = -1.0f;
	TestFalse(TEXT("Index 2 is genuinely out-of-range for this present instance"), Found->TryGetValue(2, Unused));

	const TArray<FColor> BaseColors = {
		FColor(10, 20, 30, 100),
		FColor(200, 150, 90, 200),
		FColor(5, 5, 5, 255),
	};

	TArray<FColor> OutColors;
	VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors(BaseColors, Stack, ResultStore, OutColors);

	TestEqual(TEXT("OutColors.Num() == 3"), OutColors.Num(), 3);

	// Index 0: coverage 1.0 -> White Copy@1 -> RGB (255,255,255); Alpha from BaseColors[0].A=100 ->
	// RoundToInt(100/255 * 255) = 100.
	TestEqual(TEXT("Index 0 == FColor(255,255,255,100)"), OutColors[0], FColor(255, 255, 255, 100));
	// Index 1: coverage 0.5 -> White Copy@1 -> RoundToInt(0.5*255)=RoundToInt(127.5)=128 per channel;
	// Alpha from BaseColors[1].A=200 -> RoundToInt(200/255*255)=200.
	TestEqual(TEXT("Index 1 == FColor(128,128,128,200)"), OutColors[1], FColor(128, 128, 128, 200));
	// Index 2: out-of-range -> zero coverage -> White Copy@1 with coverage 0 -> RGB (0,0,0); Alpha from
	// BaseColors[2].A=255 -> RoundToInt(255/255*255)=255.
	TestEqual(TEXT("Index 2 == FColor(0,0,0,255)"), OutColors[2], FColor(0, 0, 0, 255));

	return true;
}

// 4. AliasedInputAndOutputProduceCorrectResult.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerBatchCompositorAliasingTest, "VertexMaskForge.DynamicLayerBatchCompositor.AliasedInputAndOutputProduceCorrectResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerBatchCompositorAliasingTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("SetLayerFill(White) succeeds"), Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White));
	TestTrue(TEXT("SetLayerBlendMode(Copy) succeeds"), Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy));
	TestTrue(TEXT("SetLayerOpacity(1.0) succeeds"), Stack.SetLayerOpacity(LayerId, 1.0f));
	TestTrue(TEXT("SetLayerMaskGeneratorType(MaterialSlot) succeeds"), Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot));

	const FGuid MaskInstanceId = Stack.GetLayerMask(LayerId)->MaskInstanceId;

	FVertexMaskForgeInstanceMaskResult Result;
	Result.Values = { 1.0f, 0.5f };
	Result.bHasValue.Init(true, 2);

	FVertexMaskForgeInstanceResultStore ResultStore;
	TestTrue(TEXT("StoreOrReplace succeeds"), ResultStore.StoreOrReplace(MaskInstanceId, MoveTemp(Result)));

	TArray<FColor> Colors = {
		FColor(10, 20, 30, 100),
		FColor(200, 150, 90, 200),
		FColor(5, 5, 5, 255),
	};

	// The SAME TArray is used as both source view and destination -- this is the exact contract
	// ComposeColors' own doc comment promises is safe.
	TConstArrayView<FColor> BaseView(Colors);
	VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors(BaseView, Stack, ResultStore, Colors);
	// BaseView is deliberately not used again below -- the final assignment inside ComposeColors may
	// have reallocated Colors' backing storage, which would leave BaseView dangling.

	TestEqual(TEXT("Colors.Num() == 3 after aliased call"), Colors.Num(), 3);
	TestEqual(TEXT("Index 0 == FColor(255,255,255,100) (aliased, matches non-aliased path)"), Colors[0], FColor(255, 255, 255, 100));
	TestEqual(TEXT("Index 1 == FColor(128,128,128,200) (aliased, matches non-aliased path)"), Colors[1], FColor(128, 128, 128, 200));
	TestEqual(TEXT("Index 2 == FColor(0,0,0,255) (aliased, matches non-aliased path)"), Colors[2], FColor(0, 0, 0, 255));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
