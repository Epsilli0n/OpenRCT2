/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "CapacityOverride.h"

#include "../GameState.h"
#include "../core/DataSerialiser.h"
#include "../entity/Guest.h"
#include "RideData.h"
#include "Vehicle.h"

#include <algorithm>
#include <stdexcept>

namespace OpenRCT2::CapacityOverride
{
    static uint32_t NativeSeats(const Vehicle& vehicle)
    {
        return std::min<uint32_t>(vehicle.num_seats & kVehicleSeatNumMask, std::size(vehicle.peep));
    }

    static Manifest* Find(const Vehicle& vehicle)
    {
        auto& manifests = getGameState().capacityOverride.manifests;
        auto it = manifests.find(vehicle.id);
        return it == manifests.end() ? nullptr : &it->second;
    }

    bool Supports(const Ride& ride)
    {
        return ride.getRideTypeDescriptor().Category == RideCategory::rollerCoaster;
    }

    uint16_t Get(const Ride& ride)
    {
        const auto& overrides = getGameState().capacityOverride.overrides;
        auto it = overrides.find(ride.id);
        return it == overrides.end() ? 0 : it->second;
    }

    bool CanSet(const Ride& ride)
    {
        if (!Supports(ride) || ride.status != RideStatus::closed || ride.numRiders != 0)
            return false;
        for (const auto& [vehicle, manifest] : getGameState().capacityOverride.manifests)
            if (manifest.ride == ride.id)
                return false;
        return true;
    }

    void Set(Ride& ride, uint16_t capacity)
    {
        auto& overrides = getGameState().capacityOverride.overrides;
        if (capacity == 0)
            overrides.erase(ride.id);
        else
            overrides[ride.id] = capacity;
    }

    bool Enabled(const Vehicle& vehicle)
    {
        const auto* ride = vehicle.GetRide();
        return NativeSeats(vehicle) != 0 && ride && Supports(*ride) && Get(*ride) != 0;
    }

    bool HasManifest(const Vehicle& vehicle)
    {
        return Find(vehicle) != nullptr;
    }

    uint32_t SeatCount(const Vehicle& vehicle)
    {
        return Enabled(vehicle) ? Get(*vehicle.GetRide()) : NativeSeats(vehicle);
    }

    uint32_t BoardedCount(const Vehicle& vehicle)
    {
        const auto* manifest = Find(vehicle);
        return manifest ? manifest->boarded : vehicle.num_peeps;
    }

    uint32_t ReservedCount(const Vehicle& vehicle)
    {
        const auto* manifest = Find(vehicle);
        return manifest ? (manifest->unloading ? 0 : manifest->reserved) : vehicle.next_free_seat;
    }

    std::vector<EntityId> Passengers(const Vehicle& vehicle)
    {
        std::vector<EntityId> result;
        if (const auto* manifest = Find(vehicle))
        {
            for (const auto& passenger : manifest->passengers)
                if (!passenger.guest.IsNull())
                    result.push_back(passenger.guest);
        }
        else
        {
            for (uint32_t i = 0; i < std::min<uint32_t>(vehicle.num_peeps, std::size(vehicle.peep)); ++i)
                result.push_back(vehicle.peep[i]);
        }
        return result;
    }

    static void AdvanceBoarding(Manifest& manifest)
    {
        while (manifest.nextBoarding < manifest.passengers.size())
        {
            const auto& passenger = manifest.passengers[manifest.nextBoarding];
            if (!passenger.guest.IsNull() && !passenger.boarded)
                break;
            ++manifest.nextBoarding;
        }
    }

    bool CanReserve(const Vehicle& vehicle)
    {
        if (!Enabled(vehicle) || ReservedCount(vehicle) >= SeatCount(vehicle))
            return false;
        const auto* manifest = Find(vehicle);
        if (manifest && manifest->unloading)
            return false;
        // Read the head's departure decision for every car. Once a departure is
        // requested, finish boarding existing reservations without admitting an
        // endless stream of new passengers through the entrance.
        const auto* head = vehicle.GetHead();
        return head && head->status == Vehicle::Status::waitingForPassengers && head->sub_state == 1
            && !head->flags.has(VehicleFlag::readyToDepart);
    }

