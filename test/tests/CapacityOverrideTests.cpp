/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <gtest/gtest.h>
#include <openrct2/Cheats.h>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/actions/cheats/CheatSetAction.h>
#include <openrct2/core/DataSerialiser.h>
#include <openrct2/core/OrcaStream.hpp>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/RideObject.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/ride/CapacityOverride.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/RideEntry.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/Vehicle.h>

using namespace OpenRCT2;
namespace Capacity = OpenRCT2::CapacityOverride;

class CapacityOverrideTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;
    Ride* ride{};
    Vehicle* vehicle{};
    LegacyScene previousScene;

    void SetUp() override
    {
        previousScene = gLegacyScene;
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_TRUE(context->Initialise());
        ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_car_ride_one_car.sv6")));
        getGameState().capacityOverride = {};
        auto* object = context->GetObjectManager().LoadObject(ObjectEntryDescriptor("rct2.ride.zldb"));
        ASSERT_NE(object, nullptr);
        ride = RideAllocateAtIndex(RideId::FromUnderlying(100));
        ASSERT_NE(ride, nullptr);
        ride->subtype = ObjectManagerGetLoadedObjectEntryIndex(object);
        ride->type = static_cast<RideObject*>(object)->GetEntry().GetFirstNonNullRideType();
        ride->status = RideStatus::closed;
        ride->numRiders = 0;
        ride->numTrains = 1;
        ride->numCarsPerTrain = 1;
        ride->numStations = 1;
        ride->mode = RideMode::continuousCircuit;
        ride->getStation(StationIndex::FromUnderlying(0)).entrance = { 10, 10, 14, 0 };
        ride->getStation(StationIndex::FromUnderlying(0)).exit = { 11, 10, 14, 2 };
        ride->getStation(StationIndex::FromUnderlying(0)).start = { 10 * 32, 10 * 32 };
        vehicle = getGameState().entities.createEntity<Vehicle>();
        ASSERT_NE(vehicle, nullptr);
        vehicle->SubType = Vehicle::Type::head;
        vehicle->ride = ride->id;
        vehicle->ride_subtype = ride->subtype;
        vehicle->vehicle_type = ride->getRideEntry()->DefaultCar;
        vehicle->num_seats = ride->getRideEntry()->Cars[vehicle->vehicle_type].numSeats;
        vehicle->num_peeps = vehicle->next_free_seat = 0;
        vehicle->mass = 500;
        std::fill(std::begin(vehicle->peep), std::end(vehicle->peep), EntityId::GetNull());
        vehicle->next_vehicle_on_train = EntityId::GetNull();
        vehicle->next_vehicle_on_ride = vehicle->prev_vehicle_on_ride = vehicle->id;
        vehicle->current_station = StationIndex::FromUnderlying(0);
        vehicle->restraints_position = 255;
        vehicle->SetState(Vehicle::Status::waitingForPassengers, 1);
        vehicle->moveTo({ 336, 336, 112 });
        ride->vehicles[0] = vehicle->id;
        ride->flags.set(RideFlag::onTrack, RideFlag::tested);
        ASSERT_TRUE(Capacity::Supports(*ride));
    }

    void TearDown() override
    {
        context.reset();
        gLegacyScene = previousScene;
    }

    GameActions::Status SetCapacity(int64_t value)
    {
        GameActions::CheatSetAction action(CheatType::setRideCapacity, ride->id.ToUnderlying(), value);
        auto result = action.Query(getGameState(), getGameState().park);
        if (result.error != GameActions::Status::ok)
            return result.error;
        return action.Execute(getGameState(), getGameState().park).error;
    }

    Guest* Reserve()
    {
        auto* guest = Guest::generate({ 336, 336, 112 });
        if (!guest)
            return nullptr;
        guest->currentRide = ride->id;
        guest->currentTrain = guest->currentCar = 0;
        guest->currentRideStation = StationIndex::FromUnderlying(0);
        guest->mass = 60;
        guest->cashInPocket = 100.00_GBP;
        guest->rideSubState = PeepRideSubState::enterVehicle;
        guest->energy = 255;
        if (!Capacity::Reserve(*vehicle, *guest))
        {
            guest->remove();
            return nullptr;
        }
        guest->outsideOfPark = false;
        ++getGameState().park.numGuestsInPark;
        guest->setState(PeepState::enteringRide);
        return guest;
    }

    static void Tick(Guest& guest)
    {
        guest.stepProgress = 255;
        guest.update();
    }

    Guest* QueueAtEntrance()
    {
        auto* guest = Guest::generate({ 336, 336, 112 });
        if (!guest)
            return nullptr;
        guest->currentRide = ride->id;
        guest->currentRideStation = StationIndex::FromUnderlying(0);
        guest->state = PeepState::queuingFront;
        guest->rideSubState = PeepRideSubState::atEntrance;
        guest->energy = 255;
        guest->destinationTolerance = 0;
        ride->queueInsertGuestAtFront(guest->currentRideStation, guest);
        return guest;
    }

    void RoundTrip()
    {
        MemoryStream stream;
        ParkFileExporter exporter;
        exporter.Export(getGameState(), stream, kParkFileSaveCompressionLevel);
        auto rideId = ride->id;
        auto vehicleId = vehicle->id;
        stream.SetPosition(0);
        auto importer = ParkImporter::CreateParkFile(context->GetObjectRepository());
        auto result = importer->LoadFromStream(&stream, false);
        context->GetObjectManager().LoadObjects(result.RequiredObjects);
        importer->Import(getGameState());
        ride = GetRide(rideId);
        vehicle = getGameState().entities.tryGetEntity<Vehicle>(vehicleId);
    }
};

