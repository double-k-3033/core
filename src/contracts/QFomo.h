#include "qpi.h"

using namespace QPI;

constexpr sint64 QFOMO_INITIAL_BID_PRICE = 1000000;
constexpr uint32 QFOMO_ACTIVE_BID_CAPACITY = 256;

constexpr uint32 QFOMO_BPS = 10000;

constexpr uint32 QFOMO_ALPHA_BPS = 5000; // 50% for dividends
constexpr uint32 QFOMO_TEAM_BPS = 1000; // 10% for Team/Shareholders/Burn
constexpr uint32 QFOMO_JACKPOT_BPS = 4000; // 40% for jackpot

constexpr uint32 QFOMO_GROWTH_BPS = 1000; // 10% growth per bid
constexpr uint32 QFOMO_RECENT_WEIGHT_BPS = 2000; // 20% of dividend pool to recent group

constexpr uint32 QFOMO_X = 200; // total future bids that pay one bid
constexpr uint32 QFOMO_W = 160; // old group size
constexpr uint32 QFOMO_RECENT_LEN = QFOMO_X - QFOMO_W; // 40 recent bids
constexpr uint32 QFOMO_OLD_LEN = QFOMO_W; // 160 olds bids

constexpr uint32 QFOMO_MAX_BIDS_PER_ROUND = QFOMO_X + 1; // 201

constexpr uint32 QFOMO_DEFAULT_MAX_TIMER_TICKS = 108000; //24h = 24 * 3600 / 0.8 (~0.8s per tick)
constexpr uint32 QFOMO_DEFAULT_ADD_TIMER_TICKS = 38; // 30s = 30 / 0.8 (~0.8s per tick)

constexpr uint32 QFOMO_STATUS_ACTIVE = 1;
constexpr uint32 QFOMO_STATUS_FINALIZED = 2;

constexpr uint32 QFOMO_SUCCESS = 0;
constexpr uint32 QFOMO_ERR_INVALID_PAYMENT = 1;
constexpr uint32 QFOMO_ERR_ROUND_NOT_ACTIVE = 2;
constexpr uint32 QFOMO_ERR_BIDS_REACHED = 3;


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
        sint64 dividendReserve; //  Total dividend money allocated but not yet assigned to users
        sint64 teamReserve; // Team/shareholder/burn allocation
        sint64 totalVolume; // Total accepted bid payments in the current round

        id lastBidder;
        uint32 lastBidNumber;

        uint32 status;
    };

    struct StateData
    {
        Round round;

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
    };

    INITIALIZE()
    {
        state.mut().round.roundId = 1;
        state.mut().round.startTick = qpi.tick();
        state.mut().round.endTick = state.get().round.startTick + QFOMO_DEFAULT_MAX_TIMER_TICKS;
        state.mut().round.finalizedTick = 0;
        state.mut().round.currentBidCount = 0;
        state.mut().round.currentBidPrice = QFOMO_INITIAL_BID_PRICE;
        state.mut().round.dividendReserve = 0;
        state.mut().round.teamReserve = 0;
        state.mut().round.jackpot = 0;
        state.mut().round.totalVolume = 0;
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
    };

    PUBLIC_PROCEDURE_WITH_LOCALS(PlaceBid)
    {
        locals.invocationReward = qpi.invocationReward();

        if (state.get().round.status != QFOMO_STATUS_ACTIVE || qpi.tick() >= state.get().round.endTick)
        {
            qpi.transfer(qpi.invocator(), locals.invocationReward);
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

        if (locals.invocationReward > state.get().round.currentBidPrice)
        {
            qpi.transfer(qpi.invocator(), locals.invocationReward - state.get().round.currentBidPrice);
        }
        
        locals.dividendAmount = div<sint64>(state.get().round.currentBidPrice * QFOMO_ALPHA_BPS, QFOMO_BPS);
        locals.teamAmount = div<sint64>(state.get().round.currentBidPrice * QFOMO_TEAM_BPS, QFOMO_BPS);
        locals.jackpotAmount = state.get().round.currentBidPrice - locals.dividendAmount - locals.teamAmount;
        
        locals.acceptedBidPrice = state.get().round.currentBidPrice;

        state.mut().round.dividendReserve += locals.dividendAmount;
        state.mut().round.teamReserve += locals.teamAmount;
        state.mut().round.jackpot += locals.jackpotAmount;
        state.mut().round.totalVolume += state.get().round.currentBidPrice;
        
        state.mut().round.currentBidCount ++;
        state.mut().totalBids ++;

        state.mut().round.lastBidder = qpi.invocator();
        state.mut().round.lastBidNumber = state.get().round.currentBidCount;

        state.mut().round.currentBidPrice = locals.acceptedBidPrice + (div<sint64>(locals.acceptedBidPrice * QFOMO_GROWTH_BPS, QFOMO_BPS));
        state.mut().round.endTick = state.get().round.endTick + QFOMO_DEFAULT_ADD_TIMER_TICKS - qpi.tick() > QFOMO_DEFAULT_MAX_TIMER_TICKS ? qpi.tick() + QFOMO_DEFAULT_MAX_TIMER_TICKS : state.get().round.endTick + QFOMO_DEFAULT_ADD_TIMER_TICKS;

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

    PUBLIC_PROCEDURE(Claim)
    {
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
        output.dividendReserve = state.get().round.dividendReserve;
        output.teamReserve = state.get().round.teamReserve;
        output.jackpot = state.get().round.jackpot;
        output.totalVolume = state.get().round.totalVolume;
        output.lastBidder = state.get().round.lastBidder;
        output.lastBidNumber = state.get().round.lastBidNumber;
        output.status = state.get().round.status;
        output.totalRounds = state.get().totalRounds;
        output.totalBids = state.get().totalBids;
    }

    END_TICK()
    {
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_PROCEDURE(PlaceBid, 1);
        REGISTER_USER_PROCEDURE(Claim, 2);

        REGISTER_USER_FUNCTION(GetGame, 1);
    }
};