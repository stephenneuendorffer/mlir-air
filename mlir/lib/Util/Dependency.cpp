//===- Dependency.cpp -------------------------------------------*- C++ -*-===//
//
// Copyright (C) 2022, Xilinx Inc. All rights reserved.
// Copyright (C) 2022, Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//

#include "air/Util/Dependency.h"
#include <sys/stat.h>

#define DEBUG_TYPE "air-dependency-util"

using namespace mlir;

namespace xilinx {
namespace air {

bool areEqualIndices(mlir::Value index_0, mlir::Value index_1) {
  if (index_0 == nullptr || index_1 == nullptr) {
    // Note: memref with index is subset to memref without index (i.e. the
    // entire memref)
    return true;
  } else {
    if (index_0 == index_1)
      return true;
    else if (!index_0.getDefiningOp())
      return false;
    else if (!index_1.getDefiningOp())
      return false;
    else {
      if (auto index_0_const_op =
              dyn_cast<arith::ConstantOp>(index_0.getDefiningOp())) {
        if (auto index_1_const_op =
                dyn_cast<arith::ConstantOp>(index_1.getDefiningOp())) {
          if (index_0_const_op.getValue() == index_1_const_op.getValue())
            return true;
        }
      }
      return false;
    }
  }
}

// Recursively check for dependency to loop induction vars arising from dma src
void traceDependentInductionVar(air::DmaMemcpyInterface async_op,
                                SmallVector<Value, 1> &loop_dep_history,
                                std::vector<Operation *> &op_history) {
  // Check for immediate dependency to loop induction vars
  SmallVector<Value, 1> candidate_scalar_operands;
  for (unsigned i = 0; i < async_op.getNumDims(); i++) {
    candidate_scalar_operands.push_back(async_op.getSrcMemrefDim(i));
  }
  if (auto dmaNd_op = dyn_cast<air::DmaMemcpyNdOp>(async_op.getOperation())) {
    for (unsigned i = 0; i < dmaNd_op.getSrcOffsets().size(); i++) {
      candidate_scalar_operands.push_back(dmaNd_op.getSrcOffsets()[i]);
      candidate_scalar_operands.push_back(dmaNd_op.getSrcSizes()[i]);
      candidate_scalar_operands.push_back(dmaNd_op.getSrcStrides()[i]);
    }
  }
  for (auto operand : candidate_scalar_operands) {
    // If parent loop op is an scf.for
    if (auto for_op = mlir::scf::getForInductionVarOwner(operand)) {
      loop_dep_history.push_back(for_op.getInductionVar());
    }
    // TODO: Assuming that src.parallel won't exist under herd launch
    // If parent loop op is an scf.parallel

    // If parent loop op is an air.launch_herd
    if (auto hl_op = getHerdArgOwner(operand)) {
      for (auto id : hl_op.getIds()) {
        if (operand == id) {
          loop_dep_history.push_back(id);
        }
      }
    }
  }

  // Recursively trace dependency to loop induction vars
  for (auto operand : candidate_scalar_operands) {
    if (operand &&
        operand.getType().isa<IndexType>()) { // Only tracing scalar operands
      if (operand.getDefiningOp() &&
          mlir::dyn_cast<air::AsyncOpInterface>(operand.getDefiningOp())) {
        auto ancestor_async_op =
            dyn_cast<air::AsyncOpInterface>(operand.getDefiningOp());
        op_history.push_back(ancestor_async_op.getOperation());
        traceDependentInductionVar(ancestor_async_op, loop_dep_history,
                                   op_history);
      } else {
        // Trace dependency through a for loop
        if (auto for_op = getForRegionIterArgsOwner(operand)) {
          for (auto iter_arg : for_op.getIterOperands()) {
            if (operand == iter_arg) {
              loop_dep_history.push_back(iter_arg);
            }
          }
        }
        // Trace dependency through a parallel loop
        // TODO: decide if parallel should exist in herd launch
      }
    }
  }
}

// Recursively check for dependency to any loop induction vars
void traceDependentInductionVar(air::AsyncOpInterface async_op,
                                SmallVector<Value, 1> &loop_dep_history,
                                std::vector<Operation *> &op_history) {
  // Get child op if async_op is air.execute
  Operation *op = nullptr;
  if (auto air_region_op = dyn_cast<air::ExecuteOp>(async_op.getOperation())) {
    assert(air_region_op.getBody().front().getOperations().size() == 2 &&
           "air::ExecuteOp should have only one child operation beside the "
           "terminator");
    for (auto &child_op : air_region_op.getBody().front().getOperations()) {
      if (!dyn_cast<air::ExecuteTerminatorOp>(child_op))
        op = &child_op;
    }
  } else {
    op = async_op.getOperation();
  }

  // Check for immediate dependency to loop induction vars
  for (auto operand : op->getOperands()) {
    // If parent loop op is an scf.for
    if (auto for_op = mlir::scf::getForInductionVarOwner(operand)) {
      loop_dep_history.push_back(for_op.getInductionVar());
    }
    // If parent loop op is an scf.parallel
    if (auto parallel_op =
            mlir::scf::getParallelForInductionVarOwner(operand)) {
      for (auto induction_var : parallel_op.getInductionVars()) {
        if (operand == induction_var) {
          loop_dep_history.push_back(induction_var);
        }
      }
    }
    // If parent loop op is an air.launch_herd
    if (auto hl_op = getHerdArgOwner(operand)) {
      for (auto id : hl_op.getIds()) {
        if (operand == id) {
          loop_dep_history.push_back(id);
        }
      }
    }
  }

  // Recursively trace dependency to loop induction vars
  for (auto operand : op->getOperands()) {
    if (operand &&
        operand.getType().isa<IndexType>()) { // Only tracing scalar operands
      if (operand.getDefiningOp() &&
          mlir::dyn_cast<air::AsyncOpInterface>(operand.getDefiningOp())) {
        auto ancestor_async_op =
            dyn_cast<air::AsyncOpInterface>(operand.getDefiningOp());
        op_history.push_back(ancestor_async_op.getOperation());
        traceDependentInductionVar(ancestor_async_op, loop_dep_history,
                                   op_history);
      } else {
        // Trace dependency through a for loop
        if (auto for_op = getForRegionIterArgsOwner(operand)) {
          for (auto iter_arg : for_op.getIterOperands()) {
            if (operand == iter_arg) {
              loop_dep_history.push_back(iter_arg);
            }
          }
        }
        // Trace dependency through a parallel loop
        // TODO: decide if parallel should exist in herd launch
      }
    }
  }
}

// Recursively check for dependency to any control token (scf loop or wait all)
void traceDependentScfLoopToken(air::AsyncOpInterface async_op,
                                SmallVector<Value, 1> &control_token_history,
                                std::vector<Operation *> &op_history) {

  // Check for immediate dependency to control tokens
  for (auto token : async_op.getAsyncDependencies()) {
    if (auto for_op = getForRegionIterArgsOwner(token)) {
      control_token_history.push_back(token);
      return;
    }
    if (auto parallel_op =
            getParallelRegionInitValsOwner(async_op.getOperation(), token)) {
      control_token_history.push_back(token);
      return;
    }
    if (token.getDefiningOp() &&
        dyn_cast<air::WaitAllOp>(token.getDefiningOp())) {
      control_token_history.push_back(token);
      return;
    }
  }

  // Recursively trace dependency to scf loop tokens
  for (auto token : async_op.getAsyncDependencies()) {
    if (token.getDefiningOp() &&
        mlir::dyn_cast<air::AsyncOpInterface>(token.getDefiningOp())) {
      auto ancestor_async_op =
          dyn_cast<air::AsyncOpInterface>(token.getDefiningOp());
      op_history.push_back(ancestor_async_op.getOperation());
      traceDependentScfLoopToken(ancestor_async_op, control_token_history,
                                 op_history);
    }
  }
}

void eraseAsyncDependencyFromAsyncOp(xilinx::air::AsyncOpInterface op,
                                     Value token) {
  assert(token && "input value is nullptr");
  assert(token.getType().isa<air::AsyncTokenType>() &&
         "ssa value is not an async token");
  auto dependency_list = op.getAsyncDependencies();
  if (!dependency_list.size())
    assert(false && "dependency list is empty");
  for (int i = dependency_list.size() - 1; i >= 0; i--) {
    if (dependency_list[i] == token) {
      op.eraseAsyncDependency(i);
    }
  }
}

// Returns the scf parent op from scf.yield op
template <typename T> Operation *getScfParentOpFromYieldOp(scf::YieldOp op) {
  if (auto scfop = dyn_cast<T>(op->getParentOp())) {
    return scfop.getOperation();
  }
  return nullptr;
}

// Get loop-carried dependency token from scf loop op
Value getLoopCarriedTokenFromScfOp(scf::ParallelOp op) {
  assert(op.getInitVals().size());
  auto token = op.getInitVals()[0];
  assert(token.getType().isa<air::AsyncTokenType>() &&
         "init value is not an async token");
  return token;
}
Value getLoopCarriedTokenFromScfOp(scf::ForOp op,
                                   std::string operand_or_argument) {
  if (operand_or_argument == "operand") {
    assert(op.getIterOperands().size());
    auto token = op.getIterOperands()[0];
    assert(token.getType().isa<air::AsyncTokenType>() &&
           "iter operand is not an async token");
    return token;
  } else if (operand_or_argument == "argument") {
    assert(op.getRegionIterArgs().size());
    auto token = op.getRegionIterArgs()[0];
    assert(token.getType().isa<air::AsyncTokenType>() &&
           "iter operand is not an async token");
    return token;
  } else
    assert(false && "unknown string in operand_or_argument");
}

// Add async dependency to op if unique
void addAsyncDependencyIfNew(air::AsyncOpInterface op, Value token) {
  assert(token.getType().isa<air::AsyncTokenType>() &&
         "value is not an async token");
  bool foundTokenInDepList = false;
  if (op.getAsyncDependencies().size()) {
    for (auto old_dep : op.getAsyncDependencies())
      if (old_dep == token)
        foundTokenInDepList = true;
    if (!foundTokenInDepList) {
      op.addAsyncDependency(token);
    }
  } else {
    op.addAsyncDependency(token);
  }
}

// Return memory space as string
std::string getMemorySpaceAsString(Value memref) {
  assert(memref.getType().isa<MemRefType>() && "value is not a memref");
  auto memory_space_as_int =
      memref.getType().dyn_cast<MemRefType>().getMemorySpaceAsInt();
  std::string memorySpaceStr;
  if (memory_space_as_int == (int)MemorySpace::L1) {
    memorySpaceStr = "L1";
  } else if (memory_space_as_int == (int)MemorySpace::L2) {
    memorySpaceStr = "L2";
  } else if (memory_space_as_int == (int)MemorySpace::L3) {
    memorySpaceStr = "L3";
  } else
    assert(false && "unknown memory space");
  return memorySpaceStr;
}

// Get channel declaration through channel symbol
air::ChannelOp getChannelDeclarationThroughSymbol(air::ChannelInterface op) {
  auto module = op->getParentOfType<ModuleOp>();
  return dyn_cast<air::ChannelOp>(module.lookupSymbol(op.getChanName()));
}

// Get the other channel op through channel symbol
air::ChannelGetOp getTheOtherChannelOpThroughSymbol(air::ChannelPutOp put) {
  auto module = put->getParentOfType<ModuleOp>();
  // auto channel_op =
  //     dyn_cast<air::ChannelOp>(module.lookupSymbol(put.getChanName()));
  auto channel_op = getChannelDeclarationThroughSymbol(
      dyn_cast<air::ChannelInterface>(put.getOperation()));
  auto attr =
      channel_op->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());

  air::ChannelGetOp output = nullptr;
  module.walk([&](Operation *op) {
    if (auto get = dyn_cast<air::ChannelGetOp>(op)) {
      if (get.getChanName() == attr) {
        if (output)
          assert(false && "found multiple occurrences of channel get");
        else
          output = get;
      }
    }
  });

  if (output)
    return output;
  else
    return air::ChannelGetOp();
}
air::ChannelPutOp getTheOtherChannelOpThroughSymbol(air::ChannelGetOp get) {
  auto module = get->getParentOfType<ModuleOp>();
  auto channel_op = getChannelDeclarationThroughSymbol(
      dyn_cast<air::ChannelInterface>(get.getOperation()));
  auto attr =
      channel_op->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());

