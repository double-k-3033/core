#include "qpi.h"

using namespace QPI;

constexpr sint64 QFOMO_INITIAL_BID_PRICE = 1000000;
constexpr uint32 QFOMO_ACTIVE_BID_CAPACITY = 256;
constexpr uint32 QFOMO_PLAYER_CAPACITY = 4096;

constexpr uint32 QFOMO_BPS = 10000;

constexpr uint32 QFOMO_ALPHA_BPS = 5000; // 50% for dividends
constexpr uint32 QFOMO_TEAM_BPS = 1000; // 10% for Team/Shareholders/Burn
constexpr uint32 QFOMO_JACKPOT_BPS = 4000; // 40% for jackpot

constexpr uint32 QFOMO_GROWTH_BPS = 1000; // 10% growth per bid
constexpr uint32 QFOMO_RECENT_WEIGHT_BPS = 4000; // 40% of dividend pool to recent group

constexpr uint32 QFOMO_X = 200; // total future bids that pay one bid
constexpr uint32 QFOMO_W = 160; // old group size
constexpr uint32 QFOMO_RECENT_LEN = QFOMO_X - QFOMO_W; // 40 recent bids
constexpr uint32 QFOMO_OLD_LEN = QFOMO_W; // 160 olds bids

constexpr uint32 QFOMO_MAX_BIDS_PER_ROUND = QFOMO_X + 1; // 201

constexpr uint32 QFOMO_DEFAULT_MAX_TIMER_TICKS = 108000; //24h = 24 * 3600 / 0.8 (~0.8s per tick)
constexpr uint32 QFOMO_DEFAULT_ADD_TIMER_TICKS = 38; // 30s = 30 / 0.8 (~0.8s per tick)

constexpr uint32 QFOMO_STATUS_ACTIVE = 1;
constexpr uint32 QFOMO_STATUS_FINALIZED = 2;

constexpr uint32 QFOMO_BID_PHASE_EMPTY = 0;
constexpr uint32 QFOMO_BID_PHASE_RECENT = 1;
constexpr uint32 QFOMO_BID_PHASE_OLD = 2;
constexpr uint32 QFOMO_BID_PHASE_EXPIRED = 3;

constexpr uint32 QFOMO_SUCCESS = 0;
constexpr uint32 QFOMO_ERR_INVALID_PAYMENT = 1;
constexpr uint32 QFOMO_ERR_ROUND_NOT_ACTIVE = 2;
constexpr uint32 QFOMO_ERR_BIDS_REACHED = 3;
constexpr uint32 QFOMO_ERR_PLAYER_CAPACITY = 4;
constexpr uint32 QFOMO_ERR_PLAYER_NOT_FOUND = 5;
constexpr uint32 QFOMO_ERR_NOTHING_TO_CLAIM = 6;
constexpr uint32 QFOMO_ERR_TRANSFER_FAILED = 7;
constexpr uint32 QFOMO_ERR_RESERVE_MISMATCH = 8;




struct QFOMO : public ContractBase
{

    struct Round
    {
        uint32 roundId;

        uint64 startTick;
        uint64 endTick;
        uint64 finalizedTick;

        uint32 currentBidCount;

        sint64 currentBidPrice;

        sint64 jackpot;
        sint64 totalVolume; // Total accepted bid payments in the current round

        sint64 recentDividendAccumulator;
        sint64 oldDividendAccumulator;

        id lastBidder;
        uint32 lastBidNumber;

        uint32 status;
    };

    struct ActiveBid
    {
        uint32 roundId;
        uint32 bidNumber;

        id bidder;

        sint64 recentAccumulatorDebt;
        sint64 oldAccumulatorDebt;

        sint64 settledRecentDividend;
        sint64 settledDividend;

        uint32 phase;
    };

    struct PlayerAccount
    {
        sint64 pendingDividend;
        sint64 totalClaimed;
    };

    struct StateData
    {
        Round round;

