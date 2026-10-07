#include "BeautifyLayout.h"

#include "BeautifyComments.h"
#include "BeautifySettings.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "GraphEditor.h"
#include "K2Node_Composite.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "BeautifyBP"

DEFINE_LOG_CATEGORY(LogBeautifyBP);

namespace BeautifyBP
{
	namespace
	{
		constexpr float Grid = 16.f;
		constexpr float PureGapX = 32.f;   // between a pure node and the node it feeds
		constexpr float PureDropY = 64.f;  // pure inputs start below the exec wire running into their consumer

		struct FLayoutOptions
		{
			float ExecGapX = 64.f;   // between an exec node and the next one's pure inputs
			float GapY = 32.f;       // between stacked branches, and between stacked pure inputs
			float ChainGap = 128.f;  // between event chains
			float CommentReserve = 0.f;  // added next to a chain that gets a comment box
			bool bKeepChainsInPlace = false;
		};

		/**
		 * Blueprints flow along exec pins. The other graph types Beautify may arrange (OtherGraphSchemas, such as
		 * SimFlow) only draw flow wires, so there every pin counts.
		 */
		bool IsExecPin(const UEdGraphPin* Pin)
		{
			return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| !Pin->GetOwningNode()->GetGraph()->GetSchema()->IsA<UEdGraphSchema_K2>();
		}

		struct FLink
		{
			UEdGraphNode* Node = nullptr;            // the child
			const UEdGraphPin* ParentPin = nullptr;  // pin on the parent
			const UEdGraphPin* ChildPin = nullptr;   // pin on the child
		};

		/** Vertical span of a subtree, relative to its root node's top. Top <= 0 <= Bottom. */
		struct FExtent
		{
			float Top = 0.f;
			float Bottom = 0.f;
		};

		/**
		 * Exec flow runs left to right as a tree: a node's first exec output continues on its row,
		 * later outputs (Branch False, Sequence Then 1..n) stack below. Each pure node sits left of and
		 * below its first consumer. Every subtree gets its own vertical band and every child sits right
		 * of its parent, so nodes can't overlap.
		 */
		class FLayout
		{
		public:
			FLayout(const TArray<UEdGraphNode*>& InNodes, const TMap<UEdGraphNode*, FVector2f>& InSizes, const TMap<UEdGraphNode*, TSharedPtr<SGraphNode>>& InWidgets, const FLayoutOptions& InOptions)
				: Nodes(InNodes), Sizes(InSizes), Widgets(InWidgets), Options(InOptions)
			{
				for (UEdGraphNode* Node : Nodes)
				{
					InRange.Add(Node);
					if (Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return IsExecPin(Pin); }))
					{
						ExecNodes.Add(Node);
					}
				}
			}

