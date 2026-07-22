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
				// Required for BeginInitResource(FRenderResource*) (RenderCore/Public/RenderResource.h),
				// used to initialize the transient FColorVertexBuffer allocated for the non-destructive
				// component-level Vertex Color preview (FStaticMeshComponentLODInfo::OverrideVertexColors).
				"RenderCore",
				// SPrimaryButton (Developer/ToolWidgets/Public/SPrimaryButton.h) for the Accept button --
				// the same generic, Editor-Mode-agnostic widget class Modeling Tools Editor Mode itself
				// uses for its Accept/Complete buttons (audited in ModelingToolsEditorModeToolkit.cpp).
				// Editor-only and already Slate/Editor-Mode-independent, safe for a standalone panel.
				"ToolWidgets",
			}
		);
	}
}
