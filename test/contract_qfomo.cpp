#define NO_UEFI

#include "contract_testing.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static id getUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static sint64 idiv(sint64 a, sint64 b) { return a / b; }

struct BidSplit
{
    sint64 dividendAmount;
    sint64 teamAmount;
    sint64 jackpotBaseAmount;
    sint64 recentPool;
    sint64 oldPool;
    sint64 recentPerSlot;
    sint64 oldPerSlot;
    sint64 teamWalletAmount;
    sint64 shareholderAmount;
    sint64 burnAmount;
    sint64 nextBidPrice;
};

static BidSplit calcBidSplit(sint64 price)
{
    BidSplit s{};
    s.dividendAmount    = idiv(price * QFOMO_ALPHA_BPS,         QFOMO_BPS);
    s.teamAmount        = idiv(price * QFOMO_TEAM_BPS,          QFOMO_BPS);
    s.jackpotBaseAmount = price - s.dividendAmount - s.teamAmount;
    s.teamWalletAmount  = idiv(price * QFOMO_TEAM_WALLET_BPS,   QFOMO_BPS);
    s.shareholderAmount = idiv(price * QFOMO_SHAREHOLDER_BPS,   QFOMO_BPS);
    s.burnAmount        = s.teamAmount - s.teamWalletAmount - s.shareholderAmount;
    s.recentPool        = idiv(s.dividendAmount * QFOMO_RECENT_WEIGHT_BPS, QFOMO_BPS);
    s.oldPool           = s.dividendAmount - s.recentPool;
    s.recentPerSlot     = idiv(s.recentPool, (sint64)QFOMO_RECENT_LEN);
    s.oldPerSlot        = idiv(s.oldPool,    (sint64)QFOMO_OLD_LEN);
    s.nextBidPrice      = price + idiv(price * QFOMO_GROWTH_BPS, QFOMO_BPS);
    return s;
}

// ── state inspector ───────────────────────────────────────────────────────────

class QFomoChecker : public QFOMO, public QFOMO::StateData
{
public:
    const QFOMO::Round& getRound() const { return round; }

    QFOMO::ActiveBid getBid(uint32 slotIndex) const
    {
        return activeBids.get(slotIndex);
    }

    bool getPlayerAccount(const id& player, QFOMO::PlayerAccount& acc) const
    {
        return playerAccounts.get(player, acc);
    }

    void checkReserveInvariant() const
    {
        EXPECT_EQ(teamWalletReserve + shareholderReserve + burnReserve, teamReserve);
    }
};

// ── test harness ──────────────────────────────────────────────────────────────

static constexpr uint32 START_TICK = 1000;

class ContractTestingQFomo : protected ContractTesting
{
public:
    ContractTestingQFomo()
    {
        initEmptySpectrum();
        initEmptyUniverse();
        system.epoch = contractDescriptions[QFOMO_CONTRACT_INDEX].constructionEpoch;
        system.tick  = START_TICK;
        INIT_CONTRACT(QFOMO);
        callSystemProcedure(QFOMO_CONTRACT_INDEX, INITIALIZE);
    }

    QFomoChecker* getState() const
    {
        return (QFomoChecker*)contractStates[QFOMO_CONTRACT_INDEX];
    }

    void endTick(bool expectSuccess = true)
    {
        callSystemProcedure(QFOMO_CONTRACT_INDEX, END_TICK, expectSuccess);
    }

    QFOMO::PlaceBid_output placeBid(const id& user, sint64 amount)
    {
        QFOMO::PlaceBid_input  input{};
        QFOMO::PlaceBid_output output{};
        invokeUserProcedure(QFOMO_CONTRACT_INDEX, 1, input, output, user, amount);
        return output;
    }

    QFOMO::Claim_output claim(const id& user, sint64 amount = 0)
    {
        QFOMO::Claim_input  input{};
        QFOMO::Claim_output output{};
        invokeUserProcedure(QFOMO_CONTRACT_INDEX, 2, input, output, user, amount);
        return output;
    }

    QFOMO::GetGame_output getGame()
    {
        QFOMO::GetGame_input  input{};
        QFOMO::GetGame_output output{};
        callFunction(QFOMO_CONTRACT_INDEX, 1, input, output);
        return output;
    }