TEST_F(CapacityOverrideTests, ValidatesBoundsAndRequiresClosedEmptyCoaster)
{
    EXPECT_NE(SetCapacity(-1), GameActions::Status::ok);
    EXPECT_NE(SetCapacity(65536), GameActions::Status::ok);
    EXPECT_EQ(SetCapacity(65535), GameActions::Status::ok);
    EXPECT_EQ(Capacity::SeatCount(*vehicle), 65535u);
    ride->status = RideStatus::open;
    EXPECT_NE(SetCapacity(100), GameActions::Status::ok);
    ride->status = RideStatus::closed;
    auto* guest = Reserve();
    ASSERT_NE(guest, nullptr);
    EXPECT_NE(SetCapacity(0), GameActions::Status::ok);
    guest->remove();
    EXPECT_EQ(SetCapacity(0), GameActions::Status::ok);
    EXPECT_EQ(Capacity::SeatCount(*vehicle), static_cast<uint32_t>(vehicle->num_seats & kVehicleSeatNumMask));
    ride->type = RIDE_TYPE_FERRIS_WHEEL;
    EXPECT_NE(SetCapacity(100), GameActions::Status::ok);
}

TEST_F(CapacityOverrideTests, HundredRealGuestsBoardAndUnloadWithoutChangingVisualSeatsOrPhysics)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    getGameState().park.flags = { ParkFlag::freeEntry };
    ride->price[0] = 1.00_GBP;
    const auto originalSeats = vehicle->num_seats;
    const auto visualSeats = originalSeats & kVehicleSeatNumMask;
    std::vector<Guest*> guests;
    for (int i = 0; i < 100; ++i)
    {
        auto* guest = Reserve();
        ASSERT_NE(guest, nullptr);
        EXPECT_LT(guest->currentSeat, visualSeats);
        guests.push_back(guest);
    }
    EXPECT_EQ(Reserve(), nullptr);
    // Boarding order must use logical seats, including those sharing a visual seat.
    Tick(*guests.back());
    EXPECT_EQ(guests.back()->state, PeepState::enteringRide);
    for (auto* guest : guests)
    {
        guest->rideSubState = PeepRideSubState::freeVehicleCheck;
        Tick(*guest);
        guest->rideSubState = PeepRideSubState::enterVehicle;
        Tick(*guest);
        EXPECT_EQ(guest->state, PeepState::onRide);
        EXPECT_EQ(guest->x, kLocationNull);
        EXPECT_EQ(guest->paidOnRides, 1.00_GBP);
    }
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 100u);
    EXPECT_EQ(vehicle->num_peeps, visualSeats);
    EXPECT_EQ(vehicle->num_seats, originalSeats);
    EXPECT_EQ(vehicle->mass, 500 + 60 * visualSeats);
    EXPECT_EQ(ride->curNumCustomers, 100);
    EXPECT_EQ(ride->numRiders, 100);
    EXPECT_EQ(ride->totalProfit, 100.00_GBP);
    vehicle->SetState(Vehicle::Status::unloadingPassengers, 1);
    vehicle->update();
    EXPECT_EQ(vehicle->status, Vehicle::Status::unloadingPassengers);
    for (auto it = guests.rbegin(); it != guests.rend(); ++it)
    {
        EXPECT_EQ((*it)->state, PeepState::leavingRide);
        for (int tick = 0; tick < 4; ++tick)
            Tick(**it);
        EXPECT_NE((*it)->rideSubState, PeepRideSubState::leaveVehicle);
        EXPECT_NE((*it)->x, kLocationNull);
    }
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 0u);
    EXPECT_EQ(vehicle->num_peeps, 0);
    EXPECT_EQ(vehicle->mass, 500);
    EXPECT_TRUE(getGameState().capacityOverride.manifests.empty());
    EXPECT_TRUE(getGameState().capacityOverride.seats.empty());
    vehicle->update();
    EXPECT_EQ(vehicle->status, Vehicle::Status::movingToEndOfStation);
}

