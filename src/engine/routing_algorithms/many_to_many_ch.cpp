#include "engine/routing_algorithms/many_to_many.hpp"
#include "engine/routing_algorithms/routing_base_ch.hpp"

#include <boost/assert.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <limits>
#include <memory>
#include <vector>

namespace osrm
{
namespace engine
{
namespace routing_algorithms
{

namespace ch
{

inline bool addLoopWeight(const DataFacade<ch::Algorithm> &facade,
                          const NodeID node,
                          EdgeWeight &weight,
                          EdgeDuration &duration)
{ // Special case for CH when contractor creates a loop edge node->node
    BOOST_ASSERT(weight < 0);

    const auto loop_weight = ch::getLoopWeight<false>(facade, node);
    if (loop_weight != INVALID_EDGE_WEIGHT)
    {
        const auto new_weight_with_loop = weight + loop_weight;
        if (new_weight_with_loop >= 0)
        {
            weight = new_weight_with_loop;
            duration += ch::getLoopWeight<true>(facade, node);
            return true;
        }
    }

    // No loop found or adjusted weight is negative
    return false;
}

template <bool DIRECTION>
void relaxOutgoingEdges(const DataFacade<Algorithm> &facade,
                        const NodeID node,
                        const EdgeWeight weight,
                        const EdgeDuration duration,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const PhantomNode &)
{
    if (stallAtNode<DIRECTION>(facade, node, weight, query_heap))
    {
        return;
    }

    for (auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);
            const auto edge_weight = data.weight;

            const auto edge_duration = data.duration;

            BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
            const auto to_weight = weight + edge_weight;
            const auto to_duration = duration + edge_duration;

            // New Node discovered -> Add to Heap + Node Info Storage
            if (!query_heap.WasInserted(to))
            {
                query_heap.Insert(to, to_weight, {node, to_duration});
            }
            // Found a shorter Path -> Update weight and set new parent
            else if (std::tie(to_weight, to_duration) <
                     std::tie(query_heap.GetKey(to), query_heap.GetData(to).duration))
            {
                query_heap.GetData(to) = {node, to_duration};
                query_heap.DecreaseKey(to, to_weight);
            }
        }
    }
}

void forwardRoutingStep(const DataFacade<Algorithm> &facade,
                        const std::size_t row_index,
                        const std::size_t number_of_targets,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const std::vector<NodeBucket> &search_space_with_buckets,
                        std::vector<EdgeWeight> &weights_table,
                        std::vector<EdgeDuration> &durations_table,
                        std::vector<NodeID> &middle_nodes_table,
                        const PhantomNode &phantom_node)
{
    const auto node = query_heap.DeleteMin();
    const auto source_weight = query_heap.GetKey(node);
    const auto source_duration = query_heap.GetData(node).duration;

    // Check if each encountered node has an entry
    const auto &bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                               search_space_with_buckets.end(),
                                               node,
                                               NodeBucket::Compare());
    for (const auto &current_bucket : boost::make_iterator_range(bucket_list))
    {
        // Get target id from bucket entry
        const auto column_index = current_bucket.column_index;
        const auto target_weight = current_bucket.weight;
        const auto target_duration = current_bucket.duration;

        auto &current_weight = weights_table[row_index * number_of_targets + column_index];
        auto &current_duration = durations_table[row_index * number_of_targets + column_index];

        // Check if new weight is better
        auto new_weight = source_weight + target_weight;
        auto new_duration = source_duration + target_duration;

        if (new_weight < 0)
        {
            if (addLoopWeight(facade, node, new_weight, new_duration))
            {
                current_weight = std::min(current_weight, new_weight);
                current_duration = std::min(current_duration, new_duration);
                middle_nodes_table[row_index * number_of_targets + column_index] = node;
            }
        }
        else if (std::tie(new_weight, new_duration) < std::tie(current_weight, current_duration))
        {
            current_weight = new_weight;
            current_duration = new_duration;
            middle_nodes_table[row_index * number_of_targets + column_index] = node;
        }
    }

    relaxOutgoingEdges<FORWARD_DIRECTION>(
        facade, node, source_weight, source_duration, query_heap, phantom_node);
}

void backwardRoutingStep(const DataFacade<Algorithm> &facade,
                         const unsigned column_index,
                         typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                         std::vector<NodeBucket> &search_space_with_buckets,
                         const PhantomNode &phantom_node)
{
    const auto node = query_heap.DeleteMin();
    const auto target_weight = query_heap.GetKey(node);
    const auto target_duration = query_heap.GetData(node).duration;
    const auto parent = query_heap.GetData(node).parent;

    // Store settled nodes in search space bucket
    search_space_with_buckets.emplace_back(
        node, parent, column_index, target_weight, target_duration);

    relaxOutgoingEdges<REVERSE_DIRECTION>(
        facade, node, target_weight, target_duration, query_heap, phantom_node);
}

} // namespace ch