			/**
			 * Stacked chains come back relative to an arbitrary origin and the caller moves them onto the anchor.
			 * Chains kept in place come back in graph coordinates.
			 */
			TMap<UEdGraphNode*, FVector2f> Compute()
			{
				BuildExecForest();
				AssignAllPure();

				TArray<UEdGraphNode*> Roots = ExecRoots;
				Roots.Append(PureRoots);
				for (UEdGraphNode* Root : Roots)
				{
					LayoutSubtree(Root);
				}

				ComputeSpineX();
				if (!Options.bKeepChainsInPlace)
				{
					PutBoundEventsAfterBinders(Roots);
				}

				TArray<FBox2f> Placed;
				float Top = 0.f;
				bool bPreviousBoxed = false;
				for (UEdGraphNode* Root : Roots)
				{
					FChain& Chain = Chains.Add_GetRef({ Root, {}, IsExec(Root) });
					Place(Root, SpineX[Root], 0.f, Chain.Nodes);
					const FBox2f Box = BoundsOf(Chain.Nodes);
					const bool bBoxed = GetsCommentBox(Chain);

					// Chains without boxes (single nodes, unused pure nodes) sit closer together.
					const float Reserve = !Placed.IsEmpty() && (bBoxed || bPreviousBoxed) ? Options.CommentReserve : 0.f;
					FVector2f Delta(0.f, Top + Reserve - Box.Min.Y);
					if (Options.bKeepChainsInPlace)
					{
						// The root goes back where it was; the chain only moves down past chains it would overlap.
						const float Gap = Options.ChainGap + Options.CommentReserve;
						Delta = Root->GetPosition() - Positions[Root];
						for (bool bMoved = true; bMoved;)
						{
							bMoved = false;
							const FBox2f Moved = Box.ShiftBy(Delta);
							for (const FBox2f& Other : Placed)
							{
								if (Moved.Min.X < Other.Max.X + Options.ExecGapX && Other.Min.X < Moved.Max.X + Options.ExecGapX
									&& Moved.Min.Y < Other.Max.Y + Gap && Other.Min.Y < Moved.Max.Y + Gap)
								{
									Delta.Y = Other.Max.Y + Gap - Box.Min.Y;
									bMoved = true;
									break;
								}
							}
						}
					}
					for (UEdGraphNode* Node : Chain.Nodes)
					{
						Positions[Node] += Delta;
					}
					Placed.Add(Box.ShiftBy(Delta));
					Top = Box.Max.Y + Delta.Y + Options.ChainGap;
					bPreviousBoxed = bBoxed;
				}
				return MoveTemp(Positions);
			}

			/** Each event chain (or group of unused pure nodes) with its nodes, in placement order. Valid after Compute. */
			const TArray<FChain>& GetChains() const { return Chains; }

			/**
			 * When an exec wire comes in from a node outside the range (Beautify Selection), the vertical
			 * shift that makes that wire straight. Uses the first such wire, in root order.
			 */
			TOptional<float> ShiftForIncomingWire(const TMap<UEdGraphNode*, FVector2f>& Placed) const
			{
				for (UEdGraphNode* Root : ExecRoots)
				{
					for (const UEdGraphPin* Pin : Root->Pins)
					{
						if (Pin->Direction != EGPD_Input || !IsExecPin(Pin))
						{
							continue;
						}
						for (const UEdGraphPin* Linked : Pin->LinkedTo)
						{
							UEdGraphNode* Source = Linked->GetOwningNode();
							if (!InRange.Contains(Source))
							{
								return Source->GetPosition().Y + PinY(Source, Linked) - PinY(Root, Pin) - Placed[Root].Y;
							}
						}
					}
				}
				return {};
			}

		private:
			bool IsExec(const UEdGraphNode* Node) const { return ExecNodes.Contains(Node); }

			static const TArray<FLink>& ChildrenOf(const TMap<UEdGraphNode*, TArray<FLink>>& Map, UEdGraphNode* Node)
			{
				static const TArray<FLink> None;
				const TArray<FLink>* Found = Map.Find(Node);
				return Found ? *Found : None;
			}

