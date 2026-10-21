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
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/park/ParkSetResearchFundingAction.h>
#include <openrct2/core/EnumUtils.hpp>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/management/Finance.h>
#include <openrct2/management/Research.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/RideObject.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/ride/RideEntry.h>

using namespace OpenRCT2;

class ResearchFundingTests : public testing::Test
{
protected:
    std::unique_ptr<IContext> context;
    LegacyScene previousScene;
    bool previousSilentResearch;
    std::vector<ResearchItem> plan;

    void SetUp() override
    {
        previousScene = gLegacyScene;
        previousSilentResearch = gSilentResearch;
        gSilentResearch = true;
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        context = CreateContext();
        ASSERT_TRUE(context->Initialise());
        ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_with_ferris_wheel.sv6")));
        auto& state = getGameState();
        state.park.flags = {};
        state.park.cash = 10000.00_GBP;
        for (const auto* name : { "rct2.ride.fwh1", "rct2.ride.zldb", "rct2.ride.drnks" })
        {
            auto* object = context->GetObjectManager().LoadObject(ObjectEntryDescriptor(name));
            ASSERT_NE(object, nullptr);
            const auto type = static_cast<RideObject*>(object)->GetEntry().GetFirstNonNullRideType();
            plan.emplace_back(
                Research::EntryType::ride, ObjectManagerGetLoadedObjectEntryIndex(object), type, ResearchCategory::gentle, 0);
        }
        GameActions::ClearQueue();
        ResetPlan();
    }

    void TearDown() override
    {
        GameActions::ClearQueue();
        context.reset();
        gLegacyScene = previousScene;
        gSilentResearch = previousSilentResearch;
    }

    void ResetPlan()
    {
        auto& state = getGameState();
        state.researchItemsInvented.clear();
        state.researchItemsUninvented = plan;
        state.researchProgressStage = RESEARCH_STAGE_INITIAL_RESEARCH;
        state.researchProgress = 0;
        state.researchNextItem.reset();
        state.researchLastItem.reset();
        state.researchFundingLevel = RESEARCH_FUNDING_MAXIMUM;
        state.researchFundingMultiplier = 1;
        state.researchPriorities = 0x7f;
        state.currentTicks = 4096;
    }

    GameActions::Status SetFunding(uint8_t option, uint32_t priorities = 0x7f)
    {
        auto& state = getGameState();
        GameActions::ParkSetResearchFundingAction action(priorities, option);
        return action.Execute(state, state.park).error;
    }

    void RoundTrip()
    {
        MemoryStream stream;
        ParkFileExporter exporter;
        exporter.Export(getGameState(), stream, kParkFileSaveCompressionLevel);
        stream.SetPosition(0);
        auto importer = ParkImporter::CreateParkFile(context->GetObjectRepository());
        auto result = importer->LoadFromStream(&stream, false);
        context->GetObjectManager().LoadObjects(result.RequiredObjects);
        importer->Import(getGameState());
    }
};

TEST_F(ResearchFundingTests, EveryTierChargesItsDisplayedMonthlyCost)
{
    auto& state = getGameState();
    for (uint8_t option = 0; option < kResearchFundingOptionCount; ++option)
    {
        ASSERT_EQ(SetFunding(option), GameActions::Status::ok);
        const auto expected = kResearchCosts[std::min<int>(option, RESEARCH_FUNDING_MAXIMUM)]
            * (option > RESEARCH_FUNDING_MAXIMUM ? option - RESEARCH_FUNDING_MAXIMUM + 1 : 1);
        EXPECT_EQ(FinanceGetResearchFundingCost(), expected);
        EXPECT_EQ(ResearchGetFundingOption(state), option);
        const auto before = state.park.cash;
        const auto expenditure = state.park.expenditureTable[0][EnumValue(ExpenditureType::research)];
        for (int week = 0; week < 4; ++week)
            FinancePayResearch();
        EXPECT_EQ(state.park.cash, before - expected);
        EXPECT_EQ(state.park.expenditureTable[0][EnumValue(ExpenditureType::research)], expenditure - expected);
        EXPECT_LE(state.researchFundingLevel, RESEARCH_FUNDING_MAXIMUM);
    }
}

TEST_F(ResearchFundingTests, EachMultiplierFinishesResearchAtTheMatchingSpeed)
{
    auto& state = getGameState();
    int normalUpdates = 0;
    for (uint8_t multiplier = 1; multiplier <= kResearchMaxFundingMultiplier; ++multiplier)
    {
        ResetPlan();
        ASSERT_EQ(SetFunding(RESEARCH_FUNDING_MAXIMUM + multiplier - 1), GameActions::Status::ok);
        int updates = 0;
        while (state.researchItemsInvented.empty() && updates < 2000)
        {
            ResearchUpdate();
            ++updates;
        }
        ASSERT_FALSE(state.researchItemsInvented.empty());
        EXPECT_EQ(state.researchItemsInvented.front(), plan.front());
        if (multiplier == 1)
            normalUpdates = updates;
        EXPECT_EQ(updates, (normalUpdates + multiplier - 1) / multiplier);
    }
}

