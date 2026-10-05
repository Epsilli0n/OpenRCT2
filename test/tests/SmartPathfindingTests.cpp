/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <gtest/gtest.h>
#include <openrct2/Cheats.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/actions/cheats/CheatSetAction.h>
#include <openrct2/actions/peep/StaffHireNewAction.h>
#include <openrct2/core/DataSerialiser.h>
#include <openrct2/drawing/Colour.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Litter.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/peep/GuestPathfinding.h>
#include <openrct2/peep/SmartPathfinding.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/scripting/ScriptEngine.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/BannerElement.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <set>

using namespace OpenRCT2;
namespace Smart = OpenRCT2::SmartPathfinding;

class SmartPathfindingTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;
    LegacyScene previousScene;

    void SetUp() override
    {
        gOpenRCT2Headless = true;
        previousScene = gLegacyScene;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_TRUE(context->Initialise());
        MapInit({ 256, 256 }, Drawing::Colour::black);
        getGameState().entities.resetAllEntities();
        RideInitAll();
        CheatsReset();
        getGameState().currentTicks = 4096;
        getGameState().cheats.smartGuestNavigation = true;
        getGameState().cheats.smartHandymanDispatch = true;
    }

    void TearDown() override
    {
        context.reset();
        gLegacyScene = previousScene;
    }

    PathElement* Path(TileCoordsXYZ location)
    {
        auto* existing = MapGetPathElementAt(location);
        if (existing)
            return existing;
        auto* path = TileElementInsert(location.toCoordsXYZ(), 0xF, TileElementType::path)->asPath();
        path->setClearanceZ(location.toCoordsXYZ().z + 16);
        path->setEdges(0);
        MapGetSurfaceElementAt(location.toCoordsXY())->setOwnership({ OwnershipFlag::landOwned });
        return path;
    }

    void Connect(TileCoordsXYZ a, TileCoordsXYZ b)
    {
        Path(a);
        Path(b);
        auto direction = DirectionFromTo(a.toCoordsXY(), b.toCoordsXY());
        auto* first = MapGetPathElementAt(a);
        first->setEdges(first->getEdges() | (1 << direction));
        auto* second = MapGetPathElementAt(b);
        second->setEdges(second->getEdges() | (1 << DirectionReverse(direction)));
        Smart::Invalidate(getGameState());
    }

    void Line(TileCoordsXYZ a, TileCoordsXYZ b)
    {
        Path(a);
        while (a.x != b.x || a.y != b.y)
        {
            auto next = a;
            next.x += (b.x > a.x) - (b.x < a.x);
            next.y += (b.y > a.y) - (b.y < a.y);
            Connect(a, next);
            a = next;
        }
    }

    Guest* GuestAt(TileCoordsXYZ location)
    {
        auto* guest = getGameState().entities.createEntity<Guest>();
        guest->state = PeepState::walking;
        guest->outsideOfPark = false;
        guest->nextLoc = location.toCoordsXYZ();
        guest->setNextFlags(0, false, false);
        guest->moveToAndUpdateSpatialIndex(location.toCoordsXYZ());
        return guest;
    }

    Staff* HandymanAt(TileCoordsXYZ location)
    {
        auto* staff = getGameState().entities.createEntity<Staff>();
        staff->assignedStaffType = StaffType::handyman;
        staff->state = PeepState::patrolling;
        staff->staffOrders = STAFF_ORDERS_SWEEPING | STAFF_ORDERS_EMPTY_BINS;
        staff->patrolInfo = nullptr;
        staff->nextLoc = location.toCoordsXYZ();
        staff->setNextFlags(0, false, false);
        staff->moveToAndUpdateSpatialIndex(location.toCoordsXYZ());
        return staff;
    }

    Litter* LitterAt(TileCoordsXYZ location, Litter::Type type = Litter::Type::vomit)
    {
        auto* litter = getGameState().entities.createEntity<Litter>();
        litter->subType = type;
        litter->moveToAndUpdateSpatialIndex(location.toCoordsXYZ() + CoordsXYZ{ 16, 16, 0 });
        return litter;
    }
};

