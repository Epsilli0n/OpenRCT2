/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "SmartPathfinding.h"

#include "../GameState.h"
#include "../core/DataSerialiser.h"
#include "../entity/EntityList.h"
#include "../entity/Guest.h"
#include "../entity/Litter.h"
#include "../entity/Staff.h"
#include "../object/PathAdditionEntry.h"
#include "../ride/RideData.h"
#include "../world/Footpath.h"
#include "../world/Map.h"
#include "../world/TileElementsView.h"
#include "../world/tile_element/BannerElement.h"
#include "../world/tile_element/EntranceElement.h"
#include "../world/tile_element/PathElement.h"
#include "../world/tile_element/TrackElement.h"
#include "GuestPathfinding.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace OpenRCT2::SmartPathfinding
{
    NodeKey Key(const TileCoordsXYZ& location)
    {
        return static_cast<uint64_t>(static_cast<uint16_t>(location.x))
            | (static_cast<uint64_t>(static_cast<uint16_t>(location.y)) << 16)
            | (static_cast<uint64_t>(static_cast<uint16_t>(location.z)) << 32);
    }

    void Invalidate(GameState_t& gameState)
    {
        gameState.smartPathfinding.graphValid = false;
        gameState.smartPathfinding.routes.clear();
    }

    void Reset(GameState_t& gameState)
    {
        gameState.smartPathfinding = {};
    }

    static uint32_t AddNode(State& state, const TileCoordsXYZ& location, bool path, RideId queueRide)
    {
        auto [it, inserted] = state.nodeIndex.emplace(Key(location), static_cast<uint32_t>(state.nodes.size()));
        if (inserted)
            state.nodes.push_back({ location, queueRide, path, {}, {} });
        else if (path)
        {
            auto& node = state.nodes[it->second];
            if (!node.path)
                node.queueRide = queueRide;
            else if (node.queueRide != queueRide)
                node.queueRide = RideId::GetNull();
            state.nodes[it->second].path = true;
        }
        return it->second;
    }

    static void BuildGraph(State& state)
    {
        state.nodes.clear();
        state.nodeIndex.clear();
        state.routes.clear();
        const auto& gameState = getGameState();

        // Tile traversal and edge order are stable, including in multiplayer/replays.
        for (int32_t y = 0; y < gameState.mapSize.y; ++y)
        {
            for (int32_t x = 0; x < gameState.mapSize.x; ++x)
            {
                for (const auto* element : TileElementsView<TileElement>(TileCoordsXY{ x, y }))
                {
                    if (element->isGhost())
                        continue;
                    if (auto* path = element->asPath())
                    {
                        AddNode(
                            state, { x, y, path->baseHeight }, true,
                            path->isQueue() ? path->getRideIndex() : RideId::GetNull());
                    }
                    else if (element->asEntrance())
                        AddNode(state, { x, y, element->baseHeight }, false, RideId::GetNull());
                    else if (auto* track = element->asTrack())
                    {
                        auto* ride = GetRide(track->getRideIndex());
                        if (ride && ride->getRideTypeDescriptor().flags.has(RtdFlag::isShopOrFacility))
                            AddNode(state, { x, y, element->baseHeight }, false, RideId::GetNull());
                    }
                }
            }
        }

        for (uint32_t source = 0; source < state.nodes.size(); ++source)
        {
            auto& node = state.nodes[source];
            if (!node.path)
                continue;
            for (auto* path : TileElementsView<PathElement>(TileCoordsXY(node.location)))
            {
                if (path->isGhost() || path->baseHeight != node.location.z)
                    continue;
                auto guestEdges = PathFinding::GetPermittedEdges(false, path);
                for (Direction direction = 0; direction < 4; ++direction)
                {
                    if (!(path->getEdges() & (1 << direction)))
                        continue;
                    int32_t exitHeight = path->baseHeight;
                    if (path->isSloped())
                    {
                        if (direction == path->getSlopeDirection())
                            exitHeight += 2;
                        else if (direction != DirectionReverse(path->getSlopeDirection()))
                            continue;
                    }
                    auto next = TileCoordsXY(node.location) + TileDirectionDelta[direction];
                    if (next.x < 0 || next.y < 0 || next.x >= gameState.mapSize.x || next.y >= gameState.mapSize.y)
                        continue;
                    for (const auto* element : TileElementsView<TileElement>(next))
                    {
                        if (element->isGhost())
                            continue;
                        bool connects = false;
                        if (auto* nextPath = element->asPath())
                        {
                            connects = (nextPath->getEdges() & (1 << DirectionReverse(direction)))
                                && FootpathIsZAndDirectionValid(*nextPath, exitHeight, direction);
                        }
                        else if (element->baseHeight == exitHeight && (element->asEntrance() || element->asTrack()))
                        {
                            connects = TileElementWantsPathConnectionTowards(
                                { next.x, next.y, exitHeight, DirectionReverse(direction) }, nullptr);
                        }
                        if (!connects)
                            continue;
                        auto destination = state.nodeIndex.find(Key({ next.x, next.y, element->baseHeight }));
                        if (destination == state.nodeIndex.end())
                            continue;
                        bool guestAllowed = (guestEdges & (1 << direction)) != 0;
                        node.edges.push_back({ destination->second, direction, guestAllowed });
                        state.nodes[destination->second].incoming.push_back({ source, direction, guestAllowed });
                    }
                }
            }
        }
        state.graphTick = gameState.currentTicks;
        state.graphValid = true;
    }

    static State& Graph()
    {
        auto& state = getGameState().smartPathfinding;
        // Also catches direct script edits that do not run a game action.
        if (!state.graphValid || state.graphTick / 128 != getGameState().currentTicks / 128)
            BuildGraph(state);
        return state;
    }

    static bool GuestCanUse(const Node& node, NodeKey goal, bool ignoreQueues, RideId queueRide, bool outsideOfPark)
    {
        // Match PeepInteractWithPath, including construction rights at this height.
        // Entrances remain reachable terminals so guests can cross the park boundary there.
        if (node.path && MapIsLocationOwned(node.location.toCoordsXYZ()) == outsideOfPark)
            return false;
        return !ignoreQueues || node.queueRide.IsNull() || node.queueRide == queueRide || Key(node.location) == goal;
    }

    static const RouteField* RoutesTo(const TileCoordsXYZ& goal, bool ignoreQueues, RideId queueRide, bool outsideOfPark)
    {
        auto& state = Graph();
        auto goalKey = Key(goal);
        auto goalNode = state.nodeIndex.find(goalKey);
        if (goalNode == state.nodeIndex.end()
            || !GuestCanUse(state.nodes[goalNode->second], goalKey, ignoreQueues, queueRide, outsideOfPark))
            return nullptr;
        for (const auto& route : state.routes)
        {
            if (route.goal == goalKey && route.queueRide == queueRide && route.ignoreForeignQueues == ignoreQueues
                && route.outsideOfPark == outsideOfPark)
                return &route;
        }

        // Each field remembers the complete shortest route distance for every node.
        // Guests with the same destination share it rather than searching again.
        RouteField route{ goalKey, queueRide, ignoreQueues, outsideOfPark,
                          std::vector<uint32_t>(state.nodes.size(), kUnreachable) };
        std::vector<uint32_t> pending{ goalNode->second };
        route.distances[goalNode->second] = 0;
        for (size_t cursor = 0; cursor < pending.size(); ++cursor)
        {
            auto destination = pending[cursor];
            for (const auto& edge : state.nodes[destination].incoming)
            {
                auto source = edge.destination;
                if (!edge.guestAllowed || route.distances[source] != kUnreachable
                    || !GuestCanUse(state.nodes[source], goalKey, ignoreQueues, queueRide, outsideOfPark))
                    continue;
                route.distances[source] = route.distances[destination] + 1;
                pending.push_back(source);
            }
        }
        if (state.routes.size() >= 32)
            state.routes.pop_front();
        state.routes.push_back(std::move(route));
        return &state.routes.back();
    }

    uint32_t Distance(
        const TileCoordsXYZ& from, const TileCoordsXYZ& goal, bool ignoreQueues, RideId queueRide, bool outsideOfPark)
    {
        auto* route = RoutesTo(goal, ignoreQueues, queueRide, outsideOfPark);
        if (!route)
            return kUnreachable;
        auto& state = getGameState().smartPathfinding;
        auto source = state.nodeIndex.find(Key(from));
        return source == state.nodeIndex.end() ? kUnreachable : route->distances[source->second];
    }

    std::optional<TileCoordsXYZ> GetRideGoal(const Guest& guest, const Ride& ride)
    {
        std::array<TileCoordsXYZ, Limits::kMaxStationsPerRide> goals{};
        std::array<uint32_t, Limits::kMaxStationsPerRide> distances{};
        size_t numGoals = 0;
        std::optional<TileCoordsXYZ> closestGoal;
        uint32_t best = kUnreachable;
        const auto stations = ride.getStations();
        const bool hasEntrances = std::any_of(
            stations.begin(), stations.end(), [](const auto& station) { return !station.entrance.isNull(); });
        for (const auto& station : ride.getStations())
        {
            if (station.entrance.isNull() && (hasEntrances || ride.getStationIndex(&station).ToUnderlying() != 0))
                continue;
            auto goal = PathFinding::GetRideGoal(ride, station);
            auto distance = Distance(TileCoordsXYZ(guest.nextLoc), goal, true, ride.id, guest.outsideOfPark);
            goals[numGoals] = goal;
            distances[numGoals++] = distance;
            if (distance < best)
            {
                best = distance;
                closestGoal = goal;
            }
        }
        // Cycle over the same entrance list as native routing. Reachability of unrelated
        // stations can change after passing a no-entry sign, without changing this choice.
        if (numGoals > 1 && (ride.departFlags & RIDE_DEPART_SYNCHRONISE_WITH_ADJACENT_STATIONS))
        {
            auto selected = guest.guestNumRides % numGoals;
            if (distances[selected] != kUnreachable)
                return goals[selected];
        }
        // Fall back to the nearest reachable station if the preferred entrance is disconnected.
        return closestGoal;
    }

    uint32_t RideDistance(const Guest& guest, const Ride& ride)
    {
        auto goal = GetRideGoal(guest, ride);
        return goal ? Distance(TileCoordsXYZ(guest.nextLoc), *goal, true, ride.id, guest.outsideOfPark) : kUnreachable;
    }

    Direction GuestDirection(
        const TileCoordsXYZ& from, const TileCoordsXYZ& goal, Guest& guest, bool ignoreQueues, RideId queueRide)
    {
        auto* route = RoutesTo(goal, ignoreQueues, queueRide, guest.outsideOfPark);
        if (!route)
            return kInvalidDirection;
        auto& state = getGameState().smartPathfinding;
        auto source = state.nodeIndex.find(Key(from));
        if (source == state.nodeIndex.end())
            return kInvalidDirection;
        auto distance = route->distances[source->second];
        if (distance == kUnreachable || distance == 0)
            return kInvalidDirection;
        for (const auto& edge : state.nodes[source->second].edges)
        {
            if (edge.guestAllowed && route->distances[edge.destination] < distance
                && GuestCanUse(state.nodes[edge.destination], Key(goal), ignoreQueues, queueRide, guest.outsideOfPark))
            {
                bool permitted = false;
                for (auto* path : TileElementsView<PathElement>(TileCoordsXY(from)))
                {
                    if (!path->isGhost() && path->baseHeight == from.z)
                        permitted |= (PathFinding::GetPermittedEdges(false, path) & (1 << edge.direction)) != 0;
                }
                if (!permitted)
                {
                    Invalidate(getGameState());
                    return kInvalidDirection;
                }
                guest.guestIsLostCountdown = 200;
                guest.timeLost = 0;
                guest.peepFlags.unset(PeepFlag::lost);
                guest.pathfindGoal = { goal, 0 };
                return edge.direction;
            }
        }
        return kInvalidDirection;
    }

    static bool HasFullBin(const Node& node)
    {
        for (auto* path : TileElementsView<PathElement>(TileCoordsXY(node.location)))
        {
            if (path->isGhost() || path->baseHeight != node.location.z || !path->hasAddition() || path->isBroken()
                || path->additionIsGhost())
                continue;
            auto* addition = path->getAdditionEntry();
            if (!addition || !addition->flags.has(PathAdditionFlag::isBin))
                continue;
            auto edges = path->getEdges();
            auto contents = path->getAdditionStatus();
            for (size_t side = 0; side < 4; ++side)
            {
                if (!(edges & (1 << side)) && ((contents >> (side * 2)) & 3) == 0)
                    return true;
            }
        }
        return false;
    }

    static int32_t LitterPriority(const Node& node)
    {
        int32_t priority = 0;
        for (auto* litter : EntityTileList<Litter>(node.location.toCoordsXY()))
        {
            if (std::abs(litter->z - node.location.z * kCoordsZStep) >= 16)
                continue;
            priority += (litter->subType == Litter::Type::vomit || litter->subType == Litter::Type::vomitAlt) ? 4 : 1;
        }
        return priority;
    }

    static uint32_t Region(const TileCoordsXYZ& location)
    {
        return static_cast<uint32_t>(location.x / 16) | (static_cast<uint32_t>(location.y / 16) << 16);
    }

    static void ClearStaleAssignments(State& state)
    {
        for (auto it = state.assignments.begin(); it != state.assignments.end();)
        {
            const auto* staff = getGameState().entities.tryGetEntity<Staff>(EntityId::FromUnderlying(it->first));
            auto node = state.nodeIndex.find(it->second.destination);
            bool valid = staff && staff->assignedStaffType == StaffType::handyman
                && (staff->state == PeepState::patrolling || staff->state == PeepState::sweeping
                    || staff->state == PeepState::emptyingBin)
                && node != state.nodeIndex.end() && staff->isLocationInPatrol(state.nodes[node->second].location.toCoordsXY());
            if (valid)
            {
                if (it->second.job == Job::sweeping)
                    valid = (staff->staffOrders & STAFF_ORDERS_SWEEPING) && LitterPriority(state.nodes[node->second]) != 0;
                else if (it->second.job == Job::emptyingBin)
                    valid = (staff->staffOrders & STAFF_ORDERS_EMPTY_BINS) && HasFullBin(state.nodes[node->second]);
            }
            if (!valid)
                it = state.assignments.erase(it);
            else
                ++it;
        }
    }

    Direction HandymanDirection(Staff& staff)
    {
        auto& gameState = getGameState();
        if (!gameState.cheats.smartHandymanDispatch || staff.assignedStaffType != StaffType::handyman
            || staff.state != PeepState::patrolling || staff.getNextIsSurface())
            return kInvalidDirection;
        auto& state = Graph();
        auto sourceIt = state.nodeIndex.find(Key(TileCoordsXYZ(staff.nextLoc)));
        if (sourceIt == state.nodeIndex.end())
            return kInvalidDirection;
        const auto source = sourceIt->second;
        const auto id = staff.id.ToUnderlying();
        const auto now = gameState.currentTicks;
        state.lastVisited[Key(state.nodes[source].location)] = now;
        ClearStaleAssignments(state);

        std::vector<uint32_t> distances(state.nodes.size(), kUnreachable);
        std::vector<Direction> directions(state.nodes.size(), kInvalidDirection);
        std::vector<uint32_t> pending{ source };
        distances[source] = 0;
        for (size_t cursor = 0; cursor < pending.size(); ++cursor)
        {
            auto current = pending[cursor];
            for (const auto& edge : state.nodes[current].edges)
            {
                const auto& node = state.nodes[edge.destination];
                if (!node.path || distances[edge.destination] != kUnreachable
                    || !staff.isLocationInPatrol(node.location.toCoordsXY()))
                    continue;
                distances[edge.destination] = distances[current] + 1;
                directions[edge.destination] = current == source ? edge.direction : directions[current];
                pending.push_back(edge.destination);
            }
        }

        auto assignment = state.assignments.find(id);
        if (assignment != state.assignments.end())
        {
            auto target = state.nodeIndex.find(assignment->second.destination);
            if (target != state.nodeIndex.end() && distances[target->second] != kUnreachable && target->second != source)
            {
                // Cleaning assignments persist; idle coverage can yield to newly-created work.
                if (assignment->second.job != Job::coverage)
                    return directions[target->second];
            }
            else
            {
                state.assignments.erase(assignment);
                assignment = state.assignments.end();
            }
        }

        std::map<NodeKey, uint16_t> reserved;
        std::map<uint32_t, uint32_t> regionLoad;
        for (const auto& [otherId, otherAssignment] : state.assignments)
        {
            if (otherId == id)
                continue;
            reserved.emplace(otherAssignment.destination, otherId);
            auto node = state.nodeIndex.find(otherAssignment.destination);
            if (node != state.nodeIndex.end())
                ++regionLoad[Region(state.nodes[node->second].location)];
        }
        // Include jobs already being serviced when the cheat is first enabled.
        for (const auto* other : EntityList<Staff>())
        {
            if (other->id.ToUnderlying() == id || other->assignedStaffType != StaffType::handyman
                || (other->state != PeepState::sweeping && other->state != PeepState::emptyingBin))
                continue;
            auto key = Key(TileCoordsXYZ(other->nextLoc));
            auto node = state.nodeIndex.find(key);
            if (node != state.nodeIndex.end() && reserved.emplace(key, other->id.ToUnderlying()).second)
                ++regionLoad[Region(state.nodes[node->second].location)];
        }

        uint32_t best = kUnreachable;
        Job bestJob = Job::coverage;
        int64_t bestScore = -1;
        for (auto candidate : pending)
        {
            if (candidate == source || reserved.contains(Key(state.nodes[candidate].location)))
                continue;
            const auto& node = state.nodes[candidate];
            int32_t litter = (staff.staffOrders & STAFF_ORDERS_SWEEPING) ? LitterPriority(node) : 0;
            bool bin = (staff.staffOrders & STAFF_ORDERS_EMPTY_BINS) && HasFullBin(node);
            if (litter == 0 && !bin)
                continue;
            int64_t score = (100000 + std::min(litter, 64) * 2000 + (bin ? 2000 : 0)) / (distances[candidate] + 4);
            if (score > bestScore)
            {
                best = candidate;
                bestScore = score;
                bestJob = litter != 0 ? Job::sweeping : Job::emptyingBin;
            }
        }

        if (best == kUnreachable && assignment != state.assignments.end())
        {
            auto target = state.nodeIndex.find(assignment->second.destination);
            if (target != state.nodeIndex.end())
                return directions[target->second];
        }

        if (best == kUnreachable)
        {
            // Regional age prevents one old corner attracting every worker. Reservations
            // discourage nearby assignments, while increasing age eventually beats distance.
            std::map<uint32_t, uint64_t> regionAge;
            std::map<uint32_t, uint32_t> regionSize;
            for (auto candidate : pending)
            {
                auto region = Region(state.nodes[candidate].location);
                auto visited = state.lastVisited.find(Key(state.nodes[candidate].location));
                uint32_t age = visited == state.lastVisited.end() ? now + 1024 : now - visited->second;
                regionAge[region] += age;
                ++regionSize[region];
            }
            bestScore = std::numeric_limits<int64_t>::min();
            for (auto candidate : pending)
            {
                const auto& node = state.nodes[candidate];
                auto key = Key(node.location);
                if (candidate == source || reserved.contains(key))
                    continue;
                auto visited = state.lastVisited.find(key);
                uint32_t age = visited == state.lastVisited.end() ? now + 1024 : now - visited->second;
                auto region = Region(node.location);
                int64_t score = static_cast<int64_t>(age) + regionAge[region] / regionSize[region];
                score -= static_cast<int64_t>(distances[candidate]) * 8;
                score -= static_cast<int64_t>(regionLoad[region]) * 4096;
                for (const auto& [otherKey, otherId] : reserved)
                {
                    const auto& target = state.nodes[state.nodeIndex.at(otherKey)].location;
                    int32_t separation = std::abs(target.x - node.location.x) + std::abs(target.y - node.location.y);
                    score -= std::max(0, 12 - separation) * 128;
                }
                if (score > bestScore)
                {
                    best = candidate;
                    bestScore = score;
                }
            }
        }

        if (best == kUnreachable)
        {
            state.assignments.erase(id);
            return kInvalidDirection;
        }
        state.assignments[id] = { Key(state.nodes[best].location), bestJob };
        staff.windowInvalidateFlags |= PEEP_INVALIDATE_PEEP_ACTION;
        return directions[best];
    }

    const Assignment* GetAssignment(const Staff& staff)
    {
        const auto& gameState = getGameState();
        if (!gameState.cheats.smartHandymanDispatch)
            return nullptr;
        auto assignment = gameState.smartPathfinding.assignments.find(staff.id.ToUnderlying());
        return assignment == gameState.smartPathfinding.assignments.end() ? nullptr : &assignment->second;
    }

    void Serialise(GameState_t& gameState, DataSerialiser& ds)
    {
        auto& state = gameState.smartPathfinding;
        if (ds.isLoading())
            Reset(gameState);
        uint32_t count = static_cast<uint32_t>(state.lastVisited.size());
        ds << count;
        if (count > 4 * 1024 * 1024)
            throw std::runtime_error("Invalid smart pathfinding coverage count");
        if (ds.isSaving())
        {
            for (const auto& [key, visited] : state.lastVisited)
                ds << key << visited;
        }
        else
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                NodeKey key{};
                uint32_t visited{};
                ds << key << visited;
                state.lastVisited.emplace(key, visited);
            }
        }
        count = static_cast<uint32_t>(state.assignments.size());
        ds << count;
        if (count > kMaxEntities)
            throw std::runtime_error("Invalid smart pathfinding assignment count");
        if (ds.isSaving())
        {
            for (const auto& [id, assignment] : state.assignments)
                ds << id << assignment.destination << assignment.job;
        }
        else
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                uint16_t id{};
                Assignment assignment{};
                ds << id << assignment.destination << assignment.job;
                if (assignment.job > Job::emptyingBin)
                    throw std::runtime_error("Invalid smart pathfinding job");
                state.assignments.emplace(id, assignment);
            }
        }
    }
} // namespace OpenRCT2::SmartPathfinding
