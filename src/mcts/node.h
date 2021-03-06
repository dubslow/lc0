/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include "chess/board.h"
#include "chess/callbacks.h"
#include "chess/position.h"
#include "neural/writer.h"
#include "utils/mutex.h"

namespace lczero {

// Children of a node are stored the following way:
// * Edges and Nodes edges point to are stored separately.
// * There may be dangling edges (which don't yet point to any Node object yet)
// * Edges are stored are a simple array on heap.
// * Nodes are stored as a linked list, and contain index_ field which shows
//   which edge of a parent that node points to.
//
// Example:
//                                Parent Node
//                                    |
//        +-------------+-------------+----------------+--------------+
//        |              |            |                |              |
//   Edge 0(Nf3)    Edge 1(Bc5)     Edge 2(a4)     Edge 3(Qxf7)    Edge 4(a3)
//    (dangling)         |           (dangling)        |           (dangling)
//                   Node, Q=0.5                    Node, Q=-0.2
//
//  Is represented as:
// +--------------+
// | Parent Node  |
// +--------------+                                        +--------+
// | edges_       | -------------------------------------> | Edge[] |
// |              |    +------------+                      +--------+
// | child_       | -> | Node       |                      | Nf3    |
// +--------------+    +------------+                      | Bc5    |
//                     | index_ = 1 |                      | a4     |
//                     | q_ = 0.5   |    +------------+    | Qxf7   |
//                     | sibling_   | -> | Node       |    | a3     |
//                     +------------+    +------------+    +--------+
//                                       | index_ = 3 |
//                                       | q_ = -0.2  |
//                                       | sibling_   | -> nullptr
//                                       +------------+

class Node;
class Edge {
 public:
  // Returns move from the point of view of the player making it (if as_opponent
  // is false) or as opponent (if as_opponent is true).
  Move GetMove(bool as_opponent = false) const;

  // Returns value of Move probability returned from the neural net
  // (but can be changed by adding Dirichlet noise).
  float GetP() const { return p_; }

  // Sets move probability.
  void SetP(float val) { p_ = val; }

  // Debug information about the edge.
  std::string DebugString() const;

 private:
  void SetMove(Move move) { move_ = move; }

  // Move corresponding to this node. From the point of view of a player,
  // i.e. black's e7e5 is stored as e2e4.
  // Root node contains move a1a1.
  Move move_;

  // Probability that this move will be made. From policy head of the neural
  // network.
  float p_ = 0.0;

  friend class EdgeList;
};

// Array of Edges.
class EdgeList {
 public:
  EdgeList() {}
  EdgeList(MoveList moves);
  Edge* get() const { return edges_.get(); }
  Edge& operator[](size_t idx) const { return edges_[idx]; }
  operator bool() const { return static_cast<bool>(edges_); }
  uint16_t size() const { return size_; }

 private:
  std::unique_ptr<Edge[]> edges_;
  uint16_t size_ = 0;
};

class EdgeAndNode;
template <bool is_const>
class Edge_Iterator;

class Node {
 public:
  using Iterator = Edge_Iterator<false>;
  using ConstIterator = Edge_Iterator<true>;

  // Takes pointer to a parent node and own index in a parent.
  Node(Node* parent, uint16_t index) : index_(index), parent_(parent) {}

  // Allocates a new edge and a new node. The node has to be no edges before
  // that.
  Node* CreateSingleChildNode(Move m);

  // Creates edges from a movelist. There has to be no edges before that.
  void CreateEdges(const MoveList& moves);

  // Gets parent node.
  Node* GetParent() const { return parent_; }

  // Returns whether a node has children.
  bool HasChildren() const { return edges_; }