        Array<ActiveBid, QFOMO_ACTIVE_BID_CAPACITY> activeBids;

        HashMap<id, PlayerAccount, QFOMO_PLAYER_CAPACITY> playerAccounts;

        sint64 dividendReserve;
        sint64 teamReserve;

        uint32 totalRounds;
        uint32 totalBids;
    };

    struct GetGame_input
    {
    };

    struct GetGame_output
    {
        uint32 returnCode;
        uint32 roundId;
        uint64 startTick;
        uint64 endTick;
        uint64 finalizedTick;
        uint64 currentTick;
        uint64 remainingTick;
        uint32 currentBidCount;
        sint64 currentBidPrice;
        sint64 dividendReserve;
        sint64 teamReserve;
        sint64 jackpot;
        sint64 totalVolume;
        id lastBidder;
        uint32 lastBidNumber;
        uint32 status;
        uint32 totalRounds;
        uint32 totalBids;
    };

    struct GetPlayer_input
    {
        id player;
    };

    struct GetPlayer_output
    {
        sint64 pendingDividend;
        sint64 totalClaimed;
        uint32 exists;
    };

    struct PlaceBid_input
    {
    };

    struct PlaceBid_output
    {
        uint32 bidNumber;
        
        sint64 acceptedBidPrice;
        sint64 dividendAmount;
        sint64 teamAmount;
        sint64 jackpotAmount;
        
        sint64 nextBidPrice;
        sint64 totalJackpot;
        
        uint64 endTick;

        uint32 returnCode;
    };

    struct Claim_input
    {
    };
    
    struct Claim_output
    {
        sint64 claimedAmount;
        sint64 remainingDividend;
        uint32 returnCode;
    };

    INITIALIZE()
    {
        state.mut().round.roundId = 1;
        state.mut().round.startTick = qpi.tick();
        state.mut().round.endTick = state.get().round.startTick + QFOMO_DEFAULT_MAX_TIMER_TICKS;
        state.mut().round.finalizedTick = 0;
        state.mut().round.currentBidCount = 0;
        state.mut().round.currentBidPrice = QFOMO_INITIAL_BID_PRICE;
        state.mut().dividendReserve = 0;
        state.mut().teamReserve = 0;
        state.mut().round.jackpot = 0;
        state.mut().round.totalVolume = 0;
        state.mut().round.recentDividendAccumulator = 0;
        state.mut().round.oldDividendAccumulator = 0;
        state.mut().round.lastBidder = NULL_ID;
        state.mut().round.lastBidNumber = 0;
        state.mut().round.status = QFOMO_STATUS_ACTIVE;
        state.mut().totalRounds = 1;
        state.mut().totalBids = 0;
    }

    struct PlaceBid_locals
    {
        sint64 invocationReward;
        sint64 acceptedBidPrice;

        sint64 dividendAmount;
        sint64 teamAmount;
        sint64 jackpotAmount;

        sint64 recentPool;
        sint64 oldPool;

        sint64 recentPerSlot;
        sint64 oldPerSlot;

        sint64 assignedRecentAmount;
        sint64 assignedOldAmount;
        sint64 assignedDividendAmount;
        sint64 phantomDividendAmount;

        sint64 recentEarned;
        sint64 oldEarned;

        uint32 priorBidCount;
        uint32 recentRealCount;
        uint32 oldRealCount;

        uint32 newBidNumber;
        uint32 slotIndex;
        uint32 transitionBidNumber;
        uint32 transitionSlotIndex;
        uint32 expiredBidNumber;
        uint32 expiredSlotIndex;

        ActiveBid activeBid;
        ActiveBid newBid;

        PlayerAccount playerAccount;
        sint64 playerAccountIndex;
    };