			static void SortByPosition(TArray<UEdGraphNode*>& Array)
			{
				Array.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
				{
					return A.NodePosY != B.NodePosY ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
				});
			}

			/** Pin centre measured from the node's top. Uses the drawn widget when available, else estimates one row per pin. */
			float PinY(UEdGraphNode* Node, const UEdGraphPin* Pin) const
			{
				if (const TSharedPtr<SGraphNode>* Widget = Widgets.Find(Node))
				{
					if (const TSharedPtr<SGraphPin> PinWidget = (*Widget)->FindWidgetForPin(const_cast<UEdGraphPin*>(Pin)))
					{
						const float Y = PinWidget->GetNodeOffset().Y;
						if (Y > 0.f)
						{
							return Y;
						}
					}
				}
				if (Node->IsA<UK2Node_Knot>())
				{
					return EstimateNodeSize(Node).Y * 0.5f;  // a reroute's pins sit at its centre
				}
				int32 Row = 0;
				for (const UEdGraphPin* Other : Node->Pins)
				{
					if (Other == Pin)
					{
						break;
					}
					if (Other->Direction == Pin->Direction && !Other->bHidden)
					{
						++Row;
					}
				}
				return 40.f + 26.f * Row;
			}

			// ponytail: recursive DFS and layout; a single exec chain thousands of nodes long could exhaust the stack. Switch to an explicit stack if that ever shows up.
			void BuildExecForest()
			{
				TArray<UEdGraphNode*> Entries;
				for (UEdGraphNode* Node : ExecNodes)
				{
					const bool bHasExecParent = Node->Pins.ContainsByPredicate([this](const UEdGraphPin* Pin)
					{
						return Pin->Direction == EGPD_Input && IsExecPin(Pin) && Pin->LinkedTo.ContainsByPredicate([this](const UEdGraphPin* Linked)
						{
							return InRange.Contains(Linked->GetOwningNode());
						});
					});
					if (!bHasExecParent)
					{
						Entries.Add(Node);
					}
				}
				// Exec cycles that nothing enters come last, started from their top-left node.
				TArray<UEdGraphNode*> Rest = ExecNodes.Array();
				SortByPosition(Entries);
				SortByPosition(Rest);
				Entries.Append(Rest);
				for (UEdGraphNode* Node : Entries)
				{
					if (!Visited.Contains(Node))
					{
						ExecRoots.Add(Node);
						Visit(Node);
					}
				}
			}

			void Visit(UEdGraphNode* Node)
			{
				Visited.Add(Node);
				OnStack.Add(Node);
				ExecOrder.Add(Node);
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->Direction != EGPD_Output || !IsExecPin(Pin))
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Child = Linked->GetOwningNode();
						if (!InRange.Contains(Child))
						{
							continue;
						}
						if (OnStack.Contains(Child))
						{
							BackEdges.Add({ Node, Child });
						}
						else if (!Visited.Contains(Child))
						{
							TreeChildren.FindOrAdd(Node).Add({ Child, Pin, Linked });
							Visit(Child);
						}
					}
				}
				OnStack.Remove(Node);
			}

			/** Claims every unclaimed pure node feeding this node, depth first, in input pin order. */
			void AssignPure(UEdGraphNode* Node)
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->Direction != EGPD_Input || IsExecPin(Pin))
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Source = Linked->GetOwningNode();
						if (InRange.Contains(Source) && !IsExec(Source) && !Owned.Contains(Source))
						{
							Owned.Add(Source);
							PureChildren.FindOrAdd(Node).Add({ Source, Pin, Linked });
							AssignPure(Source);
						}
					}
				}
			}

			void AssignAllPure()
			{
				// A pure node used several times stays with the consumer that runs first.
				for (UEdGraphNode* Node : ExecOrder)
				{
					AssignPure(Node);
				}

				// Pure nodes with no exec consumer in range (unused, or the selection holds only data nodes):
				// each sink starts its own group.
				TArray<UEdGraphNode*> Rest;
				for (UEdGraphNode* Node : Nodes)
				{
					if (!IsExec(Node) && !Owned.Contains(Node))
					{
						Rest.Add(Node);
					}
				}
				SortByPosition(Rest);
				// ponytail: O(n^2) over leftover pure nodes; fine for the handful a graph has.
				while (true)
				{
					UEdGraphNode* Root = nullptr;
					for (UEdGraphNode* Node : Rest)
					{
						if (!Owned.Contains(Node) && IsPureSink(Node))
						{
							Root = Node;
							break;
						}
					}
					for (int32 Index = 0; !Root && Index < Rest.Num(); ++Index)
					{
						Root = Owned.Contains(Rest[Index]) ? nullptr : Rest[Index];  // only a data cycle gets here
					}
					if (!Root)
					{
						break;
					}
					PureRoots.Add(Root);
					Owned.Add(Root);
					AssignPure(Root);
				}
			}

			bool IsPureSink(UEdGraphNode* Node) const
			{
				return !Node->Pins.ContainsByPredicate([this](const UEdGraphPin* Pin)
				{
					return Pin->Direction == EGPD_Output && Pin->LinkedTo.ContainsByPredicate([this](const UEdGraphPin* Linked)
					{
						UEdGraphNode* Consumer = Linked->GetOwningNode();
						return InRange.Contains(Consumer) && !IsExec(Consumer) && !Owned.Contains(Consumer);
					});
				});
			}

			/** Stacks the children's bands and records each child's offset from this node. */
			FExtent LayoutSubtree(UEdGraphNode* Node)
			{
				FExtent Extent{ 0.f, Sizes[Node].Y };
				auto Stack = [this, Node, &Extent](const TArray<FLink>& Links, float NextTop, bool bPure)
				{
					for (const FLink& Link : Links)
					{
						const FExtent Child = LayoutSubtree(Link.Node);
						// Line the wire up straight when there's room, otherwise go below the previous band.
						const float Straight = PinY(Node, Link.ParentPin) - PinY(Link.Node, Link.ChildPin);
						const float Offset = FMath::Max(Straight, NextTop - Child.Top);
						RelY.Add(Link.Node, Offset);
						if (bPure)
						{
							RelX.Add(Link.Node, -PureGapX - Sizes[Link.Node].X);
						}
						NextTop = Offset + Child.Bottom + Options.GapY;
						Extent.Top = FMath::Min(Extent.Top, Offset + Child.Top);
						Extent.Bottom = FMath::Max(Extent.Bottom, Offset + Child.Bottom);
					}
				};
				const float Free = TNumericLimits<float>::Lowest();
				Stack(ChildrenOf(PureChildren, Node), IsExec(Node) ? PureDropY : Free, true);
				Stack(ChildrenOf(TreeChildren, Node), Free, false);
				return Extent;
			}

			/** How far this node's pure inputs reach to its left. */
			float LeftReach(UEdGraphNode* Node, float X = 0.f) const
			{
				float Reach = 0.f;
				for (const FLink& Link : ChildrenOf(PureChildren, Node))
				{
					const float ChildX = X + RelX[Link.Node];
					Reach = FMath::Max(Reach, FMath::Max(-ChildX, LeftReach(Link.Node, ChildX)));
				}
				return Reach;
			}

			/** Longest path in pixels: every exec node sits right of all its exec parents plus room for its own pure inputs. */
			void ComputeSpineX()
			{
				TMap<UEdGraphNode*, float> Reach;
				TMap<UEdGraphNode*, int32> InDegree;
				for (UEdGraphNode* Node : ExecNodes)
				{
					Reach.Add(Node, LeftReach(Node));
					InDegree.Add(Node, 0);
				}
				for (UEdGraphNode* Node : PureRoots)
				{
					SpineX.Add(Node, LeftReach(Node));
				}

				auto ForEachForwardEdge = [this](UEdGraphNode* Node, auto&& Callback)
				{
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin->Direction == EGPD_Output && IsExecPin(Pin))
						{
							for (const UEdGraphPin* Linked : Pin->LinkedTo)
							{
								UEdGraphNode* Child = Linked->GetOwningNode();
								if (InRange.Contains(Child) && !BackEdges.Contains({ Node, Child }))
								{
									Callback(Child);
								}
							}
						}
					}
				};

				for (UEdGraphNode* Node : ExecNodes)
				{
					ForEachForwardEdge(Node, [&InDegree](UEdGraphNode* Child) { ++InDegree[Child]; });
				}
				TArray<UEdGraphNode*> Ready;
				for (UEdGraphNode* Node : ExecNodes)
				{
					if (InDegree[Node] == 0)
					{
						Ready.Add(Node);
						SpineX.Add(Node, Reach[Node]);
					}
				}
				while (!Ready.IsEmpty())
				{
					UEdGraphNode* Node = Ready.Pop();
					const float Right = SpineX[Node] + Sizes[Node].X + Options.ExecGapX;
					ForEachForwardEdge(Node, [&](UEdGraphNode* Child)
					{
						float& ChildX = SpineX.FindOrAdd(Child, 0.f);
						ChildX = FMath::Max(ChildX, Right + Reach[Child]);
						if (--InDegree[Child] == 0)
						{
							Ready.Add(Child);
						}
					});
				}
			}

			/**
			 * A custom event wired to a Bind Event node goes right below the chain that binds it, instead of
			 * wherever it happened to sit. Bound events nest: an event bound inside a bound event's chain follows that chain.
			 */
			void PutBoundEventsAfterBinders(TArray<UEdGraphNode*>& Roots) const
			{
				TMap<UEdGraphNode*, UEdGraphNode*> RootOf;
				for (UEdGraphNode* Root : Roots)
				{
					MarkChain(Root, Root, RootOf);
				}

				TMap<UEdGraphNode*, TArray<UEdGraphNode*>> BoundBy;  // binder's chain root -> event roots it binds
				TSet<UEdGraphNode*> Bound;
				for (UEdGraphNode* Root : Roots)
				{
					for (const UEdGraphPin* Pin : Root->Pins)
					{
						if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
						{
							continue;
						}
						for (const UEdGraphPin* Linked : Pin->LinkedTo)
						{
							UEdGraphNode* const* Binder = RootOf.Find(Linked->GetOwningNode());
							if (Binder && *Binder != Root && !Bound.Contains(Root))
							{
								BoundBy.FindOrAdd(*Binder).Add(Root);
								Bound.Add(Root);
							}
						}
					}
				}
				if (Bound.IsEmpty())
				{
					return;
				}

				TArray<UEdGraphNode*> Ordered;
				TSet<UEdGraphNode*> Emitted;
				TFunction<void(UEdGraphNode*)> Emit = [&](UEdGraphNode* Root)
				{
					if (!Emitted.Contains(Root))
					{
						Emitted.Add(Root);
						Ordered.Add(Root);
						for (UEdGraphNode* Event : BoundBy.FindRef(Root))
						{
							Emit(Event);
						}
					}
				};
				for (UEdGraphNode* Root : Roots)
				{
					if (!Bound.Contains(Root))
					{
						Emit(Root);
					}
				}
				for (UEdGraphNode* Root : Roots)
				{
					Emit(Root);  // events that only bind each other
				}
				Roots = MoveTemp(Ordered);
			}

			void MarkChain(UEdGraphNode* Node, UEdGraphNode* Root, TMap<UEdGraphNode*, UEdGraphNode*>& RootOf) const
			{
				RootOf.Add(Node, Root);
				for (const FLink& Link : ChildrenOf(PureChildren, Node))
				{
					MarkChain(Link.Node, Root, RootOf);
				}
				for (const FLink& Link : ChildrenOf(TreeChildren, Node))
				{
					MarkChain(Link.Node, Root, RootOf);
				}
			}

			/** Places a node and its subtree, appending them to the chain in exec order (each node followed by its pure inputs). */
			void Place(UEdGraphNode* Node, float X, float Y, TArray<UEdGraphNode*>& ChainNodes)
			{
				Positions.Add(Node, FVector2f(X, Y));
				ChainNodes.Add(Node);
				for (const FLink& Link : ChildrenOf(PureChildren, Node))
				{
					Place(Link.Node, X + RelX[Link.Node], Y + RelY[Link.Node], ChainNodes);
				}
				for (const FLink& Link : ChildrenOf(TreeChildren, Node))
				{
					Place(Link.Node, SpineX[Link.Node], Y + RelY[Link.Node], ChainNodes);
				}
			}

			FBox2f BoundsOf(const TArray<UEdGraphNode*>& ChainNodes) const
			{
				FBox2f Box(ForceInit);
				for (UEdGraphNode* Node : ChainNodes)
				{
					Box += Positions[Node];
					Box += Positions[Node] + Sizes[Node];
				}
				return Box;
			}

			const TArray<UEdGraphNode*>& Nodes;
			const TMap<UEdGraphNode*, FVector2f>& Sizes;
			const TMap<UEdGraphNode*, TSharedPtr<SGraphNode>>& Widgets;
			const FLayoutOptions Options;
			TArray<FChain> Chains;

			TSet<UEdGraphNode*> InRange;
			TSet<UEdGraphNode*> ExecNodes;
			TSet<UEdGraphNode*> Visited;
			TSet<UEdGraphNode*> OnStack;
			TSet<UEdGraphNode*> Owned;
			TSet<TPair<UEdGraphNode*, UEdGraphNode*>> BackEdges;
			TArray<UEdGraphNode*> ExecRoots;
			TArray<UEdGraphNode*> PureRoots;
			TArray<UEdGraphNode*> ExecOrder;
			TMap<UEdGraphNode*, TArray<FLink>> TreeChildren;
			TMap<UEdGraphNode*, TArray<FLink>> PureChildren;
			TMap<UEdGraphNode*, float> RelX;
			TMap<UEdGraphNode*, float> RelY;
			TMap<UEdGraphNode*, float> SpineX;
			TMap<UEdGraphNode*, FVector2f> Positions;
		};
	}

	FVector2f EstimateNodeSize(const UEdGraphNode* Node)
	{
		if (Node->IsA<UK2Node_Knot>())
		{
			return FVector2f(42.f, 16.f);
		}
		// Rough 1:1 Slate metrics. Erring wide only loosens spacing; erring narrow causes overlaps.
		auto TextWidth = [](const FString& Text) { return 7.f * FMath::Min(Text.Len(), 48); };

		float Width = TextWidth(Node->GetNodeTitle(ENodeTitleType::ListView).ToString()) + 64.f;
		TArray<float> InputRows;
		TArray<float> OutputRows;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->bHidden)
			{
				continue;
			}
			float Row = 40.f + TextWidth(Node->GetPinDisplayName(Pin).ToString());
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.IsEmpty() && !IsExecPin(Pin))
			{
				Row += 24.f + TextWidth(Pin->GetDefaultAsString());  // inline value box
			}
			(Pin->Direction == EGPD_Input ? InputRows : OutputRows).Add(Row);
		}
		const int32 Rows = FMath::Max(1, FMath::Max(InputRows.Num(), OutputRows.Num()));
		for (int32 Index = 0; Index < Rows; ++Index)
		{
			const float Left = InputRows.IsValidIndex(Index) ? InputRows[Index] : 0.f;
			const float Right = OutputRows.IsValidIndex(Index) ? OutputRows[Index] : 0.f;
			Width = FMath::Max(Width, Left + Right + 24.f);
		}
		return FVector2f(FMath::Max(Width, 120.f), 40.f + 26.f * Rows);
	}

	bool CanBeautify(const UEdGraph* Graph)
	{
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		if (!Schema)
		{
			return false;
		}
		if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema))
		{
			if (FBlueprintEditorUtils::IsGraphReadOnly(const_cast<UEdGraph*>(Graph)))
			{
				return false;
			}
			const EGraphType Type = K2Schema->GetGraphType(Graph);
			return Type == GT_Ubergraph || Type == GT_Function || Type == GT_Macro;
		}
		// Other graph types only when listed in settings. An entry whose plugin isn't loaded resolves to null and is skipped.
		return Graph->bEditable && GetDefault<UBeautifySettings>()->OtherGraphSchemas.ContainsByPredicate([Schema](const TSoftClassPtr<UEdGraphSchema>& Listed)
		{
			const UClass* Class = Listed.Get();
			return Class && Schema->IsA(Class);
		});
	}

	int32 NumSelectedNodes(const UEdGraph* Graph)
	{
		const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
		return GraphEditor ? GraphEditor->GetSelectedNodes().Num() : 0;
	}

	namespace
	{
		bool KeepChainsInPlace(const UBeautifySettings& Settings, const TArray<UEdGraphNode*>& Nodes)
		{
			if (Settings.ChainPlacement != EBeautifyChainPlacement::Auto)
			{
				return Settings.ChainPlacement == EBeautifyChainPlacement::KeepInPlace;
			}
			// ponytail: "generated" means more than half the nodes share one Y, which is what position-less
			// MCP tools produce. A hand-made graph that happens to be one long line also gets stacked; harmless.
			TMap<int32, int32> CountByY;
			int32 Most = 0;
			for (const UEdGraphNode* Node : Nodes)
			{
				Most = FMath::Max(Most, ++CountByY.FindOrAdd(Node->NodePosY));
			}
			return Most * 2 <= Nodes.Num();
		}
	}

	int32 Beautify(UEdGraph* Graph, bool bSelectionOnly, const TMap<FString, FString>* CommentTitles)
	{
		if (!CanBeautify(Graph))
		{
			return 0;
		}

		// No editor means a script called us on a closed graph: sizes and pin rows fall back to estimates.
		const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
		SGraphPanel* Panel = GraphEditor ? GraphEditor->GetGraphPanel() : nullptr;

		TArray<UEdGraphNode*> Nodes;
		if (bSelectionOnly)
		{
			if (!GraphEditor)
			{
				return 0;
			}
			for (UObject* Object : GraphEditor->GetSelectedNodes())
			{
				UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
				if (Node && Node->GetGraph() == Graph && !Node->IsA<UEdGraphNode_Comment>())
				{
					Nodes.Add(Node);
				}
			}
		}
		else
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !Node->IsA<UEdGraphNode_Comment>())
				{
					Nodes.Add(Node);
				}
			}
		}
		if (Nodes.IsEmpty())
		{
			return 0;
		}

		// At 25% zoom and below nodes draw without pin names or value boxes, so they measure too small.
		// Measure at 1:1 instead and put the view back; it all happens inside this frame, so nothing flickers.
		const bool bLowDetail = Panel && Panel->GetCurrentLOD() <= EGraphRenderingLOD::LowDetail;
		FVector2f ViewLocation;
		float ViewZoom = 1.f;
		if (bLowDetail)
		{
			GraphEditor->GetViewLocation(ViewLocation, ViewZoom);
			GraphEditor->SetViewLocation(ViewLocation, 1.f);
			Panel->SlatePrepass(1.f);
		}

		// Widgets that haven't been laid out yet report zero size; those nodes get estimates.
		TMap<UEdGraphNode*, FVector2f> Sizes;
		TMap<UEdGraphNode*, TSharedPtr<SGraphNode>> Widgets;
		int32 NumEstimated = 0;
		for (UEdGraphNode* Node : Nodes)
		{
			FSlateRect Rect;
			const bool bMeasured = GraphEditor && GraphEditor->GetBoundsForNode(Node, Rect, 0.f) && Rect.Right > Rect.Left;
			Sizes.Add(Node, bMeasured ? FVector2f(Rect.Right - Rect.Left, Rect.Bottom - Rect.Top) : EstimateNodeSize(Node));
			NumEstimated += bMeasured ? 0 : 1;
		}
		// Pin positions are cached when pins are drawn; while zoomed out they come from the low-detail drawing.
		// All graph nodes, so an unselected node feeding the selection can be measured too.
		if (Panel && !bLowDetail)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (const TSharedPtr<SGraphNode> Widget = Panel->GetNodeWidgetFromGuid(Node->NodeGuid))
				{
					Widgets.Add(Node, Widget);
				}
			}
		}
		if (bLowDetail)
		{
			GraphEditor->SetViewLocation(ViewLocation, ViewZoom);
		}
		UE_LOG(LogBeautifyBP, Display, TEXT("%s: %d nodes, %d sizes measured by Slate, %d estimated%s."),
			*Graph->GetName(), Nodes.Num(), Nodes.Num() - NumEstimated, NumEstimated, bLowDetail ? TEXT(" (view was zoomed out; measured at 1:1)") : TEXT(""));

		// Comments only change with the whole graph; a selection leaves every comment alone.
		const UBeautifySettings& Settings = *GetDefault<UBeautifySettings>();
		const bool bComments = !bSelectionOnly;
		const TArray<FUserComment> UserComments = bComments ? FindUserComments(*Graph, Sizes) : TArray<FUserComment>();

		FLayoutOptions Options;
		Options.ExecGapX = Settings.HorizontalSpacing;
		Options.GapY = Settings.VerticalSpacing;
		Options.ChainGap = Settings.ChainSpacing;
		Options.CommentReserve = bComments && Settings.bAddCommentBoxes ? CommentPad + CommentTitleHeight : 0.f;
		Options.bKeepChainsInPlace = !bSelectionOnly && KeepChainsInPlace(Settings, Nodes);

		// Stacked layouts keep the top-left corner of the nodes they move.
		FVector2f Anchor(MAX_flt, MAX_flt);
		for (const UEdGraphNode* Node : Nodes)
		{
			Anchor = FVector2f::Min(Anchor, Node->GetPosition());
		}

		FLayout Layout(Nodes, Sizes, Widgets, Options);
		const TMap<UEdGraphNode*, FVector2f> Positions = Layout.Compute();

		FVector2f Origin(MAX_flt, MAX_flt);
		for (const TPair<UEdGraphNode*, FVector2f>& Pair : Positions)
		{
			Origin = FVector2f::Min(Origin, Pair.Value);
		}
		FVector2f Shift = Options.bKeepChainsInPlace
			? FVector2f::ZeroVector  // already in graph coordinates
			: FVector2f(FMath::GridSnap(Anchor.X, Grid) - Origin.X, FMath::GridSnap(Anchor.Y, Grid) - Origin.Y);
		if (bSelectionOnly)
		{
			// Keep the wire coming in from the rest of the graph straight; the selection's left edge stays put.
			if (const TOptional<float> ShiftY = Layout.ShiftForIncomingWire(Positions))
			{
				Shift.Y = *ShiftY;
			}
		}

		const FScopedTransaction Transaction(bSelectionOnly
			? LOCTEXT("BeautifySelectionTransaction", "Beautify Selection")
			: LOCTEXT("BeautifyGraphTransaction", "Beautify Graph"));
		for (const TPair<UEdGraphNode*, FVector2f>& Pair : Positions)
		{
			const FVector2f Position = Pair.Value + Shift;
			Pair.Key->Modify();
			// X on the grid; Y keeps the exact offset that makes wires straight.
			Pair.Key->SetPosition(FVector2f(FMath::GridSnap(Position.X, Grid), FMath::RoundToFloat(Position.Y)));
		}
		if (bComments)
		{
			UpdateComments(*Graph, Layout.GetChains(), UserComments, Sizes, CommentTitles);
		}
		// Inside the transaction, so editors that copy positions elsewhere on a graph change (SimFlow's asset) undo with us.
		Graph->NotifyGraphChanged();
		if (!bSelectionOnly && Settings.bIncludeCollapsedGraphs)
		{
			// Collapsed graphs belong to this one, so they get tidied too, in this same undo step.
			// Macros are shared with other graphs and stay as they are; math expression graphs are read-only and skip themselves.
			for (UEdGraphNode* Node : Nodes)
			{
				const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
				if (Composite && Composite->BoundGraph)
				{
					Beautify(Composite->BoundGraph, false, CommentTitles);
				}
			}
		}
		return Positions.Num();
	}
}

#undef LOCTEXT_NAMESPACE
