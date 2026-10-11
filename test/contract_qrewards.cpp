#define NO_UEFI

#include "contract_testing.h"

// Tests for the QREWARDS loyalty & dividend platform (join-to-earn model).
//
// Holders lock registered assets (share management rights -> QREWARDS via QX) and join a pool
// once; each epoch the END_TICK distribution engine measures every member's locked holdings and
// pays the pots pro-rata, a bounded number of O(1) units per tick. There is no claim step and no
// ledger scan, and locked shares cannot move while a cycle runs.

static id qrUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QR_ADMIN = qrUser(100);
static const id QR_ALICE = qrUser(1);
static const id QR_BOB   = qrUser(2);
static const id QR_CAROL = qrUser(3); // QREWARDS shareholder (contract shares)
static const id QR_FUND  = qrUser(5); // neutral depositor
static const id QR_FAKE_CONTRACT = id(40, 0, 0, 0); // a (future) contract id: upper 192 bits zero
// Packed ticker names ('A'-'Z'/'0'-'9' little-endian ASCII, as qpi.issueAsset() requires).
static const uint64 QR_TOKEN = 0x4e454b4f54ULL; // "TOKEN"
static const uint64 QR_DOGE  = 0x45474f44ULL;   // "DOGE"

class QRewardsChecker : public QREWARDS, public QREWARDS::StateData
{
public:
    uint32 numPoolsOf() const { return numPools; }
    uint32 numFreePoolsOf() const { return numFreePools; }
    uint32 totalEntriesOf() const { return totalEntries; }
    PoolMeta meta(uint64 p) const { return poolMeta.get(p); }
    id poolAdminOf(uint64 p) const { return poolMeta.get(p).admin; }
    uint32 poolNumAssets(uint64 p) const { return poolMeta.get(p).numAssets; }
    uint32 poolNumNfts(uint64 p) const { return poolMeta.get(p).numNfts; }
    uint32 poolMemberCount(uint64 p) const { return poolMeta.get(p).memberCount; }
    uint32 poolExcludedCount(uint64 p) const { return poolMeta.get(p).excludedCount; }
    uint8 poolStatus(uint64 p) const { return poolMeta.get(p).status; }
    uint8 poolPaused(uint64 p) const { return poolMeta.get(p).paused; }
    uint32 poolGeneration(uint64 p) const { return poolMeta.get(p).generation; }
    uint64 poolLastTotalWeight(uint64 p) const { return poolMeta.get(p).lastTotalWeight; }
    uint64 poolPot(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).pot; }
    uint64 poolDistributable(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).distributable; }
    uint64 pendingDivFeeQUOf() const { return pendingDivFeeQU; }
    id qpayTokenAddrOf() const { return qpayTokenDividendsAddress; }
    id qpayhubAddrOf() const { return qpayhubAddress; }
    uint8 cycleActiveOf() const { return cycleActive; }
    uint32 cycleIdOf() const { return cycleId; }
    uint64 createPoolFeeOf() const { return createPoolFee; }
    uint64 operatingFeeOf() const { return operatingFee; }
    uint64 joinFeeOf() const { return joinFee; }
    void setMemberCount(uint64 p, uint32 n) { PoolMeta m = poolMeta.get(p); m.memberCount = n; poolMeta.set(p, m); }
    void setNumNfts(uint64 p, uint32 n) { PoolMeta m = poolMeta.get(p); m.numNfts = n; poolMeta.set(p, m); }
};