    PUBLIC_PROCEDURE_WITH_LOCALS(PlaceBid)
    {
        locals.invocationReward = qpi.invocationReward();

        if (state.get().round.status != QFOMO_STATUS_ACTIVE || qpi.tick() >= state.get().round.endTick)
        {
            if(locals.invocationReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invocationReward);
            }
            output.returnCode = QFOMO_ERR_ROUND_NOT_ACTIVE;
            return;
        }

        if (state.get().round.currentBidCount >= QFOMO_MAX_BIDS_PER_ROUND)
        {
            if (locals.invocationReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invocationReward);
            }
            output.returnCode = QFOMO_ERR_BIDS_REACHED;
            return;
        }
        
        if (locals.invocationReward < state.get().round.currentBidPrice)
        {
            if (locals.invocationReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invocationReward);
            }
            output.returnCode = QFOMO_ERR_INVALID_PAYMENT;
            return;
        }

        if (!state.get().playerAccounts.get(qpi.invocator(), locals.playerAccount))
        {
            locals.playerAccount.pendingDividend = 0;
            locals.playerAccount.totalClaimed = 0;
            locals.playerAccountIndex = state.mut().playerAccounts.set(qpi.invocator(), locals.playerAccount);
            if (locals.playerAccountIndex == NULL_INDEX)
            {
                if (locals.invocationReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invocationReward);
                }
                output.returnCode = QFOMO_ERR_PLAYER_CAPACITY;
                return;
            }
        }

        if (locals.invocationReward > state.get().round.currentBidPrice)
        {
            qpi.transfer(qpi.invocator(), locals.invocationReward - state.get().round.currentBidPrice);
        }
        
        locals.dividendAmount = div<sint64>(state.get().round.currentBidPrice * QFOMO_ALPHA_BPS, QFOMO_BPS);
        locals.teamAmount = div<sint64>(state.get().round.currentBidPrice * QFOMO_TEAM_BPS, QFOMO_BPS);
        locals.jackpotAmount = state.get().round.currentBidPrice - locals.dividendAmount - locals.teamAmount;
        
        locals.acceptedBidPrice = state.get().round.currentBidPrice;

        locals.recentPool = div<sint64>(locals.dividendAmount * QFOMO_RECENT_WEIGHT_BPS, QFOMO_BPS);
        locals.oldPool = locals.dividendAmount - locals.recentPool;
        locals.recentPerSlot = div<sint64>(locals.recentPool, QFOMO_RECENT_LEN);
        locals.oldPerSlot = div<sint64>(locals.oldPool, QFOMO_OLD_LEN);

        locals.priorBidCount = state.get().round.currentBidCount;
        locals.recentRealCount = locals.priorBidCount < QFOMO_RECENT_LEN ? locals.priorBidCount : QFOMO_RECENT_LEN;
        locals.oldRealCount = locals.priorBidCount > QFOMO_RECENT_LEN ? locals.priorBidCount - QFOMO_RECENT_LEN : 0;

        locals.assignedRecentAmount = locals.recentPerSlot * locals.recentRealCount;
        locals.assignedOldAmount = locals.oldPerSlot * locals.oldRealCount;
        locals.assignedDividendAmount = locals.assignedRecentAmount + locals.assignedOldAmount;
        locals.phantomDividendAmount = locals.dividendAmount - locals.assignedDividendAmount;
        locals.jackpotAmount += locals.phantomDividendAmount;

        state.mut().dividendReserve += locals.assignedDividendAmount;
        state.mut().teamReserve += locals.teamAmount;
        state.mut().round.jackpot += locals.jackpotAmount;
        state.mut().round.totalVolume += locals.acceptedBidPrice;

        state.mut().round.recentDividendAccumulator += locals.recentPerSlot;
        state.mut().round.oldDividendAccumulator += locals.oldPerSlot;

        locals.newBidNumber = state.get().round.currentBidCount + 1;
        if (locals.newBidNumber > QFOMO_RECENT_LEN)
        {
            locals.transitionBidNumber = locals.newBidNumber - QFOMO_RECENT_LEN;
            locals.transitionSlotIndex = locals.transitionBidNumber - 1;
            locals.activeBid = state.get().activeBids.get(locals.transitionSlotIndex);

            if (locals.activeBid.roundId == state.get().round.roundId && locals.activeBid.bidNumber == locals.transitionBidNumber && locals.activeBid.phase == QFOMO_BID_PHASE_RECENT)
            {
                locals.recentEarned = state.get().round.recentDividendAccumulator - locals.activeBid.recentAccumulatorDebt;
                locals.activeBid.settledRecentDividend += locals.recentEarned;
                locals.activeBid.oldAccumulatorDebt = state.get().round.oldDividendAccumulator;
                locals.activeBid.phase = QFOMO_BID_PHASE_OLD;
                state.mut().activeBids.set(locals.transitionSlotIndex, locals.activeBid);
            }
        }

        if (locals.newBidNumber > QFOMO_X)
        {
            locals.expiredBidNumber = locals.newBidNumber - QFOMO_X;
            locals.expiredSlotIndex = locals.expiredBidNumber - 1;
            locals.activeBid = state.get().activeBids.get(locals.expiredSlotIndex);

            if (locals.activeBid.roundId == state.get().round.roundId && locals.activeBid.bidNumber == locals.expiredBidNumber && locals.activeBid.phase == QFOMO_BID_PHASE_OLD)
            {
                locals.oldEarned = state.get().round.oldDividendAccumulator - locals.activeBid.oldAccumulatorDebt;
                locals.activeBid.settledDividend = locals.activeBid.settledRecentDividend + locals.oldEarned;
                if (state.get().playerAccounts.get(locals.activeBid.bidder, locals.playerAccount))
                {
                    locals.playerAccount.pendingDividend += locals.activeBid.settledDividend;
                    state.mut().playerAccounts.set(locals.activeBid.bidder, locals.playerAccount);
                    locals.activeBid.phase = QFOMO_BID_PHASE_EXPIRED;
                    state.mut().activeBids.set(locals.expiredSlotIndex, locals.activeBid);
                }
            }
        }

        locals.slotIndex = locals.newBidNumber - 1;
        locals.newBid.roundId = state.get().round.roundId;
        locals.newBid.bidNumber = locals.newBidNumber;
        locals.newBid.bidder = qpi.invocator();
        locals.newBid.recentAccumulatorDebt = state.get().round.recentDividendAccumulator;
        locals.newBid.oldAccumulatorDebt = 0;
        locals.newBid.settledRecentDividend = 0;
        locals.newBid.settledDividend = 0;
        locals.newBid.phase = QFOMO_BID_PHASE_RECENT;
        state.mut().activeBids.set(locals.slotIndex, locals.newBid);

        state.mut().round.currentBidCount = locals.newBidNumber;
        state.mut().totalBids ++;
        state.mut().round.lastBidder = qpi.invocator();
        state.mut().round.lastBidNumber = locals.newBidNumber;

        if (locals.newBidNumber == QFOMO_MAX_BIDS_PER_ROUND)
        {
            state.mut().round.currentBidPrice = 0;
            state.mut().round.endTick = qpi.tick();
        }
        else
        {
            state.mut().round.currentBidPrice = locals.acceptedBidPrice + (div<sint64>(locals.acceptedBidPrice * QFOMO_GROWTH_BPS, QFOMO_BPS));
            state.mut().round.endTick = state.get().round.endTick + QFOMO_DEFAULT_ADD_TIMER_TICKS - qpi.tick() > QFOMO_DEFAULT_MAX_TIMER_TICKS ? qpi.tick() + QFOMO_DEFAULT_MAX_TIMER_TICKS : state.get().round.endTick + QFOMO_DEFAULT_ADD_TIMER_TICKS;
        }

        output.returnCode = QFOMO_SUCCESS;
        output.bidNumber = state.get().round.currentBidCount;
        output.acceptedBidPrice = locals.acceptedBidPrice;
        output.dividendAmount = locals.dividendAmount;
        output.teamAmount = locals.teamAmount;
        output.jackpotAmount = locals.jackpotAmount;
        output.nextBidPrice = state.get().round.currentBidPrice;
        output.totalJackpot = state.get().round.jackpot;
        output.endTick = state.get().round.endTick;
    }

    struct Claim_locals
    {
        PlayerAccount playerAccount;
        sint64 invocationReward;
        sint64 claimAmount;
        sint64 transferResult;
    };

    PUBLIC_PROCEDURE_WITH_LOCALS(Claim)
    {
        locals.invocationReward = qpi.invocationReward();

        if (locals.invocationReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invocationReward);
        }

        output.claimedAmount = 0;
        output.remainingDividend = 0;

        if (!state.get().playerAccounts.get(qpi.invocator(), locals.playerAccount))
        {
            output.returnCode = QFOMO_ERR_PLAYER_NOT_FOUND;
            return;
        }

        locals.claimAmount = locals.playerAccount.pendingDividend;

        output.remainingDividend = locals.claimAmount;

        if (locals.claimAmount <= 0)
        {
            output.returnCode = QFOMO_ERR_NOTHING_TO_CLAIM;
            return;
        }

        if (state.get().dividendReserve < locals.claimAmount)
        {
            output.returnCode = QFOMO_ERR_RESERVE_MISMATCH;
            return;
        }

        locals.transferResult = qpi.transfer(qpi.invocator(), locals.claimAmount);

        if (locals.transferResult < 0)
        {
            output.returnCode = QFOMO_ERR_TRANSFER_FAILED;
            return;
        }

        locals.playerAccount.pendingDividend = 0;
        locals.playerAccount.totalClaimed += locals.claimAmount;

        state.mut().playerAccounts.set(qpi.invocator(), locals.playerAccount);

        state.mut().dividendReserve -= locals.claimAmount;

        output.claimedAmount = locals.claimAmount;

        output.remainingDividend = 0;
        output.returnCode = QFOMO_SUCCESS;
    }

    PUBLIC_FUNCTION(GetGame)
    {
        output.returnCode = QFOMO_SUCCESS;
        output.roundId = state.get().round.roundId;
        output.startTick = state.get().round.startTick;
        output.endTick = state.get().round.endTick;
        output.finalizedTick = state.get().round.finalizedTick;
        output.currentTick = qpi.tick();
        output.remainingTick = state.get().round.endTick > qpi.tick() ? state.get().round.endTick - qpi.tick() : 0;
        output.currentBidCount = state.get().round.currentBidCount;
        output.currentBidPrice = state.get().round.currentBidPrice;
        output.dividendReserve = state.get().dividendReserve;
        output.teamReserve = state.get().teamReserve;
        output.jackpot = state.get().round.jackpot;
        output.totalVolume = state.get().round.totalVolume;
        output.lastBidder = state.get().round.lastBidder;
        output.lastBidNumber = state.get().round.lastBidNumber;
        output.status = state.get().round.status;
        output.totalRounds = state.get().totalRounds;
        output.totalBids = state.get().totalBids;
    }

    struct GetPlayer_locals
    {
        PlayerAccount playerAccount;
    };

    PUBLIC_FUNCTION_WITH_LOCALS(GetPlayer)
    {
        if (state.get().playerAccounts.get(input.player, locals.playerAccount))
        {
            output.pendingDividend = locals.playerAccount.pendingDividend;
            output.totalClaimed = locals.playerAccount.totalClaimed;
            output.exists = 1;
        }
        else
        {
            output.pendingDividend = 0;
            output.totalClaimed = 0;
            output.exists = 0;
        }
    }

    struct END_TICK_locals
    {
        uint32 bidIndex;

        ActiveBid activeBid;
        PlayerAccount playerAccount;

        sint64 recentEarned;
        sint64 oldEarned;
        sint64 settledDividend;

        sint64 transferResult;
    };

    END_TICK_WITH_LOCALS()
    {
        if (state.get().round.status == QFOMO_STATUS_FINALIZED)
        {
            if (qpi.tick() <= state.get().round.finalizedTick)
            {
                return;
            }

            state.mut().round.roundId = state.get().round.roundId + 1;
            state.mut().round.startTick = qpi.tick();
            state.mut().round.endTick = qpi.tick() + QFOMO_DEFAULT_MAX_TIMER_TICKS;
            state.mut().round.finalizedTick = 0;
            state.mut().round.currentBidCount = 0;
            state.mut().round.currentBidPrice = QFOMO_INITIAL_BID_PRICE;
            state.mut().round.jackpot = 0;
            state.mut().round.totalVolume = 0;
            state.mut().round.recentDividendAccumulator = 0;
            state.mut().round.oldDividendAccumulator = 0;
            state.mut().round.lastBidder = NULL_ID;
            state.mut().round.lastBidNumber = 0;
            state.mut().round.status = QFOMO_STATUS_ACTIVE;
            state.mut().totalRounds++;

            return;
        }
        
        if (state.get().round.status != QFOMO_STATUS_ACTIVE || qpi.tick() < state.get().round.endTick)
        {
            return;
        }

        for (locals.bidIndex = 0; locals.bidIndex < state.get().round.currentBidCount; locals.bidIndex++)
        {
            locals.activeBid = state.get().activeBids.get(locals.bidIndex);

            if (locals.activeBid.roundId != state.get().round.roundId || locals.activeBid.bidNumber != locals.bidIndex + 1)
            {
                continue;
            }

            if (locals.activeBid.phase == QFOMO_BID_PHASE_EXPIRED)
            {
                continue;
            }

            locals.recentEarned = 0;
            locals.oldEarned = 0;
            locals.settledDividend = 0;

            if (locals.activeBid.phase == QFOMO_BID_PHASE_RECENT)
            {
                locals.recentEarned = state.get().round.recentDividendAccumulator - locals.activeBid.recentAccumulatorDebt;
                locals.activeBid.settledRecentDividend = locals.recentEarned;
                locals.settledDividend = locals.recentEarned;
            }
            else if (locals.activeBid.phase == QFOMO_BID_PHASE_OLD)
            {
                locals.oldEarned = state.get().round.oldDividendAccumulator - locals.activeBid.oldAccumulatorDebt;
                locals.settledDividend = locals.activeBid.settledRecentDividend + locals.oldEarned;
            }
            else
            {
                continue;
            }

            if (!state.get().playerAccounts.get(locals.activeBid.bidder, locals.playerAccount))
            {
                return;
            }

            locals.playerAccount.pendingDividend += locals.settledDividend;
            state.mut().playerAccounts.set(locals.activeBid.bidder, locals.playerAccount);

            locals.activeBid.settledDividend = locals.settledDividend;
            locals.activeBid.phase = QFOMO_BID_PHASE_EXPIRED;

            state.mut().activeBids.set(locals.bidIndex, locals.activeBid);
        }
        
        if (state.get().round.currentBidCount > 0 && state.get().round.jackpot > 0)
        {
            locals.transferResult = qpi.transfer(state.get().round.lastBidder, state.get().round.jackpot);
            if (locals.transferResult < 0)
            {
                return;
            }
            state.mut().round.jackpot = 0;
        }

        state.mut().round.currentBidPrice = 0;
        state.mut().round.finalizedTick = qpi.tick();
        state.mut().round.status = QFOMO_STATUS_FINALIZED;
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_PROCEDURE(PlaceBid, 1);
        REGISTER_USER_PROCEDURE(Claim, 2);

        REGISTER_USER_FUNCTION(GetGame, 1);
        REGISTER_USER_FUNCTION(GetPlayer, 2);
    }
};