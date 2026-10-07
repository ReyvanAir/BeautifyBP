#include "BeautifyLayout.h"

#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "GraphEditor.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "UObject/UObjectHash.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BeautifyBP"

namespace
{
	class FBeautifyBPCommands : public TCommands<FBeautifyBPCommands>
	{
	public:
		FBeautifyBPCommands()
			: TCommands(TEXT("BeautifyBP"), LOCTEXT("CommandsContext", "Beautify Blueprint"), NAME_None, FAppStyle::GetAppStyleSetName())
		{
		}

		virtual void RegisterCommands() override
		{
			// No default chords: users bind them in Editor Preferences > Keyboard Shortcuts.
			UI_COMMAND(BeautifyGraph, "Beautify Graph", "Arrange every node in the graph you're viewing. One Ctrl+Z undoes it.", EUserInterfaceActionType::Button, FInputChord());
			UI_COMMAND(BeautifySelection, "Beautify Selection", "Arrange only the selected nodes. Unselected nodes don't move.", EUserInterfaceActionType::Button, FInputChord());
		}

		TSharedPtr<FUICommandInfo> BeautifyGraph;
		TSharedPtr<FUICommandInfo> BeautifySelection;
	};

	FSlateIcon BeautifyIcon()
	{
		return FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.StraightenConnections");
	}

	bool CanRun(const UEdGraph* Graph, bool bSelectionOnly)
	{
		return BeautifyBP::CanBeautify(Graph) && (!bSelectionOnly || BeautifyBP::NumSelectedNodes(Graph) >= 2);
	}

	void Run(UEdGraph* Graph, bool bSelectionOnly)
	{
		if (!CanRun(Graph, bSelectionOnly))
		{
			return;
		}
		const int32 NumPlaced = BeautifyBP::Beautify(Graph, bSelectionOnly);
		FNotificationInfo Info(FText::Format(LOCTEXT("Done", "Beautified {0} {0}|plural(one=node,other=nodes)"), NumPlaced));
		Info.ExpireDuration = 2.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	// Every Blueprint editor owns its own command list, so matching on it finds exactly the editor
	// whose button or shortcut fired, never another open Blueprint.
	UEdGraph* FocusedGraph(const FUICommandList* Commands)
	{
		for (const TSharedRef<IBlueprintEditor>& Editor : FModuleManager::GetModuleChecked<FBlueprintEditorModule>("Kismet").GetBlueprintEditors())
		{
			if (&Editor->GetToolkitCommands().Get() == Commands)
			{
				return Editor->GetFocusedGraph();
			}
		}
		return nullptr;
	}

	/**
	 * For editors other than the Blueprint editor (SimFlow and other listed graph types): the graph owned by the
	 * assets this editor has open, preferring the one on screen. Null when the editor has no graph Beautify may arrange.
	 */
	UEdGraph* FindOtherEditorGraph(const FToolMenuContext& Context)
	{
		const UAssetEditorToolkitMenuContext* EditorContext = Context.FindContext<UAssetEditorToolkitMenuContext>();
		if (!EditorContext)
		{
			return nullptr;
		}
		UEdGraph* Offscreen = nullptr;
		for (UObject* Asset : EditorContext->GetEditingObjects())
		{
			TArray<UObject*> Inner;
			GetObjectsWithOuter(Asset, Inner, EGetObjectsFlags::IncludeNestedObjects);
			for (UObject* Object : Inner)
			{
				UEdGraph* Graph = Cast<UEdGraph>(Object);
				// Blueprint graphs have their own button; skipping them keeps Blueprint editors to one.
				if (!Graph || !Graph->GetSchema() || Graph->GetSchema()->IsA<UEdGraphSchema_K2>() || !BeautifyBP::CanBeautify(Graph))
				{
					continue;
				}
				if (SGraphEditor::FindGraphEditorForGraph(Graph))
				{
					return Graph;
				}
				Offscreen = Offscreen ? Offscreen : Graph;
			}
		}
		return Offscreen;
	}

	FToolUIAction OtherEditorAction(bool bSelectionOnly)
	{
		FToolUIAction Action;
		Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([bSelectionOnly](const FToolMenuContext& Context)
		{
			Run(FindOtherEditorGraph(Context), bSelectionOnly);
		});
		Action.CanExecuteAction = FToolMenuCanExecuteAction::CreateLambda([bSelectionOnly](const FToolMenuContext& Context)
		{
			return CanRun(FindOtherEditorGraph(Context), bSelectionOnly);
		});
		return Action;
	}

	// Called once per Blueprint editor as it opens, with that editor's command list.
	TSharedRef<FExtender> BindCommands(const TSharedRef<FUICommandList> Commands, const TArray<UObject*> /*EditingObjects*/)
	{
		const FBeautifyBPCommands& Cmds = FBeautifyBPCommands::Get();
		if (!Commands->IsActionMapped(Cmds.BeautifyGraph))
		{
			// Weak: the list owns these actions, so a strong capture would keep the list alive forever.
			const TWeakPtr<FUICommandList> WeakCommands = Commands;
			auto Bind = [&Commands, &WeakCommands](const TSharedPtr<FUICommandInfo>& Command, bool bSelectionOnly)
			{
				Commands->MapAction(Command,
					FExecuteAction::CreateLambda([WeakCommands, bSelectionOnly]
					{
						if (const TSharedPtr<FUICommandList> List = WeakCommands.Pin())
						{
							Run(FocusedGraph(List.Get()), bSelectionOnly);
						}
					}),
					FCanExecuteAction::CreateLambda([WeakCommands, bSelectionOnly]
					{
						const TSharedPtr<FUICommandList> List = WeakCommands.Pin();
						return List && CanRun(FocusedGraph(List.Get()), bSelectionOnly);
					}));
			};
			Bind(Cmds.BeautifyGraph, false);
			Bind(Cmds.BeautifySelection, true);
		}
		return MakeShared<FExtender>();
	}
}

class FBeautifyBPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FBeautifyBPCommands::Register();

