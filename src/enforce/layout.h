// Places freshly created nodes so a converted body reads left to right.
//
// Lowering produces the right nodes and wires, but all at the same point.
// Without this the "convert to nodes" action buries the graph in a pile.
#pragma once

#include "graph.h"

#include <QSet>

struct LayoutOptions {
    double originX = 0;
    double originY = 0;
    double columnGap = 60;   // between exec steps
    double rowGap = 24;      // between stacked data feeders
};

// Lays out `nodes` (by id) inside `graph`: exec chains run along x, the data
// feeding a node stacks to its left, and nothing overlaps anything already on
// the canvas.
void layoutNodes(Graph &graph, const QSet<QString> &nodeIds,
                 const LayoutOptions &opts = {});

// Lays out the whole graph. Used by a "tidy graph" action.
void layoutGraph(Graph &graph, const LayoutOptions &opts = {});
