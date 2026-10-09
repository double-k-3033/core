#define NO_UEFI

#include "contract_testing.h"

// Tests for the QREWARDS loyalty & dividend platform (pure-snapshot model).
//
// Dividends accumulate into per-currency pots and are distributed at END_EPOCH by
// LIVE weighted holdings (read straight from the asset ledger). There is no claim
// step, no persistent reward-token balance, and no stale "ghost" positions.

static id qrUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QR_ADMIN = qrUser(100);
static const id QR_ALICE = qrUser(1);
static const id QR_BOB   = qrUser(2);
static const id QR_CAROL = qrUser(3); // QREWARDS shareholder (contract shares)
static const id QR_FUND  = qrUser(5); // neutral depositor (never a pool holder)
// NOTE: these must decode to valid uppercase-ASCII ticker names (qpi.h's issueAsset() rejects
// any packed name whose bytes aren't 'A'-'Z'/'0'-'9' per QX's own ticker convention -- the
// pristine test file's original arbitrary-integer constants (123456789ULL / 987654321ULL) both
// fail that check silently, meaning qpi.issueAsset() returns 0 and every test that depends on
// an issued asset cascades to QREWARDS_ASSET_NOT_ISSUED. This is a genuine bug in the delivered
// test file (not the QREWARDS contract itself, and not a devkit-only quirk -- the same
// validation lives in the real qpi.issueAsset() used by every other contract's test file, e.g.
// assetNameFromString("QSWAP0") in contract_qswap.cpp). Fixed by using real ticker-shaped names.
static const uint64 QR_TOKEN = 0x4e454b4f54ULL; // "TOKEN" packed little-endian ASCII
static const uint64 QR_DOGE  = 0x45474f44ULL;   // "DOGE" packed little-endian ASCII

class QRewardsChecker : public QREWARDS, public QREWARDS::StateData
{
public:
    uint32 numPoolsOf() const { return numPools; }
    id poolAdminOf(uint64 p) const { return poolMeta.get(p).admin; }
    uint32 poolNumAssets(uint64 p) const { return poolMeta.get(p).numAssets; }
    uint8 poolNumCurrencies(uint64 p) const { return poolMeta.get(p).numCurrencies; }
    uint8 poolActive(uint64 p) const { return poolMeta.get(p).active; }
    uint8 poolPaused(uint64 p) const { return poolMeta.get(p).paused; }
    uint64 poolLastTotalWeight(uint64 p) const { return poolMeta.get(p).lastTotalWeight; }
    uint64 poolPot(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).pot; }
    uint64 poolDistributable(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).distributable; }
    uint64 pendingDivFeeQUOf() const { return pendingDivFeeQU; }
    id qpayTokenAddrOf() const { return qpayTokenDividendsAddress; }
    id qpayhubAddrOf() const { return qpayhubAddress; }
    uint8 distributionModeOf() const { return distributionMode; }
    uint8 cycleActiveOf() const { return cycleActive; }
    uint64 createPoolFeeOf() const { return createPoolFee; }
    uint64 operatingFeeOf() const { return operatingFee; }
    uint32 poolNumNfts(uint64 p) const { return poolMeta.get(p).numNfts; }
};

// Minimal QBAY state access for test setup only (enabling the marketplace so mint() will run) --
// not a full checker, QBAY has its own dedicated suite in contract_qbay.cpp for that.
class QBayPoker : public QBAY, public QBAY::StateData
{
public:
    void enableMarketplace() { statusOfMarketPlace = 1; }
};