// Minimal QBAY state access for test setup only (QBAY has its own suite in contract_qbay.cpp).
class QBayPoker : public QBAY, public QBAY::StateData
{
public:
    void enableMarketplace() { statusOfMarketPlace = 1; }
    void setPossessor(uint32 nftId, const id& p)
    {
        InfoOfNFT n = NFTs.get(nftId);
        n.possessor = p;
        NFTs.set(nftId, n);
    }
    // Simulate a traditional auction that is still running (ends in year 2060).
    void startAuction(uint32 nftId)
    {
        InfoOfNFT n = NFTs.get(nftId);
        n.statusOfAuction = 1;
        packQbayDate(60, 1, 1, 0, 0, 0, n.endTimeOfAuction);
        NFTs.set(nftId, n);
    }
    void endAuction(uint32 nftId)
    {
        InfoOfNFT n = NFTs.get(nftId);
        n.statusOfAuction = 0;
        n.endTimeOfAuction = 0;
        NFTs.set(nftId, n);
    }
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
        // QBAY backs the NFT-weight feature (cross-contract getInfoOfNFTById).
        INIT_CONTRACT(QBAY);
        callSystemProcedure(QBAY_CONTRACT_INDEX, INITIALIZE);
        // mint() refuses to run until the hardcoded marketplace owner enables it; poke it directly.
        qbay()->enableMarketplace();
    }

    QRewardsChecker* getState() { return (QRewardsChecker*)contractStates[QREWARDS_CONTRACT_INDEX]; }
    QBayPoker* qbay() { return (QBayPoker*)contractStates[QBAY_CONTRACT_INDEX]; }

    // Mints a real single NFT via QBAY's mint() and returns its id (QBAY ids are sequential from 0).
    uint32 mintTestNFT(const id& creator)
    {
        increaseEnergy(creator, QBAY_SINGLE_NFT_CREATE_FEE);
        QBAY::mint_input input;
        setMemory(input, 0);
        input.typeOfMint = 1;
        QBAY::mint_output output;
        invokeUserProcedure(QBAY_CONTRACT_INDEX, 3, input, output, creator, QBAY_SINGLE_NFT_CREATE_FEE);
        EXPECT_EQ(output.returnCode, 0u);
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

    // QX -> another contract's management (QX.TransferShareManagementRights).
    sint64 grantMgmt(const id& issuer, uint64 assetName, const id& owner, sint64 n, uint32 toContract)
    {
        QX::TransferShareManagementRights_input input;
        QX::TransferShareManagementRights_output output;
        input.asset.assetName = assetName; input.asset.issuer = issuer;
        input.newManagingContractIndex = toContract;
        input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 9, input, output, owner, 0);
        return output.transferredNumberOfShares;
    }

    sint64 grantMgmtToQRewards(const id& issuer, uint64 assetName, const id& owner, sint64 n)
    {
        return grantMgmt(issuer, assetName, owner, n, QREWARDS_CONTRACT_INDEX);
    }

    // Lock: put `n` of the holder's QX-managed shares under QREWARDS management (only locked
    // shares count toward weight).
    sint64 lock(const id& holder, sint64 n, uint64 assetName = QR_TOKEN, const id& issuer = QR_ALICE)
    {
        return grantMgmt(issuer, assetName, holder, n, QREWARDS_CONTRACT_INDEX);
    }

    // Unlock: move `n` locked shares back under QX management (QX charges 100 QU).
    sint64 unlock(const id& holder, sint64 n, uint64 assetName = QR_TOKEN, const id& issuer = QR_ALICE)
    {
        QREWARDS::TransferShareManagementRights_input input;
        input.asset.assetName = assetName; input.asset.issuer = issuer;
        input.numberOfShares = n; input.newManagingContractIndex = QX_CONTRACT_INDEX;
        QREWARDS::TransferShareManagementRights_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 9, input, output, holder, 100);
        return output.transferredNumberOfShares;
    }

    QREWARDS::createPool_output createPoolFull(const id& creator, uint64 fee, uint8 mode = QREWARDS_TARGET_HOLDINGS)
    {
        QREWARDS::createPool_input input{ 0, mode };
        QREWARDS::createPool_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 1, input, output, creator, fee);
        return output;
    }

    uint64 createPool(const id& creator, uint64 fee, uint8 mode = QREWARDS_TARGET_HOLDINGS)
    {
        QREWARDS::createPool_output output = createPoolFull(creator, fee, mode);
        EXPECT_EQ(output.returnCode, QREWARDS_SUCCESS);
        return output.poolId;
    }

    sint32 registerAsset(const id& caller, uint64 poolId, uint64 assetName, const id& issuer, uint64 unit, uint32 weightBps = 10000)
    {
        QREWARDS::registerAsset_input input{ poolId, assetName, issuer, unit, weightBps, 0 };
        QREWARDS::registerAsset_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 2, input, output, caller, 0);
        return output.returnCode;
    }

    uint32 registerAssetsBatch(const id& caller, uint64 poolId, uint32 count, const uint64* names, const id& issuer, uint64 unit)
    {
        QREWARDS::registerAssets_input input;
        memset(&input, 0, sizeof(input));
        input.poolId = poolId; input.count = count;
        for (uint32 i = 0; i < count; i++)
        {
            QREWARDS::AssetSpec s;
            s.assetName = names[i]; s.issuer = issuer; s.unit = unit; s.weightBps = 10000; s.kind = 0;
            input.specs.set(i, s);
        }
        QREWARDS::registerAssets_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 10, input, output, caller, 0);
        return output.added;
    }

    sint32 removeAsset(const id& caller, uint64 poolId, uint32 index)
    {
        QREWARDS::removeAsset_input input{ poolId, index };
        QREWARDS::removeAsset_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 22, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 addDivCurrency(const id& caller, uint64 poolId, uint64 assetName, const id& issuer)
    {
        QREWARDS::addDividendCurrency_input input{ poolId, assetName, issuer };
        QREWARDS::addDividendCurrency_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 11, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 depositQU(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, 0, 0 };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, amount);
        return output.returnCode;
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

    sint32 setPlatformParams(const id& caller, uint64 createFee, uint64 operatingFee, uint64 joinFee)
    {
        QREWARDS::setPlatformParams_input input{ createFee, operatingFee, joinFee };
        QREWARDS::setPlatformParams_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 8, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setPoolAdmin(const id& caller, uint64 poolId, const id& newAdmin)
    {
        QREWARDS::setPoolAdmin_input input{ poolId, newAdmin };
        QREWARDS::setPoolAdmin_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 5, input, output, caller, 0);
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

    QREWARDS::addRecipients_output addRecipients(const id& caller, uint64 poolId, uint32 count,
                                                 const id* ws, const uint64* weights, uint64 fee)
    {
        QREWARDS::addRecipients_input input;
        memset(&input, 0, sizeof(input));
        input.poolId = poolId; input.count = count;
        for (uint32 i = 0; i < count; i++) { input.wallets.set(i, ws[i]); input.weights.set(i, weights[i]); }
        QREWARDS::addRecipients_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 16, input, output, caller, fee);
        return output;
    }

    sint32 clearRecipients(const id& caller, uint64 poolId)
    {
        QREWARDS::clearRecipients_input input{ poolId };
        QREWARDS::clearRecipients_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 17, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 join(const id& caller, uint64 poolId, const id& displace = NULL_ID, uint64 fee = QREWARDS_DEFAULT_JOIN_FEE)
    {
        QREWARDS::join_input input{ poolId, displace };
        QREWARDS::join_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 20, input, output, caller, fee);
        return output.returnCode;
    }

    sint32 leave(const id& caller, uint64 poolId)
    {
        QREWARDS::leave_input input{ poolId };
        QREWARDS::leave_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 21, input, output, caller, 0);
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

    sint32 removeNFT(const id& caller, uint64 poolId, uint32 index)
    {
        QREWARDS::removeNFT_input input{ poolId, index };
        QREWARDS::removeNFT_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 23, input, output, caller, 0);
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

    QREWARDS::getAllPoolAssets_output getAllPoolAssets(uint64 poolId)
    {
        QREWARDS::getAllPoolAssets_input input{ poolId };
        QREWARDS::getAllPoolAssets_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 3, input, output);
        return output;
    }

    QREWARDS::getPool_output getPool(uint64 poolId)
    {
        QREWARDS::getPool_input input{ poolId };
        QREWARDS::getPool_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 1, input, output);
        return output;
    }

    QREWARDS::getMember_output getMember(uint64 poolId, const id& wallet)
    {
        QREWARDS::getMember_input input{ poolId, wallet };
        QREWARDS::getMember_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 11, input, output);
        return output;
    }

    uint64 previewWeight(uint64 poolId, const id& user)
    {
        QREWARDS::previewWeight_input input{ poolId, user };
        QREWARDS::previewWeight_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 8, input, output);
        return output.weight;
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

    void endEpoch() { callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_EPOCH); }
    void beginEpoch() { callSystemProcedure(QREWARDS_CONTRACT_INDEX, BEGIN_EPOCH); }
    void endTick() { callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_TICK); }

    // Move to a new epoch and past the cycle delay; the next END_TICK starts a cycle.
    void enterCycleWindow()
    {
        system.epoch++;
        beginEpoch();
        system.tick += QREWARDS_STREAM_DELAY_TICKS;
    }

    // Drive END_TICK until the running cycle completes; returns the number of END_TICKs used.
    uint32 finishCycle()
    {
        uint32 ticks = 0;
        do
        {
            endTick();
            ticks++;
        } while (getState()->cycleActiveOf() && ticks < 100000);
        EXPECT_EQ(getState()->cycleActiveOf(), 0);
        return ticks;
    }

    uint32 runCycle()
    {
        enterCycleWindow();
        return finishCycle();
    }

    // platformOwner is hardcoded in INITIALIZE, so tests set it directly.
    void becomeOwner() { getState()->platformOwner = QR_ADMIN; }

    // Disable the operating fee so distribution tests are not perturbed by pool pausing.
    void disableOperatingFee()
    {
        becomeOwner();
        EXPECT_EQ(setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_SUCCESS);
    }
};