  // Returns sum of policy priors which have had at least one playout.
  float GetVisitedPolicy() const;
  uint32_t GetN() const { return n_; }
  uint32_t GetNInFlight() const { return n_in_flight_; }
  uint32_t GetChildrenVisits() const { return n_ > 0 ? n_ - 1 : 0; }
  // Returns n = n_if_flight.
  int GetNStarted() const { return n_ + n_in_flight_; }
  // Returns node eval, i.e. average subtree V for non-terminal node and -1/0/1
  // for terminal nodes.
  float GetQ() const { return q_; }

  // Returns whether the node is known to be draw/lose/win.
  bool IsTerminal() const { return is_terminal_; }
  uint16_t GetFullDepth() const { return full_depth_; }
  uint16_t GetMaxDepth() const { return max_depth_; }
  uint16_t GetNumEdges() const { return edges_.size(); }

  // Makes the node terminal and sets it's score.
  void MakeTerminal(GameResult result);

  // If this node is not in the process of being expanded by another thread
  // (which can happen only if n==0 and n-in-flight==1), mark the node as
  // "being updated" by incrementing n-in-flight, and return true.
  // Otherwise return false.
  bool TryStartScoreUpdate();
  // Decrements n-in-flight back.
  void CancelScoreUpdate();
  // Updates the node with newly computed value v.
  // Updates:
  // * Q (weighted average of all V in a subtree)
  // * N (+=1)
  // * N-in-flight (-=1)
  void FinalizeScoreUpdate(float v, float gamma, float beta);

  // Updates max depth, if new depth is larger.
  void UpdateMaxDepth(int depth);

  // Calculates the full depth if new depth is larger, updates it, returns
  // in depth parameter, and returns true if it was indeed updated.
  bool UpdateFullDepth(uint16_t* depth);

  V3TrainingData GetV3TrainingData(GameResult result,
                                   const PositionHistory& history) const;

  // Returns range for iterating over edges.
  ConstIterator Edges() const;
  Iterator Edges();

  class NodeRange;
  // Returns range for iterating over nodes. Note that there may be edges
  // without nodes, which will be skipped by this iteration.
  NodeRange ChildNodes() const;

  // Deletes all children.
  void ReleaseChildren();

  // Deletes all children except one.
  void ReleaseChildrenExceptOne(Node* node);

  // For a child node, returns corresponding edge.
  Edge* GetEdgeToNode(const Node* node) const;

  // Debug information about the node.
  std::string DebugString() const;

 private:
  // List of edges.
  EdgeList edges_;
  // Index of this node is parent's edge list.
  uint16_t index_;
  // Average value (from value head of neural network) of all visited nodes in
  // subtree. For terminal nodes, eval is stored.
  float q_ = 0.0f;
  // How many completed visits this node had.
  uint32_t n_ = 0;
  // (aka virtual loss). How many threads currently process this node (started
  // but not finished). This value is added to n during selection which node
  // to pick in MCTS, and also when selecting the best move.
  uint16_t n_in_flight_ = 0;

  // Maximum depth any subnodes of this node were looked at.
  uint16_t max_depth_ = 0;
  // Complete depth all subnodes of this node were fully searched.
  uint16_t full_depth_ = 0;
  // Does this node end game (with a winning of either sides or draw).
  bool is_terminal_ = false;

  // Pointer to a parent node. nullptr for the root.
  Node* parent_ = nullptr;
  // Pointer to a first child. nullptr for a leaf node.
  std::unique_ptr<Node> child_;
  // Pointer to a next sibling. nullptr if there are no further siblings.
  std::unique_ptr<Node> sibling_;

  // TODO(mooskagh) Unfriend NodeTree.
  friend class NodeTree;
  friend class Edge_Iterator<true>;
  friend class Edge_Iterator<false>;
  friend class Node_Iterator;
  friend class Edge;
};

// Contains Edge and Node pair and set of proxy functions to simplify access
// to them.
class EdgeAndNode {
 public:
  EdgeAndNode() = default;
  EdgeAndNode(Edge* edge, Node* node) : edge_(edge), node_(node) {}
  explicit operator bool() const { return edge_ != nullptr; }
  bool operator==(const EdgeAndNode& other) const { return edge_ == other.edge_; }
  bool operator!=(const EdgeAndNode& other) const { return edge_ != other.edge_; }
  bool HasNode() const { return node_ != nullptr; }
  Edge* edge() const { return edge_; }
  Node* node() const { return node_; }

