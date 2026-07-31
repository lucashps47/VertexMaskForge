#include "VertexMaskForgeDynamicLayerStack.h"

namespace
{
	// No Count/Max sentinel exists on EVertexMaskForgeLayerFill or EVertexMaskForgeBlendMode, and neither
	// enum carries a documented contiguity contract -- both are validated by explicit switch over their
	// real named enumerators, never a numeric range check. Shared by IsValid() and the SetLayer* mutators
	// below so there is only one definition of "what counts as a valid Fill/BlendMode/Opacity" in this file.

	bool IsValidLayerFill(const EVertexMaskForgeLayerFill Fill)
	{
		switch (Fill)
		{
		case EVertexMaskForgeLayerFill::None:
		case EVertexMaskForgeLayerFill::Black:
		case EVertexMaskForgeLayerFill::White:
			return true;
		default:
			return false;
		}
	}

	bool IsValidBlendMode(const EVertexMaskForgeBlendMode BlendMode)
	{
		switch (BlendMode)
		{
		case EVertexMaskForgeBlendMode::Copy:
		case EVertexMaskForgeBlendMode::Add:
		case EVertexMaskForgeBlendMode::Subtract:
		case EVertexMaskForgeBlendMode::Multiply:
		case EVertexMaskForgeBlendMode::Overlay:
		case EVertexMaskForgeBlendMode::Screen:
		case EVertexMaskForgeBlendMode::Linear:
			return true;
		default:
			return false;
		}
	}

	bool IsValidOpacityValue(const float Opacity)
	{
		return FMath::IsFinite(Opacity) && Opacity >= 0.0f && Opacity <= 1.0f;
	}

	// AUDITED (M16-K.5B.1 corrective): same discipline as the three validators above -- explicit switch
	// over the seven real named enumerators, no range check (EVertexMaskForgeGeneratorType carries no
	// Count/Max sentinel), default: false. This is the fix for a confirmed gap: MakeVertexMaskForgeGenerat
	// orParams (VertexMaskForgeRecipeTypes.h) is NOT a validator -- its own switch groups an unrecognized
	// value into the same `default:` case as MaterialSlot, silently producing a MaterialSlot Params
	// alternative for ANY invalid input, with no failure signal. SetLayerMaskGeneratorType must reject an
	// invalid GeneratorType itself, before ever reaching that factory -- see this function's own call site
	// below for exactly where.
	bool IsValidGeneratorType(const EVertexMaskForgeGeneratorType GeneratorType)
	{
		switch (GeneratorType)
		{
		case EVertexMaskForgeGeneratorType::BoundingBox:
		case EVertexMaskForgeGeneratorType::AmbientOcclusion:
		case EVertexMaskForgeGeneratorType::Curvature:
		case EVertexMaskForgeGeneratorType::Noise:
		case EVertexMaskForgeGeneratorType::DirectionalNormal:
		case EVertexMaskForgeGeneratorType::Thickness:
		case EVertexMaskForgeGeneratorType::MaterialSlot:
			return true;
		default:
			return false;
		}
	}
}

FVertexMaskForgeDynamicLayerStack FVertexMaskForgeDynamicLayerStack::MakeInitialStack()
{
	FVertexMaskForgeDynamicLayerStack Stack;
	Stack.AddLayer(TEXT("Base Layer"));
	return Stack;
}

FGuid FVertexMaskForgeDynamicLayerStack::AddLayer(const FString& Name)
{
	FVertexMaskForgeLayer NewLayer;
	NewLayer.LayerId = FGuid::NewGuid();
	NewLayer.Name = Name;
	NewLayer.Fill = EVertexMaskForgeLayerFill::None;

	Layers.Add(MoveTemp(NewLayer));
	return Layers.Last().LayerId;
}

FVertexMaskForgeDynamicLayerStack::FRemoveResult FVertexMaskForgeDynamicLayerStack::RemoveLayer(const FGuid& LayerId)
{
	FRemoveResult Result;

	const int32 Index = FindLayerIndexById(LayerId);
	if (Index == INDEX_NONE)
	{
		Result.bRemoved = false;
		Result.RemovedIndex = INDEX_NONE;
		Result.RemainingNum = Layers.Num();
		return Result;
	}

	Layers.RemoveAt(Index);

	Result.bRemoved = true;
	Result.RemovedIndex = Index;
	Result.RemainingNum = Layers.Num();
	return Result;
}

const FVertexMaskForgeLayer* FVertexMaskForgeDynamicLayerStack::FindLayerById(const FGuid& LayerId) const
{
	return Layers.FindByPredicate([&LayerId](const FVertexMaskForgeLayer& Layer) { return Layer.LayerId == LayerId; });
}

FVertexMaskForgeLayer* FVertexMaskForgeDynamicLayerStack::FindLayerByIdMutable(const FGuid& LayerId)
{
	return Layers.FindByPredicate([&LayerId](const FVertexMaskForgeLayer& Layer) { return Layer.LayerId == LayerId; });
}

int32 FVertexMaskForgeDynamicLayerStack::FindLayerIndexById(const FGuid& LayerId) const
{
	return Layers.IndexOfByPredicate([&LayerId](const FVertexMaskForgeLayer& Layer) { return Layer.LayerId == LayerId; });
}