  air::ChannelPutOp output = nullptr;
  module.walk([&](Operation *op) {
    if (auto put = dyn_cast<air::ChannelPutOp>(op)) {
      if (put.getChanName() == attr) {
        if (output)
          assert(false && "found multiple occurrences of channel put");
        else
          output = put;
      }
    }
  });

  if (output)
    return output;
  else
    return air::ChannelPutOp();
}

//===----------------------------------------------------------------------===//
// Dependency graph as a Boost graph object
//===----------------------------------------------------------------------===//

void dependencyCanonicalizer::parseCommandGraphs(func::FuncOp &toplevel,
                                                 dependencyGraph &global_graph,
                                                 dependencyContext &dep_ctx,
                                                 bool dump_dot,
                                                 std::string dump_dir) {

  // Create vertices for graphs
  // Build up host graph
  toplevel.walk([&](Operation *op) {
    if (!op->getParentOfType<air::HierarchyInterface>()) {
      addVertexFromOpImpls(op, global_graph.g, dep_ctx);
      if (auto launch = dyn_cast<air::LaunchOp>(op)) {
        addVerticesInLaunch(global_graph.subgraphs, launch, dep_ctx);
      } else if (dyn_cast<air::PartitionOp>(op) &&
                 (!op->getParentOfType<air::LaunchOp>())) {
        auto partition = dyn_cast<air::PartitionOp>(op);
        addVerticesInPartition(global_graph.subgraphs, partition, dep_ctx);
      } else if (dyn_cast<air::HerdOp>(op) &&
                 (!op->getParentOfType<air::LaunchOp>()) &&
                 (!op->getParentOfType<air::PartitionOp>())) {
        auto herd = dyn_cast<air::HerdOp>(op);
        addVerticesInHerd(global_graph.subgraphs, herd, dep_ctx);
      }
    }
  });

  // Adds edges between async ops
  parseDependencyEdgesInGraph(global_graph.g, dep_ctx);
  for (auto &G_l : global_graph.subgraphs) {
    parseDependencyEdgesInGraph(G_l.g, dep_ctx);
    for (auto &G_p : G_l.subgraphs) {
      parseDependencyEdgesInGraph(G_p.g, dep_ctx);
      for (auto &G_h : G_p.subgraphs) {
        parseDependencyEdgesInGraph(G_h.g, dep_ctx);
      }
    }
  }

  // Connect leaf vertices to launch, partition and herd terminators
  for (auto &G_l : global_graph.subgraphs) {
    connectTerminatorInGraph(G_l.g);
    for (auto &G_p : G_l.subgraphs) {
      connectTerminatorInGraph(G_p.g);
      for (auto &G_h : G_p.subgraphs) {
        connectTerminatorInGraph(G_h.g);
      }
    }
  }

  // Connect the start node per graph as graph inception point;
  // update pointer from graph to air.hierarchy op terminators
  connectStartNodeInCommandGraph(global_graph);
  updatePointerFromGraphToHierarchyTerminator(global_graph);
  updatePointerFromHierarchyOpToGraph(global_graph);
  for (auto &launchGraph : global_graph.subgraphs) {
    connectStartNodeInCommandGraph(launchGraph);
    updatePointerFromGraphToHierarchyTerminator(launchGraph);
    updatePointerFromHierarchyTerminatorToGraph(global_graph, launchGraph);
    updatePointerFromHierarchyOpToGraph(launchGraph);
    for (auto &partitionGraph : launchGraph.subgraphs) {
      connectStartNodeInCommandGraph(partitionGraph);
      updatePointerFromGraphToHierarchyTerminator(partitionGraph);
      updatePointerFromHierarchyTerminatorToGraph(launchGraph, partitionGraph);
      updatePointerFromHierarchyOpToGraph(partitionGraph);
      for (auto &herdGraph : partitionGraph.subgraphs) {
        connectStartNodeInCommandGraph(herdGraph);
        updatePointerFromGraphToHierarchyTerminator(herdGraph);
      }
    }
  }

  if (dump_dot) {
    // Dump dot graphs
    dumpDotGraphFiles(global_graph, dump_dir);
  }
}

void dependencyCanonicalizer::copyDependencyGraphToFlatGraphAndVisualize(
    func::FuncOp &toplevel, dependencyGraph &global_graph,
    dependencyContext &dep_ctx, bool dump_dot, std::string dump_dir) {
  // Create FlatGraph
  FlatGraph flat_g;
  boost::get_property(flat_g, boost::graph_graph_attribute)["rankdir"] = "LR";
  std::vector<vertex_to_flat_vertex_map> maps;

  // Copy vertices and edges to flat graph
  vertex_to_flat_vertex_map map;
  copyFromDependencyGraphToFlatGraph(global_graph.g, flat_g, map, true);
  maps.push_back(map);
  for (auto &G_l : global_graph.subgraphs) {
    vertex_to_flat_vertex_map map_l;
    copyFromDependencyGraphToFlatGraph(G_l.g, flat_g, map_l);
    maps.push_back(map_l);
    for (auto &G_p : G_l.subgraphs) {
      vertex_to_flat_vertex_map map_p;
      copyFromDependencyGraphToFlatGraph(G_p.g, flat_g, map_p);
      maps.push_back(map_p);
      for (auto &G_h : G_p.subgraphs) {
        vertex_to_flat_vertex_map map_h;
        copyFromDependencyGraphToFlatGraph(G_h.g, flat_g, map_h);
        maps.push_back(map_h);
      }
    }
  }

  // AIR channel dependency edges, overlayed as a (non-cluster) subgraph
  ChannelMap channel_map;
  unsigned index = 0;
  collectAIRChannelPutAndGetInGraph(global_graph.g, maps[index++], channel_map);
  for (auto &G_l : global_graph.subgraphs) {
    collectAIRChannelPutAndGetInGraph(G_l.g, maps[index++], channel_map);
    for (auto &G_p : G_l.subgraphs) {
      collectAIRChannelPutAndGetInGraph(G_p.g, maps[index++], channel_map);
      for (auto &G_h : G_p.subgraphs) {
        collectAIRChannelPutAndGetInGraph(G_h.g, maps[index++], channel_map);
      }
    }
  }
  FlatGraph &flat_subg_chan = flat_g.create_subgraph();
  for (auto &map : channel_map) {
    if (map.second.first == std::numeric_limits<int>::max()) {
      assert(false && "incomplete channel op map");
    } else if (map.second.second == std::numeric_limits<int>::max()) {
      assert(false && "incomplete channel op map");
    }
    auto a = add_vertex(map.second.first, flat_subg_chan);
    auto b = add_vertex(map.second.second, flat_subg_chan);
    auto e = add_edge(a, b, flat_subg_chan).first;
    put(get(boost::edge_attribute, flat_subg_chan), e,
        GraphvizAttributes{{"style", "dashed"}});
  }

  // Create subgraphs
  boost::get_property(flat_g, boost::graph_name) = "host";
  index = 1;
  unsigned idx_l = 0;
  unsigned idx_p = 0;
  unsigned idx_h = 0;

  auto vp = boost::vertices(global_graph.g);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    if (global_graph.g[*vit].asyncEventName == "LaunchOp") {
      auto G_l = *global_graph.g[*vit].nextDependencyGraph;
      FlatGraph &flat_subg_l = flat_g.create_subgraph();
      updateSubgraphFromDependencyGraph(G_l.g, flat_subg_l, maps[index], true);
      boost::get_property(flat_subg_l, boost::graph_name) =
          "cluster" + std::to_string(index);
      boost::get_property(flat_subg_l, boost::graph_graph_attribute)["label"] =
          "launch" + std::to_string(idx_l++);
      // Connect host "launch" graph nodes with "start" of launch subgraph
      add_edge(maps[0][*vit], maps[index][G_l.start_vertex], flat_g);

      auto map_idx_launch = index++;
      auto vp_l = boost::vertices(G_l.g);
      for (auto vit_l = vp_l.first; vit_l != vp_l.second; ++vit_l) {
        if (G_l.g[*vit_l].asyncEventName == "PartitionOp") {
          auto G_p = *G_l.g[*vit_l].nextDependencyGraph;
          FlatGraph &flat_subg_p = flat_subg_l.create_subgraph();
          updateSubgraphFromDependencyGraph(G_p.g, flat_subg_p, maps[index],
                                            true);
          boost::get_property(flat_subg_p, boost::graph_name) =
              "cluster" + std::to_string(index);
          boost::get_property(flat_subg_p,
                              boost::graph_graph_attribute)["label"] =
              "partition" + std::to_string(idx_p++);
          // Connect host "launch" graph nodes with "start" of partition
          // subgraph
          add_edge(maps[map_idx_launch][*vit_l], maps[index][G_p.start_vertex],
                   flat_g);

          auto map_idx_partition = index++;
          auto vp_p = boost::vertices(G_p.g);
          for (auto vit_p = vp_p.first; vit_p != vp_p.second; ++vit_p) {
            if (G_p.g[*vit_p].asyncEventName == "HerdOp") {
              auto G_h = *G_p.g[*vit_p].nextDependencyGraph;
              FlatGraph &flat_subg_h = flat_subg_p.create_subgraph();
              updateSubgraphFromDependencyGraph(G_h.g, flat_subg_h, maps[index],
                                                true);
              boost::get_property(flat_subg_h, boost::graph_name) =
                  "cluster" + std::to_string(index);
              boost::get_property(flat_subg_h,
                                  boost::graph_graph_attribute)["label"] =
                  "herd" + std::to_string(idx_h++);
              // Connect host "launch" graph nodes with "start" of herd subgraph
              add_edge(maps[map_idx_partition][*vit_p],
                       maps[index++][G_h.start_vertex], flat_g);
            }
          }
        }
      }
    }
  }

  if (dump_dot) {
    // Dump dot graphs
    std::ofstream ofs(dump_dir + "graph.dot", std::ofstream::out);
    write_graphviz(ofs, flat_g);
  }
}

void dependencyCanonicalizer::addVerticesInHerd(
    std::vector<dependencyGraph> &herd_subgraphs, air::HerdOp herd,
    dependencyContext &dep_ctx) {
  // Build up herd graph
  herd_subgraphs.push_back(dependencyGraph(herd.getOperation(), true));
  dependencyGraph *current_herd_graph = &(herd_subgraphs.back());

  herd.walk([&](Operation *herd_childop) {
    if (!dyn_cast<air::HerdOp>(herd_childop)) {
      addVertexFromOpImpls(herd_childop, current_herd_graph->g, dep_ctx);
    }
  });
}

void dependencyCanonicalizer::addVerticesInPartition(
    std::vector<dependencyGraph> &part_subgraphs, air::PartitionOp partition,
    dependencyContext &dep_ctx) {
  // Build up partition graph
  part_subgraphs.push_back(dependencyGraph(partition.getOperation(), true));
  dependencyGraph *current_part_graph = &(part_subgraphs.back());

  partition.walk([&](Operation *part_childop) {
    if (!part_childop->getParentOfType<air::HerdOp>() &&
        !dyn_cast<air::PartitionOp>(part_childop)) {
      addVertexFromOpImpls(part_childop, current_part_graph->g, dep_ctx);
      if (auto herd = dyn_cast<air::HerdOp>(part_childop)) {
        addVerticesInHerd(current_part_graph->subgraphs, herd, dep_ctx);
      }
    }
  });
}

void dependencyCanonicalizer::addVerticesInLaunch(
    std::vector<dependencyGraph> &launch_subgraphs, air::LaunchOp launch,
    dependencyContext &dep_ctx) {
  // Build up launch graph
  launch_subgraphs.push_back(dependencyGraph(launch.getOperation(), true));
  dependencyGraph *current_launch_graph = &(launch_subgraphs.back());

  launch.walk([&](Operation *launch_childop) {
    if (!launch_childop->getParentOfType<air::PartitionOp>() &&
        !dyn_cast<air::LaunchOp>(launch_childop)) {
      addVertexFromOpImpls(launch_childop, current_launch_graph->g, dep_ctx);
      if (auto partition = dyn_cast<air::PartitionOp>(launch_childop)) {
        addVerticesInPartition(current_launch_graph->subgraphs, partition,
                               dep_ctx);
      } else if (dyn_cast<air::HerdOp>(launch_childop) &&
                 (!launch_childop->getParentOfType<air::PartitionOp>())) {
        auto herd = dyn_cast<air::HerdOp>(launch_childop);
        addVerticesInHerd(current_launch_graph->subgraphs, herd, dep_ctx);
      }
    }
  });
}

