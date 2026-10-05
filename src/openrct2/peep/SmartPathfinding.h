/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Identifiers.h"
#include "../world/Location.hpp"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace OpenRCT2
{
    class DataSerialiser;
    struct GameState_t;
    struct Guest;
    struct Ride;
    struct Staff;
} // namespace OpenRCT2

namespace OpenRCT2::SmartPathfinding
{
    constexpr uint32_t kUnreachable = std::numeric_limits<uint32_t>::max();
    using NodeKey = uint64_t;

    enum class Job : uint8_t
    {
        coverage,
        sweeping,
        emptyingBin,
    };

    struct Assignment
    {
        NodeKey destination{};
        Job job{};
    };

    struct Edge
    {
        uint32_t destination;
        Direction direction;
        bool guestAllowed;
    };

    struct Node
    {
        TileCoordsXYZ location;
        RideId queueRide = RideId::GetNull();
        bool path{};
        std::vector<Edge> edges;
        std::vector<Edge> incoming;
    };

    struct RouteField
    {
        NodeKey goal;
        RideId queueRide;
        bool ignoreForeignQueues;
        std::vector<uint32_t> distances;
    };

    struct State
    {
        // Only coverage and assignments affect simulation decisions and are saved.
        std::map<NodeKey, uint32_t> lastVisited;
        std::map<uint16_t, Assignment> assignments;

        // Derived caches contain no tile pointers and can be discarded at any time.
        bool graphValid{};
        uint32_t graphTick{};
        std::vector<Node> nodes;
        std::unordered_map<NodeKey, uint32_t> nodeIndex;
        std::deque<RouteField> routes;
    };

    NodeKey Key(const TileCoordsXYZ& location);
    void Invalidate(GameState_t& gameState);
    void Reset(GameState_t& gameState);
    void Serialise(GameState_t& gameState, DataSerialiser& ds);
    uint32_t Distance(const TileCoordsXYZ& from, const TileCoordsXYZ& goal, bool ignoreForeignQueues, RideId queueRide);
    uint32_t RideDistance(const Guest& guest, const Ride& ride);
    Direction GuestDirection(
        const TileCoordsXYZ& from, const TileCoordsXYZ& goal, Guest& guest, bool ignoreForeignQueues, RideId queueRide);
    Direction HandymanDirection(Staff& staff);
    const Assignment* GetAssignment(const Staff& staff);
} // namespace OpenRCT2::SmartPathfinding