// Common setup: Alice issues 50M TOKEN (unlocked), a pool registers it (unit 1M, 1x). Locked,
// 50M -> 50 pts -> x1.5 = 75.
static uint64 setupTokenPool(ContractTestingQRewards& t, bool disableOpFee = true)
{
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    increaseEnergy(QR_ALICE, 10000000000ULL);
    increaseEnergy(QR_BOB, 10000000000ULL);
    increaseEnergy(QR_FUND, 10000000000ULL);
    if (disableOpFee) t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL), 50000000LL);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL), QREWARDS_SUCCESS);
    return pool;
}

TEST(ContractQRewards, CreatePoolAndPreviewWeight)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t, false);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QR_ADMIN);
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_ACTIVE);
    EXPECT_EQ(t.getState()->poolGeneration(pool), 1u);

    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 0u); // unlocked shares do not count
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    // 50,000,000 / 1,000,000 = 50 pts -> x1.5 multiplier -> weight 75.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 75u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);
}

// Regression: the weight multiply must not overflow uint64 (2e9 * 20000 * 1e6 > 2^64).
TEST(ContractQRewards, WeightNoOverflow)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 2000000000LL);
    t.lock(QR_ALICE, 2000000000LL);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1ULL, 1000000u), QREWARDS_SUCCESS);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 400000000000ULL);
}

TEST(ContractQRewards, JoinRequiresWeightAndFeeAndBurnsFee)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    const id self = id(QREWARDS_CONTRACT_INDEX, 0, 0, 0);

    long long bob0 = getBalance(QR_BOB);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_NO_WEIGHT); // holds nothing
    EXPECT_EQ(getBalance(QR_BOB), bob0);                 // refunded

    long long alice0 = getBalance(QR_ALICE);
    EXPECT_EQ(t.join(QR_ALICE, pool, NULL_ID, QREWARDS_DEFAULT_JOIN_FEE - 1), QREWARDS_INSUFFICIENT_FEE);
    EXPECT_EQ(getBalance(QR_ALICE), alice0);

    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_NO_WEIGHT); // holds, but nothing locked
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    alice0 = getBalance(QR_ALICE);
    long long self0 = getBalance(self);
    EXPECT_EQ(t.join(QR_ALICE, pool, NULL_ID, QREWARDS_DEFAULT_JOIN_FEE + 5000), QREWARDS_SUCCESS);
    EXPECT_EQ(alice0 - getBalance(QR_ALICE), (long long)QREWARDS_DEFAULT_JOIN_FEE); // excess refunded
    EXPECT_EQ(getBalance(self), self0);                                             // fee burned, not kept
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 1u);
    EXPECT_EQ(t.getMember(pool, QR_ALICE).status, QREWARDS_ENTRY_MEMBER);

    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_ALREADY_MEMBER);
    EXPECT_EQ(t.leave(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 0u);
    EXPECT_EQ(t.leave(QR_ALICE, pool), QREWARDS_NOT_MEMBER);
}

// Contracts (and NULL_ID) are never paid: they cannot create pools, join, or become admin.
TEST(ContractQRewards, ContractIdsCannotCreateJoinOrBecomeAdmin)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    increaseEnergy(QR_FAKE_CONTRACT, 10000000000ULL);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 10000000LL, QR_FAKE_CONTRACT);

    long long c0 = getBalance(QR_FAKE_CONTRACT);
    EXPECT_EQ(t.join(QR_FAKE_CONTRACT, pool), QREWARDS_CONTRACT_NOT_ALLOWED);
    EXPECT_EQ(t.createPoolFull(QR_FAKE_CONTRACT, QREWARDS_DEFAULT_CREATE_FEE).returnCode, QREWARDS_CONTRACT_NOT_ALLOWED);
    EXPECT_EQ(getBalance(QR_FAKE_CONTRACT), c0);

    EXPECT_EQ(t.setPoolAdmin(QR_ADMIN, pool, NULL_ID), QREWARDS_CONTRACT_NOT_ALLOWED);
    EXPECT_EQ(t.setPoolAdmin(QR_ADMIN, pool, id(QX_CONTRACT_INDEX, 0, 0, 0)), QREWARDS_CONTRACT_NOT_ALLOWED);
    EXPECT_EQ(t.setPoolAdmin(QR_ADMIN, pool, QR_BOB), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QR_BOB);
}

// #1 regression: QREWARDS must not define POST_INCOMING_TRANSFER, so QU sent to its address
// before construction (or after a failed IPO) can never abort the sending contract.
TEST(ContractQRewards, NoIncomingTransferCallback)
{
    ContractTestingQRewards t;
    EXPECT_EQ(contractSystemProcedures[QREWARDS_CONTRACT_INDEX][POST_INCOMING_TRANSFER], nullptr);
}

TEST(ContractQRewards, QuDividendPaidToMembersByStreamedCycle)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);

    // Deposit 10,000 QU: 5% (500) fee, 9,500 into the pot.
    EXPECT_EQ(t.depositQU(QR_FUND, pool, 10000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 500u);

    long long alice0 = getBalance(QR_ALICE);
    // END_EPOCH never distributes; it only flushes the fee.
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE), alice0);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);

    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - alice0, 9500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolDistributable(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u);
}

TEST(ContractQRewards, CycleStartsOncePerEpochAfterDelay)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL);

    system.epoch++;
    t.beginEpoch();
    t.endTick(); // before the delay: no cycle
    EXPECT_EQ(t.getState()->cycleIdOf(), 0u);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);

    system.tick += QREWARDS_STREAM_DELAY_TICKS;
    t.finishCycle();
    EXPECT_EQ(t.getState()->cycleIdOf(), 1u);

    t.endTick(); // same epoch: no second cycle
    EXPECT_EQ(t.getState()->cycleIdOf(), 1u);
    t.runCycle(); // next epoch
    EXPECT_EQ(t.getState()->cycleIdOf(), 2u);
}