    QFOMO::GetPlayer_output getPlayer(const id& player)
    {
        QFOMO::GetPlayer_input  input{};
        QFOMO::GetPlayer_output output{};
        input.player = player;
        callFunction(QFOMO_CONTRACT_INDEX, 2, input, output);
        return output;
    }

    QFOMO::GetLastRound_output getLastRound()
    {
        QFOMO::GetLastRound_input  input{};
        QFOMO::GetLastRound_output output{};
        callFunction(QFOMO_CONTRACT_INDEX, 3, input, output);
        return output;
    }

    sint64 contractBalance() const
    {
        return getBalance(id(QFOMO_CONTRACT_INDEX, 0, 0, 0));
    }

    void setTick(uint32 t) { system.tick = t; }
    uint32 getTick() const { return system.tick; }

    void expireRound()
    {
        system.tick = (uint32)getState()->getRound().endTick;
    }

    void drainContractBalance()
    {
        sint64 bal = contractBalance();
        if (bal > 0)
        {
            int idx = spectrumIndex(id(QFOMO_CONTRACT_INDEX, 0, 0, 0));
            EXPECT_GE(idx, 0);
            decreaseEnergy(idx, bal);
        }
    }
};

// ── tests ─────────────────────────────────────────────────────────────────────

// 1. Initial state
TEST(ContractQFomo, InitialState)
{
    ContractTestingQFomo qfomo;
    auto* s = qfomo.getState();

    EXPECT_EQ(s->getRound().roundId,         1u);
    EXPECT_EQ(s->getRound().startTick,       (uint64)START_TICK);
    EXPECT_EQ(s->getRound().endTick,         (uint64)(START_TICK + QFOMO_DEFAULT_MAX_TIMER_TICKS));
    EXPECT_EQ(s->getRound().finalizedTick,   0u);
    EXPECT_EQ(s->getRound().currentBidCount, 0u);
    EXPECT_EQ(s->getRound().currentBidPrice, (sint64)QFOMO_INITIAL_BID_PRICE);
    EXPECT_EQ(s->getRound().status,          QFOMO_STATUS_ACTIVE);
    EXPECT_EQ(s->getRound().jackpot,         0ll);
    EXPECT_EQ(s->getRound().totalVolume,     0ll);
    EXPECT_EQ(s->getRound().recentDividendAccumulator, 0ll);
    EXPECT_EQ(s->getRound().oldDividendAccumulator,    0ll);
    EXPECT_EQ(s->getRound().lastBidder,      NULL_ID);
    EXPECT_EQ(s->getRound().lastBidNumber,   0u);

    EXPECT_EQ(s->dividendReserve,    0ll);
    EXPECT_EQ(s->teamReserve,        0ll);
    EXPECT_EQ(s->teamWalletReserve,  0ll);
    EXPECT_EQ(s->shareholderReserve, 0ll);
    EXPECT_EQ(s->burnReserve,        0ll);

    EXPECT_EQ(s->totalRounds, 1u);
    EXPECT_EQ(s->totalBids,   0u);

    // No prior round
    EXPECT_EQ(s->lastRound.roundId, 0u);

    auto lr = qfomo.getLastRound();
    EXPECT_EQ(lr.exists,     0u);
    EXPECT_EQ(lr.returnCode, QFOMO_SUCCESS);

    auto game = qfomo.getGame();
    EXPECT_EQ(game.returnCode,      QFOMO_SUCCESS);
    EXPECT_EQ(game.roundId,         1u);
    EXPECT_EQ(game.status,          QFOMO_STATUS_ACTIVE);
    EXPECT_EQ(game.currentBidCount, 0u);
    EXPECT_EQ(game.currentBidPrice, (sint64)QFOMO_INITIAL_BID_PRICE);
    EXPECT_EQ(game.dividendReserve, 0ll);
    EXPECT_EQ(game.jackpot,         0ll);
    EXPECT_EQ(game.totalRounds,     1u);
    EXPECT_EQ(game.totalBids,       0u);
}