    bool Reserve(Vehicle& vehicle, Guest& guest)
    {
        if (!CanReserve(vehicle))
            return false;
        auto& state = getGameState().capacityOverride;
        if (state.seats.contains(guest.id))
            return false;
        auto& manifest = state.manifests[vehicle.id];
        if (manifest.unloading)
            return false;
        manifest.ride = vehicle.ride;
        uint32_t index = manifest.reserved == manifest.passengers.size() ? manifest.reserved : 0;
        while (index < manifest.passengers.size() && !manifest.passengers[index].guest.IsNull())
            ++index;
        if (index == manifest.passengers.size())
            manifest.passengers.push_back({});
        manifest.passengers[index] = { guest.id, false };
        ++manifest.reserved;
        manifest.nextBoarding = std::min(manifest.nextBoarding, index);
        state.seats.emplace(guest.id, Seat{ vehicle.id, index });
        const auto nativeSeats = NativeSeats(vehicle);
        guest.currentSeat = index % nativeSeats;
        if (index < nativeSeats)
        {
            vehicle.peep[index] = guest.id;
            vehicle.peep_tshirt_colours[index] = guest.tShirtColour;
            ++vehicle.next_free_seat;
        }
        return true;
    }

    bool CanBoard(const Guest& guest)
    {
        const auto& state = getGameState().capacityOverride;
        auto it = state.seats.find(guest.id);
        if (it == state.seats.end())
            return false;
        const auto& manifest = state.manifests.at(it->second.vehicle);
        return !manifest.unloading && it->second.index == manifest.nextBoarding;
    }

    bool CanUnload(const Guest& guest)
    {
        const auto& state = getGameState().capacityOverride;
        auto it = state.seats.find(guest.id);
        return it != state.seats.end() && it->second.index == state.manifests.at(it->second.vehicle).lastBoarded;
    }

    void Board(Vehicle& vehicle, Guest& guest)
    {
        auto& state = getGameState().capacityOverride;
        const auto index = state.seats.at(guest.id).index;
        auto& manifest = state.manifests.at(vehicle.id);
        auto& passenger = manifest.passengers[index];
        if (passenger.boarded)
            return;
        passenger.boarded = true;
        ++manifest.boarded;
        if (manifest.lastBoarded == kNoSeat || index > manifest.lastBoarded)
            manifest.lastBoarded = index;
        AdvanceBoarding(manifest);
        // Extra riders do not alter the original vehicle mass or sprite passenger count.
        if (index < NativeSeats(vehicle))
        {
            ++vehicle.num_peeps;
            vehicle.ApplyMass(guest.mass);
        }
    }

    void RemovePassenger(EntityId guestId)
    {
        auto& gameState = getGameState();
        auto& state = gameState.capacityOverride;
        auto seat = state.seats.find(guestId);
        if (seat == state.seats.end())
            return;
        auto manifestIt = state.manifests.find(seat->second.vehicle);
        auto& manifest = manifestIt->second;
        auto& passenger = manifest.passengers[seat->second.index];
        if (auto* vehicle = gameState.entities.tryGetEntity<Vehicle>(seat->second.vehicle);
            vehicle && seat->second.index < NativeSeats(*vehicle))
        {
            vehicle->peep[seat->second.index] = EntityId::GetNull();
            if (!manifest.unloading && vehicle->next_free_seat != 0)
                --vehicle->next_free_seat;
            if (passenger.boarded && vehicle->num_peeps != 0)
            {
                --vehicle->num_peeps;
                if (auto* guest = gameState.entities.tryGetEntity<Guest>(guestId))
                    vehicle->ApplyMass(-guest->mass);
            }
        }
        --manifest.reserved;
        if (passenger.boarded)
            --manifest.boarded;
        passenger = {};
        AdvanceBoarding(manifest);
        while (manifest.lastBoarded != kNoSeat)
        {
            const auto& last = manifest.passengers[manifest.lastBoarded];
            if (!last.guest.IsNull() && last.boarded)
                break;
            manifest.lastBoarded = manifest.lastBoarded == 0 ? kNoSeat : manifest.lastBoarded - 1;
        }
        state.seats.erase(seat);
        if (manifest.reserved == 0)
            state.manifests.erase(manifestIt);
    }

    bool BeginUnloading(Vehicle& vehicle)
    {
        auto* manifest = Find(vehicle);
        if (!manifest || manifest->unloading)
            return false;
        manifest->unloading = true;
        vehicle.next_free_seat = 0;
        return true;
    }