TEST_F(CapacityOverrideTests, SingleSeatOverrideDoesNotWaitForNativePair)
{
    ASSERT_EQ(SetCapacity(1), GameActions::Status::ok);
    vehicle->num_seats |= kVehicleSeatPairFlag;
    auto* guest = Reserve();
    ASSERT_NE(guest, nullptr);
    Tick(*guest);
    EXPECT_EQ(guest->state, PeepState::onRide);
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 1u);
    EXPECT_EQ(Reserve(), nullptr);
}

TEST_F(CapacityOverrideTests, FullLoadOver255WaitsForAllReservationsToBoard)
{
    ASSERT_EQ(SetCapacity(300), GameActions::Status::ok);
    ride->status = RideStatus::open;
    ride->departFlags = RIDE_DEPART_WAIT_FOR_LOAD | 3;
    vehicle->SetState(Vehicle::Status::waitingForPassengers, 1);
    vehicle->time_waiting = 100;
    std::vector<Guest*> guests;
    for (int i = 0; i < 300; ++i)
    {
        auto* guest = Reserve();
        ASSERT_NE(guest, nullptr);
        guests.push_back(guest);
        if (i < 299)
            Tick(*guest);
    }
    vehicle->update();
    EXPECT_EQ(vehicle->sub_state, 1);
    Tick(*guests.back());
    vehicle->update();
    EXPECT_EQ(vehicle->sub_state, 2);
}

TEST_F(CapacityOverrideTests, SaveRestoresMidBoardingAndUnloadingManifests)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    std::vector<EntityId> guests;
    for (int i = 0; i < 100; ++i)
    {
        auto* guest = Reserve();
        ASSERT_NE(guest, nullptr);
        guests.push_back(guest->id);
        if (i < 50)
            Tick(*guest);
    }
    RoundTrip();
    ASSERT_NE(vehicle, nullptr);
    EXPECT_EQ(Capacity::Get(*ride), 100);
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 50u);
    EXPECT_EQ(Capacity::ReservedCount(*vehicle), 100u);
    for (int i = 50; i < 100; ++i)
        Tick(*getGameState().entities.tryGetEntity<Guest>(guests[i]));
    vehicle->SetState(Vehicle::Status::unloadingPassengers, 1);
    vehicle->update();
    for (int tick = 0; tick < 4; ++tick)
        Tick(*getGameState().entities.tryGetEntity<Guest>(guests.back()));
    RoundTrip();
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 99u);
    EXPECT_EQ(Capacity::ReservedCount(*vehicle), 0u);
    for (int i = 98; i >= 0; --i)
        for (int tick = 0; tick < 4; ++tick)
            Tick(*getGameState().entities.tryGetEntity<Guest>(guests[i]));
    EXPECT_FALSE(Capacity::HasManifest(*vehicle));
    EXPECT_EQ(vehicle->num_peeps, 0);
}

TEST_F(CapacityOverrideTests, RemovedGuestDoesNotBlockRemainingBoarders)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    auto* first = Reserve();
    auto* second = Reserve();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->remove();
    Tick(*second);
    EXPECT_EQ(second->state, PeepState::onRide);
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 1u);
    GameActions::CheatSetAction remove(CheatType::removeAllGuests);
    EXPECT_EQ(remove.Execute(getGameState(), getGameState().park).error, GameActions::Status::ok);
    EXPECT_FALSE(Capacity::HasManifest(*vehicle));
    EXPECT_TRUE(getGameState().capacityOverride.seats.empty());
    EXPECT_EQ(vehicle->num_peeps, 0);
}

TEST_F(CapacityOverrideTests, LegacyParkLoadClearsOverrides)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    ASSERT_NE(Reserve(), nullptr);
    ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_car_ride_one_car.sv6")));
    EXPECT_TRUE(getGameState().capacityOverride.overrides.empty());
    EXPECT_TRUE(getGameState().capacityOverride.seats.empty());
}

