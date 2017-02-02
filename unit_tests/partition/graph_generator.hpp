#ifndef OSRM_UNIT_TEST_PARTITION_GRAPH_GENERATOR_HPP
#define OSRM_UNIT_TEST_PARTITION_GRAPH_GENERATOR_HPP

#include "util/coordinate.hpp"
#include "util/typedefs.hpp"

#include <algorithm>
#include <vector>

using namespace osrm::partition;
using namespace osrm::util;

struct EdgeWithSomeAdditionalData
{
    NodeID source;
    NodeID target;
    unsigned important_data;
};

std::vector<Coordinate>
makeGridCoordinates(int rows, int columns, double step_size, double lon_base, double lat_base)
{
    std::vector<Coordinate> result;

    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            result.push_back({FloatLongitude{lon_base + c * step_size},
                              FloatLatitude{lat_base + r * step_size}});

    return result;
}

std::vector<EdgeWithSomeAdditionalData> makeGridEdges(int rows, int columns, int id_base)
{
    const int min_id = id_base;
    const int max_id = id_base + rows * columns;
    const auto get_id = [id_base, columns](int r, int c) { return id_base + r * columns + c; };
    const auto valid = [min_id, max_id](int id) { return id >= min_id && id < max_id; };

    std::vector<EdgeWithSomeAdditionalData> edges;

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < columns; ++c)
        {
            auto id = static_cast<NodeID>(get_id(r, c));
            if (c > 0)
            {
                auto left = get_id(r, c - 1);
                edges.push_back({id, static_cast<NodeID>(left), 1});
            }
            if (c + 1 < columns)
            {
                auto right = get_id(r, c + 1);
                if (valid(right))
                    edges.push_back({id, static_cast<NodeID>(right), 1});
            }
            if (r > 0)
            {
                auto top = get_id(r - 1, c);
                if (valid(top))
                    edges.push_back({id, static_cast<NodeID>(top), 1});
            }
            if (r + 1 < rows)
            {
                auto bottom = get_id(r + 1, c);
                if (valid(bottom))
                    edges.push_back({id, static_cast<NodeID>(bottom), 1});
            }
        }
    }

    return edges;
}

#endif // OSRM_UNIT_TEST_PARTITION_GRAPH_GENERATOR_HPP
