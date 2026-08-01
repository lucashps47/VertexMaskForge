#include "VertexMaskForgeDisplayColorDerivation.h"

#include "VertexMaskForgeColorConversion.h"

namespace
{
	// AUDITED (M3 extraction): moved verbatim from VertexMaskForgePanel::ApplyPreviewModeDisplay in
	// SVertexMaskForgePanel.cpp -- no formula/behavior change. Private to this .cpp: not part of
	// VertexMaskForgeDisplayColorDerivation's public API.
	//
	// AUDITED (M16-K.5F): the local ToDisplayFColor duplicate that used to live here (and the
	// SVertexMaskForgePanel.cpp copy this comment used to describe as "intentionally identical") were
	// both removed -- both real callers now consume VertexMaskForgeColorConversion::ToDisplayFColor,
	// the single shared implementation.

	/**
	 * Reduces a composed RGBA color to what the given Preview Mode should actually display.
	 *
	 * AUDITED (Alpha checkpoint): AlphaChannel follows the EXACT same pattern already established
	 * for Red/Green/Blue -- Composite.W (the real Alpha, ALWAYS just the baseline's own Alpha since
	 * the Blend Modes checkpoint -- see ComposeMaskLayer/UpdateWorkingColors) is read out as grayscale (A=0 -> black,
	 * A=1 -> white, values in between -> proportional gray), exactly mirroring how the other three
	 * channel-isolation modes already work. This reduction is a DISPLAY-ONLY view of Composite --
	 * Composite itself (the real RGBA the Preview holds up to this point) is never mutated by this
	 * function; only the returned copy is reduced to grayscale.
	 */
	FVector4f ApplyPreviewModeDisplay(const FVector4f& Composite, const EVertexMaskForgePreviewMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgePreviewMode::RedChannel:
			return FVector4f(Composite.X, Composite.X, Composite.X, 1.f);
		case EVertexMaskForgePreviewMode::GreenChannel:
			return FVector4f(Composite.Y, Composite.Y, Composite.Y, 1.f);
		case EVertexMaskForgePreviewMode::BlueChannel:
			return FVector4f(Composite.Z, Composite.Z, Composite.Z, 1.f);
		case EVertexMaskForgePreviewMode::AlphaChannel:
			return FVector4f(Composite.W, Composite.W, Composite.W, 1.f);
		case EVertexMaskForgePreviewMode::RGBVertexColor:
		default:
			return Composite;
		}
	}
}

namespace VertexMaskForgeDisplayColorDerivation
{
	/**
	 * Derives the render-order color buffer actually pushed to FStaticMeshComponentLODInfo::
	 * OverrideVertexColors for the LIVE PREVIEW ONLY, by reducing WorkingColors' RAW composited RGBA
	 * through the current Preview Mode (see ApplyPreviewModeDisplay) -- e.g. "Red Channel" reduces to
	 * a Red-only grayscale.
	 *
	 * AUDITED (Preview-Mode-cannot-affect-Accept fix): DISPLAY-ONLY. Called exclusively from
	 * ApplyPreviewToEntry, for the transient PreviewComponent shown in the viewport. Accept
	 * (BuildAcceptTargets) NEVER calls this function -- it persists State.WorkingColors verbatim, so
	 * the real multi-channel RGB is written regardless of which Preview Mode happens to be selected at
	 * the moment of Accept. Read-only: never mutates WorkingColors.
	 */
	TArray<FColor> DeriveDisplayColors(const TArray<FColor>& WorkingColors, const EVertexMaskForgePreviewMode PreviewMode)
	{
		TArray<FColor> Result;
		Result.SetNumUninitialized(WorkingColors.Num());

		for (int32 i = 0; i < WorkingColors.Num(); ++i)
		{
			const FColor& Working = WorkingColors[i];
			const FVector4f WorkingF = VertexMaskForgeColorConversion::ToLinearColorF(Working);
			Result[i] = VertexMaskForgeColorConversion::ToDisplayFColor(ApplyPreviewModeDisplay(WorkingF, PreviewMode));
		}

		return Result;
	}
}