TEST_F(SmartPathfindingTests, FindsCompleteDetourBeyondLegacyDistanceAndJunctionLimits)
{
    Line({ 2, 2, 14 }, { 2, 240, 14 });
    Line({ 2, 240, 14 }, { 230, 240, 14 });
    Line({ 230, 240, 14 }, { 230, 2, 14 });
    Line({ 230, 2, 14 }, { 3, 2, 14 });
    for (int y = 10; y < 200; y += 10)
        Connect({ 2, y, 14 }, { 3, y, 14 });
    MapGetPathElementAt({ 2, 100, 14 })->setWide(true);
    auto* guest = GuestAt({ 2, 2, 14 });
    EXPECT_EQ(Smart::Distance({ 2, 2, 14 }, { 3, 2, 14 }, true, RideId::GetNull()), 931u);
    EXPECT_EQ(PathFinding::ChooseDirection({ 2, 2, 14 }, { 3, 2, 14 }, *guest, true, RideId::GetNull()), 1);
    EXPECT_EQ(guest->guestIsLostCountdown, 200);
}

TEST_F(SmartPathfindingTests, DistinguishesBridgeHeightsAndTraversesSlopes)
{
    Connect({ 10, 10, 14 }, { 11, 10, 14 });
    Connect({ 11, 10, 14 }, { 12, 10, 16 });
    auto* slope = MapGetPathElementAt({ 11, 10, 14 });
    slope->setSloped(true);
    slope->setSlopeDirection(2);
    Path({ 12, 10, 14 });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 12, 10, 16 }, true, RideId::GetNull()), 2u);
    EXPECT_EQ(Smart::Distance({ 12, 10, 16 }, { 10, 10, 14 }, true, RideId::GetNull()), 2u);
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 12, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
}

TEST_F(SmartPathfindingTests, RespectsForeignQueuesAndGuestNoEntrySigns)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    auto* queue = MapGetPathElementAt({ 11, 10, 14 });
    queue->setIsQueue(true);
    queue->setRideIndex(RideId::FromUnderlying(2));
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::FromUnderlying(1)), Smart::kUnreachable);
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::FromUnderlying(2)), 3u);
    auto* banner = TileElementInsert({ 10 * 32, 10 * 32, 14 * 8 + 16 }, 0, TileElementType::banner)->asBanner();
    banner->setAllowedEdges(0xF & ~(1 << 2));
    Smart::Invalidate(getGameState());
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::FromUnderlying(2)), Smart::kUnreachable);
    auto* staff = HandymanAt({ 10, 10, 14 });
    LitterAt({ 13, 10, 14 });
    EXPECT_EQ(Smart::HandymanDirection(*staff), 2);
}

TEST_F(SmartPathfindingTests, GuestsAvoidUnownedShortcutsAndRejectBlockedRoutes)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    Line({ 10, 10, 14 }, { 10, 12, 14 });
    Line({ 10, 12, 14 }, { 13, 12, 14 });
    Line({ 13, 12, 14 }, { 13, 10, 14 });
    MapGetSurfaceElementAt(TileCoordsXY{ 11, 10 })->setOwnership({});
    auto* guest = GuestAt({ 10, 10, 14 });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), 7u);
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, { 13, 10, 14 }, *guest, true, RideId::GetNull()), 1);

    TileElementRemove(reinterpret_cast<TileElement*>(MapGetPathElementAt({ 10, 11, 14 })));
    guest->guestIsLostCountdown = 33;
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, { 13, 10, 14 }, *guest, true, RideId::GetNull()), kInvalidDirection);
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 11, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
    EXPECT_EQ(guest->guestIsLostCountdown, 33);
}

