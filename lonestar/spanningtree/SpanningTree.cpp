/** Spanning-tree application -*- C++ -*-
 * @file
 *
 * A simple spanning tree algorithm to demonstrate the Galois system.
 *
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Accumulator.h"
#include "Galois/Bag.h"
#include "Galois/Timer.h"
#include "Galois/UnionFind.h"
#include "Galois/Graphs/LCGraph.h"
#include "Galois/ParallelSTL.h"
#include "llvm/Support/CommandLine.h"

#include "Lonestar/BoilerPlate.h"

#include <utility>
#include <algorithm>
#include <iostream>

namespace cll = llvm::cl;

const char* name = "Spanning Tree Algorithm";
const char* desc = "Computes the spanning forest of a graph";
const char* url = NULL;

enum Algo {
  demo,
  asynchronous,
  blockedasync
};

static cll::opt<std::string> inputFilename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumVal(demo, "Demonstration algorithm"),
      clEnumVal(asynchronous, "Asynchronous"),
      clEnumVal(blockedasync, "Blocked Asynchronous"),
      clEnumValEnd), cll::init(blockedasync));

struct Node: public galois::UnionFindNode<Node> {
  Node(): galois::UnionFindNode<Node>(const_cast<Node*>(this)) { }
  Node* component() { return find(); }
  void setComponent(Node* n) { m_component = n; }
};

typedef galois::graphs::LC_Linear_Graph<Node,void>
  ::with_numa_alloc<true>::type Graph;

typedef Graph::GraphNode GNode;

Graph graph;

std::ostream& operator<<(std::ostream& os, const Node& n) {
  os << "[id: " << &n << "]";
  return os;
}

typedef std::pair<GNode,GNode> Edge;

galois::InsertBag<Edge> mst;

/** 
 * Construct a spanning forest via a modified BFS algorithm. Intended as a
 * simple introduction to the Galois system and not intended to particularly
 * fast. Restrictions: graph must be strongly connected. In this case, the
 * spanning tree is over the undirected graph created by making the directed
 * graph symmetric.
 */
struct DemoAlgo {
  Node* root;

  void operator()(GNode src, galois::UserContext<GNode>& ctx) {
    for (auto ii : graph.edges(src, galois::MethodFlag::WRITE)) {
      GNode dst = graph.getEdgeDst(ii);
      Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
      if (ddata.component() == root)
        continue;
      ddata.setComponent(root);
      mst.push(std::make_pair(src, dst));
      ctx.push(dst);
    }
  }

  void operator()() {
    Graph::iterator ii = graph.begin(), ei = graph.end();
    if (ii != ei) {
      root = &graph.getData(*ii);
      galois::for_each(*ii, *this);
    }
  }
};

/**
 * Like asynchronous connected components algorithm. 
 */
struct AsyncAlgo {
  struct Merge {
    galois::Statistic& emptyMerges;
    Merge(galois::Statistic& e): emptyMerges(e) { }

    void operator()(const GNode& src) const {
      Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
      for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
        if (sdata.merge(&ddata)) {
          mst.push(std::make_pair(src, dst));
        } else {
          emptyMerges += 1;
        }
      }
    }
  };

  //! Normalize component by doing find with path compression
  struct Normalize {
    void operator()(const GNode& src) const {
      Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
      sdata.setComponent(sdata.findAndCompress());
    }
  };

  void operator()() {
    galois::Statistic emptyMerges("EmptyMerges");
    galois::do_all_local(graph, Merge(emptyMerges),
        galois::loopname("Merge"), galois::do_all_steal<true>());
    galois::do_all_local(graph, Normalize(), galois::loopname("Normalize"));
  }
};

/**
 * Improve performance of async algorithm by following machine topology.
 */
struct BlockedAsyncAlgo {
  struct WorkItem {
    GNode src;
    Graph::edge_iterator start;
  };

  struct Merge {
    typedef int tt_does_not_need_aborts;

    galois::InsertBag<WorkItem>& items;