TEST(ContractQRewards, NonMembersAreNotPaid)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // both hold 25M (weight 31)
    t.lock(QR_ALICE, 25000000LL);
    t.lock(QR_BOB, 25000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);               // only Alice joins

    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);
}

TEST(ContractQRewards, ProRataSplitBetweenMembers)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    t.lock(QR_ALICE, 25000000LL);
    t.lock(QR_BOB, 25000000LL);
    // 25M -> 25 pts -> x1.25 -> weight 31 each.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 31u);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);

    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

// A member who unlocks and sells reads as weight 0 at the next MEASURE and is pruned; the buyer
// earns once they lock and join.
TEST(ContractQRewards, SellerIsPrunedAndBuyerEarnsAfterJoining)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.lock(QR_ALICE, 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);

    EXPECT_EQ(t.unlock(QR_ALICE, 50000000LL), 50000000LL); // no cycle running: allowed
    EXPECT_EQ(t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 50000000LL, QR_BOB), 50000000LL);
    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a1 = getBalance(QR_ALICE);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE), a1);                          // seller gets nothing
    EXPECT_EQ(t.getMember(pool, QR_ALICE).status, QREWARDS_ENTRY_FREE); // pruned
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 0u);
    EXPECT_EQ(t.getState()->totalEntriesOf(), 0u);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);             // carried, nobody eligible

    t.lock(QR_BOB, 50000000LL);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);
    long long b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_BOB) - b0, 9500LL);
}

// Locked shares cannot be shuttled between members mid-cycle to be measured or paid twice:
// unlocking is refused while a cycle runs, and QX cannot move QREWARDS-managed shares.
TEST(ContractQRewards, LockedSharesCannotBeShuttledMidCycle)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    EXPECT_EQ(t.lock(QR_ALICE, 25000000LL), 25000000LL);
    EXPECT_EQ(t.lock(QR_BOB, 25000000LL), 25000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS); // list head (joined last)
    t.depositQU(QR_FUND, pool, 10000ULL);              // pot 9500
    t.getState()->streamBatchSize = 1;

    t.enterCycleWindow();
    t.endTick(); // start + freeze pot
    t.endTick(); // MEASURE Bob: 31
    // Bob tries to hand his measured shares to Alice before she is measured.
    EXPECT_EQ(t.unlock(QR_BOB, 25000000LL), 0);
    EXPECT_EQ(t.transferAsset(QR_ALICE, QR_TOKEN, QR_BOB, 25000000LL, QR_ALICE), 0);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.finishCycle();

    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 62u);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.unlock(QR_BOB, 25000000LL), 25000000LL); // cycle over: allowed again
}

// A member whose locked balance falls mid-cycle (here: depositing it as a dividend) is paid on
// the lower figure; the unpaid remainder carries.
TEST(ContractQRewards, WeightFallingMidCycleIsPaidOnLowerFigure)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_TOKEN, QR_ALICE), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    t.getState()->streamBatchSize = 1;

    t.enterCycleWindow();
    t.endTick(); // freeze
    t.endTick(); // MEASURE Alice: 75
    // Deposit 25M locked TOKEN into the pool's token currency: Alice now holds 25M -> 31.
    EXPECT_EQ(t.depositAsset(QR_ALICE, pool, 1, 25000000ULL), QREWARDS_SUCCESS);
    long long a0 = getBalance(QR_ALICE);
    t.finishCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 3926LL); // floor(9500 * 31 / 75)
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 5574u);
}

// Joining after a cycle has frozen the pot does not capture that pot.
TEST(ContractQRewards, JoinDuringCycleIsPaidFromNextCycle)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    t.lock(QR_ALICE, 25000000LL);
    t.lock(QR_BOB, 25000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL);
    t.getState()->streamBatchSize = 1;

    t.enterCycleWindow();
    t.endTick(); // freeze
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.finishCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);

    t.depositQU(QR_FUND, pool, 10000ULL);
    a0 = getBalance(QR_ALICE); b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
}

// Each END_TICK does at most streamBatchSize O(1) units. 5 members, batch 2: units are IDLE,
// 5 x MEASURE, end-of-MEASURE, 5 x PAY, end-of-PAY, FINISH, final IDLE = 15 units -> 8 ticks.
TEST(ContractQRewards, EngineWorkPerTickIsBounded)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    id users[5];
    for (int i = 0; i < 5; i++)
    {
        users[i] = qrUser(10 + i);
        increaseEnergy(users[i], 1000000000ULL);
        EXPECT_EQ(t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 10000000LL, users[i]), 10000000LL); // weight 12 each
        EXPECT_EQ(t.lock(users[i], 10000000LL), 10000000LL);
        EXPECT_EQ(t.join(users[i], pool), QREWARDS_SUCCESS);
    }
    t.depositQU(QR_FUND, pool, 10000ULL); // 9500 / 5 = 1900 each
    t.getState()->streamBatchSize = 2;
    long long before[5];
    for (int i = 0; i < 5; i++) before[i] = getBalance(users[i]);

    t.enterCycleWindow();
    for (int tick = 0; tick < 4; tick++) t.endTick(); // 8 units: ..., first PAY
    int paid = 0;
    for (int i = 0; i < 5; i++) if (getBalance(users[i]) != before[i]) paid++;
    EXPECT_EQ(paid, 1);
    for (int tick = 0; tick < 3; tick++) t.endTick(); // 14 units
    EXPECT_EQ(t.getState()->cycleActiveOf(), 1);
    t.endTick();                                       // unit 15 ends the cycle
    EXPECT_EQ(t.getState()->cycleActiveOf(), 0);
    for (int i = 0; i < 5; i++) EXPECT_EQ(getBalance(users[i]) - before[i], 1900LL);
}

// Only locked (QREWARDS-managed) shares count, the tier applies to the locked total, and the
// preview equals what MEASURE records.
TEST(ContractQRewards, OnlyLockedSharesCount)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.lock(QR_ALICE, 25000000LL), 25000000LL);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 31u); // 25 locked pts; the unlocked 25M ignored
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.lock(QR_ALICE, 25000000LL), 25000000LL);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 75u); // 50 pts, tier on the total
    t.depositQU(QR_FUND, pool, 10000ULL);
    t.runCycle();
    EXPECT_EQ(t.getMember(pool, QR_ALICE).measuredWeight, 75u);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u);
}