TEST_F(SmartPathfindingTests, GuestRoutesRespectConstructionRightsAtEachPathHeight)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    Line({ 10, 10, 17 }, { 13, 10, 17 });
    auto* surface = MapGetSurfaceElementAt(TileCoordsXY{ 11, 10 });
    surface->setBaseZ(14 * kCoordsZStep);
    surface->setOwnership({ OwnershipFlag::constructionRightsOwned });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
    EXPECT_EQ(Smart::Distance({ 10, 10, 17 }, { 13, 10, 17 }, true, RideId::GetNull()), 3u);
    surface->setOwnership({ OwnershipFlag::landOwned });
    Smart::Invalidate(getGameState());
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), 3u);
}

TEST_F(SmartPathfindingTests, OutsideGuestsStayOnOutsidePaths)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    for (int x = 10; x <= 13; ++x)
        MapGetSurfaceElementAt(TileCoordsXY{ x, 10 })->setOwnership({});
    auto* guest = GuestAt({ 10, 10, 14 });
    guest->outsideOfPark = true;
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, { 13, 10, 14 }, *guest, true, RideId::GetNull()), 2);
    // The same destination must have separate cached routes for inside and outside guests.
    guest->outsideOfPark = false;
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, { 13, 10, 14 }, *guest, true, RideId::GetNull()), kInvalidDirection);
    guest->outsideOfPark = true;
    MapGetSurfaceElementAt(TileCoordsXY{ 11, 10 })->setOwnership({ OwnershipFlag::landOwned });
    Smart::Invalidate(getGameState());
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, { 13, 10, 14 }, *guest, true, RideId::GetNull()), kInvalidDirection);
}

TEST_F(SmartPathfindingTests, SynchronizedStationsCycleAndStaySelectedAlongTheRoute)
{
    Line({ 10, 10, 14 }, { 11, 10, 14 });
    Line({ 10, 10, 14 }, { 10, 14, 14 });
    auto* ride = RideAllocateAtIndex(RideId::FromUnderlying(0));
    ASSERT_NE(ride, nullptr);
    ride->type = RIDE_TYPE_LOOPING_ROLLER_COASTER;
    ride->status = RideStatus::open;
    ride->departFlags = RIDE_DEPART_SYNCHRONISE_WITH_ADJACENT_STATIONS;
    for (auto& station : ride->getStations())
        station.entrance.setNull();
    const TileCoordsXYZ nearGoal{ 11, 10, 14 };
    const TileCoordsXYZ farGoal{ 10, 14, 14 };
    ride->getStation().entrance = { nearGoal, 0 };
    ride->getStation(StationIndex::FromUnderlying(2)).entrance = { farGoal, 0 };
    auto* guest = GuestAt({ 10, 10, 14 });
    for (uint16_t rides = 0; rides < 4; ++rides)
    {
        guest->guestNumRides = rides;
        guest->guestHeadingToRideId = ride->id;
        PathFinding::CalculateNextDestination(*guest);
        EXPECT_TRUE(TileCoordsXYZ(guest->pathfindGoal) == (rides % 2 == 0 ? nearGoal : farGoal));
        EXPECT_EQ(Smart::RideDistance(*guest, *ride), rides % 2 == 0 ? 1u : 4u);
    }
    guest->guestNumRides = 1;
    guest->nextLoc = TileCoordsXYZ{ 10, 11, 14 }.toCoordsXYZ();
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_TRUE(TileCoordsXYZ(guest->pathfindGoal) == farGoal);
    EXPECT_EQ(guest->guestHeadingToRideId, ride->id);

    // A disconnected station must not trap the guest or prevent using the ride.
    TileElementRemove(reinterpret_cast<TileElement*>(MapGetPathElementAt({ 10, 12, 14 })));
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_TRUE(TileCoordsXYZ(guest->pathfindGoal) == nearGoal);
    EXPECT_EQ(guest->guestHeadingToRideId, ride->id);

    ride->departFlags = 0;
    Connect({ 10, 11, 14 }, { 10, 12, 14 });
    Connect({ 10, 12, 14 }, { 10, 13, 14 });
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_TRUE(TileCoordsXYZ(guest->pathfindGoal) == nearGoal);
}