Graph::vertex_descriptor
dependencyCanonicalizer::addVertexFromOpImpls(Operation *op, Graph &G,
                                              dependencyContext &dep_ctx) {
  if (auto dma_op = mlir::dyn_cast<xilinx::air::DmaMemcpyInterface>(op)) {
    return addVertexFromDmaOp(dma_op, G, dep_ctx);
  } else if (auto channel_op =
                 mlir::dyn_cast<xilinx::air::ChannelInterface>(op)) {
    return addVertexFromChannelOp(channel_op, G, dep_ctx);
  } else if (auto execute_op = dyn_cast<xilinx::air::ExecuteOp>(op)) {
    return addVertexFromExecuteOp(execute_op, G, dep_ctx);
  } else if (auto wa_op = dyn_cast<xilinx::air::WaitAllOp>(op)) {
    return addVertexFromWaitAllOp(wa_op, G, dep_ctx);
  } else if (auto forop = dyn_cast<scf::ForOp>(op)) {
    return addVertexFromOp(op, dep_ctx.ForOpID, "for_loop", "ScfForOp",
                           "crimson", "box", G, dep_ctx);
  } else if (auto parallelop = dyn_cast<scf::ParallelOp>(op)) {
    return addVertexFromOp(op, dep_ctx.ParallelOpID, "parallel_loop",
                           "ScfParallelOp", "crimson", "box", G, dep_ctx);
  } else if (auto hier_op =
                 mlir::dyn_cast<xilinx::air::HierarchyInterface>(op)) {
    return addVertexFromHierarchyOp(hier_op, G, dep_ctx);
  } else if (op->mightHaveTrait<OpTrait::IsTerminator>()) {
    return addVertexFromTerminatorOp(op, G, dep_ctx);
  } else if (auto reduce_op = dyn_cast<scf::ReduceOp>(op)) {
    return addVertexFromReduceOp(reduce_op, G, dep_ctx);
  } else
    return 0;
}

// Create graph vertex from op
Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromOp(
    Operation *op, uint64_t &id, std::string event_type, std::string event_name,
    std::string color, std::string shape, Graph &G, dependencyContext &dep_ctx,
    Operation *pointer_op) {
  op->setAttr("id", mlir::IntegerAttr::get(
                        mlir::IntegerType::get(op->getContext(), 32), ++id));
  auto v = add_vertex(G);
  G[v].asyncEventName = event_name;
  G[v].asyncEventType = event_type;
  G[v].color = color;
  G[v].shape = shape;
  G[v].operationId = id;
  if (pointer_op)
    G[v].op = pointer_op;
  else
    G[v].op = op;
  // Update op-to-graph mapping
  auto entry = make_pair(event_type, id);
  dep_ctx.op_to_v.insert(make_pair(entry, v));
  dep_ctx.op_to_g.insert(make_pair(entry, &G));
  return v;
}

Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromDmaOp(
    xilinx::air::DmaMemcpyInterface op, Graph &G, dependencyContext &dep_ctx) {
  if (dyn_cast<xilinx::air::DmaMemcpyNdOp>(op.getOperation())) {
    return addVertexFromOp(op, dep_ctx.DmaOpID, "dma", "DmaMemcpyNdOp", "cyan",
                           "oval", G, dep_ctx);
  } else {
    assert(false && "Unknown dma op");
    return 0;
  }
}

Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromChannelOp(
    xilinx::air::ChannelInterface op, Graph &G, dependencyContext &dep_ctx) {
  if (auto channel_put =
          dyn_cast<xilinx::air::ChannelPutOp>(op.getOperation())) {
    std::string memorySpaceSrcStr =
        getMemorySpaceAsString(channel_put.getSrcMemref());
    auto channel_get = getTheOtherChannelOpThroughSymbol(channel_put);
    assert(channel_get && "found channel op not in pairs");
    std::string memorySpaceDstStr =
        getMemorySpaceAsString(channel_get.getDstMemref());
    std::string event_name = "ChannelPutOp@" + channel_put.getChanName().str() +
                             "(" + memorySpaceSrcStr + "-->" +
                             memorySpaceDstStr + ")";
    auto channel_op = getChannelDeclarationThroughSymbol(op);
    if (channel_op->hasAttr("broadcast_shape")) {
      auto size = extractFromI64ArrayAttr(channel_op.getSize());
      event_name += "\n(broadcast[";
      for (auto &s : size) {
        event_name += std::to_string(s);
        if (&s != &size.back())
          event_name += ",";
      }
      event_name += "]-->[";
      auto bsize = extractFromI64ArrayAttr(
          channel_op->getAttrOfType<mlir::ArrayAttr>("broadcast_shape"));
      for (auto &s : bsize) {
        event_name += std::to_string(s);
        if (&s != &bsize.back())
          event_name += ",";
      }
      event_name += "])";
    }
    return addVertexFromOp(op, dep_ctx.DmaOpID, "channel", event_name, "cyan",
                           "oval", G, dep_ctx);
  } else if (auto channel_get =
                 dyn_cast<xilinx::air::ChannelGetOp>(op.getOperation())) {
    std::string memorySpaceDstStr =
        getMemorySpaceAsString(channel_get.getDstMemref());
    auto channel_put = getTheOtherChannelOpThroughSymbol(channel_get);
    assert(channel_put && "found channel op not in pairs");
    std::string memorySpaceSrcStr =
        getMemorySpaceAsString(channel_put.getSrcMemref());
    std::string event_name = "ChannelGetOp@" + channel_get.getChanName().str() +
                             "(" + memorySpaceDstStr + "<--" +
                             memorySpaceSrcStr + ")";
    auto channel_op = getChannelDeclarationThroughSymbol(op);
    if (channel_op->hasAttr("broadcast_shape")) {
      auto size = extractFromI64ArrayAttr(channel_op.getSize());
      event_name += "\n(broadcast[";
      for (auto &s : size) {
        event_name += std::to_string(s);
        if (&s != &size.back())
          event_name += ",";
      }
      event_name += "]-->[";
      auto bsize = extractFromI64ArrayAttr(
          channel_op->getAttrOfType<mlir::ArrayAttr>("broadcast_shape"));
      for (auto &s : bsize) {
        event_name += std::to_string(s);
        if (&s != &bsize.back())
          event_name += ",";
      }
      event_name += "])";
    }
    return addVertexFromOp(op, dep_ctx.DmaOpID, "channel", event_name, "cyan",
                           "oval", G, dep_ctx);
  } else {
    assert(false && "Unknown channel op");
    return 0;
  }
}

Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromHierarchyOp(
    xilinx::air::HierarchyInterface op, Graph &G, dependencyContext &dep_ctx) {
  if (dyn_cast<xilinx::air::LaunchOp>(op.getOperation())) {
    return addVertexFromOp(op, dep_ctx.HierarchyOpID, "hierarchy", "LaunchOp",
                           "yellow", "box", G, dep_ctx);
  } else if (dyn_cast<xilinx::air::PartitionOp>(op.getOperation())) {
    return addVertexFromOp(op, dep_ctx.HierarchyOpID, "hierarchy",
                           "PartitionOp", "yellow", "box", G, dep_ctx);
  } else if (dyn_cast<xilinx::air::HerdOp>(op.getOperation())) {
    return addVertexFromOp(op, dep_ctx.HierarchyOpID, "hierarchy", "HerdOp",
                           "yellow", "box", G, dep_ctx);
  } else {
    assert(false && "Unknown hierarchy op");
    return 0;
  }
}

Graph::vertex_descriptor
dependencyCanonicalizer::addVertexFromTerminatorOp(Operation *op, Graph &G,
                                                   dependencyContext &dep_ctx) {
  if (dyn_cast<xilinx::air::LaunchTerminatorOp>(op)) {
    return addVertexFromOp(op, dep_ctx.TerminatorID, "hierarchy_terminator",
                           "LaunchTerminator", "yellow", "box", G, dep_ctx);
  } else if (dyn_cast<xilinx::air::PartitionTerminatorOp>(op)) {
    return addVertexFromOp(op, dep_ctx.TerminatorID, "hierarchy_terminator",
                           "PartitionTerminator", "yellow", "box", G, dep_ctx);
  } else if (dyn_cast<xilinx::air::HerdTerminatorOp>(op)) {
    return addVertexFromOp(op, dep_ctx.TerminatorID, "hierarchy_terminator",
                           "HerdTerminator", "yellow", "box", G, dep_ctx);
  } else if (auto yieldop = dyn_cast<scf::YieldOp>(op)) {
    if (getScfParentOpFromYieldOp<scf::ParallelOp>(yieldop)) {
      // Note: disabled parsing scf parallel yield op since it currently acts as
      // a no-op return addVertexFromOp(op, dep_ctx.TerminatorID, "terminator",
      //                        "ScfParallelYieldOp", "crimson", "box", G,
      //                        dep_ctx);
    } else if (getScfParentOpFromYieldOp<scf::ForOp>(yieldop)) {
      return addVertexFromOp(op, dep_ctx.TerminatorID, "terminator",
                             "ScfForYieldOp", "crimson", "box", G, dep_ctx);
    }
  }
  return 0;
}

// Note: in the current scf parallel spec, reduce op takes the role of yielding
// the ssa value. Hence, here we parse reduce op as a terminator.
Graph::vertex_descriptor
dependencyCanonicalizer::addVertexFromReduceOp(Operation *op, Graph &G,
                                               dependencyContext &dep_ctx) {
  return addVertexFromOp(op, dep_ctx.TerminatorID, "terminator", "ScfReduceOp",
                         "crimson", "box", G, dep_ctx);
}

Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromExecuteOp(
    xilinx::air::ExecuteOp op, Graph &G, dependencyContext &dep_ctx) {
  int iter_count = 0;
  Graph::vertex_descriptor v_prev = 0;
  Graph::vertex_descriptor v = 0;
  Operation *pointer_op = op;
  for (auto &child_op : op->getRegions().front().getOps()) {
    if (dyn_cast<linalg::LinalgOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute", "LinalgOp",
                          "chartreuse", "oval", G, dep_ctx, pointer_op);
    } else if (dyn_cast<memref::AllocOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute", "AllocOp",
                          "chartreuse", "oval", G, dep_ctx, pointer_op);
    } else if (dyn_cast<memref::DeallocOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute",
                          "DeallocOp", "chartreuse", "oval", G, dep_ctx,
                          pointer_op);
    } else if (dyn_cast<memref::CopyOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute", "CopyOp",
                          "chartreuse", "oval", G, dep_ctx, pointer_op);
    } else if (dyn_cast<mlir::AffineApplyOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute",
                          "AffineApplyOp", "chartreuse", "oval", G, dep_ctx,
                          pointer_op);
    } else if (dyn_cast<xilinx::air::ExecuteTerminatorOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute",
                          "ExecuteTerminatorOp", "chartreuse", "oval", G,
                          dep_ctx, pointer_op);
    } else if (dyn_cast<arith::MulIOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute", "MuliOp",
                          "chartreuse", "oval", G, dep_ctx, pointer_op);
    } else if (dyn_cast<arith::AddIOp>(child_op)) {
      v = addVertexFromOp(&child_op, dep_ctx.ExecuteOpID, "execute", "AddIOp",
                          "chartreuse", "oval", G, dep_ctx, pointer_op);
    } else {
      assert(false && "Unknown op in execute");
    }
    // Make connections within execute
    if (iter_count > 0) {
      add_edge(v_prev, v, G);
      pointer_op = nullptr;
    }
    v_prev = v;
    iter_count++;
  }
  return v;
}

Graph::vertex_descriptor dependencyCanonicalizer::addVertexFromWaitAllOp(
    xilinx::air::WaitAllOp op, Graph &G, dependencyContext &dep_ctx) {
  // Note: disabled parsing wait_all op inside of reduce op
  for (auto u : op.getAsyncToken().getUsers()) {
    if (dyn_cast<scf::ReduceReturnOp>(u)) {
      return 0;
    }
  }
  return addVertexFromOp(op, dep_ctx.WaitAllOpID, "wait_all", "WaitAllOp",
                         "crimson", "oval", G, dep_ctx);
}