class ContractTestingQRewards : public ContractTesting
{
public:
    ContractTestingQRewards()
    {
        initEmptySpectrum();
        initEmptyUniverse();
        system.epoch = contractDescriptions[QREWARDS_CONTRACT_INDEX].constructionEpoch;
        INIT_CONTRACT(QREWARDS);
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, INITIALIZE);
        INIT_CONTRACT(QX);
        callSystemProcedure(QX_CONTRACT_INDEX, INITIALIZE);
        // QBAY backs the NFT-weight feature (registerNFT/ComputeEntitlement/SnapshotPool all
        // cross-contract-call QBAY::getInfoOfNFTById) -- init it the same way QTREAT.h's own
        // harness does for its QBAY-backed ASIC NFTs.
        INIT_CONTRACT(QBAY);
        callSystemProcedure(QBAY_CONTRACT_INDEX, INITIALIZE);
        // statusOfMarketPlace defaults to 0 (mint()/createCollection() refuse to run until the
        // real marketPlaceOwner -- hardcoded in QBAY's own INITIALIZE, no bootstrap -- turns it
        // on); poke it directly rather than going through changeStatusOfMarketPlace, since the
        // owner identity itself isn't the thing under test here.
        ((QBayPoker*)contractStates[QBAY_CONTRACT_INDEX])->enableMarketplace();
    }

    QRewardsChecker* getState() { return (QRewardsChecker*)contractStates[QREWARDS_CONTRACT_INDEX]; }

    // Mints a real single NFT (typeOfMint=1, no collection) via QBAY's own mint() procedure and
    // returns its real id -- QBAY NFT ids are assigned sequentially from 0, so the n-th mint
    // across the whole test (any creator) is always id n-1.
    uint32 mintTestNFT(const id& creator, uint32 royalty = 0)
    {
        increaseEnergy(creator, QBAY_SINGLE_NFT_CREATE_FEE);
        QBAY::mint_input input;
        setMemory(input, 0);
        input.royalty = royalty;
        input.typeOfMint = 1;
        QBAY::mint_output output;
        invokeUserProcedure(QBAY_CONTRACT_INDEX, 3, input, output, creator, QBAY_SINGLE_NFT_CREATE_FEE);
        EXPECT_EQ(output.returnCode, 0u); // QBAY::LogInfo::success
        return nextMintedNFTId++;
    }
    uint32 nextMintedNFTId = 0;

    sint64 issueAsset(const id& issuer, uint64 assetName, sint64 numberOfShares)
    {
        QX::IssueAsset_input input{ assetName, numberOfShares, 0, 0 };
        QX::IssueAsset_output output;
        invokeUserProcedure(QX_CONTRACT_INDEX, 1, input, output, issuer, 1000000000ULL);
        return output.issuedNumberOfShares;
    }

    sint64 transferAsset(const id& issuer, uint64 assetName, const id& from, sint64 n, const id& to)
    {
        QX::TransferShareOwnershipAndPossession_input input;
        QX::TransferShareOwnershipAndPossession_output output;
        input.assetName = assetName; input.issuer = issuer;
        input.newOwnerAndPossessor = to; input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 2, input, output, from, 100);
        return output.transferredNumberOfShares;
    }

    sint64 grantMgmtToQRewards(const id& issuer, uint64 assetName, const id& owner, sint64 n)
    {
        QX::TransferShareManagementRights_input input;
        QX::TransferShareManagementRights_output output;
        input.asset.assetName = assetName; input.asset.issuer = issuer;
        input.newManagingContractIndex = QREWARDS_CONTRACT_INDEX;
        input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 9, input, output, owner, 0);
        return output.transferredNumberOfShares;
    }

    uint64 createPool(const id& creator, uint64 label, uint64 fee)
    {
        QREWARDS::createPool_input input{ label };
        QREWARDS::createPool_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 1, input, output, creator, fee);
        return output.poolId;
    }

    sint32 registerAsset(const id& caller, uint64 poolId, uint64 assetName, const id& issuer, uint64 unit, uint32 weightBps, uint8 kind)
    {
        QREWARDS::registerAsset_input input{ poolId, assetName, issuer, unit, weightBps, kind };
        QREWARDS::registerAsset_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 2, input, output, caller, 0);
        return output.returnCode;
    }

    uint32 registerAssetsBatch(const id& caller, uint64 poolId, uint32 count, const id& issuer, uint64 unit)
    {
        QREWARDS::registerAssets_input input;
        memset(&input, 0, sizeof(input));
        input.poolId = poolId; input.count = count;
        for (uint32 i = 0; i < count; i++)
        {
            QREWARDS::AssetSpec s;
            s.assetName = QR_TOKEN; s.issuer = issuer; s.unit = unit; s.weightBps = 10000; s.kind = 0;
            input.specs.set(i, s);
        }
        QREWARDS::registerAssets_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 10, input, output, caller, 0);
        return output.added;
    }

    sint32 addDivCurrency(const id& caller, uint64 poolId, uint64 assetName, const id& issuer)
    {
        QREWARDS::addDividendCurrency_input input{ poolId, assetName, issuer };
        QREWARDS::addDividendCurrency_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 11, input, output, caller, 0);
        return output.returnCode;
    }

    void depositQU(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, 0, 0 };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, amount);
    }

    sint32 depositAsset(const id& caller, uint64 poolId, uint32 currencyIndex, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, currencyIndex, amount };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 depositOperating(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositOperating_input input{ poolId };
        QREWARDS::depositOperating_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 7, input, output, caller, amount);
        return output.returnCode;
    }

    sint32 setPlatformParams(const id& caller, uint64 createFee, uint64 operatingFee)
    {
        QREWARDS::setPlatformParams_input input{ createFee, operatingFee };
        QREWARDS::setPlatformParams_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 8, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setFundingRoute(const id& caller, uint64 poolId, bit clear)
    {
        QREWARDS::setFundingRoute_input input{ poolId, clear };
        QREWARDS::setFundingRoute_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 12, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setExcluded(const id& caller, uint64 poolId, const id& address, bit excluded)
    {
        QREWARDS::setExcludedAddress_input input{ poolId, address, excluded };
        QREWARDS::setExcludedAddress_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 13, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setTargetMode(const id& caller, uint64 poolId, uint8 mode)
    {
        QREWARDS::setTargetMode_input input{ poolId, mode };
        QREWARDS::setTargetMode_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 15, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 addRecipients(const id& caller, uint64 poolId, uint32 count,
                         const id* ws, const uint64* weights, uint32& added)
    {
        QREWARDS::addRecipients_input input;
        memset(&input, 0, sizeof(input));
        input.poolId = poolId; input.count = count;
        for (uint32 i = 0; i < count; i++) { input.wallets.set(i, ws[i]); input.weights.set(i, weights[i]); }
        QREWARDS::addRecipients_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 16, input, output, caller, 0);
        added = output.added;
        return output.returnCode;
    }

    sint32 clearRecipients(const id& caller, uint64 poolId)
    {
        QREWARDS::clearRecipients_input input{ poolId };
        QREWARDS::clearRecipients_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 17, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 registerNFT(const id& caller, uint64 poolId, uint32 nftId, uint32 weightPoints, uint32* indexOut = nullptr)
    {
        QREWARDS::registerNFT_input input{ poolId, nftId, weightPoints };
        QREWARDS::registerNFT_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 18, input, output, caller, 0);
        if (indexOut) *indexOut = output.index;
        return output.returnCode;
    }

    sint32 updateNFT(const id& caller, uint64 poolId, uint32 index, uint32 weightPoints, uint8 active)
    {
        QREWARDS::updateNFT_input input{ poolId, index, weightPoints, active };
        QREWARDS::updateNFT_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 19, input, output, caller, 0);
        return output.returnCode;
    }

    QREWARDS::getPoolNFT_output getPoolNFT(uint64 poolId, uint32 index)
    {
        QREWARDS::getPoolNFT_input input{ poolId, index };
        QREWARDS::getPoolNFT_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 9, input, output);
        return output;
    }

    QREWARDS::getAllPoolNFTs_output getAllPoolNFTs(uint64 poolId)
    {
        QREWARDS::getAllPoolNFTs_input input{ poolId };
        QREWARDS::getAllPoolNFTs_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 10, input, output);
        return output;
    }

    void endEpoch()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_EPOCH);
    }

    void beginEpoch()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, BEGIN_EPOCH);
    }

    void endTick()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_TICK);
    }

    sint32 setDistributionMode(const id& caller, uint8 mode)
    {
        QREWARDS::setDistributionMode_input input{ mode };
        QREWARDS::setDistributionMode_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 14, input, output, caller, 0);
        return output.returnCode;
    }

    // platformOwner is hardcoded in INITIALIZE (no bootstrap), so tests set it directly.
    void becomeOwner() { getState()->platformOwner = QR_ADMIN; }

    // Enable streamed mode (owner = QR_ADMIN) with a given batch size. The 24h delay is fixed in
    // the contract, so streamed tests advance system.tick past it to trigger a cycle.
    void enableStreamed(uint32 batchSize)
    {
        becomeOwner();
        EXPECT_EQ(setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS); // no operating fee
        EXPECT_EQ(setDistributionMode(QR_ADMIN, QREWARDS_DIST_STREAMED), QREWARDS_SUCCESS);
        getState()->streamBatchSize = batchSize;
    }

    // Simulate a plain (standard) QU transfer landing on the contract address, which the
    // kernel delivers to POST_INCOMING_TRANSFER. (invokeUserProcedure only ever fires the
    // procedureTransaction type, which the funding-route path ignores.)
    void simulateIncomingTransfer(const id& from, sint64 amount)
    {
        increaseEnergy(id(QREWARDS_CONTRACT_INDEX, 0, 0, 0), amount);
        QpiContextSystemProcedureCall qpiContext(QREWARDS_CONTRACT_INDEX, POST_INCOMING_TRANSFER);
        QPI::PostIncomingTransfer_input input{ from, amount, QPI::TransferType::standardTransaction };
        qpiContext.call(input);
    }

    uint64 previewWeight(uint64 poolId, const id& user)
    {
        QREWARDS::previewWeight_input input{ poolId, user };
        QREWARDS::previewWeight_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 8, input, output);
        return output.weight;
    }

    uint64 getPoolPot(uint64 poolId, uint32 k, uint64& lastTotalWeight, uint8& paused)
    {
        QREWARDS::getPool_input input{ poolId };
        QREWARDS::getPool_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 1, input, output);
        lastTotalWeight = output.lastTotalWeight;
        paused = output.paused;
        return output.currencyPot.get(k);
    }

    uint32 getAllPoolAssets(uint64 poolId)
    {
        QREWARDS::getAllPoolAssets_input input{ poolId };
        QREWARDS::getAllPoolAssets_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 3, input, output);
        return output.count;
    }

    uint32 getPoolsByAdmin(const id& admin, uint32 offset, uint64& first, uint32& total)
    {
        QREWARDS::getPoolsByAdmin_input input{ admin, offset };
        QREWARDS::getPoolsByAdmin_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 6, input, output);
        first = output.poolIds.get(0);
        total = output.totalMatched;
        return output.count;
    }

    bit isExcluded(uint64 poolId, const id& address)
    {
        QREWARDS::isExcluded_input input{ poolId, address };
        QREWARDS::isExcluded_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 7, input, output);
        return output.excluded;
    }

    // Sum the shares of (assetName, issuer) possessed by `holder`, across any managing contract.
    uint64 tokenBalanceOf(uint64 assetName, const id& issuer, const id& holder)
    {
        Asset asset(issuer, assetName);
        uint64 total = 0;
        for (AssetPossessionIterator iter(asset); !iter.reachedEnd(); iter.next())
        {
            if (iter.possessor() == holder)
                total += (uint64)iter.numberOfPossessedShares();
        }
        return total;
    }

    // Claim platform ownership and disable the operating fee so distribution tests
    // are not perturbed by pool pausing.
    void disableOperatingFee()
    {
        becomeOwner();
        EXPECT_EQ(setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS);
    }
};