TEST_F(SmartPathfindingTests, ParkEntrancesRemainReachableFromBothSidesOfTheBoundary)
{
    Line({ 10, 10, 14 }, { 12, 10, 14 });
    Line({ 14, 10, 14 }, { 16, 10, 14 });
    for (int x = 14; x <= 16; ++x)
        MapGetSurfaceElementAt(TileCoordsXY{ x, 10 })->setOwnership({});
    const TileCoordsXYZ goal{ 13, 10, 14 };
    auto* entrance = TileElementInsert(goal.toCoordsXYZ(), 0xF, TileElementType::entrance)->asEntrance();
    entrance->setEntranceType(EntranceType::parkEntrance);
    entrance->setSequenceIndex(ParkEntranceSequence::centre);
    entrance->setDirection(0);
    MapGetPathElementAt({ 12, 10, 14 })->setEdges(0b0101);
    MapGetPathElementAt({ 14, 10, 14 })->setEdges(0b0101);
    auto* guest = GuestAt({ 10, 10, 14 });
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, goal, *guest, true, RideId::GetNull()), 2);
    EXPECT_EQ(PathFinding::ChooseDirection({ 16, 10, 14 }, goal, *guest, true, RideId::GetNull()), kInvalidDirection);
    guest->outsideOfPark = true;
    EXPECT_EQ(PathFinding::ChooseDirection({ 16, 10, 14 }, goal, *guest, true, RideId::GetNull()), 0);
    EXPECT_EQ(PathFinding::ChooseDirection({ 10, 10, 14 }, goal, *guest, true, RideId::GetNull()), kInvalidDirection);
}

TEST_F(SmartPathfindingTests, StationChoiceDoesNotChangeAfterPassingANoEntrySign)
{
    Line({ 10, 10, 14 }, { 11, 10, 14 });
    Line({ 10, 10, 14 }, { 10, 14, 14 });
    Connect({ 10, 13, 14 }, { 11, 13, 14 });
    auto* banner = TileElementInsert({ 10 * 32, 11 * 32, 14 * 8 + 16 }, 0, TileElementType::banner)->asBanner();
    banner->setAllowedEdges(0xF & ~(1 << 3));
    auto* ride = RideAllocateAtIndex(RideId::FromUnderlying(0));
    ASSERT_NE(ride, nullptr);
    ride->type = RIDE_TYPE_LOOPING_ROLLER_COASTER;
    ride->status = RideStatus::open;
    ride->departFlags = RIDE_DEPART_SYNCHRONISE_WITH_ADJACENT_STATIONS;
    for (auto& station : ride->getStations())
        station.entrance.setNull();
    ride->getStation().entrance = { 11, 10, 14, 0 };
    ride->getStation(StationIndex::FromUnderlying(1)).entrance = { 11, 13, 14, 0 };
    ride->getStation(StationIndex::FromUnderlying(2)).entrance = { 10, 14, 14, 0 };
    auto* guest = GuestAt({ 10, 10, 14 });
    guest->guestNumRides = 2;
    guest->guestHeadingToRideId = ride->id;
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_TRUE(
        TileCoordsXYZ(guest->pathfindGoal) == TileCoordsXYZ(ride->getStation(StationIndex::FromUnderlying(2)).entrance));
    guest->nextLoc = TileCoordsXYZ{ 10, 11, 14 }.toCoordsXYZ();
    ASSERT_EQ(Smart::Distance(TileCoordsXYZ(guest->nextLoc), { 11, 10, 14 }, true, ride->id), Smart::kUnreachable);
    PathFinding::CalculateNextDestination(*guest);
    EXPECT_TRUE(
        TileCoordsXYZ(guest->pathfindGoal) == TileCoordsXYZ(ride->getStation(StationIndex::FromUnderlying(2)).entrance));
}

TEST_F(SmartPathfindingTests, InvalidatesRememberedRoutesWhenPathsAreRemoved)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), 3u);
    TileElementRemove(reinterpret_cast<TileElement*>(MapGetPathElementAt({ 11, 10, 14 })));
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
    Connect({ 10, 10, 14 }, { 11, 10, 14 });
    Connect({ 11, 10, 14 }, { 12, 10, 14 });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), 3u);
}