// Get type-id pair from op, which will be used to look up vertex in op_to_v
std::pair<std::string, unsigned>
dependencyCanonicalizer::getTypeIdPairFromOp(Operation *op) {
  std::pair<std::string, unsigned> output;
  std::string type = getOpTypeFromOpImpls(op);
  output.first = type;
  output.second = xilinx::air::getIdAttr(op);
  return output;
}

std::string dependencyCanonicalizer::getOpTypeFromOpImpls(Operation *op) {
  if (mlir::dyn_cast<xilinx::air::DmaMemcpyInterface>(op)) {
    return "dma";
  } else if (mlir::dyn_cast<xilinx::air::ChannelInterface>(op)) {
    return "channel";
  } else if (dyn_cast<xilinx::air::WaitAllOp>(op)) {
    return "wait_all";
  } else if (dyn_cast<xilinx::air::HierarchyInterface>(op)) {
    return "hierarchy";
  } else if (dyn_cast<scf::ForOp>(op)) {
    return "for_loop";
  } else if (dyn_cast<scf::ParallelOp>(op)) {
    return "parallel_loop";
  } else if (dyn_cast<xilinx::air::LaunchTerminatorOp>(op)) {
    return "hierarchy_terminator";
  } else if (dyn_cast<xilinx::air::PartitionTerminatorOp>(op)) {
    return "hierarchy_terminator";
  } else if (dyn_cast<xilinx::air::HerdTerminatorOp>(op)) {
    return "hierarchy_terminator";
  } else if (dyn_cast<scf::YieldOp>(op)) {
    return "terminator";
  } else if (dyn_cast<scf::ReduceOp>(op)) {
    return "terminator";
  } else {
    if (dyn_cast<xilinx::air::ExecuteOp>(op->getParentOp())) {
      return "execute";
    } else {
      assert(false && "Unknown op type");
      return "";
    }
  }
}

// Get vertex descriptor from op
// "front_or_back": if op is an air.execute, then "front" returns the first op
// in region, while "back" returns the terminator in region.
std::pair<Graph::vertex_descriptor, Graph *>
dependencyCanonicalizer::getVertexFromOp(Operation *op,
                                         dependencyContext dep_ctx,
                                         std::string front_or_back) {
  std::pair<Graph::vertex_descriptor, Graph *> output;
  if (auto execute_op = dyn_cast<xilinx::air::ExecuteOp>(op)) {
    if (front_or_back == "front") {
      auto execute_front_op = &(*op->getRegions().front().op_begin());
      std::pair<std::string, unsigned> entry_pair =
          getTypeIdPairFromOp(execute_front_op);
      output.first = dep_ctx.op_to_v[entry_pair];
      output.second = dep_ctx.op_to_g[entry_pair];
    } else if (front_or_back == "back") {
      auto execute_end_op =
          op->getRegions().front().getBlocks().front().getTerminator();
      std::pair<std::string, unsigned> entry_pair =
          getTypeIdPairFromOp(execute_end_op);
      output.first = dep_ctx.op_to_v[entry_pair];
      output.second = dep_ctx.op_to_g[entry_pair];
    } else {
      assert(false &&
             "Unknown string operand (only accepts 'front' or 'back')");
    }
  } else {
    std::pair<std::string, unsigned> entry_pair = getTypeIdPairFromOp(op);
    output.first = dep_ctx.op_to_v[entry_pair];
    output.second = dep_ctx.op_to_g[entry_pair];
  }
  return output;
}

// Copy vertices and edges from dependencyGraph to FlatGraph
void dependencyCanonicalizer::copyFromDependencyGraphToFlatGraph(
    Graph g_src, FlatGraph &g_dst, vertex_to_flat_vertex_map &map,
    bool copyEdges) {
  // Copy vertices
  auto vp = boost::vertices(g_src);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    auto new_v = add_vertex(g_dst);
    // Copy vertex asyncEventName
    put(get(boost::vertex_attribute, g_dst), new_v,
        GraphvizAttributes{{"label", g_src[*vit].asyncEventName},
                           {"color", g_src[*vit].color},
                           {"shape", g_src[*vit].shape},
                           {"style", "filled"}});
    map.insert(std::make_pair(*vit, new_v));
  }
  if (copyEdges) {
    // Copy edges
    for (auto vit = vp.first; vit != vp.second; ++vit) {
      for (auto it = out_edges(*vit, g_src).first;
           it != out_edges(*vit, g_src).second; it++) {
        auto target_it = target(*it, g_src);
        add_edge(map[*vit], map[target_it], g_dst);
      }
    }
  }
}

// Update subgraph in FlatGraph from dependencyGraph
void dependencyCanonicalizer::updateSubgraphFromDependencyGraph(
    Graph subg_src, FlatGraph &subg_dst, vertex_to_flat_vertex_map map,
    bool copyEdges) {
  // Update vertices
  vertex_to_flat_vertex_map subg_map;
  auto vp = boost::vertices(subg_src);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    auto new_v = add_vertex(map[*vit], subg_dst);
    subg_map.insert(std::make_pair(*vit, new_v));
  }
  if (copyEdges) {
    // Update edges
    for (auto vit = vp.first; vit != vp.second; ++vit) {
      for (auto it = out_edges(*vit, subg_src).first;
           it != out_edges(*vit, subg_src).second; it++) {
        auto target_it = target(*it, subg_src);
        add_edge(subg_map[*vit], subg_map[target_it], subg_dst);
      }
    }
  }
}

// Collect air.channel put and get pairs
void dependencyCanonicalizer::collectAIRChannelPutAndGetInGraph(
    Graph g, vertex_to_flat_vertex_map map, ChannelMap &channel_map) {
  // Search for air.channep put/get
  auto vp = boost::vertices(g);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    if (g[*vit].asyncEventType == "channel") {
      auto channel_op = dyn_cast<air::ChannelInterface>(g[*vit].op);
      auto chan_name = channel_op.getChanName().str();
      if (channel_map.find(chan_name) == channel_map.end()) {
        // If key not found, then create map entry with given key
        channel_map.insert(std::make_pair(
            chan_name, std::make_pair(std::numeric_limits<int>::max(),
                                      std::numeric_limits<int>::max())));
      }
      if (dyn_cast<air::ChannelPutOp>(g[*vit].op)) {
        channel_map[chan_name].first = map[*vit];
      } else if (dyn_cast<air::ChannelGetOp>(g[*vit].op)) {
        channel_map[chan_name].second = map[*vit];
      } else
        assert(false && "unknown air.channel op type");
    }
  }
}

// Trace dependency of every op in a boost graph
void dependencyCanonicalizer::parseDependencyEdgesInGraph(
    Graph &g, dependencyContext dep_ctx) {
  auto vp = boost::vertices(g);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    auto op = g[*vit].op;
    if (!op)
      continue;
    connectOpToItsDepListImpls(op, g, dep_ctx);
  }
}

void dependencyCanonicalizer::connectOpToItsDepListImpls(
    Operation *op, Graph &g, dependencyContext dep_ctx) {
  SmallVector<Value, 1> dep_list;
  // air.asyncopinterface
  if (auto async_op = mlir::dyn_cast<xilinx::air::AsyncOpInterface>(op)) {
    for (auto dep_token : async_op.getAsyncDependencies()) {
      dep_list.push_back(dep_token);
    }
  }
  // scf.for
  else if (auto forop = dyn_cast<scf::ForOp>(op)) {
    for (auto iter_operand : forop.getIterOperands()) {
      dep_list.push_back(iter_operand);
    }
  }
  // scf.parallel
  else if (auto parallelop = dyn_cast<scf::ParallelOp>(op)) {
    for (auto operand : parallelop->getOperands()) {
      dep_list.push_back(operand);
    }
  }
  // scf.yield
  else if (auto yieldop = dyn_cast<scf::YieldOp>(op)) {
    for (auto operand : yieldop->getOperands()) {
      dep_list.push_back(operand);
    }
  }
  // scf.reduce
  else if (auto reduceop = dyn_cast<scf::ReduceOp>(op)) {
    for (auto operand : reduceop->getOperands()) {
      dep_list.push_back(operand);
    }
  }
  if (dep_list.size()) {
    connectOpToItsDepList(op, dep_list, g, dep_ctx);
  }
}

// Connect an async op to ops in its dependency list
void dependencyCanonicalizer::connectOpToItsDepList(
    Operation *op, SmallVector<Value, 1> dep_list, Graph &g,
    dependencyContext dep_ctx) {
  auto dst_v = getVertexFromOp(op, dep_ctx, "front").first;
  if (dep_list.size()) {
    for (auto dep_token : dep_list) {
      auto src_vector = traceOpFromToken(op, dep_token);
      if (src_vector.size()) {
        for (auto src_op : src_vector) {
          auto src_v = getVertexFromOp(src_op, dep_ctx, "back").first;
          if (!edge(src_v, dst_v, g).second) {
            add_edge(src_v, dst_v, g);
          }
        }
      }
    }
  }
}

// Trace op from a token in dependency list
std::vector<Operation *>
dependencyCanonicalizer::traceOpFromToken(Operation *op, Value dep_token) {
  std::vector<Operation *> output;
  // If dependency token is the init arg of an scf parallel loop
  // Note: checking for scf parallel first here, because its init_val is not its
  // block argument
  if (auto parallelop = getParallelRegionInitValsOwner(op, dep_token)) {
    output.push_back(parallelop);
    return output;
  }
  // Else if dependency token is the iter arg of an scf for loop
  else if (auto forop = getForRegionIterArgsOwner(dep_token)) {
    output.push_back(forop);
    return output;
  }
  // Else if dependency token originates from async op
  else if (dep_token.getDefiningOp() &&
           mlir::dyn_cast<xilinx::air::AsyncOpInterface>(
               dep_token.getDefiningOp())) {
    output.push_back(dep_token.getDefiningOp());
    return output;
  }
  // Else if dependency token is yielded from scf.for
  else if (dep_token.getDefiningOp() &&
           dyn_cast<scf::ForOp>(dep_token.getDefiningOp())) {
    auto forop = dyn_cast<scf::ForOp>(dep_token.getDefiningOp());
    auto forop_terminator = forop.getBody()->getTerminator();
    output.push_back(forop_terminator);
    return output;
  }
  // Else if dependency token is yielded from scf.parallel
  else if (dep_token.getDefiningOp() &&
           dyn_cast<scf::ParallelOp>(dep_token.getDefiningOp())) {
    auto parallelop = dyn_cast<scf::ParallelOp>(dep_token.getDefiningOp());
    for (auto parallelop_reduceop : parallelop.getOps<scf::ReduceOp>()) {
      output.push_back(parallelop_reduceop);
      return output;
    }
  }
  // Else if dependency token is from affine if (joint token from multiple ops)
  else if (dep_token.getDefiningOp() &&
           dyn_cast<mlir::AffineIfOp>(dep_token.getDefiningOp())) {
    auto aifop = dyn_cast<mlir::AffineIfOp>(dep_token.getDefiningOp());
    // The first then block
    auto then_terminator = aifop.getThenBlock()->getTerminator();
    for (auto operand : then_terminator->getOperands()) {
      if (auto op = operand.getDefiningOp()) {
        output.push_back(op);
      }
    }
    // Recursion
    mlir::AffineIfOp current_aif = aifop;
    while (getAffineIfInBlock(current_aif.getElseBlock())) {
      auto child_aif_op = getAffineIfInBlock(current_aif.getElseBlock());
      auto child_aif_terminator = child_aif_op.getThenBlock()->getTerminator();
      for (auto operand : child_aif_terminator->getOperands()) {
        if (auto op = operand.getDefiningOp()) {
          output.push_back(op);
        }
      }
      current_aif = child_aif_op;
    }
    // The last else block
    auto last_else_terminator = current_aif.getElseBlock()->getTerminator();
    for (auto operand : last_else_terminator->getOperands()) {
      if (auto op = operand.getDefiningOp()) {
        output.push_back(op);
      }
    }
    return output;
  }
  return output;
}

