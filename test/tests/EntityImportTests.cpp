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
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/actions/cheats/CheatSetAction.h>
#include <openrct2/core/DataSerialiser.h>
#include <openrct2/core/FileStream.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/world/MapAnimation.h>
#include <openrct2/world/Park.h>

using namespace OpenRCT2;

class EntityImportTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;

    void SetUp() override
    {
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_NE(context, nullptr);
        ASSERT_TRUE(context->Initialise());
    }

    void TearDown() override
    {
        context.reset();
    }
};

// This here tests that CreateEntityAt returns nullptr when trying to create an entity at an index that's already occupied.
// This behavior is what caused crashes before, corrupted saves had duplicate EntityIndex values that caused CreateEntityAt to
// return nullptr, which was then dereferenced.
TEST_F(EntityImportTests, CreateEntityAtDuplicateIndexReturnsNull)
{
    auto& gameState = getGameState();
    gameState.entities.resetAllEntities();

    // Create an entity at index 100
    auto* entity1 = gameState.entities.createEntityAt<Guest>(EntityId::FromUnderlying(100));
    ASSERT_NE(entity1, nullptr);
    EXPECT_EQ(entity1->id.ToUnderlying(), 100u);

    // Try to create another entity at the same index, which should return nullptr
    auto* entity2 = gameState.entities.createEntityAt<Guest>(EntityId::FromUnderlying(100));
    EXPECT_EQ(entity2, nullptr);
}

TEST_F(EntityImportTests, DisableGuestCrowdingPreventsThoughtAndHappinessPenalty)
{
    auto& gameState = getGameState();
    gameState.entities.resetAllEntities();

    auto* guest = gameState.entities.createEntityAt<Guest>(EntityId::FromUnderlying(100));
    ASSERT_NE(guest, nullptr);
    for (auto& thought : guest->thoughts)
    {
        thought.type = PeepThoughtType::none;
    }

    gameState.cheats.disableGuestCrowding = false;
    guest->happinessTarget = 100;
    guest->applyCrowdingPenalty();
    EXPECT_EQ(guest->happinessTarget, 86);
    EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::crowded);

    gameState.cheats.disableGuestCrowding = true;
    CheatsClearGuestCrowdingThoughts();
    EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::none);

    guest->happinessTarget = 100;
    guest->applyCrowdingPenalty();
    EXPECT_EQ(guest->happinessTarget, 100);
    EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::none);

    guest->insertNewThought(PeepThoughtType::crowded);
    EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::none);

    gameState.cheats.disableGuestCrowding = false;
    guest->happinessTarget = 10;
    guest->applyCrowdingPenalty();
    EXPECT_EQ(guest->happinessTarget, 0);
    EXPECT_EQ(guest->thoughts[0].type, PeepThoughtType::crowded);
}

TEST_F(EntityImportTests, DisableGuestCrowdingClearsOnlyCrowdingThoughts)
{
    auto& gameState = getGameState();
    gameState.entities.resetAllEntities();
    auto* guest = gameState.entities.createEntityAt<Guest>(EntityId::FromUnderlying(100));
    ASSERT_NE(guest, nullptr);
    guest->thoughts = {};
    for (auto& thought : guest->thoughts)
        thought.type = PeepThoughtType::none;

    guest->insertNewThought(PeepThoughtType::hungry);
    guest->insertNewThought(PeepThoughtType::crowded);
    guest->insertNewThought(PeepThoughtType::thirsty);
    auto firstThought = guest->thoughts[0];
    auto lastThought = guest->thoughts[2];
    guest->windowInvalidateFlags = 0;

    GameActions::CheatSetAction enable(CheatType::disableGuestCrowding, 1);
    EXPECT_EQ(enable.Query(gameState, gameState.park).error, GameActions::Status::ok);
    EXPECT_EQ(enable.Execute(gameState, gameState.park).error, GameActions::Status::ok);
    EXPECT_TRUE(gameState.cheats.disableGuestCrowding);
    EXPECT_EQ(guest->thoughts[0].type, firstThought.type);
    EXPECT_EQ(guest->thoughts[0].freshness, firstThought.freshness);
    EXPECT_EQ(guest->thoughts[1].type, lastThought.type);
    EXPECT_EQ(guest->thoughts[1].item, lastThought.item);
    for (size_t i = 2; i < guest->thoughts.size(); i++)
        EXPECT_EQ(guest->thoughts[i].type, PeepThoughtType::none);
    EXPECT_NE(guest->windowInvalidateFlags & PEEP_INVALIDATE_PEEP_THOUGHTS, 0);

    GameActions::CheatSetAction disable(CheatType::disableGuestCrowding, 0);
    EXPECT_EQ(disable.Execute(gameState, gameState.park).error, GameActions::Status::ok);
    EXPECT_FALSE(gameState.cheats.disableGuestCrowding);
}