TEST(ContractQRewards, ExcludedAddressCannotJoinOrEarn)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    t.lock(QR_ALICE, 25000000LL);
    t.lock(QR_BOB, 25000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);

    EXPECT_EQ(t.setExcluded(QR_BOB, pool, QR_ALICE, 1), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_BOB, 1), QREWARDS_SUCCESS); // member flipped in place
    EXPECT_EQ(t.isExcluded(pool, QR_BOB), 1);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 1u);
    EXPECT_EQ(t.getState()->poolExcludedCount(pool), 1u);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_EXCLUDED);

    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 31u);

    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_BOB, 0), QREWARDS_SUCCESS); // entry removed
    EXPECT_EQ(t.isExcluded(pool, QR_BOB), 0);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);
}

// Exclusions are capped per pool, so one pool cannot exhaust the shared member capacity.
TEST(ContractQRewards, ExclusionsAreCappedPerPool)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    for (uint32 i = 0; i < QREWARDS_MAX_EXCLUSIONS_PER_POOL; i++)
    {
        EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, qrUser(1000 + i), 1), QREWARDS_SUCCESS);
    }
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, qrUser(5000), 1), QREWARDS_REGISTRY_FULL);
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, NULL_ID, 1), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.getState()->poolExcludedCount(pool), QREWARDS_MAX_EXCLUSIONS_PER_POOL);
}

// A full pool admits a newcomer only by displacing a member with strictly lower live weight, so
// low-weight entries cannot crowd real holders out.
TEST(ContractQRewards, FullPoolRequiresDisplacingLowerWeight)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    const id dave = qrUser(7);
    increaseEnergy(dave, 1000000000ULL);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 1000000LL, QR_BOB); // Bob: 1 pt -> weight 1
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 1000000LL, dave);   // Dave: weight 1
    t.lock(QR_BOB, 1000000LL);
    t.lock(dave, 1000000LL);
    t.lock(QR_ALICE, 48000000LL);                                       // Alice: 48 pts -> 60
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);
    t.getState()->setMemberCount(pool, QREWARDS_MAX_MEMBERS_PER_POOL); // pretend the pool is full

    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_POOL_FULL);           // no victim named
    EXPECT_EQ(t.join(QR_ALICE, pool, QR_CAROL), QREWARDS_POOL_FULL); // not a member
    EXPECT_EQ(t.join(dave, pool, QR_BOB), QREWARDS_POOL_FULL);       // not strictly heavier
    EXPECT_EQ(t.join(QR_ALICE, pool, QR_BOB), QREWARDS_SUCCESS);     // 60 > 1
    EXPECT_EQ(t.getMember(pool, QR_BOB).status, QREWARDS_ENTRY_FREE);
    EXPECT_EQ(t.getMember(pool, QR_ALICE).status, QREWARDS_ENTRY_MEMBER);
    EXPECT_EQ(t.getState()->poolMemberCount(pool), QREWARDS_MAX_MEMBERS_PER_POOL);
}

TEST(ContractQRewards, TokenDividendPaidToMembers)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_INVALID_PARAM); // already a currency

    // 150,000 DOGE: 5% (7,500) fee -> QPAY wallet (no shareholders seeded), 142,500 into the pot.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 7500u);

    t.runCycle();
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_ALICE), 142500u);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 0u);
}

// QU dividend fee: 20% -> QREWARDS shareholders, 80% (+rounding) -> QPAYHUB.
TEST(ContractQRewards, QuDividendFeeRouting)
{
    ContractTestingQRewards t;
    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);
    uint64 pool = setupTokenPool(t);
    // INITIALIZE hardcodes qpayhubAddress to the QPAYHUB contract address id(29,0,0,0).
    const id hub = t.getState()->qpayhubAddrOf();
    EXPECT_EQ(hub, id(QPAYHUB_CONTRACT_INDEX, 0, 0, 0));

    long long carol0 = getBalance(QR_CAROL), hub0 = getBalance(hub);
    // Fee 67,600: shareholder leg 13,520 (perShare 20) -> Carol; 54,080 -> QPAYHUB.
    t.depositQU(QR_FUND, pool, 1352000ULL);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 67600u);
    t.endEpoch();
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);
    EXPECT_EQ(getBalance(QR_CAROL) - carol0, 13520LL);
    EXPECT_EQ(getBalance(hub) - hub0, 54080LL);
}

// Shareholders that are contracts are skipped (paying a contract runs its callback, which can
// abort QREWARDS); their share goes to QPAYHUB for dividend fees.
TEST(ContractQRewards, ContractShareholdersAreNotPaid)
{
    ContractTestingQRewards t;
    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, 338 }, { QR_FAKE_CONTRACT, 338 } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);
    uint64 pool = setupTokenPool(t);
    const id hub = t.getState()->qpayhubAddrOf();

    long long carol0 = getBalance(QR_CAROL), hub0 = getBalance(hub);
    t.depositQU(QR_FUND, pool, 1352000ULL); // fee 67,600; perShare 20
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_CAROL) - carol0, 6760LL);  // 338 * 20
    EXPECT_EQ(getBalance(hub) - hub0, 60840LL);        // 67,600 - 6,760
    EXPECT_EQ(getBalance(QR_FAKE_CONTRACT), 0LL);      // never paid (incl. the create-fee split)
}

// Token dividend fee: 20% -> QREWARDS shareholders (in the token), 80% -> QPAY wallet.
TEST(ContractQRewards, TokenDividendFeeSplit)
{
    ContractTestingQRewards t;
    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);
    uint64 pool = setupTokenPool(t);
    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);

    // Fee 7,500: shareholder leg 1,500, perShare 2 -> 1,352 to Carol; 6,148 to the QPAY wallet.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_CAROL), 1352u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 6148u);
}

TEST(ContractQRewards, DuplicateAssetsRejectedAndRemoveAssetReusesSlot)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL), QREWARDS_INVALID_PARAM);
    t.issueAsset(QR_ALICE, QR_DOGE, 1000000LL);
    const uint64 names[3] = { QR_TOKEN, QR_DOGE, QR_DOGE };
    EXPECT_EQ(t.registerAssetsBatch(QR_ADMIN, pool, 3, names, QR_ALICE, 1000ULL), 1u); // only DOGE is new
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 2u);

    EXPECT_EQ(t.removeAsset(QR_BOB, pool, 0), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.removeAsset(QR_ADMIN, pool, 5), QREWARDS_INVALID_INDEX);
    EXPECT_EQ(t.removeAsset(QR_ADMIN, pool, 0), QREWARDS_SUCCESS); // TOKEN out, DOGE moves to 0
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 1u);
    EXPECT_EQ(t.getAllPoolAssets(pool).assets.get(0).assetName, QR_DOGE);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL), QREWARDS_SUCCESS);
}