TEST(ContractQRewards, CreatePoolAndWeight)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QR_ADMIN);

    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_SUCCESS);

    // 50,000,000 / 1,000,000 = 50 pts -> x1.5 multiplier -> weight 75.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 75u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);
}

// A pool that misses its operating fee long enough to be deactivated refunds its remaining
// funds (pot + distributable + leftover operating balance) to the pool admin.
TEST(ContractQRewards, RefundOnDeactivation)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    // 10,000 operating buffer (< 25,000 fee) so the pool pauses from the first END_EPOCH.
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE + 10000ULL);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500 (fee 500)
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);

    t.endEpoch(); // underfunded -> paused (missed 1)
    t.endEpoch(); // paused (missed 2)
    EXPECT_EQ(t.getState()->poolActive(pool), 1);

    long long adminBefore = getBalance(QR_ADMIN);
    t.endEpoch(); // missed 3 > MAX -> deactivate + refund to admin
    EXPECT_EQ(t.getState()->poolActive(pool), 0);
    // Refund = pot (9500) + leftover operating balance (10000) = 19500.
    EXPECT_EQ(getBalance(QR_ADMIN) - adminBefore, 19500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

// Regression: the weight multiply must not overflow uint64. With unit=1, weight=100x and a
// 2e9 holding, pts*mult*weightBps = 2e9 * 20000 * 1e6 = 4e19 > 2^64 would wrap in 64-bit math;
// the 128-bit path yields the correct 4e11.
TEST(ContractQRewards, WeightNoOverflow)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 2000000000LL); // 2e9 shares
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1ULL, 1000000u, 0), QREWARDS_SUCCESS);
    // pts = 2e9 -> x2.0 multiplier -> 2e9 * 20000 * 1e6 / 1e8 = 400,000,000,000.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 400000000000ULL);
}