TEST_F(ResearchFundingTests, ChangingFundingRefreshesDateAndPreservesProgressAndPriorityEdits)
{
    auto& state = getGameState();
    state.date.monthsElapsed = 0;
    state.date.monthTicks = 0;
    state.researchProgressStage = RESEARCH_STAGE_DESIGNING;
    state.researchProgress = 12345;
    state.researchNextItem = plan.front();
    ASSERT_EQ(SetFunding(RESEARCH_FUNDING_MAXIMUM), GameActions::Status::ok);
    const auto originalDay = state.researchExpectedDay;
    ASSERT_EQ(SetFunding(RESEARCH_FUNDING_MAXIMUM + 1), GameActions::Status::ok);
    EXPECT_LT(state.researchExpectedDay, originalDay);
    EXPECT_EQ(state.researchProgress, 12345);
    EXPECT_EQ(state.researchProgressStage, RESEARCH_STAGE_DESIGNING);
    ASSERT_EQ(SetFunding(ResearchGetFundingOption(state), 1), GameActions::Status::ok);
    EXPECT_EQ(state.researchPriorities, 1);
    EXPECT_EQ(state.researchFundingMultiplier, 2);
    EXPECT_EQ(state.researchProgress, 12345);
    EXPECT_NE(SetFunding(kResearchFundingOptionCount), GameActions::Status::ok);
    EXPECT_NE(SetFunding(255), GameActions::Status::ok);
    EXPECT_EQ(state.researchFundingMultiplier, 2);
}

TEST_F(ResearchFundingTests, FundingStopsAfterFinalInventionAndNoMoneyParksAreNotCharged)
{
    auto& state = getGameState();
    ASSERT_EQ(SetFunding(kResearchFundingOptionCount - 1), GameActions::Status::ok);
    state.park.flags.set(ParkFlag::noMoney);
    const auto before = state.park.cash;
    FinancePayResearch();
    EXPECT_EQ(state.park.cash, before);
    state.park.flags.unset(ParkFlag::noMoney);
    for (int update = 0; update < 2000 && state.researchProgressStage != RESEARCH_STAGE_FINISHED_ALL; ++update)
        ResearchUpdate();
    // The game applies the automatic funding-stop action through its normal queue.
    GameActions::ProcessQueue(state);
    EXPECT_EQ(state.researchProgressStage, RESEARCH_STAGE_FINISHED_ALL);
    EXPECT_EQ(state.researchFundingLevel, RESEARCH_FUNDING_NONE);
    EXPECT_EQ(state.researchFundingMultiplier, 1);
    EXPECT_EQ(FinanceGetResearchFundingCost(), 0.00_GBP);
    EXPECT_EQ(state.researchItemsInvented.size(), plan.size());
}

TEST_F(ResearchFundingTests, ExtendedFundingAndPricingPercentagePersistInParkSaves)
{
    auto& state = getGameState();
    ASSERT_EQ(SetFunding(kResearchFundingOptionCount - 1), GameActions::Status::ok);
    state.cheats.automaticPricing = true;
    state.cheats.automaticPricingPercent = 20;
    state.researchProgress = 44444;
    state.researchProgressStage = RESEARCH_STAGE_DESIGNING;
    state.researchNextItem = plan.front();
    RoundTrip();
    EXPECT_EQ(state.researchFundingLevel, RESEARCH_FUNDING_MAXIMUM);
    EXPECT_EQ(state.researchFundingMultiplier, 10);
    EXPECT_EQ(state.cheats.automaticPricingPercent, 20);
    EXPECT_TRUE(state.cheats.automaticPricing);
    EXPECT_EQ(state.researchProgress, 44444);
    EXPECT_EQ(FinanceGetResearchFundingCost(), 4000.00_GBP);
    const auto before = state.park.cash;
    FinancePayResearch();
    EXPECT_EQ(state.park.cash, before - 1000.00_GBP);
}

TEST_F(ResearchFundingTests, MissingExtensionAndLegacySavesResetTheMultiplier)
{
    auto& state = getGameState();
    ASSERT_EQ(SetFunding(RESEARCH_FUNDING_MAXIMUM), GameActions::Status::ok);
    MemoryStream stream;
    ParkFileExporter exporter;
    exporter.Export(state, stream, kParkFileSaveCompressionLevel);
    state.researchFundingMultiplier = 10;
    stream.SetPosition(0);
    auto importer = ParkImporter::CreateParkFile(context->GetObjectRepository());
    auto result = importer->LoadFromStream(&stream, false);
    context->GetObjectManager().LoadObjects(result.RequiredObjects);
    importer->Import(state);
    EXPECT_EQ(state.researchFundingMultiplier, 1);
    state.researchFundingMultiplier = 10;
    state.cheats.automaticPricingPercent = 20;
    ASSERT_TRUE(context->LoadParkFromFile(TestData::GetParkPath("small_park_with_ferris_wheel.sv6")));
    EXPECT_EQ(state.researchFundingMultiplier, 1);
    EXPECT_EQ(state.cheats.automaticPricingPercent, 100);
    ASSERT_TRUE(LanguageOpen(LANGUAGE_ENGLISH_US));
    EXPECT_STREQ(LanguageGetString(STR_RESEARCH_FUNDING_MAXIMUM_10X), "Maximum funding (10x)");
}