TEST(ContractQRewards, GetPoolsByAdmin)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 20000000000ULL);
    increaseEnergy(QR_ALICE, 20000000000ULL);
    t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ALICE, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);

    uint64 first = 0; uint32 total = 0;
    uint32 count = t.getPoolsByAdmin(QR_ADMIN, 0, first, total);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(first, 0u);
}

TEST(ContractQRewards, OperatingFeePauseStopsDistribution)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t, false); // no operating buffer
    EXPECT_EQ(t.lock(QR_ALICE, 50000000LL), 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL);

    long long a0 = getBalance(QR_ALICE);
    t.endEpoch(); // 0 < 25,000 -> paused
    EXPECT_EQ(t.getState()->poolPaused(pool), 1);
    EXPECT_EQ(t.depositQU(QR_FUND, pool, 10000ULL), QREWARDS_POOL_PAUSED);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE), a0);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);

    // Topping up resumes it at the next END_EPOCH.
    EXPECT_EQ(t.depositOperating(QR_ADMIN, pool, 25000ULL), QREWARDS_SUCCESS);
    t.endEpoch();
    EXPECT_EQ(t.getState()->poolPaused(pool), 0);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
}

// A pool whose operating fee lapses closes: its remaining pots go to its MEMBERS (not the admin),
// only the leftover operating balance is refunded to the admin, and the slot is reused.
TEST(ContractQRewards, ClosingPoolPaysMembersNotAdminAndFreesSlot)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    increaseEnergy(QR_ALICE, 10000000000ULL);
    increaseEnergy(QR_FUND, 10000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE + 10000ULL); // buffer < fee
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL);
    t.lock(QR_ALICE, 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500

    t.endEpoch(); t.endEpoch(); // paused, missed 2
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_ACTIVE);
    t.endEpoch();               // missed 3 > 2 -> closing
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_CLOSING);
    EXPECT_EQ(t.depositQU(QR_FUND, pool, 10000ULL), QREWARDS_POOL_INACTIVE);
    EXPECT_EQ(t.depositOperating(QR_ADMIN, pool, 100000ULL), QREWARDS_POOL_INACTIVE);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_DOGE, QR_ALICE, 1ULL), QREWARDS_POOL_INACTIVE);

    long long a0 = getBalance(QR_ALICE), admin0 = getBalance(QR_ADMIN);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);     // pot to the member
    EXPECT_EQ(getBalance(QR_ADMIN) - admin0, 10000LL); // only the operating balance
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_FREE);
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 0u);
    EXPECT_EQ(t.getState()->totalEntriesOf(), 0u);
    EXPECT_EQ(t.getState()->numFreePoolsOf(), 1u);

    // The slot is reused by the next createPool, with a new generation.
    EXPECT_EQ(t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE), pool);
    EXPECT_EQ(t.getState()->poolGeneration(pool), 2u);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 0u);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
}

// A closing pool with no members keeps its pots until a holder joins and collects them.
TEST(ContractQRewards, ClosingPoolWithoutMembersWaitsForJoiners)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    increaseEnergy(QR_ALICE, 10000000000ULL);
    increaseEnergy(QR_FUND, 10000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL);
    t.depositQU(QR_FUND, pool, 10000ULL);
    t.endEpoch(); t.endEpoch(); t.endEpoch();
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_CLOSING);

    long long admin0 = getBalance(QR_ADMIN);
    t.runCycle();
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_CLOSING);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);
    EXPECT_EQ(getBalance(QR_ADMIN), admin0);

    t.lock(QR_ALICE, 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    long long a0 = getBalance(QR_ALICE);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(t.getState()->poolStatus(pool), QREWARDS_POOL_FREE);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_POOL_INACTIVE);
}

TEST(ContractQRewards, ListModeDistribution)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    increaseEnergy(QR_FUND, 10000000000ULL);
    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_TARGET_LIST);

    const id ws[3] = { QR_ALICE, QR_BOB, QR_FUND };
    const uint64 wts[3] = { 1ULL, 1ULL, 2ULL };
    const id self = id(QREWARDS_CONTRACT_INDEX, 0, 0, 0);
    long long admin0 = getBalance(QR_ADMIN), self0 = getBalance(self);
    QREWARDS::addRecipients_output out = t.addRecipients(QR_ADMIN, pool, 3, ws, wts, 3 * QREWARDS_DEFAULT_JOIN_FEE);
    EXPECT_EQ(out.returnCode, QREWARDS_SUCCESS);
    EXPECT_EQ(out.added, 3u);
    EXPECT_EQ(out.listCount, 3u);
    EXPECT_EQ(admin0 - getBalance(QR_ADMIN), (long long)(3 * QREWARDS_DEFAULT_JOIN_FEE));
    EXPECT_EQ(getBalance(self), self0); // entry fees burned

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB), f0 = getBalance(QR_FUND);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 2375LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 2375LL);
    EXPECT_EQ(getBalance(QR_FUND) - f0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

TEST(ContractQRewards, ListModeEditAndAuth)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    increaseEnergy(QR_FUND, 10000000000ULL);
    increaseEnergy(QR_BOB, 1000000000ULL); // funded so its invocations really reach the contract
    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_TARGET_LIST);

    const id one[1] = { QR_ALICE };
    const uint64 onew[1] = { 1ULL };
    EXPECT_EQ(t.addRecipients(QR_BOB, pool, 1, one, onew, QREWARDS_DEFAULT_JOIN_FEE).returnCode, QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.clearRecipients(QR_BOB, pool), QREWARDS_NOT_ADMIN);

    const id ws[2] = { QR_ALICE, QR_BOB };
    const uint64 wts[2] = { 1ULL, 1ULL };
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 2, ws, wts, 2 * QREWARDS_DEFAULT_JOIN_FEE).added, 2u);

    // Remove Bob (weight 0); Alice takes the whole pot.
    const id bob[1] = { QR_BOB };
    const uint64 zero[1] = { 0ULL };
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 1, bob, zero, 0).listCount, 1u);
    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);

    // clearRecipients empties the list: the next pot carries.
    EXPECT_EQ(t.clearRecipients(QR_ADMIN, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolMemberCount(pool), 0u);
    EXPECT_EQ(t.getState()->totalEntriesOf(), 0u);
    t.depositQU(QR_FUND, pool, 10000ULL);
    a0 = getBalance(QR_ALICE);
    t.runCycle();
    EXPECT_EQ(getBalance(QR_ALICE), a0);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);
}

