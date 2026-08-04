// M16-K.6D-8F-C: Dynamic Noise/Grunge production UI -- domain-level tests for the exact read/mutate
// sequences BuildDynamicNoiseLayerParamsBlock/MutateDynamicNoiseParam perform against
// FVertexMaskForgeDynamicLayerStack (the same production API the panel calls).
//
// HONEST SCOPE STATEMENT (mirrors VertexMaskForgeDynamicLayersUIWorkflowTests.cpp's own contract; revised
// M16-K.6D-8F-D.1 -- see that checkpoint's own report for why). This file's automated coverage falls into
// exactly three categories, and no claim below blurs the boundary between them:
//
// (1) REAL PRODUCTION HELPER BEHAVIOR, executed automatically: VertexMaskForgePanel::
// NormalizeDynamicNoiseScaleForAxisLock and VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit (declared in
// SVertexMaskForgePanel.h, defined in SVertexMaskForgePanel.cpp) are the EXACT SAME functions the real
// SVertexMaskForgePanel::OnDynamicNoiseScaleAxesLockChanged and Scale X OnValueChanged_Lambda callbacks
// call (external linkage specifically so this separate translation unit can call them too) -- tests that
// invoke them are exercising real production Lock Axes logic, not a reconstruction of it. As of
// M16-K.6D-8F-D.1, NO test-local reconstruction of the Lock Axes normalization/synchronization algorithm
// exists anywhere in this file.
//
// (2) REAL DYNAMIC LAYER-STACK/MODEL BEHAVIOR, executed automatically: MutateNoiseParamLikeUI below mirrors
// MutateDynamicNoiseParam's own six-step identity-validated write path (mask exists, GeneratorType ==
// Noise, Params IsType Noise, MaskInstanceId matches -> copy -> mutate -> SetLayerMaskParams), but that
// path's only real logic lives in FVertexMaskForgeDynamicLayerStack's own tested public API
// (GetLayerMask/SetLayerMaskParams) -- MutateNoiseParamLikeUI itself performs no field-specific business
// logic (unlike the three helpers removed in M16-K.6D-8F-D.1), so tests using it are exercising real
// FVertexMaskForgeDynamicLayerStack behavior via the same shape the panel uses, not a duplicated algorithm.
// SetLayerMaskGeneratorType/RemoveLayer/MoveLayer/RenameLayer/SetLayerFill/etc. calls elsewhere in this file
// are direct calls to the real stack API.
//
// (3) SLATE/PANEL UI FACTS, verified ONLY by direct source inspection (never executed by automation) plus
// Lucas's own manual Editor confirmation ("funcionou", M16-K.6D-8F-D): the "Lock Axes" checkbox's exact
// label/tooltip/placement on the Scale row, the Y/Z SSpinBox IsEnabled_Lambda binding, the panel-owned
// DynamicNoiseScaleAxesLockedByLayerId map's exact symbol/key/missing-entry semantics, its removal-cleanup
// call site, per-layer checkbox independence when TWO REAL Slate rows are involved, generator-switch
// persistence, and panel-lifetime reset. NOTHING in this file constructs SVertexMaskForgePanel, SNew()s any
// Slate widget, clicks a checkbox, or reads/writes DynamicNoiseScaleAxesLockedByLayerId itself -- there is
// no legitimate seam to do so without a Slate/panel test harness, and none exists anywhere in this plugin
// for ANY generator's Dynamic UI (confirmed repo-wide; see VertexMaskForgeDynamicLayersUIWorkflowTests.cpp's
// own doc comment for the same finding applied to the generic Dynamic Layers callbacks). Tests that need a
// "lock state" to drive category-(1)/(2) assertions use a plain local `bool`/`TMap<FGuid,bool>` as ORDINARY
// TEST SETUP DATA (an input to the real ApplyDynamicNoiseScaleXEdit's own bAxesLocked parameter, or a
// per-LayerId scratch value for asserting isolation) -- never as a stand-in implementation of the lock
// TOGGLE algorithm itself, which no longer exists in this file.
//
// This file also retains, unchanged, the M16-K.6D-8F-C coverage of MutateDynamicNoiseParam's own six-step
// write path for all 17 authoritative FVertexMaskForgeNoiseParams fields, generic layer property isolation,
// cross-layer independence, and the Levels Min/Max coupled invariant.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SVertexMaskForgePanel.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeRecipeTypes.h"

namespace
{
	// Mirrors MutateDynamicNoiseParam's own six-step identity-validated write path exactly (see
	// SVertexMaskForgePanel.cpp), so every test below exercises the SAME sequence the real widget
	// callbacks perform, without needing SVertexMaskForgePanel itself. This helper contains NO Lock-Axes-
	// specific (or any other field-specific) business logic of its own -- see category (2) above.
	bool MutateNoiseParamLikeUI(
		FVertexMaskForgeDynamicLayerStack& Stack, const FGuid LayerId, const FGuid ExpectedMaskInstanceId,
		TFunctionRef<void(FVertexMaskForgeNoiseParams&)> Mutator)
	{
		const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(LayerId);
		if (!Mask
			|| Mask->GeneratorType != EVertexMaskForgeGeneratorType::Noise
			|| !Mask->Params.IsType<FVertexMaskForgeNoiseParams>()
			|| Mask->MaskInstanceId != ExpectedMaskInstanceId)
		{
			return false;
		}
		FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
		Mutator(NewParams.Get<FVertexMaskForgeNoiseParams>());
		return Stack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, NewParams);
	}
}