// Connects launch, partition and herd terminators
void dependencyCanonicalizer::connectTerminatorInGraph(Graph &g) {
  auto vp = boost::vertices(g);
  Graph::vertex_descriptor terminator_v = 0;
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    if (g[*vit].asyncEventType == "hierarchy_terminator") {
      terminator_v = *vit;
    }
  }
  if (terminator_v == 0)
    return;
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    if ((terminator_v != *vit) && !out_degree(*vit, g) &&
        (g[*vit].asyncEventType != "start")) {
      add_edge(*vit, terminator_v, g);
    }
  }
}

// Create start node for graph
void dependencyCanonicalizer::connectStartNodeInCommandGraph(
    dependencyGraph &G) {
  auto v = G.start_vertex;
  auto vp = boost::vertices(G.g);
  for (auto vit = vp.first; vit != vp.second; ++vit) {
    if ((v != *vit) && !in_degree(*vit, G.g)) {
      add_edge(v, *vit, G.g);
    }
  }
}

// Adds pointer from command graph to launch, partition and herd terminators
void dependencyCanonicalizer::updatePointerFromGraphToHierarchyTerminator(
    dependencyGraph &G) {
  auto vp = boost::vertices(G.g);
  for (auto v = vp.first; v != vp.second; ++v) {
    if (G.g[*v].asyncEventType == "hierarchy_terminator") {
      G.terminator_vertex = *v;
      return;
    }
  }
}

// Adds pointer from hierarchy terminator to parent command graph
void dependencyCanonicalizer::updatePointerFromHierarchyTerminatorToGraph(
    dependencyGraph &G, dependencyGraph &subG) {
  auto vp = boost::vertices(subG.g);
  for (auto v = vp.first; v != vp.second; ++v) {
    if (subG.g[*v].asyncEventType == "hierarchy_terminator") {
      subG.g[*v].nextDependencyGraph = &G;
      return;
    }
  }
}

// Adds pointer from hierarchy op to sub command graph
void dependencyCanonicalizer::updatePointerFromHierarchyOpToGraph(
    dependencyGraph &G) {
  unsigned idx = 0;
  auto vp = boost::vertices(G.g);
  for (auto v = vp.first; v != vp.second; ++v) {
    if (G.g[*v].asyncEventType == "hierarchy") {
      G.g[*v].nextDependencyGraph = &(G.subgraphs[idx]);
      idx++;
    }
  }
  assert(idx == G.subgraphs.size() &&
         "mismatch between # graphs and hierarchy ops");
}

// Dump graphviz
void dependencyCanonicalizer::dump_graph(std::string filename, Graph G) {
  std::ofstream ofs(filename, std::ofstream::out);
  boost::dynamic_properties dp;
  dp.property("label", boost::get(&dependencyNodeEntry::asyncEventName, G));
  dp.property("color", boost::get(&dependencyNodeEntry::color, G));
  dp.property("shape", boost::get(&dependencyNodeEntry::shape, G));
  dp.property("node_id", boost::get(boost::vertex_index, G));
  dp.property("style", boost::make_constant_property<Graph::vertex_descriptor>(
                           +"filled"));
  write_graphviz_dp(ofs, G, dp);
}

// Perform transitive reduction to canonicalize the dependency graph
void dependencyCanonicalizer::canonicalizeGraphs(
    dependencyGraph &global_graph, dependencyGraph &tr_graph,
    vertex_to_vertex_map_tree &g_to_tr, bool dump_dot, std::string dump_dir) {

  // Construct empty post-canonicalization dependency graph, tr_graph
  for (auto &launchGraph : global_graph.subgraphs) {
    tr_graph.subgraphs.push_back(dependencyGraph(launchGraph.hierarchyOp));
    dependencyGraph *current_launch_graph = &(tr_graph.subgraphs.back());
    g_to_tr.submaps.push_back(vertex_to_vertex_map_tree());
    vertex_to_vertex_map_tree *current_launch_g_to_tr =
        &(g_to_tr.submaps.back());
    for (auto &partitionGraph : launchGraph.subgraphs) {
      current_launch_graph->subgraphs.push_back(
          dependencyGraph(partitionGraph.hierarchyOp));
      dependencyGraph *current_partition_graph =
          &(current_launch_graph->subgraphs.back());
      current_launch_g_to_tr->submaps.push_back(vertex_to_vertex_map_tree());
      vertex_to_vertex_map_tree *current_partition_g_to_tr =
          &(current_launch_g_to_tr->submaps.back());
      for (auto &herdGraph : partitionGraph.subgraphs) {
        current_partition_graph->subgraphs.push_back(
            dependencyGraph(herdGraph.hierarchyOp));
        current_partition_g_to_tr->submaps.push_back(
            vertex_to_vertex_map_tree());
      }
    }
  }

  // Transitive reduction
  auto global_size = global_graph.subgraphs.size();
  assert(global_size == tr_graph.subgraphs.size() &&
         "graph tree size mismatch");
  assert(global_size == g_to_tr.submaps.size() &&
         "graph tree size and map size mismatch");
  boostTransitiveReductionImpl(global_graph.g, tr_graph.g, g_to_tr.a_to_b,
                               g_to_tr.b_to_a);
  for (unsigned i = 0; i < global_size; i++) {
    auto &launchGraph = global_graph.subgraphs[i];
    auto &trLaunchGraph = tr_graph.subgraphs[i];
    auto launch_size = launchGraph.subgraphs.size();
    auto launchMap = g_to_tr.submaps[i];
    assert(launch_size == trLaunchGraph.subgraphs.size() &&
           "graph tree size mismatch");
    assert(launch_size == launchMap.submaps.size() &&
           "graph tree size and map size mismatch");
    boostTransitiveReductionImpl(launchGraph.g, trLaunchGraph.g,
                                 launchMap.a_to_b, launchMap.b_to_a);
    for (unsigned j = 0; j < launch_size; j++) {
      auto &partitionGraph = launchGraph.subgraphs[j];
      auto &trPartitionGraph = trLaunchGraph.subgraphs[j];
      auto partition_size = partitionGraph.subgraphs.size();
      auto partitionMap = launchMap.submaps[j];
      assert(partition_size == trPartitionGraph.subgraphs.size() &&
             "graph tree size mismatch");
      assert(partition_size == partitionMap.submaps.size() &&
             "graph tree size and map size mismatch");
      boostTransitiveReductionImpl(partitionGraph.g, trPartitionGraph.g,
                                   partitionMap.a_to_b, partitionMap.b_to_a);
      for (unsigned k = 0; k < partition_size; k++) {
        auto &herdGraph = partitionGraph.subgraphs[k];
        auto &trHerdGraph = trPartitionGraph.subgraphs[k];
        auto herdMap = partitionMap.submaps[k];
        boostTransitiveReductionImpl(herdGraph.g, trHerdGraph.g, herdMap.a_to_b,
                                     herdMap.b_to_a);
      }
    }
  }

  if (dump_dot) {
    // Dump dot graphs
    dumpDotGraphFiles(tr_graph, dump_dir);
  }
}

void dependencyCanonicalizer::dumpDotGraphFiles(dependencyGraph global_graph,
                                                std::string dump_dir) {
  // Dump dot graphs
  if (dump_dir != "") {
    int status = mkdir(dump_dir.c_str(), 0777);
    if ((status < 0) && (errno != EEXIST))
      dump_dir = ""; // Failed to create dir
  }
  dump_graph(dump_dir + "host.dot", global_graph.g);
  int i = 0;
  for (auto G_l : global_graph.subgraphs) {
    std::string name = xilinx::air::to_string(G_l.hierarchyOp) + "_" +
                       std::to_string(++i) + ".dot";
    dump_graph(dump_dir + name, G_l.g);
    int j = 0;
    for (auto G_p : G_l.subgraphs) {
      std::string name = xilinx::air::to_string(G_p.hierarchyOp) + "_" +
                         std::to_string(i) + "_" + std::to_string(++j) + ".dot";
      dump_graph(dump_dir + name, G_p.g);
      int k = 0;
      for (auto G_h : G_p.subgraphs) {
        std::string name = xilinx::air::to_string(G_h.hierarchyOp) + "_" +
                           std::to_string(i) + "_" + std::to_string(j) + "_" +
                           std::to_string(++k) + ".dot";
        dump_graph(dump_dir + name, G_h.g);
      }
    }
  }
}

void dependencyCanonicalizer::boostTransitiveReductionImpl(
    Graph &asyncExecuteGraph, Graph &asyncExecuteGraphTR,
    vertex_to_vertex_map &g_to_tr, vertex_to_vertex_map &tr_to_g) {

  std::vector<size_t> id_map(num_vertices(asyncExecuteGraph));
  std::iota(id_map.begin(), id_map.end(), 0u);

  boost::transitive_reduction(asyncExecuteGraph, asyncExecuteGraphTR,
                              boost::make_assoc_property_map(g_to_tr),
                              id_map.data());

  for (vertex_to_vertex_map::iterator i = g_to_tr.begin(); i != g_to_tr.end();
       ++i) {
    // Copy over graph properties
    asyncExecuteGraphTR[i->second].asyncEventName =
        asyncExecuteGraph[i->first].asyncEventName;
    asyncExecuteGraphTR[i->second].asyncEventType =
        asyncExecuteGraph[i->first].asyncEventType;
    asyncExecuteGraphTR[i->second].color = asyncExecuteGraph[i->first].color;
    asyncExecuteGraphTR[i->second].shape = asyncExecuteGraph[i->first].shape;
    asyncExecuteGraphTR[i->second].operationId =
        asyncExecuteGraph[i->first].operationId;
    asyncExecuteGraphTR[i->second].op = asyncExecuteGraph[i->first].op;
    // Build reverse map tr_to_g, for convenient vertex mapping
    tr_to_g[i->second] = i->first;
  }
}

// Update dependency list based on transformed boost graph
void dependencyCanonicalizer::updateDepList(func::FuncOp func,
                                            dependencyGraph &global_graph) {

  // Purge dependency list
  purgeAIRDepList(global_graph);
  for (auto &launchGraph : global_graph.subgraphs) {
    purgeAIRDepList(launchGraph);
    for (auto &partitionGraph : launchGraph.subgraphs) {
      purgeAIRDepList(partitionGraph);
      for (auto &herdGraph : partitionGraph.subgraphs) {
        purgeAIRDepList(herdGraph);
      }
    }
  }

  // Rewrite dependency list
  fillAIRDepListUsingGraphTR(global_graph);
  for (auto &launchGraph : global_graph.subgraphs) {
    fillAIRDepListUsingGraphTR(launchGraph);
    for (auto &partitionGraph : launchGraph.subgraphs) {
      fillAIRDepListUsingGraphTR(partitionGraph);
      for (auto &herdGraph : partitionGraph.subgraphs) {
        fillAIRDepListUsingGraphTR(herdGraph);
      }
    }
  }

  // Cleanup op ids. Only leave dma, execute and hierarchy ids
  func.walk([&](Operation *op) {
    if (auto dma_op = dyn_cast<air::DmaMemcpyInterface>(op)) {
    } else if (auto channel_op = dyn_cast<air::ChannelInterface>(op)) {
    } else if (auto hier_op = dyn_cast<air::HierarchyInterface>(op)) {
    } else {
      op->removeAttr("id");
    }
  });
}

void dependencyCanonicalizer::purgeAIRDepList(dependencyGraph &graph) {
  auto vp = boost::vertices(graph.g);
  for (auto dstTRVertex = vp.first; dstTRVertex != vp.second; ++dstTRVertex) {
    auto op = graph.g[*dstTRVertex].op;
    if (!op)
      continue;
    auto async_op = mlir::dyn_cast<xilinx::air::AsyncOpInterface>(op);
    if (!async_op)
      continue;
    auto dependency_list = async_op.getAsyncDependencies();
    for (int i = dependency_list.size() - 1; i >= 0; i--) {
      async_op.eraseAsyncDependency(i);
    }
  }
}