TEST(ContractQRewards, QuDividendDistributedAtEpoch)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // Deposit 10,000 QU: 5% (500) fee, 9,500 into the pot; Alice (sole holder) gets it all.
    t.depositQU(QR_FUND, pool, 10000ULL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 500u);

    long long aliceBefore = getBalance(QR_ALICE);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - aliceBefore, 9500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u); // fee flushed
}

// THE KEY TEST: holder 1 earns in epoch 1, sells, holder 2 buys, epoch 2 pays only
// holder 2 -- the same tokens are never counted twice and the seller gets nothing.
TEST(ContractQRewards, SellThenRebuyNoDoubleCount)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL); // Alice issues & holds all 50M
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // --- Epoch 1: Alice is the sole holder (weight 75). ---
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long aliceStart = getBalance(QR_ALICE);
    long long bobStart = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - aliceStart, 9500LL); // Alice paid
    EXPECT_EQ(getBalance(QR_BOB) - bobStart, 0LL);

    // --- Alice sells ALL her tokens to Bob (contract is never told). ---
    EXPECT_EQ(t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 50000000LL, QR_BOB), 50000000LL);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 0u);  // Alice now weightless
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 75u);   // Bob now has the weight

    // --- Epoch 2: deposit again. Only Bob should be paid; Alice gets nothing. ---
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long aliceMid = getBalance(QR_ALICE);
    long long bobMid = getBalance(QR_BOB);
    t.endEpoch();

    EXPECT_EQ(getBalance(QR_BOB) - bobMid, 9500LL);   // Bob gets the FULL 9500
    EXPECT_EQ(getBalance(QR_ALICE) - aliceMid, 0LL);  // the seller gets NOTHING
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u); // not 150 -> no double count
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