bool FVertexMaskForgeDynamicLayerStack::RenameLayer(const FGuid& LayerId, const FString& NewName)
{
	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->Name = NewName;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::MoveLayer(const FGuid& LayerId, int32 NewIndex)
{
	const int32 CurrentIndex = FindLayerIndexById(LayerId);
	if (CurrentIndex == INDEX_NONE)
	{
		return false;
	}

	if (NewIndex < 0 || NewIndex >= Layers.Num())
	{
		return false;
	}

	if (CurrentIndex == NewIndex)
	{
		return true;
	}

	FVertexMaskForgeLayer Moved = Layers[CurrentIndex];
	Layers.RemoveAt(CurrentIndex);
	Layers.Insert(MoveTemp(Moved), NewIndex);
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::MoveLayerUp(const FGuid& LayerId)
{
	const int32 CurrentIndex = FindLayerIndexById(LayerId);
	if (CurrentIndex == INDEX_NONE || CurrentIndex == 0)
	{
		return false;
	}

	Layers.Swap(CurrentIndex, CurrentIndex - 1);
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::MoveLayerDown(const FGuid& LayerId)
{
	const int32 CurrentIndex = FindLayerIndexById(LayerId);
	if (CurrentIndex == INDEX_NONE || CurrentIndex + 1 >= Layers.Num())
	{
		return false;
	}

	Layers.Swap(CurrentIndex, CurrentIndex + 1);
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerFill(const FGuid& LayerId, const EVertexMaskForgeLayerFill Fill)
{
	if (!IsValidLayerFill(Fill))
	{
		return false;
	}

	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->Fill = Fill;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerBlendMode(const FGuid& LayerId, const EVertexMaskForgeBlendMode BlendMode)
{
	if (!IsValidBlendMode(BlendMode))
	{
		return false;
	}

	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->BlendMode = BlendMode;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerOpacity(const FGuid& LayerId, const float Opacity)
{
	if (!IsValidOpacityValue(Opacity))
	{
		return false;
	}

	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->Opacity = Opacity;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerEnabled(const FGuid& LayerId, const bool bEnabled)
{
	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->bEnabled = bEnabled;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerChannelFilter(const FGuid& LayerId, const bool bAffectRed, const bool bAffectGreen, const bool bAffectBlue)
{
	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	Layer->bAffectRed = bAffectRed;
	Layer->bAffectGreen = bAffectGreen;
	Layer->bAffectBlue = bAffectBlue;
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::SetLayerMaskGeneratorType(const FGuid& LayerId, const EVertexMaskForgeGeneratorType GeneratorType)
{
	// AUDITED (M16-K.5B.1 corrective): rejected BEFORE the lookup -- an invalid GeneratorType is invalid
	// regardless of which layer (if any) LayerId resolves to, so there is no reason to spend a lookup on
	// it. This is what guarantees NOTHING observable happens for an invalid value: no layer is found or
	// touched, no FGuid is minted, no existing Mask is read or replaced, and MakeVertexMaskForgeGeneratorPa
	// rams (whose own `default:` case would otherwise silently produce a MaterialSlot Params alternative
	// for the invalid value) is never called.
	if (!IsValidGeneratorType(GeneratorType))
	{
		return false;
	}

	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	// AUDITED (M16-K.5B): requesting the SAME type the layer already has is a true no-op -- MaskInstanceId
	// and Params are left completely untouched, never recreated, even though the values would end up
	// equal either way. This is what makes SetLayerMaskGeneratorType safe to call repeatedly/idempotently
	// from a future UI without silently discarding in-progress parameter edits (deferred to K.5C).
	if (Layer->Mask.IsSet() && Layer->Mask->GeneratorType == GeneratorType)
	{
		return true;
	}

	// Either no mask yet, or an existing mask of a DIFFERENT type -- both cases assign a wholly fresh
	// instance: new MaskInstanceId, the requested GeneratorType, and that type's own default Params. Any
	// previous instance's Params are discarded outright, never merged/carried over.
	FVertexMaskForgeGeneratorMaskInstance NewInstance;
	NewInstance.MaskInstanceId = FGuid::NewGuid();
	NewInstance.GeneratorType = GeneratorType;
	NewInstance.Params = MakeVertexMaskForgeGeneratorParams(GeneratorType);
	Layer->Mask = MoveTemp(NewInstance);
	return true;
}

bool FVertexMaskForgeDynamicLayerStack::ClearLayerMask(const FGuid& LayerId)
{
	FVertexMaskForgeLayer* Layer = FindLayerByIdMutable(LayerId);
	if (!Layer)
	{
		return false;
	}

	// Idempotent -- clearing an already-unset Mask is a harmless no-op, still a success (the postcondition
	// "this layer has no mask" already held).
	Layer->Mask.Reset();
	return true;
}

const FVertexMaskForgeGeneratorMaskInstance* FVertexMaskForgeDynamicLayerStack::GetLayerMask(const FGuid& LayerId) const
{
	const FVertexMaskForgeLayer* Layer = FindLayerById(LayerId);
	if (!Layer || !Layer->Mask.IsSet())
	{
		return nullptr;
	}
	return &Layer->Mask.GetValue();
}

bool FVertexMaskForgeDynamicLayerStack::IsValid() const
{
	TSet<FGuid> SeenIds;
	SeenIds.Reserve(Layers.Num());

	for (const FVertexMaskForgeLayer& Layer : Layers)
	{
		if (!Layer.LayerId.IsValid())
		{
			return false;
		}

		bool bAlreadyInSet = false;
		SeenIds.Add(Layer.LayerId, &bAlreadyInSet);
		if (bAlreadyInSet)
		{
			return false;
		}

		if (!IsValidOpacityValue(Layer.Opacity))
		{
			return false;
		}

		if (!IsValidLayerFill(Layer.Fill))
		{
			return false;
		}

		if (!IsValidBlendMode(Layer.BlendMode))
		{
			return false;
		}
	}

	return true;
}