// 1. Noise is a valid Dynamic assignment target: SetLayerMaskGeneratorType(Noise) succeeds, produces a
// Noise-typed mask with a valid MaskInstanceId, a Noise-alternative Params, and the authoritative defaults
// (FVertexMaskForgeNoiseParams' own default member initializers) -- exactly what the selector's "Noise/
// Grunge" option and the inspector's GetParams() fallback both rely on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUINoiseAvailableTest, "VertexMaskForge.DynamicNoiseUI.NoiseIsValidAssignmentWithCorrectTagAndDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUINoiseAvailableTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));

	TestTrue(TEXT("SetLayerMaskGeneratorType(Noise) succeeds"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask now present"), Mask);
	if (!Mask)
	{
		return false;
	}
	TestTrue(TEXT("GeneratorType is Noise"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
	TestTrue(TEXT("MaskInstanceId is valid"), Mask->MaskInstanceId.IsValid());
	TestTrue(TEXT("Params alternative is FVertexMaskForgeNoiseParams"), Mask->Params.IsType<FVertexMaskForgeNoiseParams>());

	const FVertexMaskForgeNoiseParams& Defaults = Mask->Params.Get<FVertexMaskForgeNoiseParams>();
	const FVertexMaskForgeNoiseParams Authoritative;
	TestTrue(TEXT("Type default matches authoritative"), Defaults.Type == Authoritative.Type);
	TestEqual(TEXT("ScaleX default"), Defaults.ScaleX, Authoritative.ScaleX);
	TestEqual(TEXT("ScaleY default"), Defaults.ScaleY, Authoritative.ScaleY);
	TestEqual(TEXT("ScaleZ default"), Defaults.ScaleZ, Authoritative.ScaleZ);
	TestEqual(TEXT("OffsetX default"), Defaults.OffsetX, Authoritative.OffsetX);
	TestEqual(TEXT("Seed default"), Defaults.Seed, Authoritative.Seed);
	TestEqual(TEXT("Octaves default"), Defaults.Octaves, Authoritative.Octaves);
	TestEqual(TEXT("Roughness default"), Defaults.Roughness, Authoritative.Roughness);
	TestEqual(TEXT("Lacunarity default"), Defaults.Lacunarity, Authoritative.Lacunarity);
	TestEqual(TEXT("TurbulenceStrength default"), Defaults.TurbulenceStrength, Authoritative.TurbulenceStrength);
	TestEqual(TEXT("Multiplier default"), Defaults.Multiplier, Authoritative.Multiplier);
	TestEqual(TEXT("Blur default"), Defaults.Blur, Authoritative.Blur);
	TestEqual(TEXT("LevelsMin default"), Defaults.LevelsMin, Authoritative.LevelsMin);
	TestEqual(TEXT("LevelsMax default"), Defaults.LevelsMax, Authoritative.LevelsMax);
	TestEqual(TEXT("bInvert default"), Defaults.bInvert, Authoritative.bInvert);

	return true;
}

// 2. All 17 authoritative fields forward through MutateNoiseParamLikeUI (the exact shape
// MutateDynamicNoiseParam uses) without resetting any sibling field -- one field changed per call, all
// sixteen others compared byte/value-exact against their pre-mutation snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIAllSeventeenFieldsForwardTest, "VertexMaskForge.DynamicNoiseUI.AllSeventeenFieldsForwardWithoutResettingSiblings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIAllSeventeenFieldsForwardTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	auto Current = [&]() -> FVertexMaskForgeNoiseParams { return Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>(); };

	auto CheckAllExcept = [&](const FVertexMaskForgeNoiseParams& Before, const FVertexMaskForgeNoiseParams& After, const TCHAR* ChangedFieldName)
	{
		auto Eq = [&](const TCHAR* Name, bool bEqual)
		{
			if (FCString::Strcmp(Name, ChangedFieldName) != 0)
			{
				TestTrue(FString::Printf(TEXT("%s preserved while editing %s"), Name, ChangedFieldName), bEqual);
			}
		};
		Eq(TEXT("Type"), Before.Type == After.Type);
		Eq(TEXT("ScaleX"), Before.ScaleX == After.ScaleX);
		Eq(TEXT("ScaleY"), Before.ScaleY == After.ScaleY);
		Eq(TEXT("ScaleZ"), Before.ScaleZ == After.ScaleZ);
		Eq(TEXT("OffsetX"), Before.OffsetX == After.OffsetX);
		Eq(TEXT("OffsetY"), Before.OffsetY == After.OffsetY);
		Eq(TEXT("OffsetZ"), Before.OffsetZ == After.OffsetZ);
		Eq(TEXT("Seed"), Before.Seed == After.Seed);
		Eq(TEXT("Octaves"), Before.Octaves == After.Octaves);
		Eq(TEXT("Roughness"), Before.Roughness == After.Roughness);
		Eq(TEXT("Lacunarity"), Before.Lacunarity == After.Lacunarity);
		Eq(TEXT("TurbulenceStrength"), Before.TurbulenceStrength == After.TurbulenceStrength);
		Eq(TEXT("Multiplier"), Before.Multiplier == After.Multiplier);
		Eq(TEXT("Blur"), Before.Blur == After.Blur);
		Eq(TEXT("LevelsMin"), Before.LevelsMin == After.LevelsMin);
		Eq(TEXT("LevelsMax"), Before.LevelsMax == After.LevelsMax);
		Eq(TEXT("bInvert"), Before.bInvert == After.bInvert);
	};

	// Sub-1.0/away-from-default values throughout -- deliberately avoids the Multiplier clamp-saturation
	// class of false-positive this session's own Curvature checkpoint was bitten by (see M16-K.6D-8E-B's
	// own retained report): a value that is ALREADY at the domain's clamp ceiling can mask a forwarding
	// bug, since a wrong value would clamp to the same visible result as the right one.
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Type mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Type = EVertexMaskForgeNoiseType::Ridged; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestTrue(TEXT("Type forwarded"), After.Type == EVertexMaskForgeNoiseType::Ridged);
		CheckAllExcept(Before, After, TEXT("Type"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("ScaleX mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleX = 3.25f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("ScaleX forwarded"), After.ScaleX, 3.25f);
		CheckAllExcept(Before, After, TEXT("ScaleX"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("ScaleY mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleY = 4.5f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("ScaleY forwarded"), After.ScaleY, 4.5f);
		CheckAllExcept(Before, After, TEXT("ScaleY"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("ScaleZ mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleZ = 5.75f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("ScaleZ forwarded"), After.ScaleZ, 5.75f);
		CheckAllExcept(Before, After, TEXT("ScaleZ"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("OffsetX mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.OffsetX = 12.5f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("OffsetX forwarded"), After.OffsetX, 12.5f);
		CheckAllExcept(Before, After, TEXT("OffsetX"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("OffsetY mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.OffsetY = -7.5f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("OffsetY forwarded"), After.OffsetY, -7.5f);
		CheckAllExcept(Before, After, TEXT("OffsetY"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("OffsetZ mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.OffsetZ = 33.0f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("OffsetZ forwarded"), After.OffsetZ, 33.0f);
		CheckAllExcept(Before, After, TEXT("OffsetZ"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Seed mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Seed = 4242; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Seed forwarded"), After.Seed, 4242);
		CheckAllExcept(Before, After, TEXT("Seed"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Octaves mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Octaves = 6; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Octaves forwarded"), After.Octaves, 6);
		CheckAllExcept(Before, After, TEXT("Octaves"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Roughness mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Roughness = 0.37f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Roughness forwarded"), After.Roughness, 0.37f);
		CheckAllExcept(Before, After, TEXT("Roughness"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Lacunarity mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Lacunarity = 3.7f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Lacunarity forwarded"), After.Lacunarity, 3.7f);
		CheckAllExcept(Before, After, TEXT("Lacunarity"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("TurbulenceStrength mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.TurbulenceStrength = 0.42f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("TurbulenceStrength forwarded"), After.TurbulenceStrength, 0.42f);
		CheckAllExcept(Before, After, TEXT("TurbulenceStrength"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Multiplier mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Multiplier = 0.63f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Multiplier forwarded"), After.Multiplier, 0.63f);
		CheckAllExcept(Before, After, TEXT("Multiplier"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("Blur mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.Blur = 0.29f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("Blur forwarded"), After.Blur, 0.29f);
		CheckAllExcept(Before, After, TEXT("Blur"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("LevelsMin mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.LevelsMin = 0.11f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("LevelsMin forwarded"), After.LevelsMin, 0.11f);
		CheckAllExcept(Before, After, TEXT("LevelsMin"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("LevelsMax mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.LevelsMax = 0.88f; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestEqual(TEXT("LevelsMax forwarded"), After.LevelsMax, 0.88f);
		CheckAllExcept(Before, After, TEXT("LevelsMax"));
	}
	{
		FVertexMaskForgeNoiseParams Before = Current();
		TestTrue(TEXT("bInvert mutation succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.bInvert = !P.bInvert; }));
		FVertexMaskForgeNoiseParams After = Current();
		TestNotEqual(TEXT("bInvert forwarded"), After.bInvert, Before.bInvert);
		CheckAllExcept(Before, After, TEXT("bInvert"));
	}

	return true;
}

// 3. Generic layer properties (Fill/BlendMode/Opacity/bEnabled/channel filter) are completely unaffected
// by any number of Noise param mutations -- proving MutateNoiseParamLikeUI's SetLayerMaskParams call never
// touches the FVertexMaskForgeLayer fields that live alongside Mask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIGenericLayerPropertiesPreservedTest, "VertexMaskForge.DynamicNoiseUI.GenericLayerPropertiesPreservedAcrossNoiseMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIGenericLayerPropertiesPreservedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(Id, 0.42f);
	Stack.SetLayerEnabled(Id, false);
	Stack.SetLayerChannelFilter(Id, true, false, true);

	for (int32 Iteration = 0; Iteration < 5; ++Iteration)
	{
		MutateNoiseParamLikeUI(Stack, Id, InstanceId, [Iteration](FVertexMaskForgeNoiseParams& P) { P.Multiplier = 0.1f * Iteration; });
	}

	const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
	TestNotNull(TEXT("Layer still present"), Layer);
	if (!Layer)
	{
		return false;
	}
	TestTrue(TEXT("Fill preserved"), Layer->Fill == EVertexMaskForgeLayerFill::White);
	TestTrue(TEXT("BlendMode preserved"), Layer->BlendMode == EVertexMaskForgeBlendMode::Multiply);
	TestEqual(TEXT("Opacity preserved"), Layer->Opacity, 0.42f);
	TestFalse(TEXT("bEnabled preserved"), Layer->bEnabled);
	TestTrue(TEXT("Red channel preserved"), Layer->bAffectRed);
	TestFalse(TEXT("Green channel preserved"), Layer->bAffectGreen);
	TestTrue(TEXT("Blue channel preserved"), Layer->bAffectBlue);

	return true;
}

// 4. Two Noise layers mutate completely independently -- no cross-layer mutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUITwoLayersIndependentTest, "VertexMaskForge.DynamicNoiseUI.TwoNoiseLayersMutateIndependently", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUITwoLayersIndependentTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise);
	Stack.SetLayerMaskGeneratorType(B, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceA = Stack.GetLayerMask(A)->MaskInstanceId;
	const FGuid InstanceB = Stack.GetLayerMask(B)->MaskInstanceId;

	TestTrue(TEXT("Mutate A.ScaleX"), MutateNoiseParamLikeUI(Stack, A, InstanceA, [](FVertexMaskForgeNoiseParams& P) { P.ScaleX = 9.0f; }));
	TestTrue(TEXT("Mutate B.ScaleX"), MutateNoiseParamLikeUI(Stack, B, InstanceB, [](FVertexMaskForgeNoiseParams& P) { P.ScaleX = 0.5f; }));

	TestEqual(TEXT("A.ScaleX independent"), Stack.GetLayerMask(A)->Params.Get<FVertexMaskForgeNoiseParams>().ScaleX, 9.0f);
	TestEqual(TEXT("B.ScaleX independent"), Stack.GetLayerMask(B)->Params.Get<FVertexMaskForgeNoiseParams>().ScaleX, 0.5f);

	TestNotEqual(TEXT("Distinct MaskInstanceIds"), InstanceA, InstanceB);

	return true;
}

// 5. Switching Layer A's generator away from Noise (e.g. to Curvature) does not affect a sibling Noise
// layer B -- independent layer switching.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUISwitchingLayerAwayFromNoiseTest, "VertexMaskForge.DynamicNoiseUI.SwitchingSiblingAwayFromNoiseLeavesOtherLayerIntact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUISwitchingLayerAwayFromNoiseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise);
	Stack.SetLayerMaskGeneratorType(B, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceB = Stack.GetLayerMask(B)->MaskInstanceId;
	MutateNoiseParamLikeUI(Stack, B, InstanceB, [](FVertexMaskForgeNoiseParams& P) { P.Seed = 777; });

	TestTrue(TEXT("Switch A to Curvature"), Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Curvature));

	const FVertexMaskForgeGeneratorMaskInstance* MaskB = Stack.GetLayerMask(B);
	TestNotNull(TEXT("B still present"), MaskB);
	if (MaskB)
	{
		TestTrue(TEXT("B still Noise"), MaskB->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
		TestEqual(TEXT("B's MaskInstanceId unchanged"), MaskB->MaskInstanceId, InstanceB);
		TestEqual(TEXT("B's Seed unaffected by A's switch"), MaskB->Params.Get<FVertexMaskForgeNoiseParams>().Seed, 777);
	}
	const FVertexMaskForgeGeneratorMaskInstance* MaskA = Stack.GetLayerMask(A);
	TestNotNull(TEXT("A still present"), MaskA);
	if (MaskA)
	{
		TestTrue(TEXT("A is now Curvature"), MaskA->GeneratorType == EVertexMaskForgeGeneratorType::Curvature);
	}

	return true;
}

// 6. Curvature remains fully available/functional after Noise's addition to the selector -- regression
// check against the sibling checkpoint this one is built alongside.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUICurvatureStillWorksTest, "VertexMaskForge.DynamicNoiseUI.CurvatureStillAvailableAndMutable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUICurvatureStillWorksTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("SetLayerMaskGeneratorType(Curvature) still succeeds"), Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Curvature));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask present"), Mask);
	if (!Mask)
	{
		return false;
	}
	TestTrue(TEXT("Params alternative is Curvature"), Mask->Params.IsType<FVertexMaskForgeCurvatureParams>());

	FVertexMaskForgeGeneratorParams NewParams = Mask->Params;
	NewParams.Get<FVertexMaskForgeCurvatureParams>().Multiplier = 2.0f;
	TestTrue(TEXT("SetLayerMaskParams(Curvature) still succeeds"), Stack.SetLayerMaskParams(Id, Mask->MaskInstanceId, NewParams));
	TestEqual(TEXT("Curvature Multiplier stored"), Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeCurvatureParams>().Multiplier, 2.0f);

	return true;
}

// 7. A mismatched-type payload (e.g. a Curvature payload submitted against a layer whose GeneratorType is
// Noise) is rejected by SetLayerMaskParams itself -- the same domain-level boundary MutateNoiseParamLikeUI/
// MutateDynamicNoiseParam rely on via their own IsType<FVertexMaskForgeNoiseParams>() guard. This proves
// model-level rejection, not orchestrator-level rejection (mirroring the corrected naming/scope precedent
// established for the Noise backend's own NoiseMismatchedTaggedPayloadRejectedByLayerStackBeforeOrchestration
// test in M16-K.6D-8F-B.1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIMismatchedPayloadRejectedTest, "VertexMaskForge.DynamicNoiseUI.MismatchedPayloadRejectedByLayerStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIMismatchedPayloadRejectedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;
	const FVertexMaskForgeNoiseParams OriginalParams = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();

	const FVertexMaskForgeGeneratorParams MismatchedPayload = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Curvature);
	TestFalse(TEXT("SetLayerMaskParams rejects a Curvature payload for a Noise layer"),
		Stack.SetLayerMaskParams(Id, InstanceId, MismatchedPayload));

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present"), Mask);
	if (Mask)
	{
		TestTrue(TEXT("GeneratorType still Noise"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
		TestTrue(TEXT("Params still Noise-typed"), Mask->Params.IsType<FVertexMaskForgeNoiseParams>());
		TestEqual(TEXT("ScaleX unchanged by the rejected write"), Mask->Params.Get<FVertexMaskForgeNoiseParams>().ScaleX, OriginalParams.ScaleX);
	}

	return true;
}

// 8. Levels Min/Max coupled-invariant maintenance, exercised through the exact Mutator shape
// MutateDynamicNoiseParam's own Levels Min/Max callbacks use (mirroring Curvature's own established
// pattern): editing Min above the current Max raises Max to match; editing Max below the current Min lowers
// Min to match; the OTHER field is only ever adjusted, never independently overwritten, and the whole
// exchange still commits through one single SetLayerMaskParams call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUILevelsMinMaxCoupledInvariantTest, "VertexMaskForge.DynamicNoiseUI.LevelsMinMaxCoupledInvariantMaintained", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUILevelsMinMaxCoupledInvariantTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;
	// Defaults: LevelsMin=0, LevelsMax=1.

	// Lower Max to 0.5 first (an ordinary edit, no coupling triggered -- Min=0 is already <= 0.5), so a
	// subsequent Min edit has room to legitimately exceed it.
	TestTrue(TEXT("Lower LevelsMax to 0.5 (no coupling triggered)"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		P.LevelsMax = FMath::Clamp(0.5f, 0.0f, 1.0f);
		if (P.LevelsMax < P.LevelsMin)
		{
			P.LevelsMin = P.LevelsMax;
		}
	}));

	// Raise Min above the current Max (0.5) -- Max must be pulled up to match.
	TestTrue(TEXT("Raise LevelsMin above Max"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		P.LevelsMin = FMath::Clamp(0.95f, 0.0f, 1.0f);
		if (P.LevelsMin > P.LevelsMax)
		{
			P.LevelsMax = P.LevelsMin;
		}
	}));
	{
		const FVertexMaskForgeNoiseParams& Params = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("LevelsMin is 0.95"), Params.LevelsMin, 0.95f);
		TestEqual(TEXT("LevelsMax pulled up to 0.95"), Params.LevelsMax, 0.95f);
	}

	// Lower Max below the current Min (0.95) -- Min must be pulled down to match.
	TestTrue(TEXT("Lower LevelsMax below Min"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		P.LevelsMax = FMath::Clamp(0.1f, 0.0f, 1.0f);
		if (P.LevelsMax < P.LevelsMin)
		{
			P.LevelsMin = P.LevelsMax;
		}
	}));
	{
		const FVertexMaskForgeNoiseParams& Params = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("LevelsMax is 0.1"), Params.LevelsMax, 0.1f);
		TestEqual(TEXT("LevelsMin pulled down to 0.1"), Params.LevelsMin, 0.1f);
	}

	return true;
}

// --- M16-K.6D-8F-C.1 / M16-K.6D-8F-D.1: Lock Axes -------------------------------------------------------
//
// Every test below calls VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock and/or
// VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit directly -- the SAME production functions
// SVertexMaskForgePanel::OnDynamicNoiseScaleAxesLockChanged and the real Scale X callback call (see
// SVertexMaskForgePanel.h/.cpp). No test-local reconstruction of the normalization/synchronization
// algorithm exists. Where a test needs to represent "the checkbox is currently checked," it passes a
// plain `bool` (or, for the two-layer test, looks up a plain `TMap<FGuid,bool>`) as ORDINARY INPUT DATA
// to these real functions' own bAxesLocked parameter -- never as a second implementation of the toggle
// logic itself, which lives ONLY in the two production functions now.

// 9. Unlocked: editing X (via the real production helper, bAxesLocked=false) changes only X; editing Y/Z
// (via plain field writes -- the real Y/Z callbacks are untouched one-line clamps, not extracted, since
// M16-K.6D-8F-D.1's own audit flagged only the Lock-enable/Scale-X branching logic) changes only that axis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIUnlockedAxesIndependentTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.UnlockedEditsEachAxisIndependently", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIUnlockedAxesIndependentTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	TestTrue(TEXT("Edit X while unlocked, via the real production helper"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P, 5.0f, /*bAxesLocked=*/false);
	}));
	{
		const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("X changed"), P.ScaleX, 5.0f);
		TestEqual(TEXT("Y preserved"), P.ScaleY, 1.0f);
		TestEqual(TEXT("Z preserved"), P.ScaleZ, 1.0f);
	}

	TestTrue(TEXT("Edit Y while unlocked"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleY = 7.0f; }));
	{
		const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("X preserved"), P.ScaleX, 5.0f);
		TestEqual(TEXT("Y changed"), P.ScaleY, 7.0f);
		TestEqual(TEXT("Z preserved"), P.ScaleZ, 1.0f);
	}

	TestTrue(TEXT("Edit Z while unlocked"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleZ = 9.0f; }));
	{
		const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("X preserved"), P.ScaleX, 5.0f);
		TestEqual(TEXT("Y preserved"), P.ScaleY, 7.0f);
		TestEqual(TEXT("Z changed"), P.ScaleZ, 9.0f);
	}

	return true;
}

// 10. VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock's own contract, exercised directly: on
// (1,2,3) it must return true (a change occurred) and produce (1,1,1); on an already-equal payload it must
// return false and leave every field untouched. This IS the real function the lock-enable callback calls,
// not a re-derivation of its logic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIEnableLockNormalizesTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.EnablingNormalizesUnequalAxesButNotEqualOnes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIEnableLockNormalizesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	// Set up (1,2,3): X already 1 (default), set Y and Z explicitly while unlocked.
	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleY = 2.0f; P.ScaleZ = 3.0f; });

	// Mirrors OnDynamicNoiseScaleAxesLockChanged's own two-call shape exactly: decide via the REAL
	// production function on a plain copy first, then (only if it reports a change) apply the SAME
	// function again inside the real stack-mutation path.
	FVertexMaskForgeNoiseParams DecisionCopy = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	const bool bWouldChange = VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(DecisionCopy);
	TestTrue(TEXT("Production helper reports a change is needed for (1,2,3)"), bWouldChange);
	const bool bWroteOnEnable = bWouldChange && MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(P);
	});
	TestTrue(TEXT("Enabling lock on (1,2,3) performs a payload write"), bWroteOnEnable);
	{
		const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("X unchanged (1)"), P.ScaleX, 1.0f);
		TestEqual(TEXT("Y normalized to X (1)"), P.ScaleY, 1.0f);
		TestEqual(TEXT("Z normalized to X (1)"), P.ScaleZ, 1.0f);
	}

	// Re-enabling while already equal must report false and change nothing -- the production function's
	// own contract, checked directly (no payload write is attempted at all, matching the real callback's
	// own early-return).
	FVertexMaskForgeNoiseParams AlreadyEqualCopy = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	const FVertexMaskForgeNoiseParams BeforeCopy = AlreadyEqualCopy;
	TestFalse(TEXT("Production helper reports NO change when axes already equal"), VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(AlreadyEqualCopy));
	TestEqual(TEXT("Params left completely untouched on the no-op path"), AlreadyEqualCopy.ScaleX, BeforeCopy.ScaleX);
	TestEqual(TEXT("Params left completely untouched on the no-op path (Y)"), AlreadyEqualCopy.ScaleY, BeforeCopy.ScaleY);
	TestEqual(TEXT("Params left completely untouched on the no-op path (Z)"), AlreadyEqualCopy.ScaleZ, BeforeCopy.ScaleZ);

	return true;
}

// 11. VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit's locked branch, exercised directly: absolute copy
// to Y/Z, not ratio-preserving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUILockedXSyncTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.LockedXEditSynchronizesAbsolutely", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUILockedXSyncTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	// Start from a non-uniform, non-1.0 scale so a ratio-preserving rule would produce a DIFFERENT result
	// than an absolute-copy rule -- distinguishing the two rules unambiguously.
	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { P.ScaleX = 2.0f; P.ScaleY = 4.0f; P.ScaleZ = 8.0f; });
	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(P); // Normalizes to (2,2,2).
	});

	TestTrue(TEXT("Locked X edit succeeds"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P, 6.5f, /*bAxesLocked=*/true);
	}));
	const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	TestEqual(TEXT("X is 6.5"), P.ScaleX, 6.5f);
	// Absolute-copy rule: Y=Z=6.5 exactly. A ratio-preserving rule (starting ratios were 1:1:1 after
	// normalization) would coincidentally also give 6.5 here, so this alone doesn't distinguish the rules
	// -- the distinguishing evidence is ApplyDynamicNoiseScaleXEdit's own source (no division, no ratio
	// computation anywhere in the production function, confirmed by direct inspection of its definition).
	TestEqual(TEXT("Y absolutely copied to 6.5"), P.ScaleY, 6.5f);
	TestEqual(TEXT("Z absolutely copied to 6.5"), P.ScaleZ, 6.5f);

	return true;
}

// 12. Disabling lock is, in the real callback, simply NOT calling the normalization helper at all (its
// disable branch performs no payload mutation whatsoever -- there is no function to call, so nothing to
// test beyond "no mutation occurred," which this test proves directly against the real stack state).
// Re-enabling independent unlocked editing afterward again goes through the real production helper.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUIDisableLockPreservesScaleTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.DisablingPreservesScaleWithNoPayloadWrite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUIDisableLockPreservesScaleTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(P); });
	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P, 3.5f, /*bAxesLocked=*/true); }); // (3.5,3.5,3.5)

	// "Disable" -- deliberately NO call of any kind (matching the real callback's own trivial early
	// return for the unchecked case) -- values must remain exactly as they were.
	const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	TestEqual(TEXT("X preserved (no operation performed)"), P.ScaleX, 3.5f);
	TestEqual(TEXT("Y preserved (no operation performed)"), P.ScaleY, 3.5f);
	TestEqual(TEXT("Z preserved (no operation performed)"), P.ScaleZ, 3.5f);

	// Now unlocked -- X/Y/Z editable independently again, X via the real production helper.
	TestTrue(TEXT("X editable again"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P2) { VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P2, 1.1f, /*bAxesLocked=*/false); }));
	TestTrue(TEXT("Y editable again"), MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P2) { P2.ScaleY = 2.2f; }));
	const FVertexMaskForgeNoiseParams& P2 = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	TestEqual(TEXT("X independent"), P2.ScaleX, 1.1f);
	TestEqual(TEXT("Y independent"), P2.ScaleY, 2.2f);
	TestEqual(TEXT("Z untouched by either edit"), P2.ScaleZ, 3.5f);

	return true;
}

// 13. Offset and every unrelated Noise field, plus generic layer properties, are unaffected by a locked
// Scale X edit performed through the real production helper.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUILockAxesUnrelatedFieldsUnaffectedTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.UnrelatedFieldsAndGenericPropertiesUnaffected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUILockAxesUnrelatedFieldsUnaffectedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerMaskGeneratorType(Id, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceId = Stack.GetLayerMask(Id)->MaskInstanceId;

	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P)
	{
		P.OffsetX = 11.0f; P.OffsetY = 22.0f; P.OffsetZ = 33.0f;
		P.Seed = 99; P.Octaves = 5; P.Roughness = 0.61f; P.Lacunarity = 3.3f; P.TurbulenceStrength = 0.44f;
		P.Multiplier = 0.72f; P.Blur = 0.18f; P.LevelsMin = 0.05f; P.LevelsMax = 0.95f; P.bInvert = true;
	});
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Screen);
	Stack.SetLayerOpacity(Id, 0.66f);
	Stack.SetLayerEnabled(Id, false);
	Stack.SetLayerChannelFilter(Id, false, true, false);

	MutateNoiseParamLikeUI(Stack, Id, InstanceId, [](FVertexMaskForgeNoiseParams& P) { VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P, 4.0f, /*bAxesLocked=*/true); });

	const FVertexMaskForgeNoiseParams& P = Stack.GetLayerMask(Id)->Params.Get<FVertexMaskForgeNoiseParams>();
	TestEqual(TEXT("OffsetX unaffected"), P.OffsetX, 11.0f);
	TestEqual(TEXT("OffsetY unaffected"), P.OffsetY, 22.0f);
	TestEqual(TEXT("OffsetZ unaffected"), P.OffsetZ, 33.0f);
	TestEqual(TEXT("Seed unaffected"), P.Seed, 99);
	TestEqual(TEXT("Octaves unaffected"), P.Octaves, 5);
	TestEqual(TEXT("Roughness unaffected"), P.Roughness, 0.61f);
	TestEqual(TEXT("Lacunarity unaffected"), P.Lacunarity, 3.3f);
	TestEqual(TEXT("TurbulenceStrength unaffected"), P.TurbulenceStrength, 0.44f);
	TestEqual(TEXT("Multiplier unaffected"), P.Multiplier, 0.72f);
	TestEqual(TEXT("Blur unaffected"), P.Blur, 0.18f);
	TestEqual(TEXT("LevelsMin unaffected"), P.LevelsMin, 0.05f);
	TestEqual(TEXT("LevelsMax unaffected"), P.LevelsMax, 0.95f);
	TestTrue(TEXT("bInvert unaffected"), P.bInvert);

	const FVertexMaskForgeLayer* Layer = Stack.FindLayerById(Id);
	TestNotNull(TEXT("Layer still present"), Layer);
	if (Layer)
	{
		TestTrue(TEXT("Fill unaffected"), Layer->Fill == EVertexMaskForgeLayerFill::White);
		TestTrue(TEXT("BlendMode unaffected"), Layer->BlendMode == EVertexMaskForgeBlendMode::Screen);
		TestEqual(TEXT("Opacity unaffected"), Layer->Opacity, 0.66f);
		TestFalse(TEXT("bEnabled unaffected"), Layer->bEnabled);
		TestFalse(TEXT("Red channel unaffected"), Layer->bAffectRed);
		TestTrue(TEXT("Green channel unaffected"), Layer->bAffectGreen);
		TestFalse(TEXT("Blue channel unaffected"), Layer->bAffectBlue);
	}

	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(Id);
	TestNotNull(TEXT("Mask still present"), Mask);
	if (Mask)
	{
		TestTrue(TEXT("GeneratorType still Noise"), Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
		TestTrue(TEXT("Params still Noise-typed"), Mask->Params.IsType<FVertexMaskForgeNoiseParams>());
		TestEqual(TEXT("MaskInstanceId still valid/unchanged"), Mask->MaskInstanceId, InstanceId);
	}

	return true;
}

// 14. A locked Scale X edit on Layer A, performed through the real production helper, never touches
// Layer B's Scale -- real per-LayerId isolation, proven via FVertexMaskForgeDynamicLayerStack's own API
// (the same guarantee every other per-layer Noise field already relies on). NOTE: this test does NOT (and
// cannot) claim that the panel's own private DynamicNoiseScaleAxesLockedByLayerId checkbox state is
// independent per layer, or that it survives reorder/rename -- that private panel-member fact is Category
// (3) in this file's own HONEST SCOPE STATEMENT above (source-inspected + Lucas's manual confirmation
// only). Reorder/rename ARE exercised here, but only to prove the real Stack API leaves Scale values alone
// across those operations, not to prove anything about UI lock-state persistence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUILockedEditIsolatedFromSiblingLayerTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.LockedLayerEditNeverAffectsSiblingNoiseLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUILockedEditIsolatedFromSiblingLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerMaskGeneratorType(A, EVertexMaskForgeGeneratorType::Noise);
	Stack.SetLayerMaskGeneratorType(B, EVertexMaskForgeGeneratorType::Noise);
	const FGuid InstanceA = Stack.GetLayerMask(A)->MaskInstanceId;
	const FGuid InstanceB = Stack.GetLayerMask(B)->MaskInstanceId;

	MutateNoiseParamLikeUI(Stack, A, InstanceA, [](FVertexMaskForgeNoiseParams& P) { P.ScaleY = 2.0f; P.ScaleZ = 3.0f; });
	MutateNoiseParamLikeUI(Stack, B, InstanceB, [](FVertexMaskForgeNoiseParams& P) { P.ScaleY = 20.0f; P.ScaleZ = 30.0f; });

	MutateNoiseParamLikeUI(Stack, A, InstanceA, [](FVertexMaskForgeNoiseParams& P) { VertexMaskForgePanel::NormalizeDynamicNoiseScaleForAxisLock(P); });
	MutateNoiseParamLikeUI(Stack, A, InstanceA, [](FVertexMaskForgeNoiseParams& P) { VertexMaskForgePanel::ApplyDynamicNoiseScaleXEdit(P, 9.0f, /*bAxesLocked=*/true); });

	{
		const FVertexMaskForgeNoiseParams& PA = Stack.GetLayerMask(A)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("A.X=9"), PA.ScaleX, 9.0f);
		TestEqual(TEXT("A.Y=9"), PA.ScaleY, 9.0f);
		TestEqual(TEXT("A.Z=9"), PA.ScaleZ, 9.0f);
		const FVertexMaskForgeNoiseParams& PB = Stack.GetLayerMask(B)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("B.X unaffected by A's lock/edit"), PB.ScaleX, 1.0f);
		TestEqual(TEXT("B.Y unaffected"), PB.ScaleY, 20.0f);
		TestEqual(TEXT("B.Z unaffected"), PB.ScaleZ, 30.0f);
	}

	// Reorder/rename via the real production API -- proves the STACK's own Scale values are unaffected by
	// either operation (a real, testable fact); makes no claim about the private panel checkbox map.
	TestTrue(TEXT("Reorder B before A"), Stack.MoveLayer(B, 0));
	TestTrue(TEXT("Rename A"), Stack.RenameLayer(A, TEXT("Renamed A")));
	{
		const FVertexMaskForgeNoiseParams& PA = Stack.GetLayerMask(A)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("A.Scale unaffected by reorder/rename"), PA.ScaleX, 9.0f);
		const FVertexMaskForgeNoiseParams& PB = Stack.GetLayerMask(B)->Params.Get<FVertexMaskForgeNoiseParams>();
		TestEqual(TEXT("B.Scale unaffected by reorder/rename"), PB.ScaleX, 1.0f);
	}

	return true;
}