TEST(ContractQRewards, ProRataSplitBetweenHolders)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    // unit 1M, weight 1x. Alice 50M -> 50 pts -> x1.5 -> 75. Bob 50M via issue too.
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    // Give Bob 50M of the same asset (Alice transfers half? issue is one-issuer). Alice sends 25M to Bob.
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    // Alice now 25M -> 25 pts -> x1.25 -> weight = 25*12500/10000 = 31.
    // Bob 25M -> 25 pts -> x1.25 -> weight 31. total 62.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 31u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 31u);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500, total weight 62
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.endEpoch();
    // each gets floor(9500 * 31 / 62) = floor(4750) = 4750.
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u); // 9500 fully distributed
}

TEST(ContractQRewards, ExcludedHolderGetsNothing)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // Alice 25M, Bob 25M

    // Exclude Bob: Alice should take the whole pot, Bob zero, and Bob must not dilute.
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_BOB, 1), QREWARDS_SUCCESS);
    EXPECT_EQ(t.isExcluded(pool, QR_BOB), 1);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL); // Alice gets all (Bob excluded)
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 31u); // only Alice's 31 counted
}

TEST(ContractQRewards, TokenDividendDistributedAtEpoch)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0); // Alice weight 75

    // QDOGE as currency 1, funded by QR_FUND.
    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);

    // Deposit 150,000 QDOGE: 5% (7,500) token fee -> QPAY wallet (no QREWARDS shareholders
    // seeded, so the 20% shareholder leg folds to QPAY). 142,500 into the pot.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 7500u);

    t.endEpoch();
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_ALICE), 142500u); // Alice (sole holder) paid in QDOGE
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 0u);
}

// QU dividend fee routing: 20% -> QREWARDS shareholders, 80% (+rounding) -> QPAYHUB account.
TEST(ContractQRewards, QuDividendFeeRouting)
{
    ContractTestingQRewards t;

    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.becomeOwner();
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS);
    // INITIALIZE hardcodes qpayhubAddress to the QPAYHUB contract address id(29,0,0,0).
    const id hub = t.getState()->qpayhubAddrOf();
    EXPECT_EQ(hub, id(QPAYHUB_CONTRACT_INDEX, 0, 0, 0));

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    long long carolBefore = getBalance(QR_CAROL);
    long long hubBefore = getBalance(hub);

    // Deposit 1,352,000 QU: fee 67,600. shareholder leg = 20% = 13,520; perShare = 20;
    // distributed = 13,520 -> Carol. QPAYHUB leg = 54,080 -> id(29,0,0,0).
    t.depositQU(QR_FUND, pool, 1352000ULL);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 67600u);
    t.endEpoch();
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);
    EXPECT_EQ(getBalance(QR_CAROL) - carolBefore, 13520LL);
    EXPECT_EQ(getBalance(hub) - hubBefore, 54080LL);
}

// Token dividend fee: 20% -> QREWARDS shareholders (paid in the token), 80% -> QPAY wallet.
TEST(ContractQRewards, TokenDividendFeeSplit)
{
    ContractTestingQRewards t;

    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);

    // Deposit 150,000 QDOGE: fee 7,500. shareholder leg 20% = 1,500; perShare = 2;
    // distributed = 1,352 -> Carol. QPAY leg = 6,148 -> QPAY wallet. 142,500 into pot.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_CAROL), 1352u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 6148u);
}

TEST(ContractQRewards, BatchRegisterAndListAssets)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAssetsBatch(QR_ADMIN, pool, 3, QR_ALICE, 1000000ULL), 3u);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 3u);
    EXPECT_EQ(t.getAllPoolAssets(pool), 3u);
}

TEST(ContractQRewards, GetPoolsByAdmin)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 20000000000ULL);
    increaseEnergy(QR_ALICE, 20000000000ULL);

    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ALICE, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);

    uint64 first = 0; uint32 total = 0;
    uint32 count = t.getPoolsByAdmin(QR_ADMIN, 0, first, total);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(first, 0u);
}

TEST(ContractQRewards, OperatingFeePauseStopsDistribution)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    // Create pool with NO operating buffer (fee exactly createFee) -> first END_EPOCH
    // pauses it, so its pot is NOT distributed (it carries).
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500

    long long a0 = getBalance(QR_ALICE);
    t.endEpoch(); // operatingBalance 0 < 100k -> paused -> no distribution
    EXPECT_EQ(t.getState()->poolPaused(pool), 1);
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);   // nothing distributed
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u); // pot carried
}