void retrievePackedPathFromSearchSpace(const NodeID middle_node_id,
                                       const unsigned column_index,
                                       const std::vector<NodeBucket> &search_space_with_buckets,
                                       std::vector<NodeID> &packed_leg)
{
    auto bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                        search_space_with_buckets.end(),
                                        middle_node_id,
                                        NodeBucket::ColumnCompare(column_index));

    NodeID current_node_id = middle_node_id;

    BOOST_ASSERT_MSG(std::distance(bucket_list.first, bucket_list.second) == 1,
                     "The pointers are not pointing to the same element.");

    while (bucket_list.first->parent_node != current_node_id &&
           bucket_list.first != search_space_with_buckets.end())
    {
        current_node_id = bucket_list.first->parent_node;

        packed_leg.emplace_back(current_node_id);

        bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                       search_space_with_buckets.end(),
                                       current_node_id,
                                       NodeBucket::ColumnCompare(column_index));
    }
}

void calculateDistances(typename SearchEngineData<ch::Algorithm>::ManyToManyQueryHeap &query_heap,
                        const DataFacade<ch::Algorithm> &facade,
                        const std::vector<PhantomNode> &phantom_nodes,
                        const std::vector<std::size_t> &target_indices,
                        const std::size_t row_index,
                        const std::size_t source_index,
                        const PhantomNode &source_phantom,
                        const std::size_t number_of_targets,
                        const std::vector<NodeBucket> &search_space_with_buckets,
                        std::vector<EdgeDistance> &distances_table,
                        const std::vector<NodeID> &middle_nodes_table)
{
    std::vector<NodeID> packed_leg;

    for (auto column_index : util::irange<std::size_t>(0, number_of_targets))
    {
        const auto target_index = target_indices[column_index];
        const auto &target_phantom = phantom_nodes[target_index];

        if (source_index == target_index)
        {
            distances_table[row_index * number_of_targets + column_index] = 0.0;
            continue;
        }

        NodeID middle_node_id = middle_nodes_table[row_index * number_of_targets + column_index];

        if (middle_node_id == SPECIAL_NODEID) // takes care of one-ways
        {
            distances_table[row_index * number_of_targets + column_index] = INVALID_EDGE_DISTANCE;
            continue;
        }

        // Step 1: Find path from source to middle node
        ch::retrievePackedPathFromSingleManyToManyHeap(query_heap, middle_node_id, packed_leg);
        std::reverse(packed_leg.begin(), packed_leg.end());

        packed_leg.push_back(middle_node_id);

        // Step 2: Find path from middle to target node
        retrievePackedPathFromSearchSpace(
            middle_node_id, column_index, search_space_with_buckets, packed_leg);

        if (packed_leg.size() == 1 && (needsLoopForward(source_phantom, target_phantom) ||
                                       needsLoopBackwards(source_phantom, target_phantom)))
        {
            auto weight = ch::getLoopWeight<false>(facade, packed_leg.front());
            if (weight != INVALID_EDGE_WEIGHT)
                packed_leg.push_back(packed_leg.front());
        }
        if (!packed_leg.empty())
        {
            auto annotation =
                ch::calculateEBGNodeAnnotations(facade, packed_leg.begin(), packed_leg.end());

            distances_table[row_index * number_of_targets + column_index] = annotation;

            // check the direction of travel to figure out how to calculate the offset to/from
            // the source/target
            if (source_phantom.forward_segment_id.id == packed_leg.front())
            {
                //       ............       <-- calculateEGBAnnotation returns distance from 0 to 3
                //       -->s               <-- subtract offset to start at source
                //          .........       <-- want this distance as result
                // entry 0---1---2---3---   <-- 3 is exit node
                EdgeDistance offset = source_phantom.GetForwardDistance();
                distances_table[row_index * number_of_targets + column_index] -= offset;
            }
            else if (source_phantom.reverse_segment_id.id == packed_leg.front())
            {
                //       ............    <-- calculateEGBAnnotation returns distance from 0 to 3
                //          s<-------    <-- subtract offset to start at source
                //       ...             <-- want this distance
                // entry 0---1---2---3   <-- 3 is exit node
                EdgeDistance offset = source_phantom.GetReverseDistance();
                distances_table[row_index * number_of_targets + column_index] -= offset;
            }
            if (target_phantom.forward_segment_id.id == packed_leg.back())
            {
                //       ............       <-- calculateEGBAnnotation returns distance from 0 to 3
                //                   ++>t   <-- add offset to get to target
                //       ................   <-- want this distance as result
                // entry 0---1---2---3---   <-- 3 is exit node
                EdgeDistance offset = target_phantom.GetForwardDistance();
                distances_table[row_index * number_of_targets + column_index] += offset;
            }
            else if (target_phantom.reverse_segment_id.id == packed_leg.back())
            {
                //       ............       <-- calculateEGBAnnotation returns distance from 0 to 3
                //                   <++t   <-- add offset to get from target
                //       ................   <-- want this distance as result
                // entry 0---1---2---3---   <-- 3 is exit node
                EdgeDistance offset = target_phantom.GetReverseDistance();
                distances_table[row_index * number_of_targets + column_index] += offset;
            }
        }
        else
        {
            // there is no shortcut to unpack. source and target are on the same EBG Node.
            // if the offset of the target is greater than the offset of the source, subtract it
            if (target_phantom.GetForwardDistance() > source_phantom.GetForwardDistance())
            {
                //       --------->t        <-- offsets
                //       ->s                <-- subtract source offset from target offset
                //         .........        <-- want this distance as result
                // entry 0---1---2---3---   <-- 3 is exit node
                EdgeDistance offset =
                    target_phantom.GetForwardDistance() - source_phantom.GetForwardDistance();
                distances_table[row_index * number_of_targets + column_index] = offset;
            }
            else
            {
                //               s<---      <-- offsets
                //         t<---------      <-- subtract source offset from target offset
                //         ......           <-- want this distance as result
                // entry 0---1---2---3---   <-- 3 is exit node
                EdgeDistance offset =
                    target_phantom.GetReverseDistance() - source_phantom.GetReverseDistance();
                distances_table[row_index * number_of_targets + column_index] = offset;
            }
        }
        packed_leg.clear();
    }
}

