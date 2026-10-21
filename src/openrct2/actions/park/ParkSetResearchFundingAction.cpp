/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ParkSetResearchFundingAction.h"

#include "../../Diagnostic.h"
#include "../../GameState.h"
#include "../../localisation/StringIds.h"
#include "../../management/Research.h"
#include "../../ui/WindowManager.h"
#include "../../windows/Intent.h"

namespace OpenRCT2::GameActions
{
    ParkSetResearchFundingAction::ParkSetResearchFundingAction(uint32_t priorities, uint8_t fundingAmount)
        : _priorities(priorities)
        , _fundingAmount(fundingAmount)
    {
    }

    void ParkSetResearchFundingAction::AcceptParameters(GameActionParameterVisitor& visitor)
    {
        visitor.Visit("priorities", _priorities);
        visitor.Visit("fundingAmount", _fundingAmount);
    }

    uint16_t ParkSetResearchFundingAction::GetActionFlags() const
    {
        return GameAction::GetActionFlags() | Flags::AllowWhilePaused;
    }

    void ParkSetResearchFundingAction::Serialise(DataSerialiser& stream)
    {
        GameAction::Serialise(stream);
        stream << DS_TAG(_priorities) << DS_TAG(_fundingAmount);
    }

    Result ParkSetResearchFundingAction::Query(GameState_t& gameState, Park::ParkData& park) const
    {
        if (_fundingAmount >= kResearchFundingOptionCount)
        {
            LOG_ERROR("Invalid research funding amount %d", _fundingAmount);
            return Result(Status::invalidParameters, STR_ERR_INVALID_PARAMETER, STR_ERR_VALUE_OUT_OF_RANGE);
        }
        return Result();
    }

    Result ParkSetResearchFundingAction::Execute(GameState_t& gameState, Park::ParkData& park) const
    {
        auto result = Query(gameState, park);
        if (result.error != Status::ok)
            return result;
        gameState.researchPriorities = _priorities;
        // Keep the native funding field in its original range for old saves and plugins.
        gameState.researchFundingLevel = std::min<uint8_t>(_fundingAmount, RESEARCH_FUNDING_MAXIMUM);
        gameState.researchFundingMultiplier = _fundingAmount > RESEARCH_FUNDING_MAXIMUM
            ? _fundingAmount - RESEARCH_FUNDING_MAXIMUM + 1
            : 1;
        ResearchCalculateExpectedDate();

        auto windowManager = Ui::GetWindowManager();
        windowManager->BroadcastIntent(Intent(INTENT_ACTION_UPDATE_RESEARCH));
        return Result();
    }
} // namespace OpenRCT2::GameActions