TEST_F(EntityImportTests, DisableGuestCrowdingPersistsAndDefaultsOffForOlderSaves)
{
    auto& cheats = getGameState().cheats;
    for (bool enabled : { false, true })
    {
        cheats.disableGuestCrowding = enabled;
        MemoryStream stream;
        DataSerialiser writer(true, stream);
        CheatsSerialise(writer);
        cheats.disableGuestCrowding = !enabled;
        stream.SetPosition(0);
        DataSerialiser reader(false, stream);
        CheatsSerialise(reader);
        EXPECT_EQ(cheats.disableGuestCrowding, enabled);
    }

    MemoryStream oldStream;
    DataSerialiser oldWriter(true, oldStream);
    oldWriter << uint16_t{ 0 };
    cheats.disableGuestCrowding = true;
    oldStream.SetPosition(0);
    DataSerialiser oldReader(false, oldStream);
    CheatsSerialise(oldReader);
    EXPECT_FALSE(cheats.disableGuestCrowding);

    cheats.disableGuestCrowding = true;
    CheatsReset();
    EXPECT_FALSE(cheats.disableGuestCrowding);
}

TEST_F(EntityImportTests, DisableGuestCrowdingRemovesPopulationArrivalPenalties)
{
    auto& gameState = getGameState();
    auto& park = gameState.park;
    gameState.peepSpawns.clear();
    park.currentAwards.clear();
    park.flags.set(ParkFlag::noMoney);
    gameState.cheats.forcedParkRating = 999;
    gameState.currentTicks = 512;

    for (bool difficultGeneration : { false, true })
    {
        park.flags.set(ParkFlag::difficultGuestGeneration, difficultGeneration);
        for (uint32_t guests : { 1000u, 53000u })
        {
            park.numGuestsInPark = guests;
            gameState.cheats.disableGuestCrowding = false;
            Park::Update(park, gameState);
            auto normalProbability = park.guestGenerationProbability;
            EXPECT_LT(normalProbability, 700);

            gameState.cheats.disableGuestCrowding = true;
            Park::Update(park, gameState);
            EXPECT_EQ(park.guestGenerationProbability, 700);
        }
    }
}

// This test verifies that corrupted S6 files with duplicate EntityIndex values can be loaded without crashing.
TEST_F(EntityImportTests, S6ImportCorruptedDuplicateEntityIndicesDoesNotCrash)
{
    std::string testParkPath = TestData::GetParkPath("corrupted_duplicate_entity_indices.sv6");
    auto fs = FileStream(testParkPath, FileMode::open);

    auto& objManager = context->GetObjectManager();
    auto importer = ParkImporter::CreateS6(context->GetObjectRepository());
    auto loadResult = importer->LoadFromStream(&fs, false);
    objManager.LoadObjects(loadResult.RequiredObjects);

    MapAnimations::ClearAll();
    auto& gameState = getGameState();

    // This should not crash because the code should sanitize EntityIndex values before CreateEntityAt is called
    EXPECT_NO_THROW(importer->Import(gameState));
}
