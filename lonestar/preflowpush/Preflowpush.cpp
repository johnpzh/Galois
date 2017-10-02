/** Preflow-push application -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
#include "galois/Accumulator.h"
#include "galois/Bag.h"
#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "llvm/Support/CommandLine.h"

#include "Lonestar/BoilerPlate.h"

#include <boost/iterator/iterator_adaptor.hpp>

#include <fstream>
#include <iostream>

namespace cll = llvm::cl;

const char* name = "Preflow Push";
const char* desc =
    "Finds the maximum flow in a network using the preflow push technique";
const char* url = "preflow_push";

enum DetAlgo { nondet, detBase, detDisjoint };

static cll::opt<std::string> filename(cll::Positional,
                                      cll::desc("<input file>"), cll::Required);
static cll::opt<uint32_t> sourceId(cll::Positional, cll::desc("sourceID"),
                                   cll::Required);
static cll::opt<uint32_t> sinkId(cll::Positional, cll::desc("sinkID"),
                                 cll::Required);
static cll::opt<bool> useHLOrder("useHLOrder",
                                 cll::desc("Use HL ordering heuristic"),
                                 cll::init(false));
static cll::opt<bool>
    useUnitCapacity("useUnitCapacity",
                    cll::desc("Assume all capacities are unit"),
                    cll::init(false));
static cll::opt<bool> useSymmetricDirectly(
    "useSymmetricDirectly",
    cll::desc("Assume input graph is symmetric and has unit capacities"),
    cll::init(false));
static cll::opt<int>
    relabelInt("relabel",
               cll::desc("relabel interval: < 0 no relabeling, 0 use default "
                         "interval, > 0 relabel every X iterations"),
               cll::init(0));
static cll::opt<DetAlgo> detAlgo(
    cll::desc("Deterministic algorithm:"),
    cll::values(clEnumVal(nondet, "Non-deterministic"),
                clEnumVal(detBase, "Base execution"),
                clEnumVal(detDisjoint, "Disjoint execution"), clEnumValEnd),
    cll::init(nondet));

/**
 * Alpha parameter the original Goldberg algorithm to control when global
 * relabeling occurs. For comparison purposes, we keep them the same as
 * before, but it is possible to achieve much better performance by adjusting
 * the global relabel frequency.
 */
static const int ALPHA = 6;

/**
 * Beta parameter the original Goldberg algorithm to control when global
 * relabeling occurs. For comparison purposes, we keep them the same as
 * before, but it is possible to achieve much better performance by adjusting
 * the global relabel frequency.
 */
static const int BETA = 12;

struct Node {
  uint32_t id;
  int64_t excess;
  int height;
  int current;

  Node() : excess(0), height(1), current(0) {}
};

std::ostream& operator<<(std::ostream& os, const Node& n) {
  os << "("
     << "id: " << n.id << ", excess: " << n.excess << ", height: " << n.height
     << ", current: " << n.current << ")";
  return os;
}

using Graph = galois::graphs::LC_Linear_Graph<Node, int32_t>::with_numa_alloc<true>::type;
using GNode =  Graph::GraphNode;


struct PreflowPush {

  Graph graph;
  GNode sink;
  GNode source;
  int global_relabel_interval;
  bool should_global_relabel = false;;

  void checkSorting(void) {
    for (auto n : graph) {
      galois::optional<GNode> prevDst;
      for (auto e : graph.edges(n, galois::MethodFlag::UNPROTECTED)) {
        GNode dst = graph.getEdgeDst(e);
        if (prevDst) {
          Node& prevNode = graph.getData(*prevDst, galois::MethodFlag::UNPROTECTED);
          Node& currNode = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
          GALOIS_ASSERT(prevNode.id != currNode.id, "Adjacency list cannot have duplicates");
          GALOIS_ASSERT(prevNode.id <= currNode.id, "Adjacency list unsorted");
        }
        prevDst = dst;
      }
    }
  }