TEST_F(CapacityOverrideTests, ForcedEjectionClearsAllPassengersAndRestoresPhysicalMass)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    std::vector<Guest*> guests;
    for (int i = 0; i < 100; ++i)
    {
        auto* guest = Reserve();
        ASSERT_NE(guest, nullptr);
        Tick(*guest);
        guests.push_back(guest);
    }
    ride->removePeeps();
    EXPECT_FALSE(Capacity::HasManifest(*vehicle));
    EXPECT_EQ(vehicle->num_peeps, 0);
    EXPECT_EQ(vehicle->next_free_seat, 0);
    EXPECT_EQ(vehicle->mass, 500);
    EXPECT_EQ(ride->numRiders, 0);
    for (auto* guest : guests)
    {
        EXPECT_EQ(guest->state, PeepState::falling);
        EXPECT_NE(guest->x, kLocationNull);
    }
    EXPECT_EQ(SetCapacity(0), GameActions::Status::ok);
}

TEST_F(CapacityOverrideTests, OverrideDoesNotCreateSeatsInDecorativeCars)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    vehicle->num_seats = 0;
    EXPECT_EQ(Capacity::SeatCount(*vehicle), 0u);
    EXPECT_FALSE(Capacity::Enabled(*vehicle));
}

TEST_F(CapacityOverrideTests, QueueSelectionCanReserveBeyondVisualSeatCount)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    ride->status = RideStatus::open;
    ride->price[0] = 0;
    ride->getStation(StationIndex::FromUnderlying(0)).trainAtStation = 0;
    for (int i = 0; i < 101; ++i)
    {
        auto* guest = QueueAtEntrance();
        ASSERT_NE(guest, nullptr);
        Tick(*guest);
        EXPECT_EQ(guest->state, i < 100 ? PeepState::enteringRide : PeepState::queuingFront);
    }
    EXPECT_EQ(Capacity::ReservedCount(*vehicle), 100u);
    EXPECT_EQ(vehicle->next_free_seat, 2);
}

TEST_F(CapacityOverrideTests, CrashRemovesEveryOverflowGuest)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    std::vector<EntityId> guests;
    for (int i = 0; i < 100; ++i)
    {
        auto* guest = Reserve();
        ASSERT_NE(guest, nullptr);
        Tick(*guest);
        guests.push_back(guest->id);
    }
    const auto initialGuests = getGameState().park.numGuestsInPark;
    vehicle->moveTo({ 336, 336, 0 });
    vehicle->SetState(Vehicle::Status::crashing, 0);
    vehicle->update();
    EXPECT_EQ(vehicle->status, Vehicle::Status::crashed);
    EXPECT_FALSE(Capacity::HasManifest(*vehicle));
    EXPECT_EQ(vehicle->num_peeps, 0);
    EXPECT_EQ(getGameState().park.numGuestsInPark, initialGuests - 100);
    for (auto guest : guests)
        EXPECT_EQ(getGameState().entities.tryGetEntity<Guest>(guest), nullptr);
}

TEST_F(CapacityOverrideTests, SavesRequireCustomBuildUntilAllOverridesAreReset)
{
    for (const uint16_t capacity : { 100, 0 })
    {
        ASSERT_EQ(SetCapacity(capacity), GameActions::Status::ok);
        MemoryStream stream;
        ParkFileExporter exporter;
        exporter.Export(getGameState(), stream, kParkFileSaveCompressionLevel);
        stream.SetPosition(0);
        OrcaStream saved(stream, OrcaStream::Mode::reading);
        EXPECT_EQ(saved.getHeader().targetVersion, kParkFileCurrentVersion);
        EXPECT_EQ(saved.getHeader().minVersion, capacity == 0 ? kParkFileMinVersion : kParkFileCapacityOverrideMinVersion);
    }
}