// 2. First bid allocation — split math, reserve accounting, bid slot
TEST(ContractQFomo, FirstBidAllocation)
{
    ContractTestingQFomo qfomo;
    const id    user1 = getUser(1);
    const sint64 price = QFOMO_INITIAL_BID_PRICE;
    BidSplit exp       = calcBidSplit(price);

    increaseEnergy(user1, price);
    auto result = qfomo.placeBid(user1, price);

    EXPECT_EQ(result.returnCode,        QFOMO_SUCCESS);
    EXPECT_EQ(result.bidNumber,         1u);
    EXPECT_EQ(result.acceptedBidPrice,  price);
    EXPECT_EQ(result.dividendAmount,    exp.dividendAmount);
    EXPECT_EQ(result.teamAmount,        exp.teamAmount);
    EXPECT_EQ(result.teamWalletAmount,  exp.teamWalletAmount);
    EXPECT_EQ(result.shareholderAmount, exp.shareholderAmount);
    EXPECT_EQ(result.burnAmount,        exp.burnAmount);
    EXPECT_EQ(result.nextBidPrice,      exp.nextBidPrice);

    // No prior bidders → all dividend is phantom → absorbed into jackpot
    sint64 expectedJackpot = exp.jackpotBaseAmount + exp.dividendAmount;
    EXPECT_EQ(result.jackpotAmount, expectedJackpot);
    EXPECT_EQ(result.totalJackpot,  expectedJackpot);

    auto* s = qfomo.getState();
    EXPECT_EQ(s->dividendReserve,    0ll);
    EXPECT_EQ(s->teamReserve,        exp.teamAmount);
    EXPECT_EQ(s->teamWalletReserve,  exp.teamWalletAmount);
    EXPECT_EQ(s->shareholderReserve, exp.shareholderAmount);
    EXPECT_EQ(s->burnReserve,        exp.burnAmount);
    EXPECT_EQ(s->getRound().jackpot, expectedJackpot);
    EXPECT_EQ(s->getRound().totalVolume,     price);
    EXPECT_EQ(s->getRound().currentBidCount, 1u);
    EXPECT_EQ(s->getRound().lastBidder,      user1);
    EXPECT_EQ(s->getRound().lastBidNumber,   1u);
    EXPECT_EQ(s->totalBids, 1u);
    EXPECT_EQ(s->getRound().recentDividendAccumulator, exp.recentPerSlot);

    s->checkReserveInvariant();

    // Bid slot
    auto bid = s->getBid(0);
    EXPECT_EQ(bid.roundId,   1u);
    EXPECT_EQ(bid.bidNumber, 1u);
    EXPECT_EQ(bid.bidder,    user1);
    EXPECT_EQ(bid.phase,     QFOMO_BID_PHASE_RECENT);
    EXPECT_EQ(bid.recentAccumulatorDebt, exp.recentPerSlot);

    // GetPlayer
    auto gp = qfomo.getPlayer(user1);
    EXPECT_EQ(gp.exists,          1u);
    EXPECT_EQ(gp.pendingDividend, 0ll);
    EXPECT_EQ(gp.totalClaimed,    0ll);
}

// 3. Overpayment refund
TEST(ContractQFomo, OverpaymentRefund)
{
    ContractTestingQFomo qfomo;
    const id    user1  = getUser(1);
    const sint64 price  = QFOMO_INITIAL_BID_PRICE;
    const sint64 excess = 500000LL;
    const sint64 sent   = price + excess;

    increaseEnergy(user1, sent);
    sint64 balBefore = getBalance(user1);  // = sent (just credited)

    auto result = qfomo.placeBid(user1, sent);

    EXPECT_EQ(result.returnCode,       QFOMO_SUCCESS);
    EXPECT_EQ(result.acceptedBidPrice, price);

    // Net deduction from user = exact bid price (excess returned)
    EXPECT_EQ(balBefore - getBalance(user1), price);

    // Under-payment rejected and refunded
    const id user2 = getUser(2);
    increaseEnergy(user2, price - 1);
    sint64 u2Before = getBalance(user2);
    auto r2 = qfomo.placeBid(user2, price - 1);
    EXPECT_EQ(r2.returnCode, QFOMO_ERR_INVALID_PAYMENT);
    EXPECT_EQ(getBalance(user2), u2Before);  // full refund
}