    void ForgetVehicle(EntityId vehicle)
    {
        auto& state = getGameState().capacityOverride;
        auto it = state.manifests.find(vehicle);
        if (it == state.manifests.end())
            return;
        for (const auto& passenger : it->second.passengers)
            state.seats.erase(passenger.guest);
        state.manifests.erase(it);
    }

    void ClearPassengers(RideId ride)
    {
        auto& state = getGameState().capacityOverride;
        std::vector<EntityId> vehicles;
        for (const auto& [vehicle, manifest] : state.manifests)
            if (ride.IsNull() || manifest.ride == ride)
                vehicles.push_back(vehicle);
        for (auto vehicleId : vehicles)
        {
            auto* vehicle = getGameState().entities.tryGetEntity<Vehicle>(vehicleId);
            if (vehicle)
            {
                for (auto guest : Passengers(*vehicle))
                    RemovePassenger(guest);
            }
            ForgetVehicle(vehicleId);
        }
    }

    void ForgetRide(RideId ride)
    {
        ClearPassengers(ride);
        getGameState().capacityOverride.overrides.erase(ride);
    }

    void Serialise(GameState_t& gameState, DataSerialiser& ds)
    {
        uint32_t version = 1;
        ds << version;
        if (version != 1)
            throw std::runtime_error("Unsupported capacity override version");
        auto& state = gameState.capacityOverride;
        if (ds.isLoading())
            state = {};
        uint32_t count = static_cast<uint32_t>(state.overrides.size());
        ds << count;
        if (count > Limits::kMaxRidesInPark)
            throw std::runtime_error("Invalid capacity override count");
        if (ds.isSaving())
        {
            for (const auto& [ride, value] : state.overrides)
                ds << ride << value;
        }
        else
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                RideId id;
                uint16_t value{};
                ds << id << value;
                auto* ride = GetRide(id);
                if (!ride || !Supports(*ride) || value == 0 || !state.overrides.emplace(id, value).second)
                    throw std::runtime_error("Invalid capacity override ride");
            }
        }
        count = static_cast<uint32_t>(state.manifests.size());
        ds << count;
        if (count > kMaxEntities)
            throw std::runtime_error("Invalid capacity manifest count");
        uint32_t totalPassengers = 0;
        auto readWriteManifest = [&](EntityId vehicleId, Manifest& manifest) {
            ds << manifest.ride << manifest.unloading;
            uint32_t size = static_cast<uint32_t>(manifest.passengers.size());
            ds << size;
            totalPassengers += size;
            if (size > kMaxCapacity || totalPassengers > kMaxEntities)
                throw std::runtime_error("Invalid capacity passenger count");
            if (ds.isLoading())
                manifest.passengers.resize(size);
            auto* vehicle = gameState.entities.tryGetEntity<Vehicle>(vehicleId);
            if (ds.isLoading()
                && (!vehicle || vehicle->ride != manifest.ride || NativeSeats(*vehicle) == 0
                    || !state.overrides.contains(manifest.ride) || size > state.overrides.at(manifest.ride)))
                throw std::runtime_error("Invalid capacity manifest vehicle");
            for (uint32_t i = 0; i < size; ++i)
            {
                auto& passenger = manifest.passengers[i];
                ds << passenger.guest << passenger.boarded;
                if (ds.isLoading() && !passenger.guest.IsNull())
                {
                    auto* guest = gameState.entities.tryGetEntity<Guest>(passenger.guest);
                    if (!guest || guest->currentRide != manifest.ride
                        || !state.seats.emplace(passenger.guest, Seat{ vehicleId, i }).second)
                        throw std::runtime_error("Invalid capacity manifest guest");
                    ++manifest.reserved;
                    if (passenger.boarded)
                    {
                        ++manifest.boarded;
                        manifest.lastBoarded = i;
                    }
                }
            }
            if (ds.isLoading())
                AdvanceBoarding(manifest);
        };
        if (ds.isSaving())
        {
            for (auto& [vehicle, manifest] : state.manifests)
            {
                ds << vehicle;
                readWriteManifest(vehicle, manifest);
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                EntityId vehicle;
                ds << vehicle;
                auto [it, inserted] = state.manifests.emplace(vehicle, Manifest{});
                if (!inserted)
                    throw std::runtime_error("Duplicate capacity manifest vehicle");
                readWriteManifest(vehicle, it->second);
            }
        }
    }
} // namespace OpenRCT2::CapacityOverride