  void checkAugmentingPath() {
    // Use id field as visited flag
    for (Graph::iterator ii = graph.begin(), ee = graph.end(); ii != ee; ++ii) {
      GNode src             = *ii;
      graph.getData(src).id = 0;
    }

    std::deque<GNode> queue;

    graph.getData(source).id = 1;
    queue.push_back(source);

    while (!queue.empty()) {
      GNode& src = queue.front();
      queue.pop_front();
      for (auto ii : graph.edges(src)) {
        GNode dst = graph.getEdgeDst(ii);
        if (graph.getData(dst).id == 0 && graph.getEdgeData(ii) > 0) {
          graph.getData(dst).id = 1;
          queue.push_back(dst);
        }
      }
    }

    if (graph.getData(sink).id != 0) {
      assert(false && "Augmenting path exisits");
      abort();
    }
  }

  void checkHeights() {
    for (Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
      GNode src = *ii;
      int sh    = graph.getData(src).height;
      for (auto jj : graph.edges(src)) {
        GNode dst   = graph.getEdgeDst(jj);
        int64_t cap = graph.getEdgeData(jj);
        int dh      = graph.getData(dst).height;
        if (cap > 0 && sh > dh + 1) {
          std::cerr << "height violated at " << graph.getData(src) << "\n";
          abort();
        }
      }
    }
  }

  void checkConservation(PreflowPush& orig) {
    std::vector<GNode> map;
    map.resize(graph.size());

    // Setup ids assuming same iteration order in both graphs
    uint32_t id = 0;
    for (Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei;
        ++ii, ++id) {
      graph.getData(*ii).id = id;
    }
    id = 0;
    for (Graph::iterator ii = orig.graph.begin(), ei = orig.graph.end();
        ii != ei; ++ii, ++id) {
      orig.graph.getData(*ii).id = id;
      map[id]                    = *ii;
    }

    // Now do some checking
    for (Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
      GNode src        = *ii;
      const Node& node = graph.getData(src);
      uint32_t srcId   = node.id;

      if (src == source || src == sink)
        continue;

      if (node.excess != 0 && node.height != (int)graph.size()) {
        std::cerr << "Non-zero excess at " << node << "\n";
        abort();
      }

      int64_t sum = 0;
      for (auto jj : graph.edges(src)) {
        GNode dst      = graph.getEdgeDst(jj);
        uint32_t dstId = graph.getData(dst).id;
        int64_t ocap   = orig.graph.getEdgeData(
            orig.findEdge(map[srcId], map[dstId]));
        int64_t delta = 0;
        if (ocap > 0)
          delta -= (ocap - graph.getEdgeData(jj));
        else
          delta += graph.getEdgeData(jj);
        sum += delta;
      }

      if (node.excess != sum) {
        std::cerr << "Not pseudoflow: " << node.excess << " != " << sum
          << " at " << node << "\n";
        abort();
      }
    }
  }

  void verify(PreflowPush& orig) {
    // FIXME: doesn't fully check result
    checkHeights();
    checkConservation(orig);
    checkAugmentingPath();
  }

  void reduceCapacity(const Graph::edge_iterator& ii, const GNode& src, const GNode& dst, int64_t amount) {
    Graph::edge_data_type& cap1 = graph.getEdgeData(ii);
    Graph::edge_data_type& cap2 = graph.getEdgeData(findEdge(dst, src));
    cap1 -= amount;
    cap2 += amount;
  }

  Graph::edge_iterator findEdge(GNode src, GNode dst) {

    auto i     = graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
    auto end_i = graph.edge_end(src, galois::MethodFlag::UNPROTECTED);

    if ((end_i - i) < 32) {
      return findEdgeLinear(dst, i, end_i);

    } else {
      return findEdgeLog2(dst, i, end_i);
    }
  }

  Graph::edge_iterator findEdgeLinear(GNode dst,
      Graph::edge_iterator beg_e,
      Graph::edge_iterator end_e) {

    auto ii = beg_e;
    for (; ii != end_e; ++ii) {
      if (graph.getEdgeDst(ii) == dst)
        break;
    }
    assert(ii != end_e); // Never return the end iterator
    return ii;
  }

