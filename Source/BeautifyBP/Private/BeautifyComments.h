#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphNode_Comment;

namespace BeautifyBP
{
	/** Room a generated comment adds around its chain: padding on the sides and bottom, the title bar on top. */
	constexpr float CommentPad = 32.f;
	constexpr float CommentTitleHeight = 48.f;  // default 18 pt comment font

	/** An event chain (or a group of unused pure nodes) as the layout placed it. */
	struct FChain
	{
		UEdGraphNode* Root = nullptr;
		TArray<UEdGraphNode*> Nodes;  // exec order, each node followed by its pure inputs
		bool bExec = false;
	};

	/** Chains of one node (like the unused default events in a new Actor) and groups of unused pure nodes get no box. */
	inline bool GetsCommentBox(const FChain& Chain)
	{
		return Chain.bExec && Chain.Nodes.Num() >= 2;
	}

	/** A comment the user drew, and the nodes fully inside it before the layout moved anything. */
	struct FUserComment
	{
		UEdGraphNode_Comment* Comment = nullptr;
		TArray<UEdGraphNode*> Members;
	};

	/** True for a comment Beautify generated that the user hasn't retitled; retitled ones count as the user's. */
	bool IsGeneratedComment(const UEdGraphNode_Comment* Comment);

	/** Call before nodes move. Sizes holds every node the layout will place. */
	TArray<FUserComment> FindUserComments(const UEdGraph& Graph, const TMap<UEdGraphNode*, FVector2f>& Sizes);

	/**
	 * Call after nodes move, inside the Beautify transaction. Refits the user's comments around their nodes,
	 * removes comments an earlier run generated, and (if enabled) boxes each chain of two or more nodes.
	 * Titles maps an event or function name ("OnTaskTick", "Event BeginPlay") to its comment title.
	 */
	void UpdateComments(UEdGraph& Graph, const TArray<FChain>& Chains, const TArray<FUserComment>& UserComments,
		const TMap<UEdGraphNode*, FVector2f>& Sizes, const TMap<FString, FString>* Titles);
}