#ifdef ENABLE_SCRIPTING
TEST_F(SmartPathfindingTests, ScriptPathEditsRefreshRoutesImmediately)
{
    Line({ 10, 10, 14 }, { 13, 10, 14 });
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), 3u);
    auto& engine = context->GetScriptEngine();
    engine.AddNetworkPlugin(R"(
        registerPlugin({
            name: 'smart-navigation-path-edit-test', version: '1.0.0', authors: ['test'],
            type: 'remote', licence: 'MIT', targetApiVersion: 124,
            main: function() {
                context.subscribe('interval.tick', function() {
                    map.getTile(11, 10).elements.forEach(function(element) {
                        if (element.type === 'footpath') element.edges = 0;
                    });
                });
            }
        });
    )");
    engine.LoadTransientPlugins();
    engine.Tick();
    engine.GetHookEngine().Call(Scripting::HookType::intervalTick, true);
    EXPECT_EQ(MapGetPathElementAt({ 11, 10, 14 })->getEdges(), 0);
    EXPECT_FALSE(getGameState().smartPathfinding.graphValid);
    EXPECT_EQ(Smart::Distance({ 10, 10, 14 }, { 13, 10, 14 }, true, RideId::GetNull()), Smart::kUnreachable);
}
#endif

TEST_F(SmartPathfindingTests, HandymanRoutesAroundObstaclesAndIgnoresUnreachableLitter)
{
    Line({ 10, 10, 14 }, { 10, 20, 14 });
    Line({ 10, 20, 14 }, { 20, 20, 14 });
    Line({ 20, 20, 14 }, { 20, 10, 14 });
    Path({ 11, 10, 14 });
    LitterAt({ 11, 10, 14 });
    LitterAt({ 20, 10, 14 });
    auto* staff = HandymanAt({ 10, 10, 14 });
    EXPECT_EQ(Smart::HandymanDirection(*staff), 1);
    auto* assignment = Smart::GetAssignment(*staff);
    ASSERT_NE(assignment, nullptr);
    EXPECT_EQ(assignment->destination, Smart::Key({ 20, 10, 14 }));
    EXPECT_EQ(assignment->job, Smart::Job::sweeping);
    EXPECT_EQ(staff->getActionDescription().type, PeepActionDescriptionType::headingToClean);
}

TEST_F(SmartPathfindingTests, ReservesJobsAndReassignsWhenTheyDisappear)
{
    Line({ 10, 10, 14 }, { 30, 10, 14 });
    LitterAt({ 15, 10, 14 });
    LitterAt({ 25, 10, 14 });
    auto* first = HandymanAt({ 10, 10, 14 });
    auto* second = HandymanAt({ 11, 10, 14 });
    EXPECT_NE(Smart::HandymanDirection(*first), kInvalidDirection);
    EXPECT_NE(Smart::HandymanDirection(*second), kInvalidDirection);
    auto firstTarget = Smart::GetAssignment(*first)->destination;
    EXPECT_NE(firstTarget, Smart::GetAssignment(*second)->destination);
    EXPECT_EQ(firstTarget, Smart::Key({ 15, 10, 14 }));
    Litter::removeAt({ 15 * 32 + 16, 10 * 32 + 16, 14 * 8 });
    EXPECT_NE(Smart::HandymanDirection(*first), kInvalidDirection);
    EXPECT_EQ(Smart::GetAssignment(*first)->job, Smart::Job::coverage);
}

TEST_F(SmartPathfindingTests, SharesCoverageAndSpreadsIdleHandymenAcrossRegions)
{
    Line({ 5, 10, 14 }, { 80, 10, 14 });
    auto* first = HandymanAt({ 40, 10, 14 });
    auto* second = HandymanAt({ 40, 10, 14 });
    EXPECT_NE(Smart::HandymanDirection(*first), kInvalidDirection);
    EXPECT_NE(Smart::HandymanDirection(*second), kInvalidDirection);
    auto firstKey = Smart::GetAssignment(*first)->destination;
    auto secondKey = Smart::GetAssignment(*second)->destination;
    EXPECT_NE((firstKey & 0xFFFF) / 16, (secondKey & 0xFFFF) / 16);
    EXPECT_EQ(getGameState().smartPathfinding.lastVisited.at(Smart::Key({ 40, 10, 14 })), getGameState().currentTicks);
    EXPECT_EQ(first->getActionDescription().type, PeepActionDescriptionType::coveringNeglectedPaths);
}