  Graph::edge_iterator findEdgeLog2(GNode dst, Graph::edge_iterator i, Graph::edge_iterator end_i) {

    struct EdgeDstIter
      : public boost::iterator_facade< EdgeDstIter, GNode, boost::random_access_traversal_tag, GNode> {

      using Base = boost::iterator_facade<EdgeDstIter, GNode, boost::random_access_traversal_tag, GNode>;

      Graph* g;
      Graph::edge_iterator ei;

      EdgeDstIter(void) : g(nullptr) {}

      EdgeDstIter(Graph* g, Graph::edge_iterator ei) : g(g), ei(ei) {}

      private:
      friend boost::iterator_core_access;

      GNode dereference(void) const { return g->getEdgeDst(ei); }

      void increment(void) { ++ei; }

      void decrement(void) { --ei; }

      bool equal(const EdgeDstIter& that) const {
        assert(this->g == that.g);
        return this->ei == that.ei;
      }

      void advance(ptrdiff_t n) { ei += n; }

      ptrdiff_t distance_to(const EdgeDstIter& that) const {
        assert(this->g == that.g);

        return that.ei - this->ei;
      }
    };

    EdgeDstIter ai(&graph, i);
    EdgeDstIter end_ai(&graph, end_i);

    auto ret = std::lower_bound(ai, end_ai, dst);

    assert(ret != end_ai);
    assert(*ret == dst);

    return ret.ei;
  }

  void acquire(const GNode& src) {
    // LC Graphs have a different idea of locking
    for (auto ii : graph.edges(src, galois::MethodFlag::WRITE)) {
      GNode dst = graph.getEdgeDst(ii);
      graph.getData(dst, galois::MethodFlag::WRITE);
    }
  }

  void relabel(const GNode& src) {
    int minHeight = std::numeric_limits<int>::max();
    int minEdge   = 0;

    int current = 0;
    for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
      GNode dst   = graph.getEdgeDst(ii);
      int64_t cap = graph.getEdgeData(ii);
      if (cap > 0) {
        const Node& dnode = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
        if (dnode.height < minHeight) {
          minHeight = dnode.height;
          minEdge   = current;
        }
      }
      ++current;
    }

    assert(minHeight != std::numeric_limits<int>::max());
    ++minHeight;