// 4. Bids 40 and 41 — RECENT→OLD phase transition
TEST(ContractQFomo, PhaseTransitionAt41)
{
    ContractTestingQFomo qfomo;

    sint64 bidPrice = QFOMO_INITIAL_BID_PRICE;
    for (uint32 i = 0; i < 40; ++i)
    {
        const id user = getUser(i);
        increaseEnergy(user, bidPrice * 2);
        qfomo.placeBid(user, bidPrice * 2);
        bidPrice = qfomo.getState()->getRound().currentBidPrice;
    }

    // Bid 1 still RECENT after 40 bids
    EXPECT_EQ(qfomo.getState()->getBid(0).phase, QFOMO_BID_PHASE_RECENT);

    // Bid 41 triggers transition of slot 0 (bid 1) → OLD
    const id user41 = getUser(40);
    increaseEnergy(user41, bidPrice * 2);
    qfomo.placeBid(user41, bidPrice * 2);

    auto bid1 = qfomo.getState()->getBid(0);
    EXPECT_EQ(bid1.phase, QFOMO_BID_PHASE_OLD);
    EXPECT_GT(bid1.settledRecentDividend, 0ll);
    // oldAccumulatorDebt recorded at the moment of transition
    EXPECT_EQ(bid1.oldAccumulatorDebt,
              qfomo.getState()->getRound().oldDividendAccumulator);

    // Bid 41 itself is RECENT
    EXPECT_EQ(qfomo.getState()->getBid(40).phase, QFOMO_BID_PHASE_RECENT);

    // Bids 2–40 still RECENT (not yet transitioned)
    EXPECT_EQ(qfomo.getState()->getBid(1).phase, QFOMO_BID_PHASE_RECENT);

    qfomo.getState()->checkReserveInvariant();
}

// 5. Bids 200 and 201 — OLD→EXPIRED expiration of bid 1
TEST(ContractQFomo, ExpirationAt201)
{
    ContractTestingQFomo qfomo;

    sint64 bidPrice = QFOMO_INITIAL_BID_PRICE;
    for (uint32 i = 0; i < 200; ++i)
    {
        const id user = getUser(i);
        increaseEnergy(user, bidPrice * 2);
        qfomo.placeBid(user, bidPrice * 2);
        bidPrice = qfomo.getState()->getRound().currentBidPrice;
    }

    // After 200 bids, bid 1 is OLD (transitioned at bid 41)
    EXPECT_EQ(qfomo.getState()->getBid(0).phase, QFOMO_BID_PHASE_OLD);

    QFOMO::PlayerAccount acc{};
    EXPECT_TRUE(qfomo.getState()->getPlayerAccount(getUser(0), acc));
    sint64 pendingBefore = acc.pendingDividend;

    // Bid 201 — expires bid 1
    const id user201 = getUser(200);
    increaseEnergy(user201, bidPrice * 2);
    qfomo.placeBid(user201, bidPrice * 2);

    auto bid1 = qfomo.getState()->getBid(0);
    EXPECT_EQ(bid1.phase, QFOMO_BID_PHASE_EXPIRED);
    EXPECT_GT(bid1.settledDividend, 0ll);

    EXPECT_TRUE(qfomo.getState()->getPlayerAccount(getUser(0), acc));
    EXPECT_EQ(acc.pendingDividend - pendingBefore, bid1.settledDividend);

    // 201 bids = MAX → round ends immediately (endTick = current tick)
    EXPECT_EQ(qfomo.getState()->getRound().currentBidCount, QFOMO_MAX_BIDS_PER_ROUND);
    EXPECT_EQ(qfomo.getState()->getRound().currentBidPrice, 0ll);
    EXPECT_EQ(qfomo.getState()->getRound().endTick, (uint64)qfomo.getTick());

    qfomo.getState()->checkReserveInvariant();
}

// 6. Round ending through timer expiration
TEST(ContractQFomo, TimerExpirationFinalization)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    sint64 jackpotBefore  = qfomo.getState()->getRound().jackpot;
    sint64 user1BalBefore = getBalance(user1);
    EXPECT_GT(jackpotBefore, 0ll);

    qfomo.expireRound();
    qfomo.endTick();

    auto* s = qfomo.getState();
    EXPECT_EQ(s->getRound().status, QFOMO_STATUS_FINALIZED);
    EXPECT_EQ(s->getRound().jackpot, 0ll);
    EXPECT_EQ(s->teamWalletReserve,  0ll);
    EXPECT_EQ(s->burnReserve,        0ll);

    // user1 is last bidder and receives jackpot
    EXPECT_EQ(getBalance(user1) - user1BalBefore, jackpotBefore);
}