void dependencyCanonicalizer::fillAIRDepListUsingGraphTR(
    dependencyGraph &graph) {
  auto vp = boost::vertices(graph.g);
  for (auto dstTRVertex = vp.first; dstTRVertex != vp.second; ++dstTRVertex) {
    auto op = graph.g[*dstTRVertex].op;
    if (!op)
      continue;
    auto async_op = mlir::dyn_cast<xilinx::air::AsyncOpInterface>(op);
    if (!async_op)
      continue;
    auto incoming_deps = in_edges(*dstTRVertex, graph.g);
    for (in_edge_iterator it = incoming_deps.first; it != incoming_deps.second;
         it++) {
      auto TRVertex = source(*it, graph.g);
      auto src_op = graph.g[TRVertex].op;
      if (src_op && op != src_op) { // Avoid dep to itself
        if (graph.g[TRVertex].asyncEventType == "for_loop") {
          auto value = dyn_cast<scf::ForOp>(src_op).getRegionIterArgs()[0];
          async_op.addAsyncDependency(value);
        } else if (graph.g[TRVertex].asyncEventType == "parallel_loop") {
          auto value = dyn_cast<scf::ParallelOp>(src_op).getInitVals()[0];
          async_op.addAsyncDependency(value);
        } else if (graph.g[TRVertex].asyncEventType == "terminator") {
          auto parent_op = src_op->getParentOp();
          auto value = parent_op->getResult(0);
          async_op.addAsyncDependency(value);
        } else if (auto async_src_op =
                       dyn_cast<xilinx::air::AsyncOpInterface>(src_op)) {
          // Elevate src token if src op is in affine if
          // if (auto parent_affine_if_op =
          //         dyn_cast<mlir::AffineIfOp>(src_op->getParentOp())) {
          //   src_op = parent_affine_if_op.getOperation();
          // }
          while (dyn_cast<mlir::AffineIfOp>(src_op->getParentOp())) {
            auto parent_affine_if_op =
                dyn_cast<mlir::AffineIfOp>(src_op->getParentOp());
            src_op = parent_affine_if_op.getOperation();
          }
          async_op.addAsyncDependency(src_op->getResult(0));
        }
      }
    }
  }
}

// Remove repetitions in dependency lists
void dependencyCanonicalizer::removeDepListRepitition(func::FuncOp func) {
  func.walk([&](Operation *op) {
    if (auto async_op = dyn_cast<air::AsyncOpInterface>(op)) {
      if (async_op.getAsyncDependencies().size() >= 1) {
        auto dependency_list = async_op.getAsyncDependencies();
        // Initialize repetition mask
        std::vector<bool> hasRepeat;
        for (auto i = dependency_list.begin(); i != dependency_list.end();
             ++i) {
          hasRepeat.push_back(false);
        }
        // Iterate the dependency list
        for (unsigned i = 0; i < dependency_list.size(); i++) {
          for (unsigned j = i + 1; j < dependency_list.size(); j++) {
            if (dependency_list[i] == dependency_list[j]) {
              hasRepeat[j] = true;
            }
          }
        }
        for (int i = dependency_list.size() - 1; i >= 0; i--) {
          if (hasRepeat[i]) {
            async_op.eraseAsyncDependency(i);
          }
        }
      }
    }
  });
}

// Remove unused air.execute ops
void dependencyCanonicalizer::removeUnusedExecuteOp(func::FuncOp func) {
  SmallVector<air::ExecuteOp, 1> erased_ops;
  func.walk([&](air::ExecuteOp op) {
    if (op->getNumResults() == 2) {
      auto result = op->getResult(1);
      if (result.use_empty()) {
        erased_ops.push_back(op);
      }
    }
  });

  for (auto op : erased_ops) {
    for (auto user : op.getAsyncToken().getUsers()) {
      if (auto async_user = dyn_cast<air::AsyncOpInterface>(user)) {
        eraseAsyncDependencyFromAsyncOp(async_user, op.getAsyncToken());
      }
    }
    assert(op.getAsyncToken().use_empty());
    op->erase();
  }
}

// Remove wait_all ops which contain only a single operand
void dependencyCanonicalizer::removeRedundantWaitAllOps(func::FuncOp func) {
  func.walk([&](Operation *op) {
    if (air::WaitAllOp wa_op = dyn_cast<air::WaitAllOp>(op)) {
      if (wa_op.getAsyncDependencies().size() == 1) {
        wa_op.getAsyncToken().replaceAllUsesWith(
            wa_op.getAsyncDependencies()[0]);
        wa_op.erase();
      } else {
        wa_op->removeAttr("id");
      }
    }
  });
}

// air.hierarchy ops should only depend on scf loop ops
void dependencyCanonicalizer::canonicalizeAIRHierarchyDependency(
    func::FuncOp func) {
  func.walk([&](air::HierarchyInterface hier) {
    if (dyn_cast<air::LaunchOp>(hier.getOperation())) {
      // air.launch is strictly synchronous
      return;
    }
    auto async_hier = dyn_cast<air::AsyncOpInterface>(hier.getOperation());
    SmallVector<Value, 1> erased_tokens;
    // Add dependency to any control events involving this hierarchy op
    SmallVector<Value, 1> control_token_history;
    std::vector<Operation *> op_history;
    traceDependentScfLoopToken(async_hier, control_token_history, op_history);
    for (auto token : control_token_history) {
      async_hier.addAsyncDependency(token);
    }
    // Erase non-control dependencies; air hierarchy ops should only depend on
    // control events
    for (auto dep : async_hier.getAsyncDependencies()) {
      if ((!getForRegionIterArgsOwner(dep)) &&
          (!getParallelRegionInitValsOwner(hier.getOperation(), dep))) {
        if (dep.getDefiningOp() &&
            (!dyn_cast<air::WaitAllOp>(dep.getDefiningOp()))) {
          erased_tokens.push_back(dep);
        }
      }
    }
    for (auto dep : erased_tokens) {
      eraseAsyncDependencyFromAsyncOp(async_hier, dep);
    }
  });
}

// Remove unused air.hierarchy arguments
void dependencyCanonicalizer::removeRedundantAIRHierarchyArgs(
    func::FuncOp func) {
  auto module = func->getParentOfType<ModuleOp>();
  OpBuilder builder(module);
  SmallVector<air::HierarchyInterface, 1> erased_ops;
  func.walk([&](air::HierarchyInterface hier) {
    // for (unsigned hier_operand_id = 0; hier_operand_id <
    // hier.getNumKernelOperands(); hier_operand_id++) {
    for (int hier_operand_id = hier.getNumKernelOperands() - 1;
         hier_operand_id >= 0; hier_operand_id--) {
      if (hier.getKernelArguments()[hier_operand_id].use_empty()) {
        eraseAIRHierarchyOperand(hier, hier_operand_id);
      }
    }
  });
}

//===----------------------------------------------------------------------===//
// Dependency tracing
//===----------------------------------------------------------------------===//

// Trace operand's uses at current scope
void dependencyTracer::pushDepsAtCurrentScope(mlir::Value operand,
                                              air::AsyncOpInterface op, char rw,
                                              partialMemref *tile) {
  assert(operand.getType().isa<MemRefType>() &&
         "operand being traced is not a memref");
  for (auto &u : operand.getUses()) {
    // If used in DmaMemcpy Op
    if (auto dma = dyn_cast<xilinx::air::DmaMemcpyInterface>(u.getOwner())) {
      // DMA2D: Need to check for overlapping partial memrefs in use
      unsigned numDimsSrc = dma.getNumDims();
      unsigned numDimsDst = dma.getNumDims();
      if (numDimsSrc == 0)
        numDimsSrc = dma.getSrcMemref().getType().cast<MemRefType>().getRank();
      if (numDimsDst == 0)
        numDimsDst = dma.getDstMemref().getType().cast<MemRefType>().getRank();
      SmallVector<Value, 2> src_indices;
      SmallVector<Value, 2> dst_indices;
      if (auto nddma =
              dyn_cast<xilinx::air::DmaMemcpyNdOp>(dma.getOperation())) {
        if (nddma.getSrcOffsets().size()) {
          for (unsigned i = 0; i < numDimsSrc; i++) {
            src_indices.push_back(nddma.getSrcOffsets()[i]);
          }
        } else {
          for (unsigned i = 0; i < numDimsSrc; i++) {
            src_indices.push_back(nullptr);
          }
        }
        if (nddma.getDstOffsets().size()) {
          for (unsigned i = 0; i < numDimsDst; i++) {
            dst_indices.push_back(nddma.getDstOffsets()[i]);
          }
        } else {
          for (unsigned i = 0; i < numDimsDst; i++) {
            dst_indices.push_back(nullptr);
          }
        }
      } else {
        for (unsigned i = 0; i < numDimsSrc; i++) {
          src_indices.push_back(dma.getSrcMemrefDim(i));
        }
        for (unsigned i = 0; i < numDimsDst; i++) {
          dst_indices.push_back(dma.getDstMemrefDim(i));
        }
      }
      partialMemref dma_src =
          createPartialMemref(dma.getSrcMemref(), numDimsSrc, src_indices);
      partialMemref dma_dst =
          createPartialMemref(dma.getDstMemref(), numDimsDst, dst_indices);

      if (rw == 'r') {
        if (u.is(dma.getSrcMemref())) {
          if (tile == nullptr) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          } else if (areEqualIndexPartialMemrefs(tile, &dma_src)) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          }
        }
      } else if (rw == 'w') {
        if (u.is(dma.getDstMemref())) {
          if (tile == nullptr) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          } else if (areEqualIndexPartialMemrefs(tile, &dma_dst)) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          }
        }
      } else {
        if (tile == nullptr) {
          addDependencyBetweenOps(dma.getOperation(), op.getOperation());
        } else if (u.is(dma.getDstMemref())) {
          if (areEqualIndexPartialMemrefs(tile, &dma_dst)) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          }
        } else if (u.is(dma.getSrcMemref())) {
          if (areEqualIndexPartialMemrefs(tile, &dma_src)) {
            addDependencyBetweenOps(dma.getOperation(), op.getOperation());
          }
        }
      }
    }
    // Else if used in Channel Op
    else if (auto channel =
                 dyn_cast<xilinx::air::ChannelInterface>(u.getOwner())) {
      // Channel op: Need to check for overlapping partial memrefs in use
      if (auto channel_put =
              dyn_cast<xilinx::air::ChannelPutOp>(channel.getOperation())) {
        unsigned numDimsSrc =
            channel_put.getSrcMemref().getType().cast<MemRefType>().getRank();
        SmallVector<Value, 2> src_indices;
        if (channel_put.getSrcOffsets().size()) {
          for (unsigned i = 0; i < numDimsSrc; i++) {
            src_indices.push_back(channel_put.getSrcOffsets()[i]);
          }
        } else {
          for (unsigned i = 0; i < numDimsSrc; i++) {
            src_indices.push_back(nullptr);
          }
        }
        partialMemref channel_put_src = createPartialMemref(
            channel_put.getSrcMemref(), numDimsSrc, src_indices);

        if (rw == 'r') {
          if (u.is(channel_put.getSrcMemref())) {
            if (tile == nullptr) {
              addDependencyBetweenOps(channel_put.getOperation(),
                                      op.getOperation());
            } else if (areEqualIndexPartialMemrefs(tile, &channel_put_src)) {
              addDependencyBetweenOps(channel_put.getOperation(),
                                      op.getOperation());
            }
          }
        } else if (rw == 'w') {
        } else {
          if (tile == nullptr) {
            addDependencyBetweenOps(channel_put.getOperation(),
                                    op.getOperation());
          } else if (u.is(channel_put.getSrcMemref())) {
            if (areEqualIndexPartialMemrefs(tile, &channel_put_src)) {
              addDependencyBetweenOps(channel_put.getOperation(),
                                      op.getOperation());
            }
          }
        }
      } else if (auto channel_get = dyn_cast<xilinx::air::ChannelGetOp>(
                     channel.getOperation())) {
        unsigned numDimsDst =
            channel_get.getDstMemref().getType().cast<MemRefType>().getRank();
        SmallVector<Value, 2> dst_indices;
        if (channel_get.getDstOffsets().size()) {
          for (unsigned i = 0; i < numDimsDst; i++) {
            dst_indices.push_back(channel_get.getDstOffsets()[i]);
          }
        } else {
          for (unsigned i = 0; i < numDimsDst; i++) {
            dst_indices.push_back(nullptr);
          }
        }
        partialMemref channel_get_dst = createPartialMemref(
            channel_get.getDstMemref(), numDimsDst, dst_indices);

        if (rw == 'r') {
        } else if (rw == 'w') {
          if (u.is(channel_get.getDstMemref())) {
            if (tile == nullptr) {
              addDependencyBetweenOps(channel_get.getOperation(),
                                      op.getOperation());
            } else if (areEqualIndexPartialMemrefs(tile, &channel_get_dst)) {
              addDependencyBetweenOps(channel_get.getOperation(),
                                      op.getOperation());
            }
          }
        } else {
          if (tile == nullptr) {
            addDependencyBetweenOps(channel_get.getOperation(),
                                    op.getOperation());
          } else if (u.is(channel_get.getDstMemref())) {
            if (areEqualIndexPartialMemrefs(tile, &channel_get_dst)) {
              addDependencyBetweenOps(channel_get.getOperation(),
                                      op.getOperation());
            }
          }
        }
      } else
        assert(false && "Unknown air channel op");
    }

    // If used in a linalg op
    else if (auto linalgop = mlir::dyn_cast<linalg::LinalgOp>(u.getOwner())) {
      if (auto ar = dyn_cast<xilinx::air::ExecuteOp>(linalgop->getParentOp())) {
        if (rw == 'r') {
          if (u.getOperandNumber() <
              linalgop.getNumDpsInputs() + linalgop.getNumDpsInits()) {
            addDependencyBetweenOps(ar.getOperation(), op.getOperation());
          }
        } else if (rw == 'w') {
          if (u.getOperandNumber() >= linalgop.getNumDpsInputs() &&
              u.getOperandNumber() - linalgop.getNumDpsInputs() <
                  linalgop.getNumDpsInits()) {
            addDependencyBetweenOps(ar.getOperation(), op.getOperation());
          }
        } else {
          addDependencyBetweenOps(ar.getOperation(), op.getOperation());
        }
      }
    }

    // If used in hierarchy op
    else if (auto hier =
                 dyn_cast<xilinx::air::HierarchyInterface>(u.getOwner())) {
      // check if the use inside hierarchy op matches with the tracing mode
      // (r or w)
      for (unsigned hier_argument_id = 0;
           hier_argument_id < hier.getNumKernelOperands(); hier_argument_id++) {
        if (u.is(hier.getKernelOperand(hier_argument_id))) {
          auto child_op = hier.getKernelArgument(hier_argument_id);
          char rw_check = checkOperandReadOrWrite(child_op);
          if (rw == 'n' || rw_check == rw) {
            addDependencyBetweenOps(hier.getOperation(), op.getOperation());
          }
        }
      }
    }

    // If used in an unknown op
    else {
      auto unknownop = u.getOwner();
      if (auto ar =
              dyn_cast<xilinx::air::ExecuteOp>(unknownop->getParentOp())) {
        addDependencyBetweenOps(ar.getOperation(), op.getOperation());
      }
    }
  }
}

