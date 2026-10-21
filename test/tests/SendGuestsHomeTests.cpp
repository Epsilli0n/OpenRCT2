/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
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
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/world/Map.h>
#include <tuple>

using namespace OpenRCT2;

class SendGuestsHomeTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;
    LegacyScene previousScene;

    void SetUp() override
    {
        previousScene = gLegacyScene;
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_TRUE(context->Initialise());
        ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_with_ferris_wheel.sv6")));
        GameLoadInit();
        auto& state = getGameState();
        state.entities.resetAllEntities();
        state.park.numGuestsInPark = 0;
        state.park.numGuestsHeadingForPark = 0;
        ASSERT_FALSE(state.park.entrances.empty());
    }

    void TearDown() override
    {
        context.reset();
        gLegacyScene = previousScene;
    }

    Guest* AddGuest()
    {
        auto& state = getGameState();
        const auto entrance = state.park.entrances.front();
        const auto location = entrance.toTileCentre() + CoordsXYZ{ CoordsDirectionDelta[entrance.direction], 0 };
        auto* guest = Guest::generate(location);
        if (guest != nullptr)
        {
            guest->moveToAndUpdateSpatialIndex(location);
            guest->outsideOfPark = false;
            state.park.numGuestsHeadingForPark--;
            state.park.numGuestsInPark++;
        }
        return guest;
    }

    void SendHome()
    {
        auto& state = getGameState();
        GameActions::CheatSetAction action(CheatType::sendAllGuestsHome);
        ASSERT_EQ(action.Query(state, state.park).error, GameActions::Status::ok);
        ASSERT_EQ(action.Execute(state, state.park).error, GameActions::Status::ok);
    }

    static auto Stats(const Guest& guest)
    {
        return std::make_tuple(
            guest.happiness, guest.happinessTarget, guest.energy, guest.energyTarget, guest.hunger, guest.thirst, guest.nausea,
            guest.nauseaTarget, guest.toilet, guest.cashInPocket, guest.cashSpent, guest.paidToEnter, guest.paidOnRides,
            guest.paidOnFood, guest.paidOnDrink, guest.paidOnSouvenirs, guest.itemFlags, guest.guestNumRides, guest.intensity,
            guest.nauseaTolerance, guest.angriness);
    }
};

TEST_F(SendGuestsHomeTests, SetsLeavingIntentWithoutChangingStatsOrRemovingGuests)
{
    auto& state = getGameState();
    const auto parkFlags = state.park.flags;
    for (const auto peepState : { PeepState::walking, PeepState::sitting, PeepState::watching, PeepState::picked })
    {
        auto* guest = AddGuest();
        ASSERT_NE(guest, nullptr);
        guest->state = peepState;
        guest->happiness = 231;
        guest->happinessTarget = 219;
        guest->guestHeadingToRideId = RideId::FromUnderlying(0);
        guest->peepFlags.set(PeepFlag::tracking, PeepFlag::parkEntranceChosen);
        guest->pathfindGoal = { 10, 10, 14, 0 };
        const auto stats = Stats(*guest);
        const auto location = guest->getLocation();
        const auto guestCount = state.park.numGuestsInPark;

        SendHome();

        EXPECT_EQ(Stats(*guest), stats);
        EXPECT_EQ(guest->getLocation(), location);
        EXPECT_EQ(guest->state, peepState);
        EXPECT_TRUE(guest->peepFlags.has(PeepFlag::leavingPark));
        EXPECT_TRUE(guest->peepFlags.has(PeepFlag::tracking));
        EXPECT_FALSE(guest->peepFlags.has(PeepFlag::parkEntranceChosen));
        EXPECT_TRUE(guest->guestHeadingToRideId.IsNull());
        EXPECT_TRUE(guest->pathfindGoal.isNull());
        EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::goHome);
        EXPECT_EQ(state.park.numGuestsInPark, guestCount);
    }
    EXPECT_EQ(state.park.flags, parkFlags);
}