// 7. Round ending at exactly 201 bids
TEST(ContractQFomo, MaxBidRoundEnd)
{
    ContractTestingQFomo qfomo;

    sint64 bidPrice = QFOMO_INITIAL_BID_PRICE;
    for (uint32 i = 0; i < QFOMO_MAX_BIDS_PER_ROUND; ++i)
    {
        const id user = getUser(i);
        increaseEnergy(user, bidPrice * 2);
        qfomo.placeBid(user, bidPrice * 2);
        bidPrice = qfomo.getState()->getRound().currentBidPrice;
    }

    auto* s = qfomo.getState();
    EXPECT_EQ(s->getRound().currentBidCount, QFOMO_MAX_BIDS_PER_ROUND);
    EXPECT_EQ(s->getRound().currentBidPrice, 0ll);
    EXPECT_EQ(s->getRound().endTick, (uint64)qfomo.getTick());

    // endTick == currentTick so END_TICK finalizes immediately
    qfomo.endTick();
    EXPECT_EQ(s->getRound().status, QFOMO_STATUS_FINALIZED);
    EXPECT_EQ(s->getRound().jackpot, 0ll);
}

// 8. Team-wallet transfer on finalization
TEST(ContractQFomo, TeamWalletTransfer)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    BidSplit exp           = calcBidSplit(QFOMO_INITIAL_BID_PRICE);
    sint64 walletBalBefore = getBalance(QFOMO_TEAM_WALLET);

    qfomo.expireRound();
    qfomo.endTick();

    EXPECT_EQ(getBalance(QFOMO_TEAM_WALLET) - walletBalBefore, exp.teamWalletAmount);
    EXPECT_EQ(qfomo.getState()->teamWalletReserve, 0ll);
}

// 9. Shareholder dividend distribution
TEST(ContractQFomo, ShareholderDistribution)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    BidSplit exp          = calcBidSplit(QFOMO_INITIAL_BID_PRICE);
    sint64 amountPerShare = exp.shareholderAmount / (sint64)NUMBER_OF_COMPUTORS;
    sint64 expectedPayout = amountPerShare * (sint64)NUMBER_OF_COMPUTORS;
    sint64 expectedRem    = exp.shareholderAmount - expectedPayout;

    qfomo.expireRound();
    qfomo.endTick();

    if (amountPerShare > 0)
    {
        EXPECT_EQ(qfomo.getState()->shareholderReserve, expectedRem);
        // teamReserve reduced by the payout
        // only remainder + burnRem (0) stays in teamReserve
        EXPECT_EQ(qfomo.getState()->teamReserve, expectedRem);
    }
    else
    {
        EXPECT_EQ(qfomo.getState()->shareholderReserve, exp.shareholderAmount);
    }

    qfomo.getState()->checkReserveInvariant();
}

// 10. Shareholder remainder below NUMBER_OF_COMPUTORS (676)
TEST(ContractQFomo, ShareholderRemainderBelow676)
{
    ContractTestingQFomo qfomo;

    // Initial price 1,000,000 → shareholderAmount = 50,000
    // 50,000 / 676 = 73 per share  →  payout = 49,348  →  remainder = 652
    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    BidSplit exp          = calcBidSplit(QFOMO_INITIAL_BID_PRICE);
    sint64 amountPerShare = exp.shareholderAmount / (sint64)NUMBER_OF_COMPUTORS;
    sint64 payout         = amountPerShare * (sint64)NUMBER_OF_COMPUTORS;
    sint64 remainder      = exp.shareholderAmount - payout;

    EXPECT_GT(remainder, 0ll);
    EXPECT_LT(remainder, (sint64)NUMBER_OF_COMPUTORS);

    qfomo.expireRound();
    qfomo.endTick();

    EXPECT_EQ(qfomo.getState()->shareholderReserve, remainder);
    EXPECT_EQ(qfomo.getState()->teamReserve,        remainder);  // only remainder stays
    qfomo.getState()->checkReserveInvariant();
}

// 11. Burn transfer — burnReserve cleared after finalization
TEST(ContractQFomo, BurnTransfer)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    BidSplit exp = calcBidSplit(QFOMO_INITIAL_BID_PRICE);
    EXPECT_GT(exp.burnAmount, 0ll);
    EXPECT_EQ(qfomo.getState()->burnReserve, exp.burnAmount);

    qfomo.expireRound();
    qfomo.endTick();

    EXPECT_EQ(qfomo.getState()->burnReserve, 0ll);
}