TEST(ContractQRewards, FundingRouteCreditsPot)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // QR_FUND routes its plain QU transfers to this pool.
    EXPECT_EQ(t.setFundingRoute(QR_FUND, pool, 0), QREWARDS_SUCCESS);
    // A plain 20,000 QU transfer from QR_FUND -> 5% fee (1,000) -> 19,000 into the pot.
    t.simulateIncomingTransfer(QR_FUND, 20000LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 19000u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 1000u);

    // An unrouted sender is ignored (no pot change).
    t.simulateIncomingTransfer(QR_ALICE, 5000LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 19000u);
}

// Streamed mode: END_EPOCH does NOT distribute; END_TICK pays out across the epoch.
// With a large batch the whole cycle finishes in one END_TICK.
TEST(ContractQRewards, StreamedDistributionOneTick)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.enableStreamed(100000u); // streamed, no delay, big batch
    EXPECT_EQ(t.getState()->distributionModeOf(), QREWARDS_DIST_STREAMED);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0); // Alice weight 75
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500 (fee 500)

    t.beginEpoch();
    long long a0 = getBalance(QR_ALICE);

    // END_EPOCH in streamed mode must NOT pay (distribution is END_TICK's job).
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u); // still pending

    // Before the ~24h delay elapses, END_TICK starts no cycle.
    t.endTick();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);

    // Advance past the hardcoded delay; one END_TICK (big batch) completes the cycle:
    // roll pot->distributable, pay Alice the whole 9500.
    system.tick += QREWARDS_STREAM_DELAY_TICKS;
    t.endTick();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolDistributable(pool, 0), 0u);
    EXPECT_EQ(t.getState()->cycleActiveOf(), 0);

    // Further END_TICKs in the same epoch are no-ops (one cycle per epoch).
    t.endTick();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
}

// Streamed mode with batchSize = 1: the cycle spans many END_TICKs but still pays everyone
// exactly once (resumable cursor, no double-pay).
TEST(ContractQRewards, StreamedDistributionAcrossTicks)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.enableStreamed(1u); // one work unit per tick

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // Alice 25M, Bob 25M -> weight 31 each
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500

    t.beginEpoch();
    system.tick += QREWARDS_STREAM_DELAY_TICKS; // past the ~24h delay
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);

    // Drive many ticks; the cycle advances one unit each and finishes within a handful.
    for (int i = 0; i < 12; i++) t.endTick();

    EXPECT_EQ(t.getState()->cycleActiveOf(), 0);
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL); // floor(9500 * 31 / 62)
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolDistributable(pool, 0), 0u);
}

// List-mode pool: admin uploads a weighted recipient list; the pot is distributed pro-rata to that
// list each epoch (no assets, no holder lookup). No QU is attached per wallet -- it comes from the pot.
TEST(ContractQRewards, ListModeDistribution)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);

    // Switch to list mode and upload three recipients: weights 1, 1, 2 (total 4).
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool, QREWARDS_TARGET_LIST), QREWARDS_SUCCESS);
    const id ws[3] = { QR_ALICE, QR_BOB, QR_FUND };
    const uint64 wts[3] = { 1ULL, 1ULL, 2ULL };
    uint32 added = 0;
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 3, ws, wts, added), QREWARDS_SUCCESS);
    EXPECT_EQ(added, 3u);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB), f0 = getBalance(QR_FUND);

    t.endEpoch();
    // pot 9500 over total weight 4: Alice 1/4 = 2375, Bob 1/4 = 2375, Fund 2/4 = 4750.
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 2375LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 2375LL);
    EXPECT_EQ(getBalance(QR_FUND) - f0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

// addRecipients with weight 0 removes an entry; clearRecipients invalidates the whole list; and
// only the pool admin may manage the list.
TEST(ContractQRewards, ListModeEditAndAuth)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);
    // QR_BOB must have a spectrum entry for invokeUserProcedure to actually reach the contract
    // (spectrumIndex() returns -1 for a never-funded identity, which makes the test harness's
    // invokeUserProcedure bail out *before* calling the real procedure, leaving output zeroed --
    // i.e. a vacuous QREWARDS_SUCCESS that has nothing to do with the contract's own admin check).
    increaseEnergy(QR_BOB, 1000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool, QREWARDS_TARGET_LIST), QREWARDS_SUCCESS);

    // A non-admin cannot set the mode or the list.
    uint32 added = 0;
    const id one[1] = { QR_ALICE };
    const uint64 onew[1] = { 1ULL };
    EXPECT_EQ(t.setTargetMode(QR_BOB, pool, QREWARDS_TARGET_LIST), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.addRecipients(QR_BOB, pool, 1, one, onew, added), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.clearRecipients(QR_BOB, pool), QREWARDS_NOT_ADMIN);

    // Equal split across Alice and Bob (weight 1 each).
    const id ws[2] = { QR_ALICE, QR_BOB };
    const uint64 wts[2] = { 1ULL, 1ULL };
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 2, ws, wts, added), QREWARDS_SUCCESS);
    EXPECT_EQ(added, 2u);

    // Remove Bob (weight 0); only Alice remains and takes the whole pot.
    const id bob[1] = { QR_BOB };
    const uint64 zero[1] = { 0ULL };
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 1, bob, zero, added), QREWARDS_SUCCESS);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL); // sole recipient
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);      // removed
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);

    // clearRecipients empties the list: a later deposit has no recipients and the pot carries over.
    EXPECT_EQ(t.clearRecipients(QR_ADMIN, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a1 = getBalance(QR_ALICE);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a1, 0LL);        // nobody to pay
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u); // pot retained
}