// Create partial memref tile
partialMemref dependencyTracer::createPartialMemref(mlir::Value memrefValue,
                                                    unsigned numDims) {
  partialMemref tile;
  tile.memrefValue = memrefValue;
  tile.numDims = numDims;
  for (unsigned i = 0; i < numDims; i++) {
    tile.memrefIndices.push_back(nullptr);
  }
  return tile;
}
partialMemref
dependencyTracer::createPartialMemref(mlir::Value memrefValue, unsigned numDims,
                                      SmallVector<Value, 2> memrefIndices) {
  partialMemref tile;
  tile.memrefValue = memrefValue;
  tile.numDims = numDims;
  for (unsigned i = 0; i < numDims; i++) {
    tile.memrefIndices.push_back(memrefIndices[i]);
  }
  return tile;
}

// Get partial memref tiles from op
void dependencyTracer::getPartialMemrefFromOp(
    Operation *sink_op, SmallVector<partialMemref, 1> &sink_op_memref_reads,
    SmallVector<partialMemref, 1> &sink_op_memref_writes,
    SmallVector<Value, 1> &sink_op_scalar_ins,
    SmallVector<Value, 1> &sink_op_scalar_outs) {

  // If the sink op is linalg op
  if (auto sink_op_linalgop = dyn_cast<linalg::LinalgOp>(sink_op)) {
    for (auto linalg_ins : sink_op_linalgop.getDpsInputOperands()) {
      auto ins_value = linalg_ins->get();
      if (ins_value.getType().isa<MemRefType>()) {
        unsigned memRefRank = ins_value.getType().cast<MemRefType>().getRank();
        partialMemref tile = createPartialMemref(ins_value, memRefRank);
        sink_op_memref_reads.push_back(tile);
      } else if (ins_value.getType().isa<IndexType>()) {
        sink_op_scalar_ins.push_back(ins_value);
      }
    }
    for (auto linalg_outs : sink_op_linalgop.getDpsInitOperands()) {
      auto outs_value = linalg_outs->get();
      if (outs_value.getType().isa<MemRefType>()) {
        unsigned memRefRank = outs_value.getType().cast<MemRefType>().getRank();
        partialMemref tile = createPartialMemref(outs_value, memRefRank);
        sink_op_memref_reads.push_back(
            tile); // linalg op both reads and writes the output memref
        sink_op_memref_writes.push_back(tile);
      } else if (outs_value.getType().isa<IndexType>()) {
        sink_op_scalar_ins.push_back(outs_value); // linalg op both reads and
                                                  // writes the output memref
        sink_op_scalar_outs.push_back(outs_value);
      }
    }
    if (sink_op_linalgop->getNumResults()) {
      for (auto linalg_results : sink_op_linalgop->getResults()) {
        if (linalg_results.getType().isa<MemRefType>()) {
          unsigned memRefRank =
              linalg_results.getType().cast<MemRefType>().getRank();
          partialMemref tile = createPartialMemref(linalg_results, memRefRank);
          sink_op_memref_writes.push_back(tile);
        } else if (linalg_results.getType().isa<IndexType>()) {
          sink_op_scalar_outs.push_back(linalg_results);
        }
      }
    }
  }

  // If the sink op is memref::dealloc
  else if (auto sink_op_memdealloc = dyn_cast<memref::DeallocOp>(sink_op)) {
    unsigned memRefRank =
        sink_op_memdealloc.getMemref().getType().cast<MemRefType>().getRank();
    partialMemref tile =
        createPartialMemref(sink_op_memdealloc.getMemref(), memRefRank);
    sink_op_memref_reads.push_back(tile);
    sink_op_memref_writes.push_back(
        tile); // dealloc erases (i.e. writes to) output memref
  }

  // If the sink op is memref::copy
  else if (auto sink_op_memref_copy = dyn_cast<memref::CopyOp>(sink_op)) {
    unsigned memRefRankSrc =
        sink_op_memref_copy.getSource().getType().cast<MemRefType>().getRank();
    partialMemref tileSrc =
        createPartialMemref(sink_op_memref_copy.getSource(), memRefRankSrc);
    sink_op_memref_reads.push_back(tileSrc);
    unsigned memRefRankDst =
        sink_op_memref_copy.getTarget().getType().cast<MemRefType>().getRank();
    partialMemref tileDst =
        createPartialMemref(sink_op_memref_copy.getTarget(), memRefRankDst);
    sink_op_memref_reads.push_back(tileDst);
    sink_op_memref_writes.push_back(tileDst);
  }

  // If the sink op is an air::DmaMemcpy op
  else if (auto sink_op_dma =
               mlir::dyn_cast<xilinx::air::DmaMemcpyInterface>(sink_op)) {
    SmallVector<Value, 2> src_indices;
    SmallVector<Value, 2> dst_indices;
    unsigned numDimsSrc = sink_op_dma.getNumDims();
    unsigned numDimsDst = sink_op_dma.getNumDims();
    // air.dmamemcpynd op has unknown # of dims (thus numdims defaults to
    // 0)
    if (numDimsSrc == 0) {
      numDimsSrc =
          sink_op_dma.getSrcMemref().getType().cast<MemRefType>().getRank();
      numDimsDst =
          sink_op_dma.getDstMemref().getType().cast<MemRefType>().getRank();
    }
    // Special case with ND DMA op
    if (auto sink_op_nddma = dyn_cast<air::DmaMemcpyNdOp>(sink_op)) {
      // air.dmamemcpynd op has extra scalar operands
      for (unsigned i = 0; i < sink_op_nddma.getDstOffsets().size(); i++)
        sink_op_scalar_outs.push_back(sink_op_nddma.getDstOffsets()[i]);
      for (unsigned i = 0; i < sink_op_nddma.getDstSizes().size(); i++)
        sink_op_scalar_outs.push_back(sink_op_nddma.getDstSizes()[i]);
      for (unsigned i = 0; i < sink_op_nddma.getDstStrides().size(); i++)
        sink_op_scalar_outs.push_back(sink_op_nddma.getDstStrides()[i]);
      for (unsigned i = 0; i < sink_op_nddma.getSrcOffsets().size(); i++)
        sink_op_scalar_ins.push_back(sink_op_nddma.getSrcOffsets()[i]);
      for (unsigned i = 0; i < sink_op_nddma.getSrcSizes().size(); i++)
        sink_op_scalar_ins.push_back(sink_op_nddma.getSrcSizes()[i]);
      for (unsigned i = 0; i < sink_op_nddma.getSrcStrides().size(); i++)
        sink_op_scalar_ins.push_back(sink_op_nddma.getSrcStrides()[i]);
      if (sink_op_nddma.getSrcOffsets().size()) {
        for (unsigned i = 0; i < numDimsSrc; i++) {
          src_indices.push_back(sink_op_nddma.getSrcOffsets()[i]);
        }
      } else {
        for (unsigned i = 0; i < numDimsSrc; i++) {
          src_indices.push_back(nullptr);
        }
      }
      if (sink_op_nddma.getDstOffsets().size()) {
        for (unsigned i = 0; i < numDimsDst; i++) {
          dst_indices.push_back(sink_op_nddma.getDstOffsets()[i]);
        }
      } else {
        for (unsigned i = 0; i < numDimsDst; i++) {
          dst_indices.push_back(nullptr);
        }
      }
    } else {
      for (unsigned i = 0; i < numDimsSrc; i++) {
        sink_op_scalar_ins.push_back(sink_op_dma.getSrcMemrefDim(i));
        src_indices.push_back(sink_op_dma.getSrcMemrefDim(i));
      }
      for (unsigned i = 0; i < numDimsDst; i++) {
        sink_op_scalar_outs.push_back(sink_op_dma.getDstMemrefDim(i));
        dst_indices.push_back(sink_op_dma.getDstMemrefDim(i));
      }
    }
    partialMemref tile_in = createPartialMemref(sink_op_dma.getSrcMemref(),
                                                numDimsSrc, src_indices);
    sink_op_memref_reads.push_back(tile_in);
    partialMemref tile_out = createPartialMemref(sink_op_dma.getDstMemref(),
                                                 numDimsDst, dst_indices);
    sink_op_memref_writes.push_back(tile_out);
  }

  // If the sink op is channel put
  else if (auto sink_op_channel_put = dyn_cast<air::ChannelPutOp>(sink_op)) {
    unsigned numDimsSrc = sink_op_channel_put.getSrcMemref()
                              .getType()
                              .cast<MemRefType>()
                              .getRank();
    for (unsigned i = 0; i < sink_op_channel_put.getSrcOffsets().size(); i++)
      sink_op_scalar_ins.push_back(sink_op_channel_put.getSrcOffsets()[i]);
    for (unsigned i = 0; i < sink_op_channel_put.getSrcSizes().size(); i++)
      sink_op_scalar_ins.push_back(sink_op_channel_put.getSrcSizes()[i]);
    for (unsigned i = 0; i < sink_op_channel_put.getSrcStrides().size(); i++)
      sink_op_scalar_ins.push_back(sink_op_channel_put.getSrcStrides()[i]);
    SmallVector<Value, 2> src_indices;
    if (sink_op_channel_put.getSrcOffsets().size()) {
      for (unsigned i = 0; i < numDimsSrc; i++) {
        src_indices.push_back(sink_op_channel_put.getSrcOffsets()[i]);
      }
    } else {
      for (unsigned i = 0; i < numDimsSrc; i++) {
        src_indices.push_back(nullptr);
      }
    }
    partialMemref tile_in = createPartialMemref(
        sink_op_channel_put.getSrcMemref(), numDimsSrc, src_indices);
    sink_op_memref_reads.push_back(tile_in);
  }

  // If the sink op is channel get
  else if (auto sink_op_channel_get = dyn_cast<air::ChannelGetOp>(sink_op)) {
    unsigned numDimsDst = sink_op_channel_get.getDstMemref()
                              .getType()
                              .cast<MemRefType>()
                              .getRank();
    for (unsigned i = 0; i < sink_op_channel_get.getDstOffsets().size(); i++)
      sink_op_scalar_outs.push_back(sink_op_channel_get.getDstOffsets()[i]);
    for (unsigned i = 0; i < sink_op_channel_get.getDstSizes().size(); i++)
      sink_op_scalar_outs.push_back(sink_op_channel_get.getDstSizes()[i]);
    for (unsigned i = 0; i < sink_op_channel_get.getDstStrides().size(); i++)
      sink_op_scalar_outs.push_back(sink_op_channel_get.getDstStrides()[i]);
    SmallVector<Value, 2> dst_indices;
    if (sink_op_channel_get.getDstOffsets().size()) {
      for (unsigned i = 0; i < numDimsDst; i++) {
        dst_indices.push_back(sink_op_channel_get.getDstOffsets()[i]);
      }
    } else {
      for (unsigned i = 0; i < numDimsDst; i++) {
        dst_indices.push_back(nullptr);
      }
    }
    partialMemref tile_out = createPartialMemref(
        sink_op_channel_get.getDstMemref(), numDimsDst, dst_indices);
    sink_op_memref_writes.push_back(tile_out);
  }

  // If the sink op is arith::MulIOp
  else if (auto sink_op_arith = dyn_cast<arith::MulIOp>(sink_op)) {
    sink_op_scalar_ins.push_back(sink_op_arith.getLhs());
    sink_op_scalar_ins.push_back(sink_op_arith.getRhs());
    sink_op_scalar_outs.push_back(sink_op_arith.getResult());
  }

  // If the sink op is arith::AddIOp
  else if (auto sink_op_arith = dyn_cast<arith::AddIOp>(sink_op)) {
    sink_op_scalar_ins.push_back(sink_op_arith.getLhs());
    sink_op_scalar_ins.push_back(sink_op_arith.getRhs());
    sink_op_scalar_outs.push_back(sink_op_arith.getResult());
  }

  // If the sink op is mlir::AffineApplyOp
  else if (auto sink_op_apply = dyn_cast<mlir::AffineApplyOp>(sink_op)) {
    for (auto applyop_operand : sink_op_apply.getMapOperands()) {
      sink_op_scalar_ins.push_back(applyop_operand);
    }
    sink_op_scalar_outs.push_back(sink_op_apply.getResult());
  }

  // If the sink op is an unknown op
  else {
    for (auto sink_op_op : sink_op->getOperands()) {
      if (sink_op_op.getType().isa<MemRefType>()) {
        unsigned memRefRank = sink_op_op.getType().cast<MemRefType>().getRank();
        partialMemref tile = createPartialMemref(sink_op_op, memRefRank);
        sink_op_memref_reads.push_back(
            tile); // Assuming all operands are both read and written to
        sink_op_memref_writes.push_back(tile);
      } else if (sink_op_op.getType().isa<IndexType>()) {
        sink_op_scalar_ins.push_back(sink_op_op); // Assuming all operands are
                                                  // both read and written to
        sink_op_scalar_outs.push_back(sink_op_op);
      }
    }
    if (sink_op->getNumResults()) {
      for (auto sink_op_results : sink_op->getResults()) {
        if (sink_op_results.getType().isa<MemRefType>()) {
          unsigned memRefRank =
              sink_op_results.getType().cast<MemRefType>().getRank();
          partialMemref tile = createPartialMemref(sink_op_results, memRefRank);
          sink_op_memref_writes.push_back(tile);
        } else if (sink_op_results.getType().isa<IndexType>()) {
          sink_op_scalar_outs.push_back(sink_op_results);
        }
      }
    }
  }
}