// 12. Jackpot payout to last bidder
TEST(ContractQFomo, JackpotPayout)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    const id user2 = getUser(2);

    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    sint64 price2 = qfomo.getState()->getRound().currentBidPrice;
    increaseEnergy(user2, price2);
    qfomo.placeBid(user2, price2);

    sint64 jackpot        = qfomo.getState()->getRound().jackpot;
    sint64 user2BalBefore = getBalance(user2);
    EXPECT_GT(jackpot, 0ll);
    EXPECT_EQ(qfomo.getState()->getRound().lastBidder, user2);

    qfomo.expireRound();
    qfomo.endTick();

    EXPECT_EQ(qfomo.getState()->getRound().jackpot, 0ll);
    EXPECT_EQ(getBalance(user2) - user2BalBefore, jackpot);
}

// 13. Failed transfer followed by retry
TEST(ContractQFomo, FailedTransferRetry)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    sint64 jackpot          = qfomo.getState()->getRound().jackpot;
    sint64 teamWalletBefore = qfomo.getState()->teamWalletReserve;
    EXPECT_GT(jackpot, 0ll);
    EXPECT_GT(teamWalletBefore, 0ll);

    // Drain contract balance — teamWallet transfer will fail
    qfomo.expireRound();
    qfomo.drainContractBalance();
    qfomo.endTick();

    // Round NOT finalized: transfer failed before finalization
    EXPECT_NE(qfomo.getState()->getRound().status, QFOMO_STATUS_FINALIZED);
    // teamWalletReserve unchanged (transfer never completed)
    EXPECT_EQ(qfomo.getState()->teamWalletReserve, teamWalletBefore);

    // Restore funds: contract needs at least jackpot + teamReserve
    sint64 needed = qfomo.getState()->getRound().jackpot
                  + qfomo.getState()->teamReserve + 50000LL;
    increaseEnergy(id(QFOMO_CONTRACT_INDEX, 0, 0, 0), needed);

    // Retry END_TICK — now fully finalizes
    qfomo.endTick();
    EXPECT_EQ(qfomo.getState()->getRound().status, QFOMO_STATUS_FINALIZED);
    EXPECT_EQ(qfomo.getState()->teamWalletReserve, 0ll);
    EXPECT_EQ(qfomo.getState()->burnReserve,        0ll);
    EXPECT_EQ(qfomo.getState()->getRound().jackpot, 0ll);
}

// 14. Dividend claim before rollover (expired during PlaceBid)
TEST(ContractQFomo, DividendClaimBeforeRollover)
{
    ContractTestingQFomo qfomo;

    // Place 201 bids; bid 1 expires during bid 201
    sint64 bidPrice = QFOMO_INITIAL_BID_PRICE;
    for (uint32 i = 0; i < QFOMO_MAX_BIDS_PER_ROUND; ++i)
    {
        const id user = getUser(i);
        increaseEnergy(user, bidPrice * 2);
        qfomo.placeBid(user, bidPrice * 2);
        bidPrice = qfomo.getState()->getRound().currentBidPrice;
    }

    // User 0 (bid 1) has pending dividend from expiry during PlaceBid
    auto gp = qfomo.getPlayer(getUser(0));
    EXPECT_EQ(gp.exists,          1u);
    EXPECT_GT(gp.pendingDividend, 0ll);

    sint64 expectedClaim = gp.pendingDividend;
    sint64 balBefore     = getBalance(getUser(0));

    auto cr = qfomo.claim(getUser(0));
    EXPECT_EQ(cr.returnCode,     QFOMO_SUCCESS);
    EXPECT_EQ(cr.claimedAmount,  expectedClaim);
    EXPECT_EQ(cr.remainingDividend, 0ll);
    EXPECT_EQ(getBalance(getUser(0)) - balBefore, expectedClaim);

    auto gp2 = qfomo.getPlayer(getUser(0));
    EXPECT_EQ(gp2.pendingDividend, 0ll);
    EXPECT_EQ(gp2.totalClaimed,    expectedClaim);

    // Claim again → nothing to claim
    auto cr2 = qfomo.claim(getUser(0));
    EXPECT_EQ(cr2.returnCode, QFOMO_ERR_NOTHING_TO_CLAIM);
}