// Security finding: setPlatformParams let the single platformOwner key set createPoolFee/
// operatingFee to ANY uint64 value with no upper bound. A spiked operatingFee would make every
// pool's operatingBalance instantly insufficient, mass-pausing (and after
// QREWARDS_MAX_MISSED_EPOCHS, mass-deactivating) the entire platform at the next END_EPOCH --
// a single-key, zero-cost, platform-wide DoS. Fixed with a hardwired ceiling: the owner can move
// either fee anywhere in [0, cap] (raise or lower), but never past the cap.
TEST(ContractQRewards, PlatformFeeCapsEnforced)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    t.becomeOwner();

    // At the cap: accepted.
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MAX_CREATE_POOL_FEE, QREWARDS_MAX_OPERATING_FEE), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->createPoolFeeOf(), QREWARDS_MAX_CREATE_POOL_FEE);
    EXPECT_EQ(t.getState()->operatingFeeOf(), QREWARDS_MAX_OPERATING_FEE);

    // The owner can freely lower fees within the cap.
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_DEFAULT_OPERATING_FEE), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->createPoolFeeOf(), QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(t.getState()->operatingFeeOf(), QREWARDS_DEFAULT_OPERATING_FEE);

    // One past the cap on operatingFee: rejected, state unchanged.
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_MAX_OPERATING_FEE + 1), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.getState()->operatingFeeOf(), QREWARDS_DEFAULT_OPERATING_FEE);

    // One past the cap on createPoolFee: rejected, state unchanged.
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MAX_CREATE_POOL_FEE + 1, QREWARDS_DEFAULT_OPERATING_FEE), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.getState()->createPoolFeeOf(), QREWARDS_DEFAULT_CREATE_FEE);

    // The old exploit value (UINT64_MAX) is rejected outright.
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, ~0ULL), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.getState()->operatingFeeOf(), QREWARDS_DEFAULT_OPERATING_FEE);
}

// ============================================================================
// NFT-backed weight (QBAY integration): registerNFT/updateNFT, the audit functions, and
// ComputeEntitlement's single-user path. All of these cross-contract-call QBAY's real
// getInfoOfNFTById -- the NFTs minted here are real QBAY state, not fabricated.
// ============================================================================

TEST(ContractQRewards, RegisterNFTHappyPathAndAuditFunctions)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);

    uint32 idx = 999;
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 42, &idx), QREWARDS_SUCCESS);
    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(t.getState()->poolNumNfts(pool), 1u);

    auto one = t.getPoolNFT(pool, 0);
    EXPECT_EQ(one.returnCode, QREWARDS_SUCCESS);
    EXPECT_EQ(one.nftId, nftId);
    EXPECT_EQ(one.weightPoints, 42u);
    EXPECT_EQ(one.active, 1u);

    auto all = t.getAllPoolNFTs(pool);
    EXPECT_EQ(all.returnCode, QREWARDS_SUCCESS);
    EXPECT_EQ(all.count, 1u);
    EXPECT_EQ(all.nfts.get(0).nftId, nftId);

    EXPECT_EQ(t.getPoolNFT(pool, 1).returnCode, QREWARDS_INVALID_INDEX); // out of range
}

TEST(ContractQRewards, RegisterNFTRejectsNonAdminDuplicateZeroWeightAndUnmintedId)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 1000);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);

    EXPECT_EQ(t.registerNFT(QR_BOB, pool, nftId, 10), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 0), QREWARDS_INVALID_PARAM); // weightPoints==0

    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_SUCCESS);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_INVALID_PARAM); // duplicate

    // Never-minted id: QBAY's getInfoOfNFTById returns possessor==NULL_ID for an out-of-range id.
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId + 1000, 10), QREWARDS_ASSET_NOT_ISSUED);
    EXPECT_EQ(t.getState()->poolNumNfts(pool), 1u);
}

TEST(ContractQRewards, RegisterNFTRejectsWhenPoolRegistryFull)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    // Reaching the real 128-NFT cap would need 128 real mints; poke numNfts directly to the cap
    // instead (same shortcut this suite already uses for other boundary checks) -- registerNFT's
    // full-registry guard reads numNfts off PoolMeta, so this exercises the real guard faithfully.
    auto meta = t.getState()->poolMeta.get(pool);
    meta.numNfts = QREWARDS_MAX_NFT_PER_POOL;
    t.getState()->poolMeta.set(pool, meta);

    uint32 nftId = t.mintTestNFT(QR_ALICE);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_REGISTRY_FULL);
}