TEST_F(SmartPathfindingTests, RespectsPatrolBoundariesAndEnabledDuties)
{
    Line({ 8, 8, 14 }, { 30, 8, 14 });
    auto* staff = HandymanAt({ 9, 8, 14 });
    for (int x = 8; x < 12; ++x)
        staff->setPatrolArea({ x * 32, 8 * 32 }, true);
    LitterAt({ 20, 8, 14 });
    LitterAt({ 10, 8, 14 });
    EXPECT_NE(Smart::HandymanDirection(*staff), kInvalidDirection);
    EXPECT_EQ(Smart::GetAssignment(*staff)->destination, Smart::Key({ 10, 8, 14 }));
    staff->staffOrders = 0;
    EXPECT_NE(Smart::HandymanDirection(*staff), kInvalidDirection);
    EXPECT_EQ(Smart::GetAssignment(*staff)->job, Smart::Job::coverage);
    EXPECT_LT(Smart::GetAssignment(*staff)->destination & 0xFFFF, 12u);
}

TEST_F(SmartPathfindingTests, TenHandymenSpreadOutAndKeepAssignmentsAfterReload)
{
    for (int coordinate = 5; coordinate <= 205; coordinate += 20)
    {
        Line({ 5, coordinate, 14 }, { 205, coordinate, 14 });
        Line({ coordinate, 5, 14 }, { coordinate, 205, 14 });
    }
    std::vector<Staff*> staff;
    std::set<uint32_t> assignedRegions;
    std::vector<Smart::NodeKey> goals;
    for (int i = 0; i < 10; ++i)
    {
        auto* handyman = HandymanAt({ 105, 105, 14 });
        staff.push_back(handyman);
        ASSERT_TRUE(DirectionValid(Smart::HandymanDirection(*handyman)));
        auto goal = Smart::GetAssignment(*handyman)->destination;
        goals.push_back(goal);
        auto x = static_cast<uint16_t>(goal);
        auto y = static_cast<uint16_t>(goal >> 16);
        assignedRegions.insert(x / 16 | (y / 16 << 16));
    }
    EXPECT_GE(assignedRegions.size(), 8u);
    MemoryStream stream;
    DataSerialiser writer(true, stream);
    Smart::Serialise(getGameState(), writer);
    Smart::Reset(getGameState());
    stream.SetPosition(0);
    DataSerialiser reader(false, stream);
    Smart::Serialise(getGameState(), reader);
    for (size_t i = 0; i < staff.size(); ++i)
    {
        EXPECT_TRUE(DirectionValid(Smart::HandymanDirection(*staff[i])));
        EXPECT_EQ(Smart::GetAssignment(*staff[i])->destination, goals[i]);
    }
}

