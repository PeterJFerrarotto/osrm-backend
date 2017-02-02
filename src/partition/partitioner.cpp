#include "partition/partitioner.hpp"
#include "partition/bisection_graph.hpp"
#include "partition/recursive_bisection.hpp"

#include "storage/io.hpp"
#include "util/coordinate.hpp"

#include "util/log.hpp"

#include <iterator>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <boost/assert.hpp>

#include <tbb/task_scheduler_init.h>

#include "util/geojson_debug_logger.hpp"
#include "util/geojson_debug_policies.hpp"
#include "util/json_container.hpp"

namespace osrm
{
namespace partition
{

struct CompressedNodeBasedGraphEdge
{
    NodeID source;
    NodeID target;
};

struct CompressedNodeBasedGraph
{
    CompressedNodeBasedGraph(storage::io::FileReader &reader)
    {
        // Reads:  | Fingerprint | #e | #n | edges | coordinates |
        // - uint64: number of edges (from, to) pairs
        // - uint64: number of nodes and therefore also coordinates
        // - (uint32_t, uint32_t): num_edges * edges
        // - (int32_t, int32_t: num_nodes * coordinates (lon, lat)
        //
        // Gets written in Extractor::WriteCompressedNodeBasedGraph

        const auto num_edges = reader.ReadElementCount64();
        const auto num_nodes = reader.ReadElementCount64();

        edges.resize(num_edges);
        coordinates.resize(num_nodes);

        reader.ReadInto(edges);
        reader.ReadInto(coordinates);
    }

    std::vector<CompressedNodeBasedGraphEdge> edges;
    std::vector<util::Coordinate> coordinates;
};

CompressedNodeBasedGraph LoadCompressedNodeBasedGraph(const std::string &path)
{
    const auto fingerprint = storage::io::FileReader::VerifyFingerprint;
    storage::io::FileReader reader(path, fingerprint);

    CompressedNodeBasedGraph graph{reader};
    return graph;
}

struct NodeBasedGraphToEdgeBasedGraphMapping
{
    NodeBasedGraphToEdgeBasedGraphMapping(storage::io::FileReader &reader)
    {
        // Reads:  | Fingerprint | #mappings | u v head tail | u v head tail | ..
        // - uint64: number of mappings (u, v, head, tail) chunks
        // - NodeID u, NodeID v, EdgeID head, EdgeID tail
        //
        // Gets written in NodeBasedGraphToEdgeBasedGraphMappingWriter

        const auto num_mappings = reader.ReadElementCount64();

        heads.reserve(num_mappings);
        tails.reserve(num_mappings);

        for (std::uint64_t i{0}; i < num_mappings; ++i)
        {

            const auto u = reader.ReadOne<NodeID>();    // node based graph `from` nodes
            const auto v = reader.ReadOne<NodeID>();    // node based graph `to` nodes
            const auto head = reader.ReadOne<EdgeID>(); // edge based graph forward nodes
            const auto tail = reader.ReadOne<EdgeID>(); // edge based graph backward nodes

            heads.insert({head, {u, v}});
            tails.insert({tail, {u, v}});
        }
    }

    struct NodeBasedNodes
    {
        NodeID u, v;
    };

    NodeBasedNodes Lookup(EdgeID edge_based_node)
    {
        auto heads_it = heads.find(edge_based_node);
        if (heads_it != end(heads))
            return heads_it->second;

        auto tails_it = tails.find(edge_based_node);
        if (tails_it != end(tails))
            return tails_it->second;

        BOOST_ASSERT(false);
        return NodeBasedNodes{SPECIAL_NODEID, SPECIAL_NODEID};
    }