// 15. Dividend claim after rollover (settled by END_TICK, claimed in next round)
TEST(ContractQFomo, DividendClaimAfterRollover)
{
    ContractTestingQFomo qfomo;

    // Place 41 bids so bid 1 reaches OLD phase but does NOT expire yet
    sint64 bidPrice = QFOMO_INITIAL_BID_PRICE;
    for (uint32 i = 0; i < 41; ++i)
    {
        const id user = getUser(i);
        increaseEnergy(user, bidPrice * 2);
        qfomo.placeBid(user, bidPrice * 2);
        bidPrice = qfomo.getState()->getRound().currentBidPrice;
    }

    EXPECT_EQ(qfomo.getState()->getBid(0).phase, QFOMO_BID_PHASE_OLD);

    // No pending dividend for user 0 yet (not expired)
    auto gp_before = qfomo.getPlayer(getUser(0));
    EXPECT_EQ(gp_before.pendingDividend, 0ll);

    // Finalize round via timer
    qfomo.expireRound();
    qfomo.endTick();
    EXPECT_EQ(qfomo.getState()->getRound().status, QFOMO_STATUS_FINALIZED);

    // END_TICK settles bid 1 (OLD phase → credited to player)
    auto gp_after = qfomo.getPlayer(getUser(0));
    EXPECT_GT(gp_after.pendingDividend, 0ll);

    // Advance to next round
    uint32 finalizedTick = (uint32)qfomo.getState()->getRound().finalizedTick;
    qfomo.setTick(finalizedTick + 1);
    qfomo.endTick();
    EXPECT_EQ(qfomo.getState()->getRound().roundId, 2u);

    // User 0 can still claim in the new round
    sint64 pending   = gp_after.pendingDividend;
    sint64 balBefore = getBalance(getUser(0));

    auto cr = qfomo.claim(getUser(0));
    EXPECT_EQ(cr.returnCode,    QFOMO_SUCCESS);
    EXPECT_EQ(cr.claimedAmount, pending);
    EXPECT_EQ(getBalance(getUser(0)) - balBefore, pending);

    auto gp_claimed = qfomo.getPlayer(getUser(0));
    EXPECT_EQ(gp_claimed.pendingDividend, 0ll);
    EXPECT_EQ(gp_claimed.totalClaimed,    pending);
}

// 16. Empty-round finalization (timer expires with zero bids)
TEST(ContractQFomo, EmptyRoundFinalization)
{
    ContractTestingQFomo qfomo;

    EXPECT_EQ(qfomo.getState()->getRound().currentBidCount, 0u);
    EXPECT_EQ(qfomo.contractBalance(), 0ll);

    qfomo.expireRound();
    qfomo.endTick();

    auto* s = qfomo.getState();
    EXPECT_EQ(s->getRound().status,      QFOMO_STATUS_FINALIZED);
    EXPECT_EQ(s->getRound().jackpot,     0ll);
    EXPECT_EQ(s->teamWalletReserve,      0ll);
    EXPECT_EQ(s->shareholderReserve,     0ll);
    EXPECT_EQ(s->burnReserve,            0ll);
    EXPECT_EQ(s->teamReserve,            0ll);
    EXPECT_EQ(s->dividendReserve,        0ll);

    auto lr = qfomo.getLastRound();
    EXPECT_EQ(lr.exists,            1u);
    EXPECT_EQ(lr.roundId,           1u);
    EXPECT_EQ(lr.totalBids,         0u);
    EXPECT_EQ(lr.jackpotPaid,       0ll);
    EXPECT_EQ(lr.winner,            NULL_ID);
    EXPECT_EQ(lr.winningBidNumber,  0u);
    EXPECT_EQ(lr.returnCode,        QFOMO_SUCCESS);
}