TEST_F(SmartPathfindingTests, ToggleAndCoverageStateRoundTrip)
{
    auto& state = getGameState();
    state.smartPathfinding.lastVisited[Smart::Key({ 10, 10, 14 })] = 123;
    state.smartPathfinding.assignments[4] = { Smart::Key({ 11, 10, 14 }), Smart::Job::coverage };
    MemoryStream saved;
    DataSerialiser writer(true, saved);
    CheatsSerialise(writer);
    Smart::Serialise(state, writer);
    CheatsReset();
    saved.SetPosition(0);
    DataSerialiser reader(false, saved);
    CheatsSerialise(reader);
    Smart::Serialise(state, reader);
    EXPECT_TRUE(state.cheats.smartGuestNavigation);
    EXPECT_TRUE(state.cheats.smartHandymanDispatch);
    EXPECT_EQ(state.smartPathfinding.lastVisited.at(Smart::Key({ 10, 10, 14 })), 123u);
    EXPECT_EQ(state.smartPathfinding.assignments.at(4).destination, Smart::Key({ 11, 10, 14 }));
    EXPECT_FALSE(state.smartPathfinding.graphValid);
    GameActions::CheatSetAction disable(CheatType::smartHandymanDispatch, 0);
    ASSERT_EQ(disable.Execute(state, state.park).error, GameActions::Status::ok);
    EXPECT_TRUE(state.smartPathfinding.assignments.empty());
    EXPECT_TRUE(state.cheats.smartGuestNavigation);
    MemoryStream old;
    DataSerialiser oldWriter(true, old);
    oldWriter << uint16_t{ 0 };
    old.SetPosition(0);
    DataSerialiser oldReader(false, old);
    CheatsSerialise(oldReader);
    EXPECT_FALSE(state.cheats.smartGuestNavigation);
    EXPECT_FALSE(state.cheats.smartHandymanDispatch);
}

TEST_F(SmartPathfindingTests, DispatchesFullBinsAndStopsWhenBinsAreEmptied)
{
    Line({ 10, 10, 14 }, { 20, 10, 14 });
    auto* object = context->GetObjectManager().LoadObject(ObjectEntryDescriptor("rct2.footpath_item.litter1"));
    ASSERT_NE(object, nullptr);
    auto* bin = MapGetPathElementAt({ 20, 10, 14 });
    bin->setAdditionEntryIndex(ObjectManagerGetLoadedObjectEntryIndex(object));
    bin->setAdditionStatus(0);
    auto* staff = HandymanAt({ 10, 10, 14 });
    EXPECT_EQ(Smart::HandymanDirection(*staff), 2);
    ASSERT_NE(Smart::GetAssignment(*staff), nullptr);
    EXPECT_EQ(Smart::GetAssignment(*staff)->job, Smart::Job::emptyingBin);
    EXPECT_EQ(Smart::GetAssignment(*staff)->destination, Smart::Key({ 20, 10, 14 }));
    EXPECT_EQ(staff->getActionDescription().type, PeepActionDescriptionType::headingToEmptyBin);
    bin->setAdditionStatus(0xFF);
    EXPECT_NE(Smart::HandymanDirection(*staff), kInvalidDirection);
    EXPECT_EQ(Smart::GetAssignment(*staff)->job, Smart::Job::coverage);
}

TEST_F(SmartPathfindingTests, GuestsActuallyWalkRoutesInARealPark)
{
    ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("pathfinding-tests.sv6")));
    GameLoadInit();
    getGameState().cheats.smartGuestNavigation = true;
    const std::pair<const char*, TileCoordsXYZ> scenarios[] = {
        { "UBend", { 17, 9, 14 } },
        { "StraightUpBridge", { 12, 15, 14 } },
        { "StraightUpSlope", { 14, 15, 14 } },
        { "SelfCrossingPath", { 6, 5, 14 } },
    };
    for (const auto& [name, start] : scenarios)
    {
        SCOPED_TRACE(name);
        Ride* target = nullptr;
        for (auto& ride : RideManager(getGameState()))
        {
            if (ride.getName().starts_with(name))
                target = &ride;
        }
        ASSERT_NE(target, nullptr);
        target->status = RideStatus::open;
        auto entrance = target->getStation().entrance;
        TileCoordsXYZ goal = { entrance.x - TileDirectionDelta[entrance.direction].x,
                               entrance.y - TileDirectionDelta[entrance.direction].y, entrance.z };
        auto* guest = Guest::generate(start.toCoordsXYZ().toTileCentre());
        ASSERT_NE(guest, nullptr);
        guest->outsideOfPark = false;
        guest->guestHeadingToRideId = target->id;
        auto direction = PathFinding::ChooseDirection(start, goal, *guest, true, target->id);
        ASSERT_TRUE(DirectionValid(direction));
        guest->peepDirection = direction;
        guest->setDestination(guest->getLocation() + CoordsXYZ{ CoordsDirectionDelta[direction], 0 }, 2);
        for (int step = 0; step < 4000 && TileCoordsXYZ(guest->getLocation()) != goal; ++step)
            guest->performNextAction();
        EXPECT_TRUE(TileCoordsXYZ(guest->getLocation()) == goal);
        PeepEntityRemove(guest);
    }
}

