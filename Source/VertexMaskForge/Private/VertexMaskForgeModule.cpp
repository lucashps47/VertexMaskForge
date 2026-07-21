#include "VertexMaskForgeModule.h"

#include "SVertexMaskForgePanel.h"

#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FVertexMaskForgeModule"

namespace VertexMaskForge
{
	static const FName TabId(TEXT("VertexMaskForge"));
	static const FName ToolsMenuName(TEXT("LevelEditor.MainMenu.Tools"));
}

void FVertexMaskForgeModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		VertexMaskForge::TabId,
		FOnSpawnTab::CreateRaw(this, &FVertexMaskForgeModule::SpawnVertexMaskForgeTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Vertex Mask Forge"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Open the Vertex Mask Forge tool."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
		.SetAutoGenerateMenuEntry(false);

	ToolMenusStartupHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FVertexMaskForgeModule::RegisterMenus));
}

void FVertexMaskForgeModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(ToolMenusStartupHandle);

	UnregisterMenus();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(VertexMaskForge::TabId);
}

void FVertexMaskForgeModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(VertexMaskForge::ToolsMenuName);
	if (!ToolsMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("VertexMaskForgeCustomTools");

	Section.AddSubMenu(
		"CustomTools",
		LOCTEXT("CustomToolsSubMenu", "Custom Tools"),
		LOCTEXT("CustomToolsSubMenuTooltip", "Custom in-house editor tools."),
		FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
		{
			if (!SubMenu)
			{
				return;
			}

			FToolMenuSection& SubSection = SubMenu->FindOrAddSection("VertexMaskForge");

			SubSection.AddMenuEntry(
				"VertexMaskForge",
				LOCTEXT("VertexMaskForgeEntry", "Vertex Mask Forge"),
				LOCTEXT("VertexMaskForgeEntryTooltip", "Open the Vertex Mask Forge tool."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(VertexMaskForge::TabId);
				})));
		}),
		/*bInOpenSubMenuOnClick=*/false);
}

void FVertexMaskForgeModule::UnregisterMenus()
{
	UToolMenus::UnregisterOwner(this);
}

TSharedRef<SDockTab> FVertexMaskForgeModule::SpawnVertexMaskForgeTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SVertexMaskForgePanel)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVertexMaskForgeModule, VertexMaskForge)