  // Proxy functions for easier access to node/edge.
  float GetQ(float default_q) const {
    return (node_ && node_->GetN() > 0) ? node_->GetQ() : default_q;
  }
  // N-related getters, from Node (if exists).
  uint32_t GetN() const { return node_ ? node_->GetN() : 0; }
  int GetNStarted() const { return node_ ? node_->GetNStarted() : 0; }
  uint32_t GetNInFlight() const { return node_ ? node_->GetNInFlight() : 0; }

  // Whether the node is known to be terminal.
  bool IsTerminal() const { return node_ ? node_->IsTerminal() : false; }

  // Edge related getters.
  float GetP() const { return edge_->GetP(); }
  Move GetMove(bool flip = false) const { return edge_->GetMove(flip); }

  // Returns U = numerator * p / N.
  // Passed numerator is expected to be equal to (cpuct * sqrt(N[parent])).
  float GetU(float numerator) const {
    return numerator * GetP() / (1 + GetNStarted());
  }

  std::string DebugString() const;

 protected:
  // nullptr means that the whole pair is "null". (E.g. when search for a node
  // didn't find anything, or as end iterator signal).
  Edge* edge_ = nullptr;
  // nullptr means that the edge doesn't yet have node extended.
  Node* node_ = nullptr;
};

// TODO(crem) Replace this with less hacky iterator once we support C++17.
// This class has multiple hypostases within one class:
// * Range (begin() and end() functions)
// * Iterator (operator++() and operator*())
// * Element, pointed by iterator (EdgeAndNode class mainly, but Edge_Iterator
//   is useful too when client wants to call GetOrSpawnNode).
//   It's safe to slice EdgeAndNode off Edge_Iterator.
// It's more customary to have those as three classes, but
// creating zoo of classes and copying them around while iterating seems
// excessive.
//
// All functions are not thread safe (must be externally synchronized), but
// it's fine if Node/Edges state change between calls to functions of the
// iterator (e.g. advancing the iterator).
template <bool is_const>
class Edge_Iterator : public EdgeAndNode {
 public:
  using Ptr = std::conditional_t<is_const, const std::unique_ptr<Node>*,
                                 std::unique_ptr<Node>*>;

  // Creates "end()" iterator.
  Edge_Iterator() {}

  // Creates "begin()" iterator. Also happens to be a range constructor.
  Edge_Iterator(const EdgeList& edges, Ptr node_ptr)
      : EdgeAndNode(edges.size() ? edges.get() : nullptr, nullptr),
        node_ptr_(node_ptr),
        total_count_(edges.size()) {
    if (edge_) Actualize();
  }

  // Function to support range interface.
  Edge_Iterator<is_const> begin() { return *this; }
  Edge_Iterator<is_const> end() { return {}; }

  // Functions to support iterator interface.
  // Equality comparison operators are inherited from EdgeAndNode.
  void operator++() {
    // If it was the last edge in array, become end(), otherwise advance.
    if (++current_idx_ == total_count_) {
      edge_ = nullptr;
    } else {
      ++edge_;
      Actualize();
    }
  }
  Edge_Iterator& operator*() { return *this; }