		const FAssetEditorExtender Extender = FAssetEditorExtender::CreateStatic(&BindCommands);
		ExtenderHandle = Extender.GetHandle();
		FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet").GetMenuExtensibilityManager()->GetExtenderDelegates().Add(Extender);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBeautifyBPModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		if (FBlueprintEditorModule* Kismet = FModuleManager::GetModulePtr<FBlueprintEditorModule>("Kismet"))
		{
			Kismet->GetMenuExtensibilityManager()->GetExtenderDelegates().RemoveAll([this](const FAssetEditorExtender& Delegate)
			{
				return Delegate.GetHandle() == ExtenderHandle;
			});
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		FBeautifyBPCommands::Unregister();
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		const FBeautifyBPCommands& Cmds = FBeautifyBPCommands::Get();

		// Toolbar: main click beautifies the graph, the arrow holds Beautify Selection.
		// Commands resolve against the toolbar's own editor command list (mapped in BindCommands).
		FToolMenuSection& Toolbar = UToolMenus::Get()->ExtendMenu("AssetEditor.BlueprintEditor.ToolBar")->FindOrAddSection("BeautifyBP");
		Toolbar.AddEntry(FToolMenuEntry::InitToolBarButton(Cmds.BeautifyGraph, LOCTEXT("BeautifyLabel", "Beautify"), TAttribute<FText>(), BeautifyIcon()));
		Toolbar.AddEntry(FToolMenuEntry::InitComboButton("BeautifyOptions", FUIAction(),
			FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
			{
				Menu->FindOrAddSection("BeautifyBP").AddMenuEntry(FBeautifyBPCommands::Get().BeautifySelection);
			}),
			LOCTEXT("BeautifyOptionsLabel", "Beautify Options"),
			LOCTEXT("BeautifyOptionsTooltip", "More Beautify actions"),
			BeautifyIcon(),
			/*bInSimpleComboBox*/ true));

		// Other editors (SimFlow and other listed graph types). Every asset editor toolbar inherits from the default one,
		// so the section only appears where the open asset owns a graph Beautify may arrange. No shortcut here:
		// commands can only be bound into another plugin's editor from that plugin.
		UToolMenus::Get()->ExtendMenu("AssetEditor.DefaultToolBar")->AddDynamicSection("BeautifyBPOther", FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			if (!FindOtherEditorGraph(Menu->Context))
			{
				return;
			}
			const FBeautifyBPCommands& Commands = FBeautifyBPCommands::Get();
			FToolMenuSection& Section = Menu->AddSection("BeautifyBP");
			Section.AddEntry(FToolMenuEntry::InitToolBarButton("BeautifyGraph", OtherEditorAction(false),
				LOCTEXT("BeautifyLabel", "Beautify"), Commands.BeautifyGraph->GetDescription(), BeautifyIcon()));
			Section.AddEntry(FToolMenuEntry::InitComboButton("BeautifyOptions", FUIAction(),
				FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
				{
					const TSharedPtr<FUICommandInfo>& Selection = FBeautifyBPCommands::Get().BeautifySelection;
					SubMenu->FindOrAddSection("BeautifyBP").AddMenuEntry("BeautifySelection", Selection->GetLabel(), Selection->GetDescription(), BeautifyIcon(), OtherEditorAction(true));
				}),
				LOCTEXT("BeautifyOptionsLabel", "Beautify Options"),
				LOCTEXT("BeautifyOptionsTooltip", "More Beautify actions"),
				BeautifyIcon(),
				/*bInSimpleComboBox*/ true));
		}));

		// Node right-click menu. Every node menu inherits from the base schema menu, so one extension covers them all.
		// The graph editor's command list doesn't hold our commands, so these entries carry their own actions.
		UToolMenus::Get()->ExtendMenu("GraphEditor.GraphContextMenu.EdGraphSchema")->AddDynamicSection("BeautifyBP", FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
		{
			const UGraphNodeContextMenuContext* Context = Menu->FindContext<UGraphNodeContextMenuContext>();
			if (!Context || !Context->Node || Context->Pin || !BeautifyBP::CanBeautify(Context->Graph))
			{
				return;
			}
			const TWeakObjectPtr<UEdGraph> Graph = const_cast<UEdGraph*>(Context->Graph.Get());
			FToolMenuSection& Section = Menu->AddSection("BeautifyBP", LOCTEXT("BeautifySection", "Beautify"));
			auto Add = [&Section, &Graph](const TSharedPtr<FUICommandInfo>& Command, bool bSelectionOnly)
			{
				Section.AddMenuEntry(Command->GetCommandName(), Command->GetLabel(), Command->GetDescription(), BeautifyIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([Graph, bSelectionOnly] { Run(Graph.Get(), bSelectionOnly); }),
						FCanExecuteAction::CreateLambda([Graph, bSelectionOnly] { return CanRun(Graph.Get(), bSelectionOnly); })));
			};
			Add(FBeautifyBPCommands::Get().BeautifySelection, true);
			Add(FBeautifyBPCommands::Get().BeautifyGraph, false);
		}));
	}

	FDelegateHandle ExtenderHandle;
};

IMPLEMENT_MODULE(FBeautifyBPModule, BeautifyBP)

#undef LOCTEXT_NAMESPACE
