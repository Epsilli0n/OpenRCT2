/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Identifiers.h"

#include <cstdint>
#include <map>
#include <vector>

namespace OpenRCT2
{
    class DataSerialiser;
    struct GameState_t;
    struct Guest;
    struct Ride;
    struct Vehicle;
} // namespace OpenRCT2

namespace OpenRCT2::CapacityOverride
{
    constexpr uint32_t kMaxCapacity = 65535;
    constexpr uint32_t kNoSeat = UINT32_MAX;

    struct Passenger
    {
        EntityId guest = EntityId::GetNull();
        bool boarded{};
    };

    struct Manifest
    {
        RideId ride = RideId::GetNull();
        std::vector<Passenger> passengers;
        bool unloading{};
        uint32_t reserved{};
        uint32_t boarded{};
        uint32_t nextBoarding{};
        uint32_t lastBoarded = kNoSeat;
    };

    struct Seat
    {
        EntityId vehicle;
        uint32_t index;
    };

    struct State
    {
        std::map<RideId, uint16_t> overrides;
        std::map<EntityId, Manifest> manifests;
        // Derived from manifests on load; does not change the native entity layout.
        std::map<EntityId, Seat> seats;
    };

    bool Supports(const Ride& ride);
    uint16_t Get(const Ride& ride);
    bool CanSet(const Ride& ride);
    void Set(Ride& ride, uint16_t capacity);
    bool Enabled(const Vehicle& vehicle);
    bool HasManifest(const Vehicle& vehicle);
    uint32_t SeatCount(const Vehicle& vehicle);
    uint32_t BoardedCount(const Vehicle& vehicle);
    uint32_t ReservedCount(const Vehicle& vehicle);
    std::vector<EntityId> Passengers(const Vehicle& vehicle);
    bool CanReserve(const Vehicle& vehicle);
    bool Reserve(Vehicle& vehicle, Guest& guest);
    bool CanBoard(const Guest& guest);
    bool CanUnload(const Guest& guest);
    void Board(Vehicle& vehicle, Guest& guest);
    void RemovePassenger(EntityId guest);
    bool BeginUnloading(Vehicle& vehicle);
    void ForgetVehicle(EntityId vehicle);
    void ClearPassengers(RideId ride = RideId::GetNull());
    void ForgetRide(RideId ride);
    void Serialise(GameState_t& gameState, DataSerialiser& ds);
} // namespace OpenRCT2::CapacityOverride