    //! Add the next edge between components to the worklist
    template<bool MakeContinuation, int Limit, typename Pusher>
    void process(const GNode& src, const Graph::edge_iterator& start, Pusher& pusher) const {
      Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
      int count = 0;
      for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
        ++count;
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
        if (sdata.merge(&ddata)) {
          mst.push(std::make_pair(src, dst));
          if (Limit == 0 || count != Limit)
            continue;
        }

        if (MakeContinuation || (Limit != 0 && count == Limit)) {
          WorkItem item = { src, ii + 1 };
          pusher.push(item);
          break;
        }
      }
    }

    void operator()(const GNode& src) const {
      Graph::edge_iterator start = graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
      if (galois::Substrate::ThreadPool::getPackage() == 0) {
        process<true, 0>(src, start, items);
      } else {
        process<true, 1>(src, start, items);
      }
    }

    void operator()(const WorkItem& item, galois::UserContext<WorkItem>& ctx) const {
      process<true, 0>(item.src, item.start, ctx);
    }
  };

  //! Normalize component by doing find with path compression
  struct Normalize {
    void operator()(const GNode& src) const {
      Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
      sdata.setComponent(sdata.findAndCompress());
    }
  };

  void operator()() {
    galois::InsertBag<WorkItem> items;
    Merge merge = { items };
    galois::do_all_local(graph, merge, galois::loopname("Initialize"));
    galois::for_each_local(items, merge,
        galois::loopname("Merge"), galois::wl<galois::worklists::dChunkedFIFO<128> >());
    galois::do_all_local(graph, Normalize(), galois::loopname("Normalize"));
  }
};

struct is_bad_graph {
  bool operator()(const GNode& n) const {
    Node& me = graph.getData(n);
    for (auto ii : graph.edges(n)) {
      GNode dst = graph.getEdgeDst(ii);
      Node& data = graph.getData(dst);
      if (me.component() != data.component()) {
        std::cerr << "not in same component: " << me << " and " << data << "\n";
        return true;
      }
    }
    return false;
  }
};

struct is_bad_mst {
  bool operator()(const Edge& e) const {
    return graph.getData(e.first).component() != graph.getData(e.second).component();
  }
};

struct CheckAcyclic {
  struct Accum {
    galois::GAccumulator<unsigned> roots;
  };

  Accum* accum;

  void operator()(const GNode& n) const {
    Node& data = graph.getData(n);
    if (data.component() == &data)
      accum->roots += 1;
  }

  bool operator()() {
    Accum a;
    accum = &a;
    galois::do_all_local(graph, *this);
    unsigned numRoots = a.roots.reduce();
    unsigned numEdges = std::distance(mst.begin(), mst.end());
    if (graph.size() - numRoots != numEdges) {
      std::cerr << "Generated graph is not a forest. "
        << "Expected " << graph.size() - numRoots << " edges but "
        << "found " << numEdges << "\n";
      return false;
    }

    std::cout << "Num trees: " << numRoots << "\n";
    std::cout << "Tree edges: " << numEdges << "\n";
    return true;
  }
};

bool verify() {
  if (galois::ParallelSTL::find_if(graph.begin(), graph.end(), is_bad_graph()) == graph.end()) {
    if (galois::ParallelSTL::find_if(mst.begin(), mst.end(), is_bad_mst()) == mst.end()) {
      CheckAcyclic c;
      return c();
    }
  }
  return false;
}

template<typename Algo>
void run() {
  Algo algo;

  galois::StatTimer T;
  T.start();
  algo();
  T.stop();
}

int main(int argc, char** argv) {
  galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  galois::StatTimer Tinitial("InitializeTime");
  Tinitial.start();
  galois::graphs::readGraph(graph, inputFilename);
  std::cout << "Num nodes: " << graph.size() << "\n";
  Tinitial.stop();

  //galois::preAlloc(numThreads + graph.size() / galois::runtime::MM::hugePageSize * 60);
  galois::reportPageAlloc("MeminfoPre");
  switch (algo) {
    case demo: run<DemoAlgo>(); break;
    case asynchronous: run<AsyncAlgo>(); break;
    case blockedasync: run<BlockedAsyncAlgo>(); break;
    default: std::cerr << "Unknown algo: " << algo << "\n";
  }
  galois::reportPageAlloc("MeminfoPost");

  if (!skipVerify && !verify()) {
    std::cerr << "verification failed\n";
    assert(0 && "verification failed");
    abort();
  }

  return 0;
}