// Add dependency edge
void dependencyTracer::addDependencyBetweenOps(Operation *source,
                                               Operation *sink) {
  auto async_sink = dyn_cast<air::AsyncOpInterface>(sink);
  assert(async_sink && "sink op has no async interface");
  if (source->getBlock() == sink->getBlock() && source->isBeforeInBlock(sink)) {
    if (auto async_source = dyn_cast<air::AsyncOpInterface>(source)) {
      addAsyncDependencyIfNew(async_sink, async_source.getAsyncToken());
      return;
    }
  }
  for (auto parent = source->getParentOp(); !isa<mlir::ModuleOp>(parent);
       parent = parent->getParentOp()) {
    if (parent->getBlock() == sink->getBlock() &&
        parent->isBeforeInBlock(sink)) {
      if (auto async_source = dyn_cast<air::AsyncOpInterface>(parent)) {
        addAsyncDependencyIfNew(async_sink, async_source.getAsyncToken());
        return;
      }
    }
  }
}

// Check if two partial memref tiles have identical indices
bool dependencyTracer::areEqualIndexPartialMemrefs(partialMemref *tile_0,
                                                   partialMemref *tile_1) {
  if (tile_0->numDims != tile_1->numDims) {
    // Unequal # dimensions
    return false;
  } else {
    for (unsigned i = 0; i < tile_0->numDims; i++) {
      if (!areEqualIndices(tile_0->memrefIndices[i], tile_1->memrefIndices[i]))
        return false;
    }
  }
  return true;
}

char dependencyTracer::checkOperandReadOrWrite(mlir::Value operand) {
  assert(operand.getType().isa<MemRefType>() &&
         "operand being traced is not a memref");
  bool foundWriteAccess = false;
  bool foundReadAccess = false;
  for (auto &u : operand.getUses()) {
    // If used in DmaMemcpy Op
    if (auto dma = dyn_cast<xilinx::air::DmaMemcpyInterface>(u.getOwner())) {
      if (u.is(dma.getSrcMemref())) {
        foundReadAccess = true;
      } else if (u.is(dma.getDstMemref())) {
        foundWriteAccess = true;
      } else {
        assert(false && "Unknown operand in air.dma");
      }
    }
    // If used in Channel Put Op
    else if (auto channel_put =
                 dyn_cast<xilinx::air::ChannelPutOp>(u.getOwner())) {
      if (u.is(channel_put.getSrcMemref())) {
        foundReadAccess = true;
      } else {
        assert(false && "Unknown operand in air.channel_put");
      }
    }
    // If used in Channel Get Op
    else if (auto channel_get =
                 dyn_cast<xilinx::air::ChannelGetOp>(u.getOwner())) {
      if (u.is(channel_get.getDstMemref())) {
        foundWriteAccess = true;
      } else {
        assert(false && "Unknown operand in air.channel_get");
      }
    }
    // If used in a linalg op
    else if (auto linalgop = mlir::dyn_cast<linalg::LinalgOp>(u.getOwner())) {
      if (u.getOperandNumber() <
          linalgop.getNumDpsInputs() + linalgop.getNumDpsInits()) {
        foundReadAccess = true;
      } else if (u.getOperandNumber() >= linalgop.getNumDpsInputs() &&
                 u.getOperandNumber() - linalgop.getNumDpsInputs() <
                     linalgop.getNumDpsInits()) {
        foundWriteAccess = true;
      } else {
        assert(false && "Unknown operand in linalg op");
      }
    }
    // If unknown op, then assume write access for safety
    else
      foundWriteAccess = true;
  }
  if (foundWriteAccess)
    return 'w';
  else if (foundReadAccess)
    return 'r';
  else
    return 'w';
}

void dependencyTracer::reconnectLoopCarriedDependencyFromOp(Operation *op) {
  // Get async sink op wrt op
  air::AsyncOpInterface async_op = nullptr;
  if (dyn_cast<air::AsyncOpInterface>(op)) {
    async_op = dyn_cast<air::AsyncOpInterface>(op);
  } else if (auto scf_par = dyn_cast<scf::ParallelOp>(op)) {
    auto token = getLoopCarriedTokenFromScfOp(scf_par);
    assert(token.getDefiningOp());
    async_op = dyn_cast<air::AsyncOpInterface>(token.getDefiningOp());
  } else if (auto scf_for = dyn_cast<scf::ForOp>(op)) {
    auto token = getLoopCarriedTokenFromScfOp(scf_for, "operand");
    assert(token.getDefiningOp());
    async_op = dyn_cast<air::AsyncOpInterface>(token.getDefiningOp());
  } else
    assert(false && "unsupported op for loop-carried dependency");

  assert(async_op);

  // Get parent scf loop op
  auto parent = op->getParentOp();
  if (auto scf_par = dyn_cast<scf::ParallelOp>(parent)) {
    // Get scf parallel's loop-carried token
    auto token = getLoopCarriedTokenFromScfOp(scf_par);

    // Connect dependency between op and token
    addAsyncDependencyIfNew(async_op, token);

    // Get scf parallel's wait_all op before reduce
    SmallVector<scf::ReduceOp, 1> reduce_ops;
    for (auto scf_par_reduce : scf_par.getOps<scf::ReduceOp>()) {
      reduce_ops.push_back(scf_par_reduce);
    }
    assert(reduce_ops.size() == 1);
    auto reduce_wait_all =
        dyn_cast<air::WaitAllOp>(reduce_ops[0].getOperand().getDefiningOp());
    assert(reduce_wait_all);

    // Connect op's async token to scf reduce
    addAsyncDependencyIfNew(reduce_wait_all, op->getResult(0));

    // Recurse with parent
    reconnectLoopCarriedDependencyFromOp(parent);
  } else if (auto scf_for = dyn_cast<scf::ForOp>(parent)) {
    // Get scf for's loop-carried token
    auto token = getLoopCarriedTokenFromScfOp(scf_for, "argument");

    // Connect dependency between op and token
    addAsyncDependencyIfNew(async_op, token);

    // Get scf for's wait_all op before yield
    auto scf_for_yield =
        dyn_cast<scf::YieldOp>(scf_for.getBody()->getTerminator());
    auto yield_wait_all =
        dyn_cast<air::WaitAllOp>(scf_for_yield.getOperand(0).getDefiningOp());
    assert(yield_wait_all);

    // Connect op's async token to scf reduce
    addAsyncDependencyIfNew(yield_wait_all, op->getResult(0));

    // Recurse with parent
    reconnectLoopCarriedDependencyFromOp(parent);
  } else
    return;
}

// Trace tile index deps
void dependencyTracer::traceTileIndices(
    SmallVector<partialMemref, 1> read_operands,
    SmallVector<partialMemref, 1> write_operands,
    SmallVector<Value, 1> in_scalars, SmallVector<Value, 1> out_scalars,
    air::AsyncOpInterface sink_air_op) {
  for (auto operand : read_operands) {
    for (unsigned i = 0; i < operand.numDims; i++) {
      pushTileIndexAsDep(operand.memrefIndices[i], sink_air_op);
    }
  }
  for (auto operand : write_operands) {
    for (unsigned i = 0; i < operand.numDims; i++) {
      pushTileIndexAsDep(operand.memrefIndices[i], sink_air_op);
    }
  }
  for (auto scalar : in_scalars) {
    pushTileIndexAsDep(scalar, sink_air_op);
  }
  for (auto scalar : out_scalars) {
    pushTileIndexAsDep(scalar, sink_air_op);
  }
}

// Add tile index deps to op
void dependencyTracer::pushTileIndexAsDep(mlir::Value tile_index,
                                          air::AsyncOpInterface op) {
  if (tile_index != nullptr) {
    // If tile_index is not a nullptr
    // If created by async_region
    if (auto defop = tile_index.getDefiningOp<air::ExecuteOp>()) {
      addAsyncDependencyIfNew(op, defop.getResult(0));
    }
    // If created by hierarchy (as loop iter)
    else if (auto hier = dyn_cast<air::HierarchyInterface>(
                 tile_index.getParentRegion()->getParentOp())) {
      for (auto id : hier.getIds()) {
        if (id == tile_index) {
          addAsyncDependencyIfNew(op, tile_index);
        }
      }
    }
  }
}

} // namespace air
} // namespace xilinx