// 17. LastRoundSummary values
TEST(ContractQFomo, LastRoundSummaryValues)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    const id user2 = getUser(2);

    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    sint64 price2 = qfomo.getState()->getRound().currentBidPrice;
    increaseEnergy(user2, price2);
    qfomo.placeBid(user2, price2);

    uint64 expectedStart   = qfomo.getState()->getRound().startTick;
    uint64 expectedEnd     = qfomo.getState()->getRound().endTick;
    sint64 expectedJackpot = qfomo.getState()->getRound().jackpot;
    sint64 expectedVolume  = qfomo.getState()->getRound().totalVolume;
    uint32 expectedBids    = qfomo.getState()->getRound().currentBidCount;

    qfomo.expireRound();
    uint32 finalizeTick = qfomo.getTick();
    qfomo.endTick();

    auto lr = qfomo.getLastRound();
    EXPECT_EQ(lr.returnCode,       QFOMO_SUCCESS);
    EXPECT_EQ(lr.exists,           1u);
    EXPECT_EQ(lr.roundId,          1u);
    EXPECT_EQ(lr.startTick,        expectedStart);
    EXPECT_EQ(lr.endTick,          expectedEnd);
    EXPECT_EQ(lr.finalizedTick,    (uint64)finalizeTick);
    EXPECT_EQ(lr.totalBids,        expectedBids);
    EXPECT_EQ(lr.totalVolume,      expectedVolume);
    EXPECT_EQ(lr.jackpotPaid,      expectedJackpot);
    EXPECT_EQ(lr.winner,           user2);
    EXPECT_EQ(lr.winningBidNumber, 2u);

    // Mirrors state
    EXPECT_EQ(qfomo.getState()->lastRound.roundId,          lr.roundId);
    EXPECT_EQ(qfomo.getState()->lastRound.jackpotPaid,      lr.jackpotPaid);
    EXPECT_EQ(qfomo.getState()->lastRound.winner,           lr.winner);
    EXPECT_EQ(qfomo.getState()->lastRound.winningBidNumber, lr.winningBidNumber);
}

// 18. Next-round initialization
TEST(ContractQFomo, NextRoundInitialization)
{
    ContractTestingQFomo qfomo;

    const id user1 = getUser(1);
    increaseEnergy(user1, QFOMO_INITIAL_BID_PRICE);
    qfomo.placeBid(user1, QFOMO_INITIAL_BID_PRICE);

    qfomo.expireRound();
    qfomo.endTick();

    EXPECT_EQ(qfomo.getState()->getRound().status, QFOMO_STATUS_FINALIZED);
    EXPECT_EQ(qfomo.getState()->totalRounds, 1u);

    // Advance one tick beyond finalizedTick
    uint32 finalizedTick = (uint32)qfomo.getState()->getRound().finalizedTick;
    qfomo.setTick(finalizedTick + 1);
    qfomo.endTick();

    auto* s = qfomo.getState();
    EXPECT_EQ(s->getRound().roundId,         2u);
    EXPECT_EQ(s->getRound().status,          QFOMO_STATUS_ACTIVE);
    EXPECT_EQ(s->getRound().currentBidCount, 0u);
    EXPECT_EQ(s->getRound().currentBidPrice, (sint64)QFOMO_INITIAL_BID_PRICE);
    EXPECT_EQ(s->getRound().jackpot,         0ll);
    EXPECT_EQ(s->getRound().totalVolume,     0ll);
    EXPECT_EQ(s->getRound().startTick,       (uint64)(finalizedTick + 1));
    EXPECT_EQ(s->getRound().endTick,
              (uint64)(finalizedTick + 1 + QFOMO_DEFAULT_MAX_TIMER_TICKS));
    EXPECT_EQ(s->getRound().recentDividendAccumulator, 0ll);
    EXPECT_EQ(s->getRound().oldDividendAccumulator,    0ll);
    EXPECT_EQ(s->getRound().lastBidder,  NULL_ID);
    EXPECT_EQ(s->getRound().lastBidNumber, 0u);
    EXPECT_EQ(s->totalRounds, 2u);

    // Round 1 summary still accessible
    auto lr = qfomo.getLastRound();
    EXPECT_EQ(lr.roundId, 1u);
    EXPECT_EQ(lr.exists,  1u);

    // GetGame reflects new round
    auto game = qfomo.getGame();
    EXPECT_EQ(game.roundId,         2u);
    EXPECT_EQ(game.status,          QFOMO_STATUS_ACTIVE);
    EXPECT_EQ(game.currentBidCount, 0u);
    EXPECT_EQ(game.currentBidPrice, (sint64)QFOMO_INITIAL_BID_PRICE);
    EXPECT_EQ(game.totalRounds,     2u);
}