TEST_F(CapacityOverrideTests, MaximumWaitStopsAdmissionWhileExistingPassengersFinishBoarding)
{
    ASSERT_EQ(SetCapacity(1000), GameActions::Status::ok);
    ride->status = RideStatus::open;
    ride->price[0] = 0;
    ride->departFlags = RIDE_DEPART_WAIT_FOR_LOAD | 3 | RIDE_DEPART_WAIT_FOR_MAXIMUM_LENGTH;
    ride->maxWaitingTime = 20;
    ride->getStation(StationIndex::FromUnderlying(0)).trainAtStation = 0;
    auto* boarded = Reserve();
    ASSERT_NE(boarded, nullptr);
    Tick(*boarded);
    auto* approaching = Reserve();
    ASSERT_NE(approaching, nullptr);

    // Admission still works just before the native 20-second maximum expires.
    vehicle->time_waiting = 20 * 32 - 1;
    vehicle->update();
    auto* admitted = QueueAtEntrance();
    ASSERT_NE(admitted, nullptr);
    Tick(*admitted);
    ASSERT_EQ(admitted->state, PeepState::enteringRide);
    ASSERT_EQ(Capacity::ReservedCount(*vehicle), 3u);

    vehicle->update();
    ASSERT_TRUE(vehicle->flags.has(VehicleFlag::readyToDepart));
    ASSERT_EQ(vehicle->sub_state, 1);
    auto* queued = QueueAtEntrance();
    ASSERT_NE(queued, nullptr);
    for (int tick = 0; tick < 100; ++tick)
    {
        Tick(*queued);
        vehicle->update();
        EXPECT_EQ(queued->state, PeepState::queuingFront);
        EXPECT_EQ(Capacity::ReservedCount(*vehicle), 3u);
    }

    // These two passengers already have seats; they must not be abandoned.
    approaching->rideSubState = PeepRideSubState::enterVehicle;
    admitted->rideSubState = PeepRideSubState::enterVehicle;
    Tick(*approaching);
    Tick(*admitted);
    EXPECT_EQ(approaching->state, PeepState::onRide);
    EXPECT_EQ(admitted->state, PeepState::onRide);
    vehicle->update();
    EXPECT_EQ(vehicle->sub_state, 2);
    EXPECT_EQ(Capacity::BoardedCount(*vehicle), 3u);
    EXPECT_EQ(queued->state, PeepState::queuingFront);
    EXPECT_EQ(ride->getStation(StationIndex::FromUnderlying(0)).trainAtStation, RideStation::kNoTrain);
}

TEST_F(CapacityOverrideTests, ExpiredBoardingCutoffSurvivesReloadAndReopensOnNextStationVisit)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    ride->status = RideStatus::open;
    ride->departFlags = RIDE_DEPART_WAIT_FOR_LOAD | 3 | RIDE_DEPART_WAIT_FOR_MAXIMUM_LENGTH;
    ride->maxWaitingTime = 20;
    auto* guest = Reserve();
    ASSERT_NE(guest, nullptr);
    const auto guestId = guest->id;
    vehicle->time_waiting = 20 * 32;
    vehicle->update();
    ASSERT_TRUE(vehicle->flags.has(VehicleFlag::readyToDepart));
    RoundTrip();
    EXPECT_EQ(Reserve(), nullptr);
    guest = getGameState().entities.tryGetEntity<Guest>(guestId);
    ASSERT_NE(guest, nullptr);
    Tick(*guest);
    vehicle->update();
    ASSERT_EQ(vehicle->sub_state, 2);

    vehicle->SetState(Vehicle::Status::unloadingPassengers, 1);
    vehicle->update();
    for (int tick = 0; tick < 4; ++tick)
        Tick(*guest);
    vehicle->update();
    ASSERT_FALSE(Capacity::HasManifest(*vehicle));

    vehicle->SetState(Vehicle::Status::waitingForPassengers, 0);
    vehicle->update();
    EXPECT_EQ(vehicle->sub_state, 1);
    EXPECT_EQ(vehicle->time_waiting, 0);
    EXPECT_FALSE(vehicle->flags.has(VehicleFlag::readyToDepart));
    EXPECT_NE(Reserve(), nullptr);
}

TEST_F(CapacityOverrideTests, DepartureCutoffAppliesToTrailingCarsBehindDecorativeHead)
{
    ASSERT_EQ(SetCapacity(100), GameActions::Status::ok);
    auto* tail = getGameState().entities.createEntity<Vehicle>();
    ASSERT_NE(tail, nullptr);
    tail->SubType = Vehicle::Type::tail;
    tail->ride = ride->id;
    tail->num_seats = vehicle->num_seats;
    tail->prev_vehicle_on_ride = vehicle->id;
    tail->next_vehicle_on_train = EntityId::GetNull();
    vehicle->next_vehicle_on_train = tail->id;
    vehicle->num_seats = 0;

    EXPECT_TRUE(Capacity::CanReserve(*tail));
    vehicle->flags.set(VehicleFlag::readyToDepart);
    EXPECT_FALSE(Capacity::CanReserve(*tail));
    vehicle->flags.unset(VehicleFlag::readyToDepart);
    vehicle->sub_state = 2;
    EXPECT_FALSE(Capacity::CanReserve(*tail));
}