  private:
    std::unordered_map<EdgeID, NodeBasedNodes> heads;
    std::unordered_map<EdgeID, NodeBasedNodes> tails;
};

NodeBasedGraphToEdgeBasedGraphMapping
LoadNodeBasedGraphToEdgeBasedGraphMapping(const std::string &path)
{
    const auto fingerprint = storage::io::FileReader::VerifyFingerprint;
    storage::io::FileReader reader(path, fingerprint);

    NodeBasedGraphToEdgeBasedGraphMapping mapping{reader};
    return mapping;
}

void LogGeojson(const std::string &filename, const std::vector<std::uint32_t> &bisection_ids)
{
    // reload graph, since we destroyed the old one
    auto compressed_node_based_graph = LoadCompressedNodeBasedGraph(filename);

    util::Log() << "Loaded compressed node based graph: "
                << compressed_node_based_graph.edges.size() << " edges, "
                << compressed_node_based_graph.coordinates.size() << " nodes";

    groupEdgesBySource(begin(compressed_node_based_graph.edges),
                       end(compressed_node_based_graph.edges));

    auto graph =
        makeBisectionGraph(compressed_node_based_graph.coordinates,
                           adaptToBisectionEdge(std::move(compressed_node_based_graph.edges)));

    const auto get_level = [](const std::uint32_t lhs, const std::uint32_t rhs) {
        auto xored = lhs ^ rhs;
        std::uint32_t level = log(xored) / log(2.0);
        return level;
    };

    const auto reverse_bits = [](std::uint32_t x) {
        x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
        x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
        x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
        x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
        x = ((x >> 16) & 0xffffu) | ((x & 0xffffu) << 16);
        return x;
    };

    std::vector<std::vector<util::Coordinate>> border_vertices(33);

    for (NodeID nid = 0; nid < graph.NumberOfNodes(); ++nid)
    {
        const auto source_id = reverse_bits(bisection_ids[nid]);
        for (const auto &edge : graph.Edges(nid))
        {
            const auto target_id = reverse_bits(bisection_ids[edge.target]);
            if (source_id != target_id)
            {
                auto level = get_level(source_id, target_id);
                border_vertices[level].push_back(graph.Node(nid).coordinate);
                border_vertices[level].push_back(graph.Node(edge.target).coordinate);
            }
        }
    }

    util::ScopedGeojsonLoggerGuard<util::CoordinateVectorToMultiPoint> guard(
        "border_vertices.geojson");
    std::size_t level = 0;
    for (auto &bv : border_vertices)
    {
        if (!bv.empty())
        {
            std::sort(bv.begin(), bv.end(), [](const auto lhs, const auto rhs) {
                return std::tie(lhs.lon, lhs.lat) < std::tie(rhs.lon, rhs.lat);
            });
            bv.erase(std::unique(bv.begin(), bv.end()), bv.end());

            util::json::Object jslevel;
            jslevel.values["level"] = util::json::Number(level++);
            guard.Write(bv, jslevel);
        }
    }
}

int Partitioner::Run(const PartitionConfig &config)
{
    auto compressed_node_based_graph =
        LoadCompressedNodeBasedGraph(config.compressed_node_based_graph_path.string());

    util::Log() << "Loaded compressed node based graph: "
                << compressed_node_based_graph.edges.size() << " edges, "
                << compressed_node_based_graph.coordinates.size() << " nodes";

    groupEdgesBySource(begin(compressed_node_based_graph.edges),
                       end(compressed_node_based_graph.edges));

    auto graph =
        makeBisectionGraph(compressed_node_based_graph.coordinates,
                           adaptToBisectionEdge(std::move(compressed_node_based_graph.edges)));

    RecursiveBisection recursive_bisection(config.maximum_cell_size,
                                           config.balance,
                                           config.boundary_factor,
                                           config.num_optimizing_cuts,
                                           graph);

    LogGeojson(config.compressed_node_based_graph_path.string(),
               recursive_bisection.BisectionIDs());

    // TODO: fast head/tail keyed lookup for u,v
    auto mapping = LoadNodeBasedGraphToEdgeBasedGraphMapping(config.nbg_ebg_mapping_path.string());

    util::Log() << "Loaded node based graph to edge based graph mapping";

    return 0;
}

} // namespace partition
} // namespace osrm
