#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/DeveloperSettings.h"

#include "BeautifySettings.generated.h"

UENUM()
enum class EBeautifyChainPlacement : uint8
{
	/** Stack when the graph looks generated (most nodes on one line), otherwise keep chains where they are. */
	Auto,
	/** Stack event chains top to bottom, in their current order. */
	Stack,
	/** Keep each event chain where it is; a chain only moves down if it would overlap another. */
	KeepInPlace UMETA(DisplayName = "Keep In Place"),
};

/** Project Settings > Plugins > Beautify BP. Saved in DefaultEditor.ini, so the whole team gets the same layout. */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Beautify BP"))
class UBeautifySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** Gap between one exec node and the next (measured from the next node's leftmost pure input). */
	UPROPERTY(Config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "16", ClampMax = "512"))
	float HorizontalSpacing = 64.f;

	/** Gap between stacked branches (Branch False, Sequence Then 1...) and between stacked pure inputs. */
	UPROPERTY(Config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "8", ClampMax = "256"))
	float VerticalSpacing = 32.f;

	/** Gap between event chains, on top of the room comment boxes need. */
	UPROPERTY(Config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "16", ClampMax = "1024"))
	float ChainSpacing = 128.f;

	/** Beautify Graph only: Beautify Selection always keeps the selection's top-left corner. */
	UPROPERTY(Config, EditAnywhere, Category = "Layout")
	EBeautifyChainPlacement ChainPlacement = EBeautifyChainPlacement::Auto;

	/** Beautify Graph also tidies the graphs inside collapsed nodes, in the same undo step. Macros are shared and never touched. */
	UPROPERTY(Config, EditAnywhere, Category = "Layout")
	bool bIncludeCollapsedGraphs = true;

	/**
	 * Graph types besides Blueprints that Beautify may arrange, such as SimFlowGraphSchema. Every pin in these
	 * graphs is treated as flow, so only add graphs that are drawn as flow (left to right), not data graphs like materials.
	 * An entry whose plugin isn't in the project is simply skipped.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Graph Types")
	TArray<TSoftClassPtr<UEdGraphSchema>> OtherGraphSchemas;

	/** Beautify Graph wraps each event chain of two or more nodes in a titled, colored comment box. */
	UPROPERTY(Config, EditAnywhere, Category = "Comments")
	bool bAddCommentBoxes = true;

	/** BeginPlay, Tick and other engine events. */
	UPROPERTY(Config, EditAnywhere, Category = "Comments", meta = (EditCondition = "bAddCommentBoxes"))
	FLinearColor EngineEventColor = FLinearColor(0.20f, 0.40f, 0.80f);

	/** Input action, key and axis events. */
	UPROPERTY(Config, EditAnywhere, Category = "Comments", meta = (EditCondition = "bAddCommentBoxes"))
	FLinearColor InputEventColor = FLinearColor(0.20f, 0.65f, 0.45f);

	UPROPERTY(Config, EditAnywhere, Category = "Comments", meta = (EditCondition = "bAddCommentBoxes"))
	FLinearColor CustomEventColor = FLinearColor(0.85f, 0.55f, 0.20f);

	/** Function and macro graphs. */
	UPROPERTY(Config, EditAnywhere, Category = "Comments", meta = (EditCondition = "bAddCommentBoxes"))
	FLinearColor FunctionColor = FLinearColor(0.55f, 0.35f, 0.80f);

	/** Chains that start somewhere else, such as an exec node nothing calls. */
	UPROPERTY(Config, EditAnywhere, Category = "Comments", meta = (EditCondition = "bAddCommentBoxes"))
	FLinearColor OtherColor = FLinearColor(0.50f, 0.50f, 0.50f);
};
