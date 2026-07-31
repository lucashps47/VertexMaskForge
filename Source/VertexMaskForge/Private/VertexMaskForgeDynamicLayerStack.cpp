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