  // If there is node, return it. Otherwise spawn a new one and return it.
  Node* GetOrSpawnNode(Node* parent) {
    if (node_) return node_;  // If there is already a node, return it.
    Actualize();              // But maybe other thread already did that.
    if (node_) return node_;  // If it did, return.
    // Now we are sure we have to create a new node.
    // Suppose there are nodes with idx 3 and 7, and we want to insert one with
    // idx 5. Here is how it looks like:
    //    node_ptr_ -> &Node(idx_.3).sibling_  ->  Node(idx_.7)
    // Here is how we do that:
    // 1. Store pointer to a node idx_.7:
    //    node_ptr_ -> &Node(idx_.3).sibling_  ->  nullptr
    //    tmp -> Node(idx_.7)
    std::unique_ptr<Node> tmp = std::move(*node_ptr_);
    // 2. Create fresh Node(idx_.5):
    //    node_ptr_ -> &Node(idx_.3).sibling_  ->  Node(idx_.5)
    //    tmp -> Node(idx_.7)
    *node_ptr_ = std::make_unique<Node>(parent, current_idx_);
    // 3. Attach stored pointer back to a list:
    //    node_ptr_ ->
    //         &Node(idx_.3).sibling_ -> Node(idx_.5).sibling_ -> Node(idx_.7)
    (*node_ptr_)->sibling_ = std::move(tmp);
    // 4. Actualize:
    //    node_ -> &Node(idx_.5)
    //    node_ptr_ -> &Node(idx_.5).sibling_ -> Node(idx_.7)
    Actualize();
    return node_;
  }

 private:
  void Actualize() {
    // If node_ptr_ is behind, advance it.
    // This is needed (and has to be 'while' rather than 'if') as other threads
    // could spawn new nodes between &node_ptr_ and *node_ptr_ while we didn't
    // see.
    while (*node_ptr_ && (*node_ptr_)->index_ < current_idx_) {
      node_ptr_ = &(*node_ptr_)->sibling_;
    }
    // If in the end node_ptr_ points to the node that we need, populate node_
    // and advance node_ptr_.
    if (*node_ptr_ && (*node_ptr_)->index_ == current_idx_) {
      node_ = (*node_ptr_).get();
      node_ptr_ = &node_->sibling_;
    } else {
      node_ = nullptr;
    }
  }

  // Pointer to a pointer to the next node. Has to be a pointer to pointer
  // as we'd like to update it when spawning a new node.
  Ptr node_ptr_;
  uint16_t current_idx_ = 0;
  uint16_t total_count_ = 0;
};

class Node_Iterator {
 public:
  Node_Iterator(Node* node) : node_(node) {}
  Node* operator*() { return node_; }
  Node* operator->() { return node_; }
  bool operator==(Node_Iterator& other) { return node_ == other.node_; }
  bool operator!=(Node_Iterator& other) { return node_ != other.node_; }
  void operator++() { node_ = node_->sibling_.get(); }

 private:
  Node* node_;
};

class Node::NodeRange {
 public:
  Node_Iterator begin() { return Node_Iterator(node_); }
  Node_Iterator end() { return Node_Iterator(nullptr); }

 private:
  NodeRange(Node* node) : node_(node) {}
  Node* node_;
  friend class Node;
};

class NodeTree {
 public:
  ~NodeTree() { DeallocateTree(); }
  // Adds a move to current_head_.
  void MakeMove(Move move);
  // Resets the current head to ensure it doesn't carry over details from a
  // previous search.
  void TrimTreeAtHead();
  // Sets the position in a tree, trying to reuse the tree.
  // If @auto_garbage_collect, old tree is garbage collected immediately. (may
  // take some milliseconds)
  void ResetToPosition(const std::string& starting_fen,
                       const std::vector<Move>& moves);
  const Position& HeadPosition() const { return history_.Last(); }
  int GetPlyCount() const { return HeadPosition().GetGamePly(); }
  bool IsBlackToMove() const { return HeadPosition().IsBlackToMove(); }
  Node* GetCurrentHead() const { return current_head_; }
  Node* GetGameBeginNode() const { return gamebegin_node_.get(); }
  const PositionHistory& GetPositionHistory() const { return history_; }

 private:
  void DeallocateTree();
  // A node which to start search from.
  Node* current_head_ = nullptr;
  // Root node of a game tree.
  std::unique_ptr<Node> gamebegin_node_;
  PositionHistory history_;
};

}  // namespace lczero
