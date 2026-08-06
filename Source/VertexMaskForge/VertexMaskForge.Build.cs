using UnrealBuildTool;

public class VertexMaskForge : ModuleRules
{
	public VertexMaskForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"ToolMenus",
				"ContentBrowser",
				"MeshDescription",
				"StaticMeshDescription",
				"MeshConversion",
				"GeometryCore",
				// UDynamicMeshComponent -- the Source-Topology (Nanite) preview component, the same
				// mechanism UE's own Paint Vertex Colors tool uses to render an FDynamicMesh3 directly
				// (never a UStaticMeshComponent, whose OverrideVertexColors Nanite's renderer ignores).
				"GeometryFramework",
				// Required for BeginInitResource(FRenderResource*) (RenderCore/Public/RenderResource.h),
				// used to initialize the transient FColorVertexBuffer allocated for the non-destructive
				// component-level Vertex Color preview (FStaticMeshComponentLODInfo::OverrideVertexColors).
				"RenderCore",
				// SPrimaryButton (Developer/ToolWidgets/Public/SPrimaryButton.h) for the Accept button --
				// the same generic, Editor-Mode-agnostic widget class Modeling Tools Editor Mode itself
				// uses for its Accept/Complete buttons (audited in ModelingToolsEditorModeToolkit.cpp).
				// Editor-only and already Slate/Editor-Mode-independent, safe for a standalone panel.
				"ToolWidgets",
				// M20-E: restored for VertexMaskForgeComponentOverrideBridge's "Send to Mesh Paint
				// Texture" bridge -- UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors
				// (Engine/Plugins/MeshPainting/Source/MeshPaintEditorMode/Public/MeshPaintModeHelpers.h)
				// is the native "From Vertex" import seam this bridge invokes.
				"MeshPaintEditorMode",
				// MeshPaintEditorMode's own module depends on MeshPaintingToolset; required transitively
				// for the same reason it was required by the original M20-C implementation.
				"MeshPaintingToolset",
				// UMeshPaintModeSubsystem : public UEditorSubsystem -- GEditor->GetEditorSubsystem<>()
				// needs this module's own generated class construction (Z_Construct_UClass_UEditorSubsystem)
				// at link time, or LNK2019 results. Confirmed against the installed UE 5.8 engine source
				// exactly as the original M20-C implementation's own compile-fix history describes.
				"EditorSubsystem",
			}
		);
	}
}
