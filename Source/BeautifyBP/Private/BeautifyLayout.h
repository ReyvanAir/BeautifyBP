#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBeautifyBP, Log, All);

class UEdGraph;
class UEdGraphNode;

namespace BeautifyBP
{
	/** Size used for a node whose widget hasn't been drawn (or when no editor shows the graph). */
	FVector2f EstimateNodeSize(const UEdGraphNode* Node);

	/** True for editable event graphs, functions and macros. Anim graphs use a K2-derived schema, so they're excluded by graph type. */
	bool CanBeautify(const UEdGraph* Graph);

	/** Selected node count in the editor showing this graph; 0 when no editor shows it. */
	int32 NumSelectedNodes(const UEdGraph* Graph);

	/**
	 * Arranges every node in the graph, or only the selected ones, as one undo step. Returns how many nodes were placed.
	 * The whole-graph pass also updates comment boxes; CommentTitles maps event or function names to box titles.
	 */
	int32 Beautify(UEdGraph* Graph, bool bSelectionOnly, const TMap<FString, FString>* CommentTitles = nullptr);
}