    Node& node = graph.getData(src, galois::MethodFlag::UNPROTECTED);
    if (minHeight < (int)graph.size()) {
      node.height  = minHeight;
      node.current = minEdge;
    } else {
      node.height = graph.size();
    }
  }

  
  template <typename C>
  bool discharge(const GNode& src, C& ctx) {
    // Node& node = graph.getData(src, galois::MethodFlag::WRITE);
    Node& node = graph.getData(src, galois::MethodFlag::UNPROTECTED);
    // int prevHeight = node.height;
    bool relabeled = false;

    if (node.excess == 0 || node.height >= (int)graph.size()) {
      return false;
    }

    while (true) {
      // galois::MethodFlag flag = relabeled ? galois::MethodFlag::UNPROTECTED :
      // galois::MethodFlag::WRITE;
      galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
      bool finished           = false;
      int current             = node.current;

      auto ii = graph.edge_begin(src, flag);
      auto ee = graph.edge_end(src, flag);

      std::advance(ii, node.current);

      for (; ii != ee; ++ii, ++current) {
        GNode dst   = graph.getEdgeDst(ii);
        int64_t cap = graph.getEdgeData(ii);
        if (cap == 0) // || current < node.current)
          continue;

        Node& dnode = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
        if (node.height - 1 != dnode.height)
          continue;

        // Push flow
        int64_t amount = std::min(node.excess, cap);
        reduceCapacity(ii, src, dst, amount);

        // Only add once
        if (dst != sink && dst != source && dnode.excess == 0)
          ctx.push(dst);

        assert(node.excess >= amount);
        node.excess -= amount;
        dnode.excess += amount;

        if (node.excess == 0) {
          finished     = true;
          node.current = current;
          break;
        }
      }

      if (finished)
        break;

      relabel(src);
      relabeled = true;

      if (node.height == (int)graph.size())
        break;

      // prevHeight = node.height;
    }

    return relabeled;
  }

  template <DetAlgo version, bool useCAS = true>
  struct UpdateHeights {
    PreflowPush& app;
    UpdateHeights(PreflowPush& a) : app(a) {}

    struct LocalState {
      LocalState(UpdateHeights<version, useCAS>& self,
          galois::PerIterAllocTy& alloc) {}
    };

    typedef std::tuple<galois::per_iter_alloc, galois::local_state<LocalState>>
      function_traits;

    // struct IdFn {
    //  unsigned long operator()(const GNode& item) const {
    //    return app.graph.getData(item, galois::MethodFlag::UNPROTECTED).id;
    //  }
    //};

    /**
     * Do reverse BFS on residual graph.
     */
    template <typename C>
    void operator()(const GNode& src, C& ctx) {
      if (version != nondet) {

        if (ctx.isFirstPass()) {
          for (auto ii : app.graph.edges(src, galois::MethodFlag::WRITE)) {
            GNode dst = app.graph.getEdgeDst(ii);
            int64_t rdata =
              app.graph.getEdgeData(app.findEdge(dst, src));
            if (rdata > 0) {
              app.graph.getData(dst, galois::MethodFlag::WRITE);
            }
          }
        }

        if (version == detDisjoint && ctx.isFirstPass()) {
          return;
        } else {
          app.graph.getData(src, galois::MethodFlag::WRITE);
          ctx.cautiousPoint();
        }
      }

      for (auto ii : app.graph.edges(src, useCAS ? galois::MethodFlag::UNPROTECTED
            : galois::MethodFlag::WRITE)) {
        GNode dst     = app.graph.getEdgeDst(ii);
        int64_t rdata = app.graph.getEdgeData(app.findEdge(dst, src));
        if (rdata > 0) {
          Node& node = app.graph.getData(dst, galois::MethodFlag::UNPROTECTED);
          int newHeight =
            app.graph.getData(src, galois::MethodFlag::UNPROTECTED).height + 1;
          if (useCAS) {
            int oldHeight;
            while (newHeight < (oldHeight = node.height)) {
              if (__sync_bool_compare_and_swap(&node.height, oldHeight,
                    newHeight)) {
                ctx.push(dst);
                break;
              }
            }
          } else {
            if (newHeight < node.height) {
              node.height = newHeight;
              ctx.push(dst);
            }
          }
        }
      } // end for
    } // end operator


  };


  template <typename IncomingWL>
  void globalRelabel(IncomingWL& incoming) {
    typedef galois::worklists::Deterministic<> DWL;

    galois::StatTimer T1("ResetHeightsTime");
    T1.start();
    galois::do_all(galois::iterate(graph),
        [&] (const GNode& src) {
          Node& node   = graph.getData(src, galois::MethodFlag::UNPROTECTED);
          node.height  = graph.size();
          node.current = 0;
          if (src == sink)
            node.height = 0;
        },
        galois::loopname("ResetHeights"));
    T1.stop();

    galois::StatTimer T("UpdateHeightsTime");
    T.start();

    switch (detAlgo) {
      case nondet:
        galois::for_each(galois::iterate( { sink } ), UpdateHeights<nondet>(*this),
            galois::loopname("UpdateHeights"),
            galois::wl<galois::worklists::BulkSynchronous<>>());
        break;
      case detBase:
        galois::for_each(galois::iterate( { sink } ), UpdateHeights<detBase>(*this), 
            galois::wl<DWL>(),
            galois::loopname("UpdateHeights"));
        break;
      case detDisjoint:
        galois::for_each(galois::iterate( { sink } ), UpdateHeights<detDisjoint>(*this),
            galois::wl<DWL>(), 
            galois::loopname("UpdateHeights"));
        break;
      default:
        std::cerr << "Unknown algorithm" << detAlgo << "\n";
        abort();
    }
    T.stop();

    galois::StatTimer T2("FindWorkTime");
    T2.start();
    galois::do_all(galois::iterate(graph),
        [&incoming, this] (const GNode& src) {
          Node& node = this->graph.getData(src, galois::MethodFlag::UNPROTECTED);
          if (src == this->sink || src == this->source ||
              node.height >= (int)this->graph.size())
            return;
          if (node.excess > 0)
            incoming.push_back(src);
        },
        galois::loopname("FindWork"));
    T2.stop();
  }


  struct Counter {
    galois::GAccumulator<int> accum;
    galois::substrate::PerThreadStorage<int> local;
  };

  template <DetAlgo version>
    struct Process {

      Counter& counter;
      PreflowPush& app;

      struct LocalState {
        LocalState(Process<version>& self, galois::PerIterAllocTy& alloc) {}
      };

      struct DeterministicId {
        Process<version>* self;
        uint32_t operator()(const GNode& item) const {
          return self->app.graph.getData(item, galois::MethodFlag::UNPROTECTED).id;
        }
      };

      DeterministicId getDetermisticId() { return DeterministicId{this}; }

      struct ParallelBreak {
        Process<version>* self;
        bool operator()() {
          if (self->app.global_relabel_interval > 0 &&
              self->counter.accum.reduce() >= self->app.global_relabel_interval) {
            self->app.should_global_relabel = true;
            return true;
          }
          return false;
        }
      };

      ParallelBreak getParallelBreak() { return ParallelBreak{this}; }

      Process(Counter& c, PreflowPush& a) : counter(c), app(a) {}

  template <typename C>
      void operator()(GNode& src, C& ctx) {
        if (version != nondet) {
          if (ctx.isFirstPass()) {
            app.acquire(src);
          }
          if (version == detDisjoint && ctx.isFirstPass()) {
            return;
          } else {
            app.graph.getData(src, galois::MethodFlag::WRITE);
            ctx.cautiousPoint();
          }
        }

        int increment = 1;
        if (app.discharge(src, ctx)) {
          increment += BETA;
        }

        counter.accum += increment;
      }
    };

  struct ProcessNonDet {
    typedef std::tuple<galois::parallel_break> function_traits;

    Counter& counter;
    PreflowPush& app;
    int limit;
    ProcessNonDet(Counter& c, PreflowPush& a) : counter(c), app(a) {
      limit = app.global_relabel_interval / numThreads;
    }

    template <typename C>
      void operator()(GNode& src, C& ctx) {
        int increment = 1;
        app.acquire(src);
        if (app.discharge(src, ctx)) {
          increment += BETA;
        }

        int v = *counter.local.getLocal() += increment;
        if (app.global_relabel_interval > 0 && v >= limit) {
          app.should_global_relabel = true;
          ctx.breakLoop();
          return;
        }
      }
  };

  template <typename EdgeTy>
  static void writePfpGraph(const std::string& inputFile,
        const std::string& outputFile) {
      typedef galois::graphs::FileGraph ReaderGraph;
      typedef ReaderGraph::GraphNode ReaderGNode;

      ReaderGraph reader;
      reader.fromFile(inputFile);

      typedef galois::graphs::FileGraphWriter Writer;
      typedef galois::LargeArray<EdgeTy> EdgeData;
      typedef typename EdgeData::value_type edge_value_type;

      Writer p;
      EdgeData edgeData;

      // Count edges
      size_t numEdges = 0;
      for (ReaderGraph::iterator ii = reader.begin(), ei = reader.end(); ii != ei;
          ++ii) {
        ReaderGNode rsrc = *ii;
        for (auto jj : reader.edges(rsrc)) {
          ReaderGNode rdst = reader.getEdgeDst(jj);
          if (rsrc == rdst)
            continue;
          if (!reader.hasNeighbor(rdst, rsrc))
            ++numEdges;
          ++numEdges;
        }
      }

      p.setNumNodes(reader.size());
      p.setNumEdges(numEdges);
      p.setSizeofEdgeData(sizeof(edge_value_type));

      p.phase1();
      for (ReaderGraph::iterator ii = reader.begin(), ei = reader.end(); ii != ei;
          ++ii) {
        ReaderGNode rsrc = *ii;
        for (auto jj : reader.edges(rsrc)) {
          ReaderGNode rdst = reader.getEdgeDst(jj);
          if (rsrc == rdst)
            continue;
          if (!reader.hasNeighbor(rdst, rsrc))
            p.incrementDegree(rdst);
          p.incrementDegree(rsrc);
        }
      }

      EdgeTy one = 1;
      static_assert(sizeof(one) == sizeof(uint32_t), "Unexpected edge data size");
      one = galois::convert_le32toh(one);

      p.phase2();
      edgeData.create(numEdges);
      for (ReaderGraph::iterator ii = reader.begin(), ei = reader.end(); ii != ei;
          ++ii) {
        ReaderGNode rsrc = *ii;
        for (auto jj : reader.edges(rsrc)) {
          ReaderGNode rdst = reader.getEdgeDst(jj);
          if (rsrc == rdst)
            continue;
          if (!reader.hasNeighbor(rdst, rsrc))
            edgeData.set(p.addNeighbor(rdst, rsrc), 0);
          EdgeTy cap = useUnitCapacity ? one : reader.getEdgeData<EdgeTy>(jj);
          edgeData.set(p.addNeighbor(rsrc, rdst), cap);
        }
      }

      edge_value_type* rawEdgeData = p.finish<edge_value_type>();
      std::uninitialized_copy(std::make_move_iterator(edgeData.begin()),
          std::make_move_iterator(edgeData.end()), rawEdgeData);

      using Wnode = Writer::GraphNode;

      struct IdLess {
        bool operator()(
            const galois::graphs::EdgeSortValue<Wnode, edge_value_type>& e1,
            const galois::graphs::EdgeSortValue<Wnode, edge_value_type>& e2) const {
          return e1.dst < e2.dst;
        }
      };

      for (Writer::iterator i = p.begin(), end_i = p.end(); i != end_i; ++i) {
        p.sortEdges<edge_value_type>(*i, IdLess());
      }

      p.toFile(outputFile);
    }

  void initializeGraph(std::string inputFile, uint32_t sourceId, uint32_t sinkId) {
    if (useSymmetricDirectly) {
      galois::graphs::readGraph(graph, inputFile);
      for (auto ss : graph)
        for (auto ii : graph.edges(ss))
          graph.getEdgeData(ii) = 1;
    } else {
      if (inputFile.find(".gr.pfp") != inputFile.size() - strlen(".gr.pfp")) {
        std::string pfpName = inputFile + ".pfp";
        std::ifstream pfpFile(pfpName.c_str());
        if (!pfpFile.good()) {
          std::cout << "Writing new input file: " << pfpName << "\n";
          writePfpGraph<Graph::edge_data_type>(inputFile, pfpName);
        }
        inputFile = pfpName;
      }
      galois::graphs::readGraph(graph, inputFile);

      // Assume that input edge data has already been converted instead
#if 0 // def HAVE_BIG_ENDIAN
      // Convert edge data to host ordering
      for (auto ss : newApp->graph) {
        for (auto ii : newApp->graph.edges(ss)) {
          Graph::edge_data_type& cap = newApp->graph.getEdgeData(ii);
          static_assert(sizeof(cap) == sizeof(uint32_t), "Unexpected edge data size");
          cap = galois::convert_le32toh(cap);
        }
      }
#endif
    }

    if (sourceId == sinkId || sourceId >= graph.size() || sinkId >= graph.size()) {
      std::cerr << "invalid source or sink id\n";
      abort();
    }

    uint32_t id = 0;
    for (Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++id) {
      if (id == sourceId) {
        source                   = *ii;
        graph.getData(source).height = graph.size();
      } else if (id == sinkId) {
        sink = *ii;
      }
      graph.getData(*ii).id = id;
    }
  }

  template <typename C>
  void initializePreflow(C& initial) {
    for (auto ii : graph.edges(source)) {
      GNode dst   = graph.getEdgeDst(ii);
      int64_t cap = graph.getEdgeData(ii);
      reduceCapacity(ii, source, dst, cap);
      Node& node = graph.getData(dst);
      node.excess += cap;
      if (cap > 0)
        initial.push_back(dst);
    }
  }

  void run() {

    auto obimIndexer = [this] (const GNode& n) {
      return -this->graph.getData(n, galois::MethodFlag::UNPROTECTED).height;
    };

    typedef galois::worklists::Deterministic<> DWL;
    typedef galois::worklists::dChunkedFIFO<16> Chunk;
    typedef galois::worklists::OrderedByIntegerMetric<decltype(obimIndexer), Chunk> OBIM;

    galois::InsertBag<GNode> initial;
    initializePreflow(initial);

    while (initial.begin() != initial.end()) {
      galois::StatTimer T_discharge("DischargeTime");
      T_discharge.start();
      Counter counter;
      switch (detAlgo) {
        case nondet:
          if (useHLOrder) {
            galois::for_each(galois::iterate(initial), ProcessNonDet(counter, *this),
                galois::loopname("Discharge"),
                galois::wl<OBIM>(obimIndexer));
          } else {
            galois::for_each(galois::iterate(initial), ProcessNonDet(counter, *this),
                galois::loopname("Discharge"));
          }
          break;
        case detBase: {
                        Process<detBase> fn(counter, *this);
                        galois::for_each(
                            galois::iterate(initial), fn, galois::loopname("Discharge"), galois::wl<DWL>(),
                            galois::per_iter_alloc(),
                            galois::det_parallel_break<Process<detBase>::ParallelBreak>(fn.getParallelBreak()),
                            galois::det_id<Process<detBase>::DeterministicId>(fn.getDetermisticId()));
                      } break;
        case detDisjoint: {
                        Process<detDisjoint> fn(counter, *this);
                        galois::for_each(
                            galois::iterate(initial), fn, galois::loopname("Discharge"), galois::wl<DWL>(),
                            galois::per_iter_alloc(),
                            galois::det_parallel_break<Process<detDisjoint>::ParallelBreak>(fn.getParallelBreak()),
                            galois::det_id<Process<detDisjoint>::DeterministicId>(fn.getDetermisticId()));
                      } break;
        default:
                          std::cerr << "Unknown algorithm" << detAlgo << "\n";
                          abort();
      }
      T_discharge.stop();

      if (should_global_relabel) {
        galois::StatTimer T_global_relabel("GlobalRelabelTime");
        T_global_relabel.start();
        initial.clear();
        globalRelabel(initial);
        should_global_relabel = false;
        std::cout << " Flow after global relabel: "
          << graph.getData(sink).excess << "\n";
        T_global_relabel.stop();
      } else {
        break;
      }
    }
  }
};


int main(int argc, char** argv) {
  galois::SharedMemSys G;
  constexpr bool serial = false;
  LonestarStart(argc, argv, name, desc, url);

  PreflowPush app;
  app.initializeGraph(filename, sourceId, sinkId);

  app.checkSorting();

  if (relabelInt == 0) {
    app.global_relabel_interval =
        app.graph.size() * ALPHA + app.graph.sizeEdges() / 3;
  } else {
    app.global_relabel_interval = relabelInt;
  }
  std::cout << "number of nodes: " << app.graph.size() << "\n";
  std::cout << "global relabel interval: " << app.global_relabel_interval
            << "\n";
  std::cout << "serial execution: " << (serial ? "yes" : "no") << "\n";

  galois::StatTimer T;
  T.start();
  app.run();
  T.stop();

  std::cout << "Flow is " << app.graph.getData(app.sink).excess << "\n";

  if (!skipVerify) {
    PreflowPush orig;
    orig.initializeGraph(filename, sourceId, sinkId);
    app.verify(orig);
    std::cout << "(Partially) Verified\n";
  }

  return 0;
}