TEST_F(SmartPathfindingTests, HandymanActuallyWalksToAndCleansDistantVomit)
{
    ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("pathfinding-tests.sv6")));
    GameLoadInit();
    auto& state = getGameState();
    state.cheats.smartHandymanDispatch = true;
    Ride* ride = nullptr;
    for (auto& candidate : RideManager(state))
    {
        if (candidate.getName().starts_with("UBend"))
            ride = &candidate;
    }
    ASSERT_NE(ride, nullptr);
    auto entrance = ride->getStation().entrance;
    TileCoordsXYZ goal = { entrance.x - TileDirectionDelta[entrance.direction].x,
                           entrance.y - TileDirectionDelta[entrance.direction].y, entrance.z };
    TileCoordsXYZ start{ 17, 9, 14 };
    auto litterId = LitterAt(goal)->id;
    GameActions::StaffHireNewAction hire(false, StaffType::handyman, kObjectEntryIndexNull, STAFF_ORDERS_SWEEPING);
    auto result = hire.Execute(state, state.park);
    ASSERT_EQ(result.error, GameActions::Status::ok);
    auto staffId = result.getData<GameActions::StaffHireNewActionResult>().StaffEntityId;
    auto* staff = state.entities.getEntity<Staff>(staffId);
    ASSERT_NE(staff, nullptr);
    staff->state = PeepState::patrolling;
    staff->nextLoc = start.toCoordsXYZ();
    staff->setNextFlags(0, false, false);
    staff->moveToAndUpdateSpatialIndex(start.toCoordsXYZ().toTileCentre());
    auto direction = Smart::HandymanDirection(*staff);
    ASSERT_TRUE(DirectionValid(direction));
    staff->peepDirection = direction;
    staff->setDestination(staff->getLocation() + CoordsXYZ{ CoordsDirectionDelta[direction], 0 }, 2);
    for (int tick = 0; tick < 8000 && staff->staffLitterSwept == 0; ++tick)
    {
        ++state.currentTicks;
        staff->update();
        state.entities.updateEntitiesSpatialIndex();
    }
    EXPECT_GT(staff->staffLitterSwept, 0u);
    EXPECT_EQ(state.entities.tryGetEntity<Litter>(litterId), nullptr);
}

TEST_F(SmartPathfindingTests, ParkSaveRestoresOptionalCoverageChunkAndLabels)
{
    ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_with_ferris_wheel.sv6")));
    auto& state = getGameState();
    state.cheats.smartGuestNavigation = true;
    state.cheats.smartHandymanDispatch = true;
    auto key = Smart::Key({ 10, 10, 14 });
    state.smartPathfinding.lastVisited[key] = 42;
    MemoryStream stream;
    ParkFileExporter exporter;
    exporter.Export(state, stream, kParkFileSaveCompressionLevel);
    CheatsReset();
    stream.SetPosition(0);
    auto importer = ParkImporter::CreateParkFile(context->GetObjectRepository());
    auto result = importer->LoadFromStream(&stream, false);
    context->GetObjectManager().LoadObjects(result.RequiredObjects);
    importer->Import(state);
    EXPECT_TRUE(state.cheats.smartGuestNavigation);
    EXPECT_TRUE(state.cheats.smartHandymanDispatch);
    EXPECT_EQ(state.smartPathfinding.lastVisited.at(key), 42u);
    ASSERT_TRUE(LanguageOpen(LANGUAGE_ENGLISH_US));
    EXPECT_STREQ(CheatsGetName(CheatType::smartGuestNavigation), "Smart guest navigation");
    EXPECT_STREQ(CheatsGetName(CheatType::smartHandymanDispatch), "Smart handyman dispatch");
}