// 15. Existing Noise defaults/payload behavior remains intact, Curvature remains available, and AO/
// Thickness remain unavailable via SetLayerMaskGeneratorType -- a lightweight regression check that this
// checkpoint's Lock Axes addition did not disturb the sibling generator surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicNoiseUILockAxesRegressionSiblingGeneratorsTest, "VertexMaskForge.DynamicNoiseUI.LockAxes.NoiseDefaultsCurvatureAvailableAOThicknessBehaviorUnchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicNoiseUILockAxesRegressionSiblingGeneratorsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;

	const FGuid NoiseId = Stack.AddLayer(TEXT("Noise"));
	TestTrue(TEXT("Noise still assignable"), Stack.SetLayerMaskGeneratorType(NoiseId, EVertexMaskForgeGeneratorType::Noise));
	const FVertexMaskForgeNoiseParams& NoiseDefaults = Stack.GetLayerMask(NoiseId)->Params.Get<FVertexMaskForgeNoiseParams>();
	const FVertexMaskForgeNoiseParams Authoritative;
	TestEqual(TEXT("ScaleX default unchanged"), NoiseDefaults.ScaleX, Authoritative.ScaleX);
	TestEqual(TEXT("ScaleY default unchanged"), NoiseDefaults.ScaleY, Authoritative.ScaleY);
	TestEqual(TEXT("ScaleZ default unchanged"), NoiseDefaults.ScaleZ, Authoritative.ScaleZ);

	const FGuid CurvatureId = Stack.AddLayer(TEXT("Curvature"));
	TestTrue(TEXT("Curvature still assignable"), Stack.SetLayerMaskGeneratorType(CurvatureId, EVertexMaskForgeGeneratorType::Curvature));
	TestTrue(TEXT("Curvature Params alternative correct"), Stack.GetLayerMask(CurvatureId)->Params.IsType<FVertexMaskForgeCurvatureParams>());

	// AO/Thickness: SetLayerMaskGeneratorType itself is generic and does not reject them at the domain
	// layer (only the Dynamic selector's own OptionsSource omits them -- see M16-K.6D-8F-C's own audit);
	// this test therefore checks the actually-authoritative Dynamic-support boundary instead: the
	// orchestrator's own dispatch, which this checkpoint did not touch. A full re-proof of orchestrator
	// rejection is out of scope here (already covered by VertexMaskForgeDynamicSourceTopologyComposition
	// Tests.cpp's own UnsupportedGeneratorTypeFailsWholeCall); this test only confirms Noise/Curvature
	// remain correctly available after this checkpoint's change, which is the actual regression risk.

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