TEST_F(SendGuestsHomeTests, LeavesQueuesWithoutDisruptingBoardingOrRidingGuests)
{
    auto* ride = RideAllocateAtIndex(RideId::FromUnderlying(100));
    ASSERT_NE(ride, nullptr);
    ride->type = RIDE_TYPE_FERRIS_WHEEL;
    ride->numStations = 1;
    auto& station = ride->getStation(StationIndex::FromUnderlying(0));
    station.queueLength = 0;
    station.lastPeepInQueue = EntityId::GetNull();
    std::vector<Guest*> queued;
    for (int i = 0; i < 3; ++i)
    {
        auto* guest = AddGuest();
        ASSERT_NE(guest, nullptr);
        guest->state = PeepState::queuing;
        guest->currentRide = ride->id;
        guest->currentRideStation = StationIndex::FromUnderlying(0);
        guest->guestNextInQueue = station.lastPeepInQueue;
        station.lastPeepInQueue = guest->id;
        station.queueLength++;
        queued.push_back(guest);
    }
    for (const auto peepState : { PeepState::queuingFront, PeepState::enteringRide, PeepState::onRide, PeepState::leavingRide })
    {
        auto* guest = AddGuest();
        ASSERT_NE(guest, nullptr);
        guest->state = peepState;
        guest->currentRide = ride->id;
        guest->currentSeat = 3;
        const auto stats = Stats(*guest);

        SendHome();

        EXPECT_EQ(guest->state, peepState);
        EXPECT_EQ(guest->currentRide, ride->id);
        EXPECT_EQ(guest->currentSeat, 3);
        EXPECT_EQ(Stats(*guest), stats);
    }
    EXPECT_EQ(station.queueLength, 0);
    EXPECT_TRUE(station.lastPeepInQueue.IsNull());
    for (const auto* guest : queued)
        EXPECT_EQ(guest->state, PeepState::one);
    for (const auto* guest : EntityList<Guest>())
        EXPECT_TRUE(guest->peepFlags.has(PeepFlag::leavingPark));
    SendHome();
    EXPECT_EQ(station.queueLength, 0);
}

TEST_F(SendGuestsHomeTests, DoesNotAffectStaffOutsideGuestsOrFutureArrivals)
{
    auto* outside = AddGuest();
    ASSERT_NE(outside, nullptr);
    outside->outsideOfPark = true;
    const auto stats = Stats(*outside);
    const auto flags = outside->peepFlags;
    auto* staff = getGameState().entities.createEntity<Staff>();
    ASSERT_NE(staff, nullptr);
    staff->state = PeepState::patrolling;
    const auto staffFlags = staff->peepFlags;

    SendHome();

    EXPECT_EQ(Stats(*outside), stats);
    EXPECT_EQ(outside->peepFlags, flags);
    EXPECT_EQ(staff->state, PeepState::patrolling);
    EXPECT_EQ(staff->peepFlags, staffFlags);
    auto* arrival = AddGuest();
    ASSERT_NE(arrival, nullptr);
    EXPECT_FALSE(arrival->peepFlags.has(PeepFlag::leavingPark));
    ASSERT_TRUE(LanguageOpen(LANGUAGE_ENGLISH_US));
    EXPECT_STREQ(CheatsGetName(CheatType::sendAllGuestsHome), "Send all guests home");
}

TEST_F(SendGuestsHomeTests, GuestsWalkOutThroughTheParkExitWithEitherNavigationMode)
{
    auto& state = getGameState();
    for (const bool smartNavigation : { false, true })
    {
        SCOPED_TRACE(smartNavigation);
        state.cheats.smartGuestNavigation = smartNavigation;
        auto* guest = AddGuest();
        ASSERT_NE(guest, nullptr);
        const auto id = guest->id;
        SendHome();
        ASSERT_NE(state.entities.getEntity<Guest>(id), nullptr);

        for (int tick = 0; tick < 4000; ++tick)
        {
            auto* remaining = state.entities.getEntity<Guest>(id);
            if (remaining == nullptr)
                break;
            remaining->update();
            state.currentTicks++;
        }
        EXPECT_EQ(state.entities.getEntity<Guest>(id), nullptr);
        EXPECT_EQ(state.park.numGuestsInPark, 0u);
    }
}