// List entries: NULL_ID/contract ids rejected, only new entries are charged, updates are free.
TEST(ContractQRewards, ListRejectsReservedIdsAndChargesOnlyNewEntries)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 100000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_TARGET_LIST);

    const id ws[3] = { NULL_ID, QR_FAKE_CONTRACT, QR_ALICE };
    const uint64 wts[3] = { 1ULL, 1ULL, 1ULL };
    long long admin0 = getBalance(QR_ADMIN);
    QREWARDS::addRecipients_output out = t.addRecipients(QR_ADMIN, pool, 3, ws, wts, 3 * QREWARDS_DEFAULT_JOIN_FEE);
    EXPECT_EQ(out.added, 1u);
    EXPECT_EQ(out.rejected, 2u);
    EXPECT_EQ(admin0 - getBalance(QR_ADMIN), (long long)QREWARDS_DEFAULT_JOIN_FEE);

    const id alice[1] = { QR_ALICE };
    const uint64 five[1] = { 5ULL };
    admin0 = getBalance(QR_ADMIN);
    out = t.addRecipients(QR_ADMIN, pool, 1, alice, five, QREWARDS_DEFAULT_JOIN_FEE);
    EXPECT_EQ(out.added, 1u);
    EXPECT_EQ(getBalance(QR_ADMIN), admin0); // update: fully refunded
    EXPECT_EQ(t.getMember(pool, QR_ALICE).listWeight, 5u);

    // Without enough reward a new entry is rejected (and nothing is kept).
    const id bob[1] = { QR_BOB };
    out = t.addRecipients(QR_ADMIN, pool, 1, bob, five, QREWARDS_DEFAULT_JOIN_FEE - 1);
    EXPECT_EQ(out.rejected, 1u);
    EXPECT_EQ(getBalance(QR_ADMIN), admin0);
}

// The target mode can only change during setup, so a funded holdings pool can never be switched
// to a self-serving list.
TEST(ContractQRewards, TargetModeLockedOnceFundedOrPopulated)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool, QREWARDS_TARGET_LIST), QREWARDS_SUCCESS);
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool, QREWARDS_TARGET_HOLDINGS), QREWARDS_SUCCESS);
    t.lock(QR_ALICE, 50000000LL);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool, QREWARDS_TARGET_LIST), QREWARDS_POOL_BUSY);

    uint64 pool2 = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    t.depositQU(QR_FUND, pool2, 10000ULL);
    EXPECT_EQ(t.setTargetMode(QR_ADMIN, pool2, QREWARDS_TARGET_LIST), QREWARDS_POOL_BUSY);

    uint64 listPool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_TARGET_LIST);
    EXPECT_EQ(t.join(QR_ALICE, listPool), QREWARDS_WRONG_MODE);
    const id one[1] = { QR_BOB };
    const uint64 onew[1] = { 1ULL };
    EXPECT_EQ(t.addRecipients(QR_ADMIN, pool, 1, one, onew, QREWARDS_DEFAULT_JOIN_FEE).returnCode, QREWARDS_WRONG_MODE);
    EXPECT_EQ(t.createPoolFull(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 7).returnCode, QREWARDS_INVALID_PARAM);
}

TEST(ContractQRewards, PlatformFeeBoundsEnforced)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_BOB, 10000000000ULL);
    t.becomeOwner();

    EXPECT_EQ(t.setPlatformParams(QR_BOB, QREWARDS_DEFAULT_CREATE_FEE, 0, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_NOT_PLATFORM_OWNER);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MIN_CREATE_POOL_FEE, 0, QREWARDS_MIN_JOIN_FEE), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->createPoolFeeOf(), QREWARDS_MIN_CREATE_POOL_FEE);
    EXPECT_EQ(t.getState()->joinFeeOf(), QREWARDS_MIN_JOIN_FEE);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MAX_CREATE_POOL_FEE, QREWARDS_MAX_OPERATING_FEE, QREWARDS_MAX_JOIN_FEE), QREWARDS_SUCCESS);

    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MIN_CREATE_POOL_FEE - 1, 0, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_MAX_CREATE_POOL_FEE + 1, 0, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, QREWARDS_MAX_OPERATING_FEE + 1, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, ~0ULL, QREWARDS_DEFAULT_JOIN_FEE), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0, QREWARDS_MIN_JOIN_FEE - 1), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0, QREWARDS_MAX_JOIN_FEE + 1), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.getState()->operatingFeeOf(), QREWARDS_MAX_OPERATING_FEE);
    EXPECT_EQ(t.getState()->joinFeeOf(), QREWARDS_MAX_JOIN_FEE);
}

// Only QX may hand share management rights to QREWARDS; shares pushed from another contract would
// be stranded under QREWARDS management.
TEST(ContractQRewards, ManagementRightsAcceptedOnlyFromQX)
{
    ContractTestingQRewards t;
    INIT_CONTRACT(QSWAP);
    t.callSystemProcedure(QSWAP_CONTRACT_INDEX, INITIALIZE);
    increaseEnergy(QR_ALICE, 10000000000ULL);
    t.issueAsset(QR_ALICE, QR_TOKEN, 1000000LL);

    EXPECT_EQ(t.grantMgmt(QR_ALICE, QR_TOKEN, QR_ALICE, 1000LL, QSWAP_CONTRACT_INDEX), 1000LL);
    QSWAP::TransferShareManagementRights_input in;
    in.asset.assetName = QR_TOKEN; in.asset.issuer = QR_ALICE;
    in.numberOfShares = 1000; in.newManagingContractIndex = QREWARDS_CONTRACT_INDEX;
    QSWAP::TransferShareManagementRights_output out;
    t.invokeUserProcedure(QSWAP_CONTRACT_INDEX, 11, in, out, QR_ALICE, 0);
    EXPECT_EQ(out.transferredNumberOfShares, 0);

    EXPECT_EQ(t.grantMgmtToQRewards(QR_ALICE, QR_TOKEN, QR_ALICE, 1000LL), 1000LL);
}