template <>
std::pair<std::vector<EdgeDuration>, std::vector<EdgeDistance>>
manyToManySearch(SearchEngineData<ch::Algorithm> &engine_working_data,
                 const DataFacade<ch::Algorithm> &facade,
                 const std::vector<PhantomNode> &phantom_nodes,
                 const std::vector<std::size_t> &source_indices,
                 const std::vector<std::size_t> &target_indices,
                 const bool calculate_distance,
                 const bool calculate_duration)
{
    (void)calculate_duration; // TODO: stub to use when computing durations become optional

    const auto number_of_sources = source_indices.size();
    const auto number_of_targets = target_indices.size();
    const auto number_of_entries = number_of_sources * number_of_targets;

    std::vector<EdgeWeight> weights_table(number_of_entries, INVALID_EDGE_WEIGHT);
    std::vector<EdgeDuration> durations_table(number_of_entries, MAXIMAL_EDGE_DURATION);
    std::vector<EdgeDistance> distances_table;
    std::vector<NodeID> middle_nodes_table(number_of_entries, SPECIAL_NODEID);

    std::vector<NodeBucket> search_space_with_buckets;

    // Populate buckets with paths from all accessible nodes to destinations via backward searches
    for (std::uint32_t column_index = 0; column_index < target_indices.size(); ++column_index)
    {
        const auto index = target_indices[column_index];
        const auto &phantom = phantom_nodes[index];

        engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(
            facade.GetNumberOfNodes());
        auto &query_heap = *(engine_working_data.many_to_many_heap);
        insertTargetInHeap(query_heap, phantom);

        // Explore search space
        while (!query_heap.Empty())
        {
            backwardRoutingStep(
                facade, column_index, query_heap, search_space_with_buckets, phantom);
        }
    }

    // Order lookup buckets
    std::sort(search_space_with_buckets.begin(), search_space_with_buckets.end());

    // Find shortest paths from sources to all accessible nodes
    for (std::uint32_t row_index = 0; row_index < source_indices.size(); ++row_index)
    {
        const auto source_index = source_indices[row_index];
        const auto &source_phantom = phantom_nodes[source_index];

        // Clear heap and insert source nodes
        engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(
            facade.GetNumberOfNodes());
        auto &query_heap = *(engine_working_data.many_to_many_heap);
        insertSourceInHeap(query_heap, source_phantom);

        // Explore search space
        while (!query_heap.Empty())
        {
            forwardRoutingStep(facade,
                               row_index,
                               number_of_targets,
                               query_heap,
                               search_space_with_buckets,
                               weights_table,
                               durations_table,
                               middle_nodes_table,
                               source_phantom);
        }

        if (calculate_distance)
        {
            distances_table.resize(number_of_entries, INVALID_EDGE_DISTANCE);
            calculateDistances(query_heap,
                               facade,
                               phantom_nodes,
                               target_indices,
                               row_index,
                               source_index,
                               source_phantom,
                               number_of_targets,
                               search_space_with_buckets,
                               distances_table,
                               middle_nodes_table);
        }
    }

    return std::make_pair(durations_table, distances_table);
}

} // namespace routing_algorithms
} // namespace engine
} // namespace osrm
