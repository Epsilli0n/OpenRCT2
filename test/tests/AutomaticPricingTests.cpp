/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <gtest/gtest.h>
#include <openrct2/Cheats.h>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/actions/cheats/CheatSetAction.h>
#include <openrct2/core/DataSerialiser.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/object/ClimateObject.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/RideObject.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/RideEntry.h>
#include <openrct2/ride/ShopItem.h>

using namespace OpenRCT2;

class AutomaticPricingTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;
    uint16_t nextRide = 0;

    void SetUp() override
    {
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_TRUE(context->Initialise());
        RideInitAll();
        getGameState().entities.resetAllEntities();
        getGameState().park.flags = { ParkFlag::freeEntry };
        getGameState().park.entranceFee = 0;
        getGameState().cheats.automaticPricing = true;
    }

    Ride* AddRide(std::string_view objectId)
    {
        auto* object = context->GetObjectManager().LoadObject(ObjectEntryDescriptor(objectId));
        if (object == nullptr)
            return nullptr;
        auto* ride = RideAllocateAtIndex(RideId::FromUnderlying(nextRide++));
        ride->subtype = ObjectManagerGetLoadedObjectEntryIndex(object);
        ride->type = static_cast<RideObject*>(object)->GetEntry().GetFirstNonNullRideType();
        ride->value = kRideValueUndefined;
        return ride;
    }
};

TEST_F(AutomaticPricingTests, CustomCheatNamesResolveThroughEnglishFallback)
{
    ASSERT_TRUE(LanguageOpen(LANGUAGE_ENGLISH_US));
    EXPECT_STREQ(CheatsGetName(CheatType::disableGuestCrowding), "Disable guest overcrowding");
    EXPECT_STREQ(CheatsGetName(CheatType::automaticPricing), "Automatic ride and shop pricing");
}

TEST_F(AutomaticPricingTests, ToggleAppliesDistinctRideLimitsAndStopsUpdatingWhenDisabled)
{
    auto& gameState = getGameState();
    auto* first = AddRide("rct2.ride.fwh1");
    auto* second = AddRide("rct2.ride.fwh1");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->value = 3.20_GBP;
    second->value = 7.10_GBP;
    gameState.cheats.automaticPricing = false;
    gameState.currentTicks = 1;

    GameActions::CheatSetAction enable(CheatType::automaticPricing, 1);
    ASSERT_EQ(enable.Query(gameState, gameState.park).error, GameActions::Status::ok);
    ASSERT_EQ(enable.Execute(gameState, gameState.park).error, GameActions::Status::ok);
    EXPECT_EQ(first->price[0], 6.40_GBP);
    EXPECT_EQ(second->price[0], 14.20_GBP);
    EXPECT_TRUE(first->windowInvalidateFlags.has(RideInvalidateFlag::income));

    GameActions::CheatSetAction disable(CheatType::automaticPricing, 0);
    ASSERT_EQ(disable.Execute(gameState, gameState.park).error, GameActions::Status::ok);
    first->value = 1.00_GBP;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(first->price[0], 6.40_GBP);
}

TEST_F(AutomaticPricingTests, RefreshesDailyAndPeriodicallyAsRideValuesChange)
{
    auto& gameState = getGameState();
    auto* ride = AddRide("rct2.ride.fwh1");
    ASSERT_NE(ride, nullptr);
    ride->value = 3.00_GBP;
    gameState.currentTicks = 128;
    CheatsUpdateAutomaticPrices();
    EXPECT_EQ(ride->price[0], 6.00_GBP);

    ride->value = 2.00_GBP;
    gameState.currentTicks = 129;
    CheatsUpdateAutomaticPrices();
    EXPECT_EQ(ride->price[0], 6.00_GBP);
    gameState.currentTicks = 256;
    CheatsUpdateAutomaticPrices();
    EXPECT_EQ(ride->price[0], 4.00_GBP);

    ride->value = 4.00_GBP;
    gameState.currentTicks = 257;
    gameState.date.monthTicks = 2116;
    ASSERT_TRUE(gameState.date.IsDayStart());
    CheatsUpdateAutomaticPrices();
    EXPECT_EQ(ride->price[0], 8.00_GBP);
}

TEST_F(AutomaticPricingTests, PaidEntryDiscountUsesGuestRoundingAndExistingGuestFlags)
{
    auto& gameState = getGameState();
    auto* ride = AddRide("rct2.ride.fwh1");
    ASSERT_NE(ride, nullptr);
    ride->value = 8.30_GBP;
    gameState.park.flags = { ParkFlag::unlockAllPrices };
    gameState.park.entranceFee = 10.00_GBP;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 4.00_GBP);

    auto* guest = gameState.entities.createEntity<Guest>();
    ASSERT_NE(guest, nullptr);
    guest->outsideOfPark = false;
    guest->peepFlags.set(PeepFlag::hasPaidForParkEntry);
    gameState.park.entranceFee = 0;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 4.00_GBP);
    guest->outsideOfPark = true;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 16.60_GBP);
}