TEST(ContractQRewards, UpdateNFTAdminOnlyChangesWeightAndActive)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 1000);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_SUCCESS);

    EXPECT_EQ(t.updateNFT(QR_BOB, pool, 0, 20, 1), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 5, 20, 1), QREWARDS_INVALID_INDEX); // out of range
    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 0, 0, 1), QREWARDS_INVALID_PARAM); // weightPoints==0

    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 0, 99, 0), QREWARDS_SUCCESS); // also deactivates
    auto after = t.getPoolNFT(pool, 0);
    EXPECT_EQ(after.weightPoints, 99u);
    EXPECT_EQ(after.active, 0u);
}

TEST(ContractQRewards, PreviewWeightCountsHeldNFTExcludesNonHolderAndExcludedAddress)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE); // Alice is both creator and possessor
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 42), QREWARDS_SUCCESS);

    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 42u); // holds it
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);     // doesn't hold it

    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_ALICE, 1), QREWARDS_SUCCESS);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 0u); // excluded overrides real holding
}

// Holdings mode blends fungible-asset weight and NFT weight into one combined total for the
// same holder -- not two separate pots, one summed weight (confirmed directly via
// poolLastTotalWeight, not just inferred from a payout amount).
TEST(ContractQRewards, EndEpochBlendsFungibleAssetAndNFTWeightForSameHolder)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);
    t.disableOperatingFee();

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    ASSERT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_SUCCESS);
    // 50,000,000 / 1,000,000 = 50 pts -> x1.5 multiplier -> weight 75 (same as CreatePoolAndWeight).

    uint32 nftId = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 25), QREWARDS_SUCCESS);

    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 100u); // 75 (asset) + 25 (NFT) blended

    // The NFT (registered first -> index 0) is only re-verified in SnapshotPool on an epoch
    // where (0 + epoch) mod N == 0 (the round-robin spread) -- previewWeight has no such spread
    // (it's a bounded single-user query, see ComputeEntitlement's own comment), so the 100
    // above is unconditionally live, but SnapshotPool needs a "due" epoch to match it.
    system.epoch = 800; // 800 mod 8 == 0
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500 after the 5% dividend fee
    long long before = getBalance(QR_ALICE);
    t.endEpoch();

    // Fungible weight (75) is never spread; the NFT's 25 points ARE spread, so on the one epoch
    // out of N where it's checked it counts scaled by N (75 + 25*8 = 275) -- the long-run average
    // NFT contribution is still 25 (200/8 on this epoch, 0 on the other 7), but any single frozen
    // snapshot legitimately shows the scaled figure, not the average.
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u + 25u * QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS);
    EXPECT_EQ(getBalance(QR_ALICE) - before, 9500LL); // sole holder of all weight -> the whole pot
}

// The round-robin spread (QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS == 8): with two registered NFTs
// held by two different wallets, only one id's holder is checked (and paid) in any given epoch,
// and the checked id's weight is scaled by N -- the exact mechanism QTREAT already uses for its
// own QBAY-backed dividend NFTs, now proven against this contract's own real cross-contract call.
TEST(ContractQRewards, RoundRobinSpreadChecksOneNFTPerEpochAndScalesItsWeightByN)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);
    t.disableOperatingFee();

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 aliceNft = t.mintTestNFT(QR_ALICE);
    uint32 bobNft = t.mintTestNFT(QR_BOB);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, aliceNft, 10), QREWARDS_SUCCESS); // index 0
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, bobNft, 10), QREWARDS_SUCCESS);   // index 1

    // index 0 is due when (0 + epoch) mod 8 == 0.
    system.epoch = 800; // 800 mod 8 == 0
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long aliceBefore = getBalance(QR_ALICE), bobBefore = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 10u * QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS); // only index 0, scaled by N
    EXPECT_EQ(getBalance(QR_ALICE) - aliceBefore, 9500LL); // sole holder of the (scaled) weight that counted
    EXPECT_EQ(getBalance(QR_BOB) - bobBefore, 0LL);        // index 1 wasn't due this epoch

    // index 1 is due when (1 + epoch) mod 8 == 0 -> epoch mod 8 == 7.
    system.epoch = 807;
    t.depositQU(QR_FUND, pool, 10000ULL); // fresh pot 9500
    aliceBefore = getBalance(QR_ALICE); bobBefore = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 10u * QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS); // only index 1, scaled by N
    EXPECT_EQ(getBalance(QR_ALICE) - aliceBefore, 0LL);  // index 0 wasn't due this epoch
    EXPECT_EQ(getBalance(QR_BOB) - bobBefore, 9500LL);
}