// ============================================================================
// NFT-backed weight (QBAY integration). The NFTs are real QBAY state minted via QBAY::mint.
// ============================================================================

TEST(ContractQRewards, RegisterNFTHappyPathAndAuditFunctions)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);

    uint32 idx = 999;
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 42, &idx), QREWARDS_SUCCESS);
    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(t.getState()->poolNumNfts(pool), 1u);

    QREWARDS::getPoolNFT_output one = t.getPoolNFT(pool, 0);
    EXPECT_EQ(one.returnCode, QREWARDS_SUCCESS);
    EXPECT_EQ(one.nftId, nftId);
    EXPECT_EQ(one.weightPoints, 42u);
    EXPECT_EQ(one.active, 1u);

    QREWARDS::getAllPoolNFTs_output all = t.getAllPoolNFTs(pool);
    EXPECT_EQ(all.count, 1u);
    EXPECT_EQ(all.nfts.get(0).nftId, nftId);
    EXPECT_EQ(t.getPoolNFT(pool, 1).returnCode, QREWARDS_INVALID_INDEX);
}

TEST(ContractQRewards, RegisterNFTRejectsBadInput)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 1000);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);

    EXPECT_EQ(t.registerNFT(QR_BOB, pool, nftId, 10), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 0), QREWARDS_INVALID_PARAM); // weightPoints == 0
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_SUCCESS);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_INVALID_PARAM); // duplicate
    // An id >= QBAY's capacity would alias nftId inside QBAY's array: rejected.
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId + QREWARDS_QBAY_MAX_NFT_ID, 10), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId + 1000, 10), QREWARDS_ASSET_NOT_ISSUED); // never minted
    EXPECT_EQ(t.getState()->poolNumNfts(pool), 1u);
}

TEST(ContractQRewards, RegisterNFTRejectsWhenPoolRegistryFull)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    t.getState()->setNumNfts(pool, QREWARDS_MAX_NFT_PER_POOL);
    uint32 nftId = t.mintTestNFT(QR_ALICE);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_REGISTRY_FULL);
}

TEST(ContractQRewards, UpdateAndRemoveNFT)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 1000);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 a = t.mintTestNFT(QR_ALICE);
    uint32 b = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, a, 10), QREWARDS_SUCCESS);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, b, 20), QREWARDS_SUCCESS);

    EXPECT_EQ(t.updateNFT(QR_BOB, pool, 0, 20, 1), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 5, 20, 1), QREWARDS_INVALID_INDEX);
    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 0, 0, 1), QREWARDS_INVALID_PARAM);
    EXPECT_EQ(t.updateNFT(QR_ADMIN, pool, 0, 99, 0), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getPoolNFT(pool, 0).weightPoints, 99u);
    EXPECT_EQ(t.getPoolNFT(pool, 0).active, 0u);

    EXPECT_EQ(t.removeNFT(QR_ADMIN, pool, 0), QREWARDS_SUCCESS); // b moves to index 0
    EXPECT_EQ(t.getState()->poolNumNfts(pool), 1u);
    EXPECT_EQ(t.getPoolNFT(pool, 0).nftId, b);
    EXPECT_EQ(t.registerNFT(QR_ADMIN, pool, a, 10), QREWARDS_SUCCESS); // slot reusable
}

TEST(ContractQRewards, NftHolderCanJoinAndPreviewExcludesNonHolders)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE);
    uint32 nftId = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 42), QREWARDS_SUCCESS);

    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 42u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_NO_WEIGHT);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS); // NFT weight alone qualifies

    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_ALICE, 1), QREWARDS_SUCCESS);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 0u);
}

// Every registered NFT is checked in every cycle (no rotating schedule): asset and NFT weight
// blend for the same member.
TEST(ContractQRewards, NftWeightCountedEveryCycle)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.lock(QR_ALICE, 50000000LL); // Alice: 75 asset weight
    uint32 aliceNft = t.mintTestNFT(QR_ALICE);
    uint32 bobNft = t.mintTestNFT(QR_BOB);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, aliceNft, 25), QREWARDS_SUCCESS);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, bobNft, 100), QREWARDS_SUCCESS);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 100u); // 75 + 25
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);

    for (int cycle = 0; cycle < 2; cycle++)
    {
        t.depositQU(QR_FUND, pool, 10000ULL); // 9500
        long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
        t.runCycle();
        EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 200u);
        EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
        EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    }
}

// QBAY moves possession to the top bidder while an auction runs; such an NFT does not count.
TEST(ContractQRewards, NftInRunningAuctionDoesNotCount)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // 31 each
    t.lock(QR_ALICE, 25000000LL);
    t.lock(QR_BOB, 25000000LL);
    uint32 nftId = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);

    t.qbay()->startAuction(nftId);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 31u);
    t.depositQU(QR_FUND, pool, 10000ULL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.runCycle();
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 62u);
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);

    t.qbay()->endAuction(nftId);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 41u);
    t.depositQU(QR_FUND, pool, 10000ULL);
    t.runCycle();
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 72u);
}

// An NFT credited in NFT_MEASURE but moved away before payment is withdrawn by NFT_VERIFY.
TEST(ContractQRewards, NftMovedBeforePaymentIsNotPaid)
{
    ContractTestingQRewards t;
    uint64 pool = setupTokenPool(t);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_CAROL); // Alice: tokens gone
    t.lock(QR_BOB, 25000000LL);
    uint32 nftId = t.mintTestNFT(QR_ALICE);
    ASSERT_EQ(t.registerNFT(QR_ADMIN, pool, nftId, 10), QREWARDS_SUCCESS);
    EXPECT_EQ(t.join(QR_ALICE, pool), QREWARDS_SUCCESS); // NFT only: 10
    EXPECT_EQ(t.join(QR_BOB, pool), QREWARDS_SUCCESS);   // 31
    t.depositQU(QR_FUND, pool, 10000ULL);
    t.getState()->streamBatchSize = 1;

    t.enterCycleWindow();
    t.endTick(); // freeze
    t.endTick(); // NFT_MEASURE: Alice credited 10
    t.endTick(); // MEASURE Bob
    t.endTick(); // MEASURE Alice: 10 (W = 41)
    t.qbay()->setPossessor(nftId, QR_CAROL);
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.finishCycle();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 7182LL); // floor(9500 * 31 / 41)
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 2318u);
}