TEST_F(AutomaticPricingTests, HandlesZeroUnratedAndCappedValuesAndPriceLocks)
{
    auto& gameState = getGameState();
    auto* ride = AddRide("rct2.ride.fwh1");
    ASSERT_NE(ride, nullptr);
    ride->price[0] = 1.20_GBP;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 1.20_GBP);
    ride->value = 50.00_GBP;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], kRideMaxPrice);
    ride->value = 0;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 0);

    ride->price[0] = 1.20_GBP;
    ride->value = 3.00_GBP;
    gameState.park.flags = {};
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 1.20_GBP);
    gameState.park.flags = { ParkFlag::freeEntry, ParkFlag::noMoney };
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[0], 1.20_GBP);
}

TEST_F(AutomaticPricingTests, ShopsUseClimateValuesAndBothItemSlots)
{
    auto& gameState = getGameState();
    auto& manager = context->GetObjectManager();
    auto* climate = manager.LoadObject(ObjectEntryDescriptor("rct2.climate.cool_and_wet"), 0);
    ASSERT_NE(climate, nullptr);
    const auto& thresholds = static_cast<ClimateObject*>(climate)->getItemThresholds();
    auto* drinks = AddRide("rct2.ride.drnks");
    auto* secondDrinks = AddRide("rct2.ride.drnks");
    auto* information = AddRide("rct2.ride.infok");
    ASSERT_NE(drinks, nullptr);
    ASSERT_NE(secondDrinks, nullptr);
    ASSERT_NE(information, nullptr);
    gameState.park.samePriceThroughoutPark |= EnumToFlag(ShopItem::drink);

    gameState.weatherCurrent.temperature = thresholds.warm;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(drinks->price[0], GetShopItemDescriptor(ShopItem::drink).HotValue);
    EXPECT_EQ(secondDrinks->price[0], drinks->price[0]);
    gameState.weatherCurrent.temperature = thresholds.cold;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(drinks->price[0], GetShopItemDescriptor(ShopItem::drink).ColdValue);
    EXPECT_EQ(secondDrinks->price[0], drinks->price[0]);
    for (size_t i = 0; i < 2; i++)
    {
        auto item = information->getRideEntry()->shop_item[i];
        ASSERT_NE(item, ShopItem::none);
        EXPECT_EQ(information->price[i], GetShopItemDescriptor(item).GetValue());
    }
}

TEST_F(AutomaticPricingTests, PricesPhotosAndToiletsWithoutChargingForFirstAid)
{
    auto* ride = AddRide("rct2.ride.fwh1");
    auto* toilets = AddRide("rct2.ride.tlt1");
    auto* firstAid = AddRide("rct2.ride.faid1");
    ASSERT_NE(ride, nullptr);
    ASSERT_NE(toilets, nullptr);
    ASSERT_NE(firstAid, nullptr);
    ride->flags.set(RideFlag::onRidePhoto);
    ride->value = 3.00_GBP;
    CheatsUpdateAutomaticPrices(true);
    EXPECT_EQ(ride->price[1], GetShopItemDescriptor(ride->getRideTypeDescriptor().PhotoItem).GetValue());
    EXPECT_EQ(toilets->price[0], 0.10_GBP);
    EXPECT_EQ(firstAid->price[0], 0);
}

TEST_F(AutomaticPricingTests, PersistsAndDefaultsOffForOlderSaves)
{
    auto& cheats = getGameState().cheats;
    for (bool enabled : { false, true })
    {
        cheats.automaticPricing = enabled;
        MemoryStream stream;
        DataSerialiser writer(true, stream);
        CheatsSerialise(writer);
        cheats.automaticPricing = !enabled;
        stream.SetPosition(0);
        DataSerialiser reader(false, stream);
        CheatsSerialise(reader);
        EXPECT_EQ(cheats.automaticPricing, enabled);
    }
    MemoryStream oldStream;
    DataSerialiser oldWriter(true, oldStream);
    oldWriter << uint16_t{ 0 };
    cheats.automaticPricing = true;
    oldStream.SetPosition(0);
    DataSerialiser oldReader(false, oldStream);
    CheatsSerialise(oldReader);
    EXPECT_FALSE(cheats.automaticPricing);
    cheats.automaticPricing = true;
    CheatsReset();
    EXPECT_FALSE(cheats.automaticPricing);
}
