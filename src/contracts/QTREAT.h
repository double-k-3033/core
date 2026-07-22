using namespace QPI;

// ============================================================================
// QTREAT — dividend distribution for QTREAT holders + QDOGE staking
//
// FUNDS
//   1. Dividend fund (default): any plain QU sent to the contract, plus all
//      dividends received from SC-shares/assets the contract holds (routed in
//      POST_INCOMING_TRANSFER, qRWA pattern). Paid out to QTREAT token holders
//      each epoch, weighted by min(beginBalance, endBalance).
//   2. Staking fund (secondary): funded ONLY via DepositStakingFund. Pays
//      QDOGE stakers 20M QU per epoch, proportional to stake.
//
// DIVIDEND-PAYING ASSETS (QVAULT/qRWA style)
//   Admin moves SC shares / assets into the contract via DepositGeneralAsset.
//   When those SCs call distributeDividends, the QU arrives with
//   TransferType::qpiDistributeDividends and is routed to the dividend fund.
//
// QDOGE STAKING
//   - Single-transaction staking: the user calls
//     QX.TransferShareManagementRights(QDOGE, amount, newMgmtIdx = QTREAT).
//     The amount in that call IS the stake amount. PRE_ACQUIRE_SHARES rejects
//     invalid transfers (below minimum / unstake pending) so the QX call just
//     fails and the tokens stay tradable; POST_ACQUIRE_SHARES credits the
//     stake for exactly the shares received. Staked == managed at all times,
//     so over-transfers are structurally impossible. (QX hard-rejects
//     contract-initiated acquireShares, so a pull model is not available.)
//   - Single 52-epoch staking program, starting at the first BEGIN_EPOCH
//     after deployment. The ENTIRE program (rewards, bonus accrual, raffle,
//     and acceptance of new stakes) ends 52 epochs later; extending it
//     requires a contract upgrade. Unstaking always works.
//   - Unstaking is one transaction: RequestUnstake collects the 100 QU QX
//     fee upfront; after the 2-epoch delay the contract auto-releases the
//     QDOGE back to QX in END_EPOCH. FinalizeUnstake is a free manual
//     fallback for claiming between the delay expiring and epoch end.
//   - Minimum stake: 10,000,000 QDOGE.
//   - 20,000,000 QU distributed per epoch to stakers, pro-rata, capped by
//     the staking fund balance.
//   - Bonus: 12 CONSECUTIVE epochs with >= 50,000,000 QDOGE staked earn
//     1 QTREAT token (max 4 per wallet, lifetime). Any epoch below the
//     threshold resets the consecutive counter to zero.
//     Bonus tokens are claimed via ClaimQtreatBonus (causer pays QX fee)
//     from a pool the admin loads with DepositQtreatTokens.
//   - Raffle: each epoch for the first 52 epochs, one random eligible staker
//     wins 1 QTREAT (entropy bought from the RANDOM contract, RandomLottery
//     pattern). Eligible = active stake and NOT marked for unstaking.
//     Wins are credited to pendingBonus and do NOT count against the
//     4-token 50M-bonus cap.
//   - Bonus/raffle QTREAT tokens are AUTO-DELIVERED at END_EPOCH: the
//     contract transfers them to the winner and releases management to QX
//     (delivery fee paid from the staking fund). ClaimQtreatBonus remains a
//     manual fallback (causer pays) for when delivery is blocked (e.g. the
//     bonus pool or staking fund is momentarily short).
//
// ASIC MINER POOL (second NFT collection: 400 parts, 100 each of
// motherboard / ASIC chip / power supply / cooling fan)
//   A wallet that collects a complete 4-part set has its ASIC registered by
//   the admin; the contract computes the set's weight from part rarities
//   (common 1 / uncommon 2 / rare 3 / epic 5 / legendary 8 points, so one
//   ASIC weighs 4..32). The mining fund (DepositMiningFund) pays
//   20M QU/epoch pro-rata by weight. Max 100 registered ASICs.
//
// Pattern lineage: GGWP (staking custody, epoch payout), qRWA (dual snapshot,
// POST_INCOMING_TRANSFER routing, general assets), QVAULT (asset treasury).
// STARTER SKELETON — fill in issuer IDs / asset names, then test against the
// qubic/core test harness before deploying.
// ============================================================================

// ---- Capacities (must be powers of two for HashMap) ----
constexpr uint64 QTREAT_MAX_HOLDERS      = 131072;  // 2^17 dividend recipients
constexpr uint64 QTREAT_MAX_STAKERS      = 65536;   // 2^16 QDOGE stakers
constexpr uint64 QTREAT_MAX_ASSETS       = 1024;    // dividend-paying assets held
constexpr uint64 QTREAT_MIN_ELIGIBLE_BAL = 1;       // dust filter for holder snapshot (tune)

// ---- Asset identities ----
// Both tokens issued by QDOGEEESKYPAICECHEAHOXPULEOADTKGEJHAVYPFKHLEWGXXZQUGIGMBUTZE
constexpr uint64 QTREAT_TOKEN_ASSETNAME  = 92639312630865ULL; // "QTREAT"
constexpr uint64 QTREAT_QDOGE_ASSETNAME   = 297549120593ULL;   // "QDOGE"

// ---- QDOGE staking parameters ----
constexpr uint64 QTREAT_MIN_STAKE               = 10000000ULL;   // 10M QDOGE minimum position
constexpr uint64 QTREAT_STAKER_REWARD_PER_EPOCH = 20000000ULL;   // 20M QU / epoch to stakers
constexpr uint64 QTREAT_PHASE_EPOCHS            = 52;            // one staking phase
constexpr uint64 QTREAT_NUM_PHASES              = 1;             // single 52-epoch program (extend only via contract upgrade)
constexpr uint64 QTREAT_TOTAL_REWARD_EPOCHS     = QTREAT_PHASE_EPOCHS * QTREAT_NUM_PHASES; // 52
constexpr uint64 QTREAT_UNSTAKE_DELAY_EPOCHS    = 2;

// ---- Progressive staking (rewards buying + staking MORE each epoch) ----
// A staker earns a growth-streak point in an epoch when BOTH hold:
//   (a) their staked amount grew by >= QTREAT_PROGRESSIVE_MIN_STEP, and
//   (b) their total observable QDOGE (staked + pending unstake + wallet
//       under QX) exceeds their personal all-time high-water mark.
// (b) defeats drip-feeding: moving tokens from wallet to stake never raises
// the total, only actually acquiring more QDOGE does. Each streak point adds
// QTREAT_PROGRESSIVE_BONUS_PERMILLE to the staker's reward weight, capped at
// QTREAT_PROGRESSIVE_MAX_STREAK points (1.5x). Any epoch without qualifying
// growth resets the streak to zero.
constexpr uint64 QTREAT_PROGRESSIVE_MIN_STEP       = 1000000ULL; // 1M QDOGE min growth per epoch
constexpr uint64 QTREAT_PROGRESSIVE_BONUS_PERMILLE = 25;         // +2.5% weight per streak epoch
constexpr uint64 QTREAT_PROGRESSIVE_MAX_STREAK     = 20;         // cap: +50% => 1.5x weight
// Streaks cannot begin until this many epochs after the staking program
// starts. The launch window locks in everyone's holdings baseline first
// (buys during the quiet period ratchet the high-water mark without
// awarding streaks), so there is no day-one streak advantage.
constexpr uint64 QTREAT_PROGRESSIVE_START_DELAY_EPOCHS = 4;

// ---- QTREAT bonus for large stakers ----
constexpr uint64 QTREAT_BONUS_THRESHOLD       = 50000000ULL; // >= 50M QDOGE staked
constexpr uint64 QTREAT_BONUS_INTERVAL_EPOCHS = 12;          // 1 QTREAT per 12 qualifying epochs
constexpr uint64 QTREAT_BONUS_MAX_PER_WALLET  = 4;           // lifetime cap

// ---- Raffle: one random staker wins 1 QTREAT each epoch, first 52 epochs ----
constexpr uint64 QTREAT_RAFFLE_EPOCHS         = 52;
constexpr uint16 QTREAT_RAFFLE_ENTROPY_BITS   = 256;
constexpr uint8  QTREAT_RAFFLE_COLLATERAL_TIER = 0;
constexpr uint64 QTREAT_RAFFLE_ENTROPY_FEE    = RANDOM_BITFEE * QTREAT_RAFFLE_ENTROPY_BITS; // 25,600 QU per draw

// ---- ASIC miner pool (self-serve via QBAY ownership proofs) ----
// Second NFT collection: 400 parts (100 each of motherboard / ASIC chip /
// power supply / cooling fan). The admin loads a one-time part CATALOG
// (QBAY NFT id -> category + rarity) which auto-locks once all 400 entries
// match the minted rarity distribution. After the lock, HOLDERS register
// their own rigs: the contract verifies possession of all four part NFTs
// directly against QBAY, so no admin attestation is ever needed. Each
// END_EPOCH re-verifies every active rig against QBAY and auto-unregisters
// rigs whose parts were sold, freeing the parts for the buyer.
constexpr uint64 QTREAT_ASIC_CATALOG_CAPACITY = 512;        // catalog map capacity (400 used)
constexpr uint64 QTREAT_ASIC_PARTS_TOTAL      = 400;        // catalog locks at this size
constexpr uint64 QTREAT_MAX_ASIC_RIGS         = 128;        // rig slots (max 100 complete sets)
constexpr uint64 QTREAT_MAX_TOTAL_ASICS       = 100;        // max complete sets (100 of each part)
// The per-epoch mining payout rate is ADMIN-SETTABLE (SetMiningRate),
// bounded by the hardcoded maximum below, so the admin can make one large
// deposit and control the drain rate (e.g. 20M/epoch for years, or 75M
// during a sale-funded boost) without weekly operations. The admin can
// never withdraw funds — a rate change only speeds up or slows down
// payouts to miners. Each epoch pays min(rate, miningFund).
constexpr uint64 QTREAT_MINING_REWARD_DEFAULT = 20000000ULL;   // 20M QU / epoch at launch
constexpr uint64 QTREAT_MINING_REWARD_MAX     = 100000000ULL;  // hard cap on the settable rate
static_assert(QTREAT_MINING_REWARD_DEFAULT <= QTREAT_MINING_REWARD_MAX);
// Rarity points per part: 0=common 1=uncommon 2=rare 3=epic 4=legendary
constexpr uint64 QTREAT_RARITY_POINTS_COMMON    = 1;
constexpr uint64 QTREAT_RARITY_POINTS_UNCOMMON  = 2;
constexpr uint64 QTREAT_RARITY_POINTS_RARE      = 3;
constexpr uint64 QTREAT_RARITY_POINTS_EPIC      = 5;
constexpr uint64 QTREAT_RARITY_POINTS_LEGENDARY = 8;
constexpr uint64 QTREAT_MAX_RARITY = 4;
// Minted supply per rarity within EACH 100-piece part category. Registration
// tracks per-category usage so the admin can never register more parts of a
// rarity than were minted.
// Favorable curve: 60% of mints are uncommon or better; 2 legendaries per
// category => 8 total, two possible all-legendary god rigs.
constexpr uint64 QTREAT_RARITY_SUPPLY_COMMON    = 40;
constexpr uint64 QTREAT_RARITY_SUPPLY_UNCOMMON  = 30;
constexpr uint64 QTREAT_RARITY_SUPPLY_RARE      = 18;
constexpr uint64 QTREAT_RARITY_SUPPLY_EPIC      = 10;
constexpr uint64 QTREAT_RARITY_SUPPLY_LEGENDARY = 2;
static_assert(QTREAT_RARITY_SUPPLY_COMMON + QTREAT_RARITY_SUPPLY_UNCOMMON + QTREAT_RARITY_SUPPLY_RARE
            + QTREAT_RARITY_SUPPLY_EPIC + QTREAT_RARITY_SUPPLY_LEGENDARY == 100);
static_assert(QTREAT_MAX_TOTAL_ASICS == 100); // 100 parts per category => exactly 100 complete sets

// ---- QDOGE drip for part-NFT holders (year one) ----
// Every cataloged part NFT earns weekly QDOGE for its current QBAY
// possessor for QTREAT_DRIP_EPOCHS epochs after the catalog locks (i.e.
// one year from mint completion), paid on-chain from a pool the admin
// funds with DepositDripQdoge. Rates are per part per epoch, by rarity.
// Unsold parts (possessed by the admin) earn nothing.
constexpr uint64 QTREAT_DRIP_EPOCHS = 52;
constexpr uint64 QTREAT_DRIP_QDOGE_COMMON    = 10000;
constexpr uint64 QTREAT_DRIP_QDOGE_UNCOMMON  = 20000;
constexpr uint64 QTREAT_DRIP_QDOGE_RARE      = 30000;
constexpr uint64 QTREAT_DRIP_QDOGE_EPIC      = 50000;
constexpr uint64 QTREAT_DRIP_QDOGE_LEGENDARY = 100000;

// ---- Shareholder dividend cut ----
// 5% of each epoch's dividend fund is paid to the contract's 676 IPO
// shares via qpi.distributeDividends (the protocol splits it per share
// natively). The remaining 95% goes to QTREAT token + NFT holders.
constexpr uint64 QTREAT_DIVIDEND_SHAREHOLDER_PERMILLE = 50;
static_assert(QTREAT_DIVIDEND_SHAREHOLDER_PERMILLE < 1000); // cut must leave holders a share

// ---- NFT holder dividends ----
// NFT ownership lives in the on-chain QBAY (QubicBay) contract, so the
// holder tally is FULLY AUTOMATIC: the 200 QBAY NFT ids of the dividend
// collection are hardcoded, and each END_EPOCH the contract queries QBAY
// for every id's current possessor and rebuilds the wallet -> count map.
// Sales take effect at the next epoch without any admin involvement.
constexpr uint64 QTREAT_MAX_NFT_HOLDERS = 1024;     // tally map capacity (2^N)
constexpr uint64 QTREAT_MAX_DIVIDEND_NFTS = 256;    // id list capacity (200 used)

// ---- Dividend snapshot exclusions (exchange/fundraising wallets) ----
constexpr uint64 QTREAT_MAX_EXCLUDE_ADDRESSES = 4;

// ---- QX ----
constexpr uint16 QTREAT_QX_CONTRACT_INDEX = 1;
constexpr sint64 QTREAT_QX_TRANSFER_FEE   = 100LL;

// ---- Return codes ----
constexpr uint32 QTREAT_OK = 0;
constexpr uint32 QTREAT_ERR_ACCESS_DENIED = 1;
constexpr uint32 QTREAT_ERR_ZERO_AMOUNT = 2;
constexpr uint32 QTREAT_ERR_INSUFFICIENT_STAKE = 3;
constexpr uint32 QTREAT_ERR_UNSTAKE_PENDING = 4;
constexpr uint32 QTREAT_ERR_UNSTAKE_NOT_READY = 5;
constexpr uint32 QTREAT_ERR_NOT_STAKER = 6;
constexpr uint32 QTREAT_ERR_BELOW_MIN_STAKE = 7;
constexpr uint32 QTREAT_ERR_ACQUIRE_FAILED = 8;
constexpr uint32 QTREAT_ERR_TRANSFER_FAILED = 9;
constexpr uint32 QTREAT_ERR_NO_PENDING_BONUS = 10;
constexpr uint32 QTREAT_ERR_INSUFFICIENT_FEE = 11;
constexpr uint32 QTREAT_ERR_INVALID_INPUT = 12;
constexpr uint32 QTREAT_ERR_PHASES_ENDED = 13;
constexpr uint32 QTREAT_ERR_BONUS_POOL_EMPTY = 14;
constexpr uint32 QTREAT_ERR_EXTERNAL_CALL = 15;

struct QTREAT2 {};

struct QTREAT : public ContractBase
{
    // One record per staker — packs stake, unstake request, and bonus tracking.
    struct StakerInfo
    {
        uint64 staked;         // active staked QDOGE
        uint64 unstakeAmount;  // pending unstake (0 = none)
        uint64 unstakeEpoch;   // epoch the unstake was requested
        uint64 bonusEpochs;    // epochs accumulated at >= 50M staked (mod 12 carry)
        uint64 hwmHoldings;    // all-time high of staked + pending unstake + wallet QDOGE
        uint64 lastStaked;     // staked amount at the previous END_EPOCH evaluation
        uint64 growthStreak;   // consecutive qualifying growth epochs (capped)
        uint64 bonusAwarded;   // lifetime QTREAT bonus tokens earned (cap 4)
        uint64 pendingBonus;   // earned but unclaimed QTREAT tokens
    };

    // Draw context hashed with RANDOM entropy (RandomLottery pattern).
    struct RaffleSeed
    {
        bit_4096 entropy;
        uint64 epoch;
        uint64 tick;
        uint64 eligibleCount;
    };

    // One registered rig: the owner and its four QBAY part NFT ids.
    struct AsicRig
    {
        id owner;
        uint32 partMotherboard;
        uint32 partChip;
        uint32 partPsu;
        uint32 partFan;
        uint64 weight;
        uint64 active; // 0 = free slot / unregistered
    };

    // Key for the general-asset balance map (qRWA pattern).
    struct AssetKey
    {
        id issuer;
        uint64 assetName;
        bool operator==(const AssetKey& o) const { return issuer == o.issuer && assetName == o.assetName; }
        bool operator!=(const AssetKey& o) const { return issuer != o.issuer || assetName != o.assetName; }
    };

    struct StateData
    {
        id adminAddress;

        Asset qtreatToken;   // dividend token (external, on QX)
        Asset qdogeToken;    // staking token (external, on QX)

        // ---- Fund 1: dividends for QTREAT holders ----
        uint64 dividendFund;
        uint64 totalDividendsDistributed;

        // ---- Fund 2: QU rewards for QDOGE stakers ----
        uint64 stakingFund;
        uint64 totalStakingRewardsDistributed;

        // ---- Holder snapshot (begin + end epoch, min() anti-gaming, qRWA) ----
        HashMap<id, uint64, QTREAT_MAX_HOLDERS> beginBalances;
        HashMap<id, uint64, QTREAT_MAX_HOLDERS> endBalances;
        uint64 totalHoldersSnapshot;

        // ---- QDOGE staking ----
        HashMap<id, StakerInfo, QTREAT_MAX_STAKERS> stakers;
        uint64 totalStaked;
        uint64 stakingStartEpoch;    // set on first BEGIN_EPOCH; the program runs 52 epochs from here

        // ---- QTREAT bonus token pool (held by SC, loaded by admin) ----
        uint64 qtreatBonusPool;

        // Addresses excluded from the holder dividend snapshot (admin-set;
        // e.g. exchange hot wallets). NULL_ID = empty slot.
        Array<id, QTREAT_MAX_EXCLUDE_ADDRESSES> excludeAddresses;

        // ---- NFT holder tally (rebuilt from QBAY every epoch) ----
        Array<uint32, QTREAT_MAX_DIVIDEND_NFTS> dividendNftIds; // QBAY NFT ids of the dividend collection
        uint64 dividendNftIdCount;
        HashMap<id, uint64, QTREAT_MAX_NFT_HOLDERS> nftCounts;  // possessor -> count (current epoch)
        uint64 totalNftCount;

        uint64 totalShareholderDividends; // cumulative QU paid to the 676 IPO shares

        // ---- ASIC miner pool ----
        HashMap<uint64, uint64, QTREAT_ASIC_CATALOG_CAPACITY> asicCatalog;  // QBAY NFT id -> category*8 + rarity
        Array<uint64, 32> asicCatalogBuckets; // [category*8 + rarity] loaded parts (must match mint supply to lock)
        uint64 asicCatalogSize;
        uint64 asicCatalogLocked;             // 1 once all 400 parts are loaded; registration opens, loading closes
        HashMap<uint64, uint64, QTREAT_ASIC_CATALOG_CAPACITY> asicUsedParts; // QBAY NFT id -> rig slot + 1
        Array<AsicRig, QTREAT_MAX_ASIC_RIGS> asicRigs;
        uint64 asicRigHighWater;              // highest slot ever used + 1
        uint64 totalAsicCount;                // active rigs
        uint64 totalMiningWeight;
        uint64 miningFund;
        uint64 miningRewardRate;              // admin-settable, <= QTREAT_MINING_REWARD_MAX
        uint64 totalMiningRewardsDistributed;

        // ---- QDOGE drip ----
        uint64 dripQdogePool;                 // QDOGE tokens held for the drip
        uint64 dripStartEpoch;                // set when the catalog locks; drip runs 52 epochs
        uint64 totalDripQdogeDistributed;
        HashMap<id, uint64, QTREAT_ASIC_CATALOG_CAPACITY> dripTally; // per-epoch scratch: possessor -> QDOGE due

        // ---- Bonus delivery tracking ----
        uint64 totalBonusDelivered; // QTREAT tokens auto-pushed to winners

        // ---- Raffle tracking ----
        uint64 totalRaffleAwarded;
        id lastRaffleWinner;
        uint64 lastRaffleEpoch;

        // ---- Dividend-paying assets held by the contract (qRWA/QVAULT) ----
        HashMap<AssetKey, uint64, QTREAT_MAX_ASSETS> generalAssetBalances;
        HashMap<id, uint64, QTREAT_MAX_ASSETS> scDividendTracker; // source SC -> cumulative dividends received
    };

    // ======================== I/O structs ========================
    struct DepositDividends_input {}; struct DepositDividends_output { uint32 returnCode; };
    struct DepositStakingFund_input {}; struct DepositStakingFund_output { uint32 returnCode; };

    struct RequestUnstake_input { uint64 amount; }; struct RequestUnstake_output { uint32 returnCode; };
    struct RequestUnstake_locals { StakerInfo info; };
    struct FinalizeUnstake_input {}; struct FinalizeUnstake_output { uint32 returnCode; };
    struct FinalizeUnstake_locals { StakerInfo info; sint64 rel; };

    struct ClaimQtreatBonus_input {}; struct ClaimQtreatBonus_output { uint32 returnCode; uint64 claimed; };
    struct ClaimQtreatBonus_locals { StakerInfo info; sint64 xfer; sint64 rel; };

    struct DepositQtreatTokens_input { uint64 amount; }; struct DepositQtreatTokens_output { uint32 returnCode; };
    struct DepositQtreatTokens_locals { sint64 xfer; };

    struct DepositGeneralAsset_input { Asset asset; uint64 amount; };
    struct DepositGeneralAsset_output { uint32 returnCode; };
    struct DepositGeneralAsset_locals { sint64 managed; sint64 xfer; AssetKey key; uint64 bal; };

    struct RevokeGeneralAsset_input { Asset asset; uint64 amount; };
    struct RevokeGeneralAsset_output { uint32 returnCode; };
    struct RevokeGeneralAsset_locals { AssetKey key; uint64 bal; sint64 xfer; sint64 rel; };

    // Self-serve unfreeze: release the caller's own managed shares of any
    // non-QDOGE asset back to QX (qRWA/MSVAULT pattern).
    struct ReleaseManagedShares_input { Asset asset; uint64 amount; };
    struct ReleaseManagedShares_output { uint32 returnCode; };
    struct ReleaseManagedShares_locals { sint64 managed; sint64 rel; StakerInfo info; uint64 accounted; };

    struct SetExcludeAddress_input { uint64 slot; id address; };
    struct SetExcludeAddress_output { uint32 returnCode; };

    struct DepositMiningFund_input {}; struct DepositMiningFund_output { uint32 returnCode; };

    // Admin: set the per-epoch mining payout rate (bounded; 0 pauses).
    // Cannot move funds anywhere except to miners, faster or slower.

    // Admin one-time catalog load: QBAY NFT id -> part category + rarity.
    // Auto-locks when all 400 entries are loaded (each rarity bucket must
    // exactly match the minted distribution to reach 400).
    struct SetMiningRate_input { uint64 ratePerEpoch; };
    struct SetMiningRate_output { uint32 returnCode; };

    struct DepositDripQdoge_input { uint64 amount; };
    struct DepositDripQdoge_output { uint32 returnCode; };
    struct DepositDripQdoge_locals { sint64 xfer; };

    struct LoadAsicPart_input { uint32 nftId; uint8 category; uint8 rarity; };
    struct LoadAsicPart_output { uint32 returnCode; uint64 catalogSize; uint64 locked; };
    struct LoadAsicPart_locals { uint64 budget; uint64 dummy; uint64 partPoints; };

    // HOLDER self-registration: caller proves possession of all 4 part NFTs
    // via QBAY; the contract computes the weight from the catalog.
    struct RegisterAsic_input { uint32 partMotherboard; uint32 partChip; uint32 partPsu; uint32 partFan; };
    struct RegisterAsic_output { uint32 returnCode; uint64 rigIndex; uint64 asicWeight; };
    struct RegisterAsic_locals
    {
        AsicRig rig; uint64 points; uint64 partPoints; uint64 packed; uint64 r; sint64 i; uint32 partId; uint64 dummy;
        QBAY::getInfoOfNFTById_input qbayIn;
        QBAY::getInfoOfNFTById_output qbayOut;
        sint64 freeSlot;
    };

    // Rig owner voluntarily unregisters (e.g. before selling a part).
    struct UnregisterAsic_input { uint64 rigIndex; };
    struct UnregisterAsic_output { uint32 returnCode; };
    struct UnregisterAsic_locals { AsicRig rig; };

    struct GetMinerInfo_input { id wallet; };
    struct GetMinerInfo_output { uint64 asicCount; uint64 weight; uint64 totalAsicCount; uint64 totalMiningWeight; uint64 miningFund; };
    struct GetMinerInfo_locals { AsicRig rig; sint64 i; };

    struct GetAsicCatalogInfo_input { uint32 nftId; };
    struct GetAsicCatalogInfo_output { uint64 catalogSize; uint64 locked; uint64 isCataloged; uint64 category; uint64 rarity; uint64 usedByRigPlusOne; };
    struct GetAsicCatalogInfo_locals { uint64 packed; uint64 used; };

    struct GetNftInfo_input { id wallet; };
    struct GetNftInfo_output { uint64 nftCount; uint64 totalNftCount; };
    struct GetNftInfo_locals { uint64 val; };

    struct GetExcludeAddresses_input {};
    struct GetExcludeAddresses_output { Array<id, QTREAT_MAX_EXCLUDE_ADDRESSES> addresses; };
    struct GetExcludeAddresses_locals { sint64 i; };

    struct GetStakingInfo_input { id staker; };
    struct GetStakingInfo_output
    {
        uint64 staked; uint64 unstakeAmount; uint64 unstakeEpoch;
        uint64 bonusEpochs; uint64 bonusAwarded; uint64 pendingBonus;
        uint64 growthStreak; uint64 hwmHoldings;
        uint64 totalStaked; uint64 stakingFund; uint32 isStaker;
    };
    struct GetStakingInfo_locals { StakerInfo info; };

    struct GetPhaseInfo_input {};
    struct GetPhaseInfo_output
    {
        uint64 stakingStartEpoch;  // 0 = phases not started yet
        uint64 currentPhase;       // 1 while active, 0 = not started or ended
        uint64 epochsRemaining;    // reward epochs left across all phases
    };
    struct GetPhaseInfo_locals { uint64 elapsed; };

    struct GetRaffleInfo_input {};
    struct GetRaffleInfo_output
    {
        uint64 totalRaffleAwarded;
        id lastRaffleWinner;
        uint64 lastRaffleEpoch;
        uint64 raffleEpochsRemaining;
    };
    struct GetRaffleInfo_locals { uint64 elapsed; };

    struct GetFunds_input {};
    struct GetFunds_output
    {
        uint64 dividendFund; uint64 stakingFund; uint64 qtreatBonusPool;
        uint64 totalDividendsDistributed; uint64 totalStakingRewardsDistributed;
        uint64 totalBonusDelivered;
        uint64 totalShareholderDividends;
        uint64 miningFund;
        uint64 miningRewardRate;
        uint64 totalMiningRewardsDistributed;
        uint64 dripQdogePool;
        uint64 dripStartEpoch;
        uint64 totalDripQdogeDistributed;
    };

    // ======================== Read-only functions ========================

    PUBLIC_FUNCTION_WITH_LOCALS(GetStakingInfo)
    {
        output.isStaker = state.get().stakers.get(input.staker, locals.info) ? 1 : 0;
        if (output.isStaker)
        {
            output.staked = locals.info.staked;
            output.unstakeAmount = locals.info.unstakeAmount;
            output.unstakeEpoch = locals.info.unstakeEpoch;
            output.bonusEpochs = locals.info.bonusEpochs;
            output.bonusAwarded = locals.info.bonusAwarded;
            output.pendingBonus = locals.info.pendingBonus;
            output.growthStreak = locals.info.growthStreak;
            output.hwmHoldings = locals.info.hwmHoldings;
        }
        output.totalStaked = state.get().totalStaked;
        output.stakingFund = state.get().stakingFund;
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetPhaseInfo)
    {
        output.stakingStartEpoch = state.get().stakingStartEpoch;
        output.currentPhase = 0;
        output.epochsRemaining = 0;
        if (state.get().stakingStartEpoch == 0) return;
        locals.elapsed = qpi.epoch() - state.get().stakingStartEpoch;
        if (locals.elapsed < QTREAT_TOTAL_REWARD_EPOCHS)
        {
            output.currentPhase = div(locals.elapsed, QTREAT_PHASE_EPOCHS) + 1; // always 1 (single 52-epoch phase)
            output.epochsRemaining = QTREAT_TOTAL_REWARD_EPOCHS - locals.elapsed;
        }
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetRaffleInfo)
    {
        output.totalRaffleAwarded = state.get().totalRaffleAwarded;
        output.lastRaffleWinner = state.get().lastRaffleWinner;
        output.lastRaffleEpoch = state.get().lastRaffleEpoch;
        output.raffleEpochsRemaining = 0;
        if (state.get().stakingStartEpoch != 0)
        {
            locals.elapsed = qpi.epoch() - state.get().stakingStartEpoch;
            if (locals.elapsed < QTREAT_RAFFLE_EPOCHS)
            {
                output.raffleEpochsRemaining = QTREAT_RAFFLE_EPOCHS - locals.elapsed;
            }
        }
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetMinerInfo)
    {
        // Aggregate the wallet's active rigs from the rig table.
        for (locals.i = 0; locals.i < (sint64)state.get().asicRigHighWater; locals.i++)
        {
            locals.rig = state.get().asicRigs.get(locals.i);
            if (locals.rig.active == 0 || locals.rig.owner != input.wallet) continue;
            output.asicCount = output.asicCount + 1;
            output.weight = output.weight + locals.rig.weight;
        }
        output.totalAsicCount = state.get().totalAsicCount;
        output.totalMiningWeight = state.get().totalMiningWeight;
        output.miningFund = state.get().miningFund;
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetAsicCatalogInfo)
    {
        output.catalogSize = state.get().asicCatalogSize;
        output.locked = state.get().asicCatalogLocked;
        locals.packed = 0;
        output.isCataloged = state.get().asicCatalog.get((uint64)input.nftId, locals.packed) ? 1 : 0;
        if (output.isCataloged)
        {
            output.category = div(locals.packed, 8ULL);
            output.rarity = mod(locals.packed, 8ULL);
        }
        locals.used = 0;
        state.get().asicUsedParts.get((uint64)input.nftId, locals.used);
        output.usedByRigPlusOne = locals.used;
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetNftInfo)
    {
        locals.val = 0;
        state.get().nftCounts.get(input.wallet, locals.val);
        output.nftCount = locals.val;
        output.totalNftCount = state.get().totalNftCount;
    }

    PUBLIC_FUNCTION_WITH_LOCALS(GetExcludeAddresses)
    {
        for (locals.i = 0; locals.i < (sint64)QTREAT_MAX_EXCLUDE_ADDRESSES; locals.i++)
        {
            output.addresses.set(locals.i, state.get().excludeAddresses.get(locals.i));
        }
    }

    PUBLIC_FUNCTION(GetFunds)
    {
        output.dividendFund = state.get().dividendFund;
        output.stakingFund = state.get().stakingFund;
        output.qtreatBonusPool = state.get().qtreatBonusPool;
        output.totalDividendsDistributed = state.get().totalDividendsDistributed;
        output.totalStakingRewardsDistributed = state.get().totalStakingRewardsDistributed;
        output.totalBonusDelivered = state.get().totalBonusDelivered;
        output.totalShareholderDividends = state.get().totalShareholderDividends;
        output.miningFund = state.get().miningFund;
        output.miningRewardRate = state.get().miningRewardRate;
        output.totalMiningRewardsDistributed = state.get().totalMiningRewardsDistributed;
        output.dripQdogePool = state.get().dripQdogePool;
        output.dripStartEpoch = state.get().dripStartEpoch;
        output.totalDripQdogeDistributed = state.get().totalDripQdogeDistributed;
    }

    // ======================== Fund intake ========================

    // Explicit deposit into the QTREAT-holder dividend fund.
    PUBLIC_PROCEDURE(DepositDividends)
    {
        if (qpi.invocationReward() == 0) { output.returnCode = QTREAT_ERR_ZERO_AMOUNT; return; }
        state.mut().dividendFund = sadd(state.get().dividendFund, (uint64)qpi.invocationReward());
        output.returnCode = QTREAT_OK;
    }

    // Secondary fund: QU attached to this call goes to QDOGE stakers.
    PUBLIC_PROCEDURE(DepositStakingFund)
    {
        if (qpi.invocationReward() == 0) { output.returnCode = QTREAT_ERR_ZERO_AMOUNT; return; }
        state.mut().stakingFund = sadd(state.get().stakingFund, (uint64)qpi.invocationReward());
        output.returnCode = QTREAT_OK;
    }

    // ======================== QDOGE staking ========================
    // Staking happens entirely through QX.TransferShareManagementRights —
    // see PRE_ACQUIRE_SHARES / POST_ACQUIRE_SHARES at the bottom of the file.
    // Unstaking: RequestUnstake -> 2-epoch delay -> FinalizeUnstake.

    PUBLIC_PROCEDURE_WITH_LOCALS(RequestUnstake)
    {
        // The QX release fee (100 QU) is collected here, once. After the
        // 2-epoch delay the contract releases the shares automatically in
        // END_EPOCH — no second transaction needed. (FinalizeUnstake remains
        // as a free manual fallback.) All error paths refund in full.
        if (!state.get().stakers.get(qpi.invocator(), locals.info) || locals.info.staked == 0)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_NOT_STAKER; return;
        }
        if (input.amount == 0 || input.amount > locals.info.staked)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INSUFFICIENT_STAKE; return;
        }
        if (locals.info.unstakeAmount > 0)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_UNSTAKE_PENDING; return;
        }
        // Full exit or keep >= minimum — no dust positions.
        if (input.amount < locals.info.staked && locals.info.staked - input.amount < QTREAT_MIN_STAKE)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_BELOW_MIN_STAKE; return;
        }
        if (qpi.invocationReward() < QTREAT_QX_TRANSFER_FEE)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INSUFFICIENT_FEE; return;
        }
        if (qpi.invocationReward() > QTREAT_QX_TRANSFER_FEE)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - QTREAT_QX_TRANSFER_FEE);
        }
        // The retained 100 QU stays in the contract balance and is consumed
        // by qpi.releaseShares at auto-release time.

        locals.info.staked -= input.amount;
        locals.info.unstakeAmount = input.amount;
        locals.info.unstakeEpoch = qpi.epoch();
        // Full exit: reset the consecutive-epoch counters now. The END_EPOCH
        // pass is skipped when this is the last staker (totalStaked==0), so a
        // retained record could otherwise carry a stale streak into a restake.
        if (locals.info.staked == 0)
        {
            locals.info.bonusEpochs = 0;
            locals.info.growthStreak = 0;
            locals.info.lastStaked = 0;
        }
        state.mut().stakers.set(qpi.invocator(), locals.info);
        state.mut().totalStaked = state.get().totalStaked - input.amount;
        output.returnCode = QTREAT_OK;
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(FinalizeUnstake)
    {
        // Manual fallback: unstakes are normally auto-released in END_EPOCH.
        // The QX release fee was prepaid in RequestUnstake, so this is free —
        // any attached QU is refunded in full on every path.
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (!state.get().stakers.get(qpi.invocator(), locals.info) || locals.info.unstakeAmount == 0)
        {
            output.returnCode = QTREAT_ERR_NOT_STAKER; return;
        }
        if (qpi.epoch() < locals.info.unstakeEpoch + QTREAT_UNSTAKE_DELAY_EPOCHS)
        {
            output.returnCode = QTREAT_ERR_UNSTAKE_NOT_READY; return;
        }

        locals.rel = qpi.releaseShares(state.get().qdogeToken, qpi.invocator(), qpi.invocator(),
            (sint64)locals.info.unstakeAmount, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
            QTREAT_QX_TRANSFER_FEE);
        if (locals.rel < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }

        locals.info.unstakeAmount = 0;
        locals.info.unstakeEpoch = 0;
        if (locals.info.staked == 0 && locals.info.pendingBonus == 0
            && locals.info.bonusAwarded == 0)
        {
            state.mut().stakers.removeByKey(qpi.invocator());
        }
        else
        {
            // Keep the record: bonusAwarded enforces the lifetime 4-token cap.
            state.mut().stakers.set(qpi.invocator(), locals.info);
        }
        output.returnCode = QTREAT_OK;
    }

    // ======================== QTREAT bonus ========================

    // Admin loads the bonus pool with QTREAT tokens (must be under SC management first).
    PUBLIC_PROCEDURE_WITH_LOCALS(DepositQtreatTokens)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (input.amount == 0) { output.returnCode = QTREAT_ERR_ZERO_AMOUNT; return; }
        locals.xfer = qpi.transferShareOwnershipAndPossession(
            state.get().qtreatToken.assetName, state.get().qtreatToken.issuer,
            qpi.invocator(), qpi.invocator(), (sint64)input.amount, SELF);
        if (locals.xfer < 0) { output.returnCode = QTREAT_ERR_ACQUIRE_FAILED; return; }
        state.mut().qtreatBonusPool = sadd(state.get().qtreatBonusPool, input.amount);
        output.returnCode = QTREAT_OK;
    }

    // Claim earned QTREAT bonus tokens (1 per 12 qualifying epochs, max 4 lifetime).
    PUBLIC_PROCEDURE_WITH_LOCALS(ClaimQtreatBonus)
    {
        // Refund attached QU on every early error return (the framework does
        // not auto-refund invocationReward).
        if (!state.get().stakers.get(qpi.invocator(), locals.info) || locals.info.pendingBonus == 0)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_NO_PENDING_BONUS; return;
        }
        if (state.get().qtreatBonusPool < locals.info.pendingBonus)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_BONUS_POOL_EMPTY; return;
        }

        // Causer pays the QX release fee.
        if (qpi.invocationReward() < QTREAT_QX_TRANSFER_FEE)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INSUFFICIENT_FEE;
            return;
        }
        if (qpi.invocationReward() > QTREAT_QX_TRANSFER_FEE)
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - QTREAT_QX_TRANSFER_FEE);

        // Two-step (GGWP ClaimStakingRewards): SC -> claimer, then hand
        // management back to QX so the tokens are freely tradable.
        locals.xfer = qpi.transferShareOwnershipAndPossession(
            state.get().qtreatToken.assetName, state.get().qtreatToken.issuer,
            SELF, SELF, (sint64)locals.info.pendingBonus, qpi.invocator());
        if (locals.xfer < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }

        // Ownership transferred — commit accounting BEFORE the release (as the
        // auto-delivery does). On release failure the claimer still owns the
        // tokens (self-releasable); a retry can't double-pay since SELF no
        // longer holds them.
        state.mut().qtreatBonusPool = state.get().qtreatBonusPool - locals.info.pendingBonus;
        output.claimed = locals.info.pendingBonus;
        locals.info.pendingBonus = 0;
        state.mut().stakers.set(qpi.invocator(), locals.info);

        locals.rel = qpi.releaseShares(state.get().qtreatToken, qpi.invocator(), qpi.invocator(),
            (sint64)output.claimed, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
            QTREAT_QX_TRANSFER_FEE);
        // Release failure is non-fatal: the bonus is delivered and self-releasable.
        output.returnCode = QTREAT_OK;
    }

    // ======================== Dividend-paying assets (admin) ========================

    // Move dividend-paying SC shares/assets into the contract (qRWA pattern).
    // Admin must first grant this SC management rights over the shares.
    PUBLIC_PROCEDURE_WITH_LOCALS(DepositGeneralAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (input.amount == 0 || input.asset.issuer == NULL_ID) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }

        locals.managed = qpi.numberOfShares(input.asset,
            { qpi.invocator(), SELF_INDEX }, { qpi.invocator(), SELF_INDEX });
        if (locals.managed < (sint64)input.amount) { output.returnCode = QTREAT_ERR_ACQUIRE_FAILED; return; }

        // Ensure the accounting slot exists BEFORE the irreversible transfer:
        // a new key must fit, else custody moves with no recorded balance.
        locals.key.issuer = input.asset.issuer;
        locals.key.assetName = input.asset.assetName;
        if (!state.get().generalAssetBalances.contains(locals.key)
            && state.get().generalAssetBalances.population() >= QTREAT_MAX_ASSETS)
        {
            output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
        }

        locals.xfer = qpi.transferShareOwnershipAndPossession(
            input.asset.assetName, input.asset.issuer,
            qpi.invocator(), qpi.invocator(), (sint64)input.amount, SELF);
        if (locals.xfer < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }

        locals.bal = 0;
        state.get().generalAssetBalances.get(locals.key, locals.bal);
        state.mut().generalAssetBalances.set(locals.key, sadd(locals.bal, input.amount));
        output.returnCode = QTREAT_OK;
    }

    // Anyone: release YOUR OWN managed shares back to QX. Non-QDOGE: the whole
    // managed balance. QDOGE: only the surplus above staked + pending unstake,
    // so stranded drip QDOGE is recoverable but the stake stays locked.
    // Causer pays the QX release fee.
    PUBLIC_PROCEDURE_WITH_LOCALS(ReleaseManagedShares)
    {
        locals.managed = qpi.numberOfShares(input.asset,
            { qpi.invocator(), SELF_INDEX }, { qpi.invocator(), SELF_INDEX });
        if (input.amount == 0 || locals.managed < (sint64)input.amount)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
        }
        if (input.asset.assetName == state.get().qdogeToken.assetName
            && input.asset.issuer == state.get().qdogeToken.issuer)
        {
            // Protect the stake: only managed - staked - pending is releasable.
            locals.info.staked = 0; locals.info.unstakeAmount = 0;
            state.get().stakers.get(qpi.invocator(), locals.info);
            locals.accounted = sadd(locals.info.staked, locals.info.unstakeAmount);
            if ((sint64)locals.accounted + (sint64)input.amount > locals.managed)
            {
                if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
            }
        }
        if (qpi.invocationReward() < QTREAT_QX_TRANSFER_FEE)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INSUFFICIENT_FEE; return;
        }
        if (qpi.invocationReward() > QTREAT_QX_TRANSFER_FEE)
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - QTREAT_QX_TRANSFER_FEE);

        locals.rel = qpi.releaseShares(input.asset, qpi.invocator(), qpi.invocator(),
            (sint64)input.amount, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
            QTREAT_QX_TRANSFER_FEE);
        if (locals.rel < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }
        output.returnCode = QTREAT_OK;
    }

    // Mining rewards intake: QU attached goes to the ASIC miner pool.
    PUBLIC_PROCEDURE(DepositMiningFund)
    {
        if (qpi.invocationReward() == 0) { output.returnCode = QTREAT_ERR_ZERO_AMOUNT; return; }
        state.mut().miningFund = sadd(state.get().miningFund, (uint64)qpi.invocationReward());
        output.returnCode = QTREAT_OK;
    }

    // Admin, one-time: load one part into the catalog (QBAY NFT id ->
    // category + rarity). Each (category, rarity) bucket is capped at the
    // minted supply, so the catalog can only reach 400 entries by exactly
    // matching the announced distribution — at which point it locks forever
    // and holder self-registration opens.
    PUBLIC_PROCEDURE_WITH_LOCALS(LoadAsicPart)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (state.get().asicCatalogLocked != 0) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        if (input.category >= 4 || input.rarity > QTREAT_MAX_RARITY) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        if (state.get().asicCatalog.contains((uint64)input.nftId)) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }

        locals.budget = QTREAT_RARITY_SUPPLY_COMMON;
        if (input.rarity == 1) locals.budget = QTREAT_RARITY_SUPPLY_UNCOMMON;
        if (input.rarity == 2) locals.budget = QTREAT_RARITY_SUPPLY_RARE;
        if (input.rarity == 3) locals.budget = QTREAT_RARITY_SUPPLY_EPIC;
        if (input.rarity == 4) locals.budget = QTREAT_RARITY_SUPPLY_LEGENDARY;
        if (state.get().asicCatalogBuckets.get((sint64)input.category * 8 + (sint64)input.rarity) >= locals.budget)
        {
            output.returnCode = QTREAT_ERR_INVALID_INPUT; return; // bucket already full per mint distribution
        }

        if (state.mut().asicCatalog.set((uint64)input.nftId, (uint64)input.category * 8 + (uint64)input.rarity) == NULL_INDEX)
        {
            output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
        }
        state.mut().asicCatalogBuckets.set((sint64)input.category * 8 + (sint64)input.rarity,
            state.get().asicCatalogBuckets.get((sint64)input.category * 8 + (sint64)input.rarity) + 1);
        state.mut().asicCatalogSize = state.get().asicCatalogSize + 1;
        if (state.get().asicCatalogSize == QTREAT_ASIC_PARTS_TOTAL)
        {
            state.mut().asicCatalogLocked = 1;
            // Mint is complete: the one-year QDOGE drip clock starts now.
            state.mut().dripStartEpoch = qpi.epoch();
        }
        output.catalogSize = state.get().asicCatalogSize;
        output.locked = state.get().asicCatalogLocked;
        output.returnCode = QTREAT_OK;
    }

    // HOLDER self-registration: the caller presents their four part NFT ids.
    // The contract checks the catalog (slot categories + rarities), verifies
    // the caller possesses every part directly against QBAY on-chain state,
    // and rejects parts already used by another rig. No admin involved.
    PUBLIC_PROCEDURE_WITH_LOCALS(RegisterAsic)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (state.get().asicCatalogLocked == 0) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        if (state.get().totalAsicCount >= QTREAT_MAX_TOTAL_ASICS) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }

        locals.points = 0;
        for (locals.i = 0; locals.i < 4; locals.i++)
        {
            if (locals.i == 0) locals.partId = input.partMotherboard;
            if (locals.i == 1) locals.partId = input.partChip;
            if (locals.i == 2) locals.partId = input.partPsu;
            if (locals.i == 3) locals.partId = input.partFan;

            // Catalog: the id must exist and belong to this slot's category.
            locals.packed = 0;
            if (!state.get().asicCatalog.get((uint64)locals.partId, locals.packed)
                || div(locals.packed, 8ULL) != (uint64)locals.i)
            {
                output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
            }
            // Not already part of a registered rig.
            locals.dummy = 0;
            if (state.get().asicUsedParts.get((uint64)locals.partId, locals.dummy))
            {
                output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
            }
            // Ownership proof: QBAY's on-chain possessor must be the caller.
            locals.qbayIn.NFTId = locals.partId;
            CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
            // A failed QBAY call leaves qbayOut stale; never accept it as proof.
            if (interContractCallError != NoCallError)
            {
                output.returnCode = QTREAT_ERR_EXTERNAL_CALL; return;
            }
            if (locals.qbayOut.possessor != qpi.invocator())
            {
                output.returnCode = QTREAT_ERR_ACCESS_DENIED; return;
            }

            locals.r = mod(locals.packed, 8ULL);
            locals.partPoints = QTREAT_RARITY_POINTS_COMMON;
            if (locals.r == 1) locals.partPoints = QTREAT_RARITY_POINTS_UNCOMMON;
            if (locals.r == 2) locals.partPoints = QTREAT_RARITY_POINTS_RARE;
            if (locals.r == 3) locals.partPoints = QTREAT_RARITY_POINTS_EPIC;
            if (locals.r == 4) locals.partPoints = QTREAT_RARITY_POINTS_LEGENDARY;
            locals.points += locals.partPoints;
        }

        // Find a rig slot: reuse an inactive one, else extend the high-water.
        locals.freeSlot = -1;
        for (locals.i = 0; locals.i < (sint64)state.get().asicRigHighWater; locals.i++)
        {
            if (state.get().asicRigs.get(locals.i).active == 0) { locals.freeSlot = locals.i; break; }
        }
        if (locals.freeSlot < 0)
        {
            if (state.get().asicRigHighWater >= QTREAT_MAX_ASIC_RIGS)
            {
                output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
            }
            locals.freeSlot = (sint64)state.get().asicRigHighWater;
            state.mut().asicRigHighWater = state.get().asicRigHighWater + 1;
        }

        locals.rig.owner = qpi.invocator();
        locals.rig.partMotherboard = input.partMotherboard;
        locals.rig.partChip = input.partChip;
        locals.rig.partPsu = input.partPsu;
        locals.rig.partFan = input.partFan;
        locals.rig.weight = locals.points;
        locals.rig.active = 1;
        state.mut().asicRigs.set(locals.freeSlot, locals.rig);
        state.mut().asicUsedParts.set((uint64)input.partMotherboard, (uint64)locals.freeSlot + 1);
        state.mut().asicUsedParts.set((uint64)input.partChip, (uint64)locals.freeSlot + 1);
        state.mut().asicUsedParts.set((uint64)input.partPsu, (uint64)locals.freeSlot + 1);
        state.mut().asicUsedParts.set((uint64)input.partFan, (uint64)locals.freeSlot + 1);
        state.mut().totalAsicCount = state.get().totalAsicCount + 1;
        state.mut().totalMiningWeight = sadd(state.get().totalMiningWeight, locals.points);

        output.rigIndex = (uint64)locals.freeSlot;
        output.asicWeight = locals.points;
        output.returnCode = QTREAT_OK;
    }

    // Rig owner voluntarily unregisters their rig (e.g. before selling a
    // part). Sales without this call are caught automatically by the
    // END_EPOCH re-verification against QBAY.
    PUBLIC_PROCEDURE_WITH_LOCALS(UnregisterAsic)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.rigIndex >= state.get().asicRigHighWater) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        locals.rig = state.get().asicRigs.get((sint64)input.rigIndex);
        if (locals.rig.active == 0 || locals.rig.owner != qpi.invocator())
        {
            output.returnCode = QTREAT_ERR_ACCESS_DENIED; return;
        }
        state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partMotherboard);
        state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partChip);
        state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partPsu);
        state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partFan);
        state.mut().totalAsicCount = state.get().totalAsicCount - 1;
        state.mut().totalMiningWeight = state.get().totalMiningWeight - locals.rig.weight;
        locals.rig.active = 0;
        state.mut().asicRigs.set((sint64)input.rigIndex, locals.rig);
        output.returnCode = QTREAT_OK;
    }

    PUBLIC_PROCEDURE(SetMiningRate)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (input.ratePerEpoch > QTREAT_MINING_REWARD_MAX) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        state.mut().miningRewardRate = input.ratePerEpoch;
        output.returnCode = QTREAT_OK;
    }

    // Admin: fund the QDOGE drip pool. QDOGE must first be staged under the
    // contract's management via QX.TransferShareManagementRights (the
    // admin's QDOGE transfers are exempt from stake handling).
    PUBLIC_PROCEDURE_WITH_LOCALS(DepositDripQdoge)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (input.amount == 0) { output.returnCode = QTREAT_ERR_ZERO_AMOUNT; return; }
        locals.xfer = qpi.transferShareOwnershipAndPossession(
            state.get().qdogeToken.assetName, state.get().qdogeToken.issuer,
            qpi.invocator(), qpi.invocator(), (sint64)input.amount, SELF);
        if (locals.xfer < 0) { output.returnCode = QTREAT_ERR_ACQUIRE_FAILED; return; }
        state.mut().dripQdogePool = sadd(state.get().dripQdogePool, input.amount);
        output.returnCode = QTREAT_OK;
    }


    // Admin: set/clear an address excluded from the holder dividend snapshot
    // (e.g. an exchange hot wallet). NULL_ID clears the slot. Takes effect at
    // the next BEGIN_EPOCH snapshot.
    PUBLIC_PROCEDURE(SetExcludeAddress)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().adminAddress) { output.returnCode = QTREAT_ERR_ACCESS_DENIED; return; }
        if (input.slot >= QTREAT_MAX_EXCLUDE_ADDRESSES) { output.returnCode = QTREAT_ERR_INVALID_INPUT; return; }
        state.mut().excludeAddresses.set((sint64)input.slot, input.address);
        output.returnCode = QTREAT_OK;
    }

    // Admin: withdraw a previously deposited general asset. Two-step: transfer
    // ownership from the contract back to the admin, then release management
    // to QX so the shares are tradable again. Bounded by the deposit
    // accounting, so it can never touch the QTREAT bonus pool or user stakes.
    // Causer pays the QX release fee.
    PUBLIC_PROCEDURE_WITH_LOCALS(RevokeGeneralAsset)
    {
        if (qpi.invocator() != state.get().adminAddress)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_ACCESS_DENIED; return;
        }
        locals.key.issuer = input.asset.issuer;
        locals.key.assetName = input.asset.assetName;
        locals.bal = 0;
        state.get().generalAssetBalances.get(locals.key, locals.bal);
        if (input.amount == 0 || input.amount > locals.bal)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INVALID_INPUT; return;
        }
        if (qpi.invocationReward() < QTREAT_QX_TRANSFER_FEE)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QTREAT_ERR_INSUFFICIENT_FEE; return;
        }
        if (qpi.invocationReward() > QTREAT_QX_TRANSFER_FEE)
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - QTREAT_QX_TRANSFER_FEE);

        locals.xfer = qpi.transferShareOwnershipAndPossession(
            input.asset.assetName, input.asset.issuer,
            SELF, SELF, (sint64)input.amount, qpi.invocator());
        if (locals.xfer < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }
        locals.rel = qpi.releaseShares(input.asset, qpi.invocator(), qpi.invocator(),
            (sint64)input.amount, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
            QTREAT_QX_TRANSFER_FEE);
        if (locals.rel < 0) { output.returnCode = QTREAT_ERR_TRANSFER_FAILED; return; }

        if (locals.bal - input.amount == 0)
        {
            state.mut().generalAssetBalances.removeByKey(locals.key);
        }
        else
        {
            state.mut().generalAssetBalances.set(locals.key, locals.bal - input.amount);
        }
        output.returnCode = QTREAT_OK;
    }

    // ======================== Registration ========================

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(GetStakingInfo, 1);
        REGISTER_USER_FUNCTION(GetPhaseInfo, 2);
        REGISTER_USER_FUNCTION(GetFunds, 3);
        REGISTER_USER_FUNCTION(GetRaffleInfo, 4);
        REGISTER_USER_FUNCTION(GetExcludeAddresses, 5);
        REGISTER_USER_FUNCTION(GetNftInfo, 6);
        REGISTER_USER_FUNCTION(GetMinerInfo, 7);
        REGISTER_USER_FUNCTION(GetAsicCatalogInfo, 8);

        REGISTER_USER_PROCEDURE(DepositDividends, 1);
        REGISTER_USER_PROCEDURE(DepositStakingFund, 2);
        REGISTER_USER_PROCEDURE(RequestUnstake, 3);
        REGISTER_USER_PROCEDURE(FinalizeUnstake, 4);
        REGISTER_USER_PROCEDURE(ClaimQtreatBonus, 5);
        REGISTER_USER_PROCEDURE(DepositQtreatTokens, 6);
        REGISTER_USER_PROCEDURE(DepositGeneralAsset, 7);
        REGISTER_USER_PROCEDURE(SetExcludeAddress, 8);
        REGISTER_USER_PROCEDURE(RevokeGeneralAsset, 9);
        REGISTER_USER_PROCEDURE(ReleaseManagedShares, 10);
        REGISTER_USER_PROCEDURE(DepositMiningFund, 11);
        REGISTER_USER_PROCEDURE(LoadAsicPart, 12);
        REGISTER_USER_PROCEDURE(RegisterAsic, 13);
        REGISTER_USER_PROCEDURE(UnregisterAsic, 14);
        REGISTER_USER_PROCEDURE(DepositDripQdoge, 15);
        REGISTER_USER_PROCEDURE(SetMiningRate, 16);
    }

    INITIALIZE()
    {
        // Shared issuer of QTREAT and QDOGE:
        // QDOGEEESKYPAICECHEAHOXPULEOADTKGEJHAVYPFKHLEWGXXZQUGIGMBUTZE
        // (ID() takes the first 56 chars; the trailing "UTZE" is the checksum.)
        state.mut().qtreatToken.issuer = ID(
            _Q, _D, _O, _G, _E, _E, _E, _S,
            _K, _Y, _P, _A, _I, _C, _E, _C,
            _H, _E, _A, _H, _O, _X, _P, _U,
            _L, _E, _O, _A, _D, _T, _K, _G,
            _E, _J, _H, _A, _V, _Y, _P, _F,
            _K, _H, _L, _E, _W, _G, _X, _X,
            _Z, _Q, _U, _G, _I, _G, _M, _B
        );
        state.mut().qtreatToken.assetName = QTREAT_TOKEN_ASSETNAME;

        state.mut().qdogeToken.issuer = state.get().qtreatToken.issuer; // same issuer
        state.mut().qdogeToken.assetName = QTREAT_QDOGE_ASSETNAME;

        // Hardcoded to the issuer identity (GGWP lesson: a NULL_ID bootstrap
        // lets any first caller seize admin). Change here if admin != issuer.
        state.mut().adminAddress = state.get().qtreatToken.issuer;

        // ---- Dividend NFT collection: QBAY NFT ids (QubicBay collection 15,
        // snapshot 2026-07-18; ids are permanent once minted). Possession is
        // read live from QBAY every epoch — no holder list is stored here.
        state.mut().dividendNftIds.set(0, 4968);
        state.mut().dividendNftIds.set(1, 4969);
        state.mut().dividendNftIds.set(2, 4970);
        state.mut().dividendNftIds.set(3, 4971);
        state.mut().dividendNftIds.set(4, 4972);
        state.mut().dividendNftIds.set(5, 4973);
        state.mut().dividendNftIds.set(6, 4974);
        state.mut().dividendNftIds.set(7, 4975);
        state.mut().dividendNftIds.set(8, 4976);
        state.mut().dividendNftIds.set(9, 4977);
        state.mut().dividendNftIds.set(10, 4978);
        state.mut().dividendNftIds.set(11, 4979);
        state.mut().dividendNftIds.set(12, 4980);
        state.mut().dividendNftIds.set(13, 4981);
        state.mut().dividendNftIds.set(14, 4982);
        state.mut().dividendNftIds.set(15, 4983);
        state.mut().dividendNftIds.set(16, 4984);
        state.mut().dividendNftIds.set(17, 4985);
        state.mut().dividendNftIds.set(18, 4986);
        state.mut().dividendNftIds.set(19, 4987);
        state.mut().dividendNftIds.set(20, 4988);
        state.mut().dividendNftIds.set(21, 4989);
        state.mut().dividendNftIds.set(22, 4990);
        state.mut().dividendNftIds.set(23, 4991);
        state.mut().dividendNftIds.set(24, 4992);
        state.mut().dividendNftIds.set(25, 4993);
        state.mut().dividendNftIds.set(26, 4994);
        state.mut().dividendNftIds.set(27, 4995);
        state.mut().dividendNftIds.set(28, 4996);
        state.mut().dividendNftIds.set(29, 4997);
        state.mut().dividendNftIds.set(30, 4998);
        state.mut().dividendNftIds.set(31, 4999);
        state.mut().dividendNftIds.set(32, 5000);
        state.mut().dividendNftIds.set(33, 5001);
        state.mut().dividendNftIds.set(34, 5002);
        state.mut().dividendNftIds.set(35, 5003);
        state.mut().dividendNftIds.set(36, 5004);
        state.mut().dividendNftIds.set(37, 5005);
        state.mut().dividendNftIds.set(38, 5006);
        state.mut().dividendNftIds.set(39, 5007);
        state.mut().dividendNftIds.set(40, 5008);
        state.mut().dividendNftIds.set(41, 5009);
        state.mut().dividendNftIds.set(42, 5010);
        state.mut().dividendNftIds.set(43, 5011);
        state.mut().dividendNftIds.set(44, 5012);
        state.mut().dividendNftIds.set(45, 5013);
        state.mut().dividendNftIds.set(46, 5014);
        state.mut().dividendNftIds.set(47, 5015);
        state.mut().dividendNftIds.set(48, 5016);
        state.mut().dividendNftIds.set(49, 5017);
        state.mut().dividendNftIds.set(50, 5018);
        state.mut().dividendNftIds.set(51, 5019);
        state.mut().dividendNftIds.set(52, 5020);
        state.mut().dividendNftIds.set(53, 5021);
        state.mut().dividendNftIds.set(54, 5022);
        state.mut().dividendNftIds.set(55, 5023);
        state.mut().dividendNftIds.set(56, 5024);
        state.mut().dividendNftIds.set(57, 5025);
        state.mut().dividendNftIds.set(58, 5026);
        state.mut().dividendNftIds.set(59, 5027);
        state.mut().dividendNftIds.set(60, 5028);
        state.mut().dividendNftIds.set(61, 5029);
        state.mut().dividendNftIds.set(62, 5030);
        state.mut().dividendNftIds.set(63, 5031);
        state.mut().dividendNftIds.set(64, 5032);
        state.mut().dividendNftIds.set(65, 5033);
        state.mut().dividendNftIds.set(66, 5034);
        state.mut().dividendNftIds.set(67, 5035);
        state.mut().dividendNftIds.set(68, 5036);
        state.mut().dividendNftIds.set(69, 5037);
        state.mut().dividendNftIds.set(70, 5038);
        state.mut().dividendNftIds.set(71, 5039);
        state.mut().dividendNftIds.set(72, 5040);
        state.mut().dividendNftIds.set(73, 5041);
        state.mut().dividendNftIds.set(74, 5042);
        state.mut().dividendNftIds.set(75, 5043);
        state.mut().dividendNftIds.set(76, 5044);
        state.mut().dividendNftIds.set(77, 5045);
        state.mut().dividendNftIds.set(78, 5046);
        state.mut().dividendNftIds.set(79, 5047);
        state.mut().dividendNftIds.set(80, 5048);
        state.mut().dividendNftIds.set(81, 5049);
        state.mut().dividendNftIds.set(82, 5050);
        state.mut().dividendNftIds.set(83, 5051);
        state.mut().dividendNftIds.set(84, 5052);
        state.mut().dividendNftIds.set(85, 5053);
        state.mut().dividendNftIds.set(86, 5054);
        state.mut().dividendNftIds.set(87, 5055);
        state.mut().dividendNftIds.set(88, 5056);
        state.mut().dividendNftIds.set(89, 5057);
        state.mut().dividendNftIds.set(90, 5058);
        state.mut().dividendNftIds.set(91, 5059);
        state.mut().dividendNftIds.set(92, 5060);
        state.mut().dividendNftIds.set(93, 5061);
        state.mut().dividendNftIds.set(94, 5062);
        state.mut().dividendNftIds.set(95, 5063);
        state.mut().dividendNftIds.set(96, 5064);
        state.mut().dividendNftIds.set(97, 5065);
        state.mut().dividendNftIds.set(98, 5066);
        state.mut().dividendNftIds.set(99, 5067);
        state.mut().dividendNftIds.set(100, 5068);
        state.mut().dividendNftIds.set(101, 5069);
        state.mut().dividendNftIds.set(102, 5070);
        state.mut().dividendNftIds.set(103, 5071);
        state.mut().dividendNftIds.set(104, 5072);
        state.mut().dividendNftIds.set(105, 5073);
        state.mut().dividendNftIds.set(106, 5074);
        state.mut().dividendNftIds.set(107, 5075);
        state.mut().dividendNftIds.set(108, 5076);
        state.mut().dividendNftIds.set(109, 5077);
        state.mut().dividendNftIds.set(110, 5078);
        state.mut().dividendNftIds.set(111, 5079);
        state.mut().dividendNftIds.set(112, 5080);
        state.mut().dividendNftIds.set(113, 5081);
        state.mut().dividendNftIds.set(114, 5082);
        state.mut().dividendNftIds.set(115, 5083);
        state.mut().dividendNftIds.set(116, 5084);
        state.mut().dividendNftIds.set(117, 5085);
        state.mut().dividendNftIds.set(118, 5086);
        state.mut().dividendNftIds.set(119, 5087);
        state.mut().dividendNftIds.set(120, 5088);
        state.mut().dividendNftIds.set(121, 5089);
        state.mut().dividendNftIds.set(122, 5090);
        state.mut().dividendNftIds.set(123, 5091);
        state.mut().dividendNftIds.set(124, 5092);
        state.mut().dividendNftIds.set(125, 5093);
        state.mut().dividendNftIds.set(126, 5094);
        state.mut().dividendNftIds.set(127, 5095);
        state.mut().dividendNftIds.set(128, 5096);
        state.mut().dividendNftIds.set(129, 5097);
        state.mut().dividendNftIds.set(130, 5098);
        state.mut().dividendNftIds.set(131, 5099);
        state.mut().dividendNftIds.set(132, 5100);
        state.mut().dividendNftIds.set(133, 5101);
        state.mut().dividendNftIds.set(134, 5102);
        state.mut().dividendNftIds.set(135, 5103);
        state.mut().dividendNftIds.set(136, 5104);
        state.mut().dividendNftIds.set(137, 5105);
        state.mut().dividendNftIds.set(138, 5106);
        state.mut().dividendNftIds.set(139, 5107);
        state.mut().dividendNftIds.set(140, 5108);
        state.mut().dividendNftIds.set(141, 5109);
        state.mut().dividendNftIds.set(142, 5110);
        state.mut().dividendNftIds.set(143, 5111);
        state.mut().dividendNftIds.set(144, 5112);
        state.mut().dividendNftIds.set(145, 5113);
        state.mut().dividendNftIds.set(146, 5114);
        state.mut().dividendNftIds.set(147, 5117);
        state.mut().dividendNftIds.set(148, 5118);
        state.mut().dividendNftIds.set(149, 5119);
        state.mut().dividendNftIds.set(150, 5120);
        state.mut().dividendNftIds.set(151, 5121);
        state.mut().dividendNftIds.set(152, 5122);
        state.mut().dividendNftIds.set(153, 5123);
        state.mut().dividendNftIds.set(154, 5124);
        state.mut().dividendNftIds.set(155, 5125);
        state.mut().dividendNftIds.set(156, 5126);
        state.mut().dividendNftIds.set(157, 5127);
        state.mut().dividendNftIds.set(158, 5128);
        state.mut().dividendNftIds.set(159, 5129);
        state.mut().dividendNftIds.set(160, 5130);
        state.mut().dividendNftIds.set(161, 5131);
        state.mut().dividendNftIds.set(162, 5132);
        state.mut().dividendNftIds.set(163, 5133);
        state.mut().dividendNftIds.set(164, 5134);
        state.mut().dividendNftIds.set(165, 5135);
        state.mut().dividendNftIds.set(166, 5136);
        state.mut().dividendNftIds.set(167, 5137);
        state.mut().dividendNftIds.set(168, 5138);
        state.mut().dividendNftIds.set(169, 5139);
        state.mut().dividendNftIds.set(170, 5140);
        state.mut().dividendNftIds.set(171, 5141);
        state.mut().dividendNftIds.set(172, 5142);
        state.mut().dividendNftIds.set(173, 5143);
        state.mut().dividendNftIds.set(174, 5144);
        state.mut().dividendNftIds.set(175, 5145);
        state.mut().dividendNftIds.set(176, 5146);
        state.mut().dividendNftIds.set(177, 5147);
        state.mut().dividendNftIds.set(178, 5148);
        state.mut().dividendNftIds.set(179, 5149);
        state.mut().dividendNftIds.set(180, 5150);
        state.mut().dividendNftIds.set(181, 5151);
        state.mut().dividendNftIds.set(182, 5152);
        state.mut().dividendNftIds.set(183, 5153);
        state.mut().dividendNftIds.set(184, 5154);
        state.mut().dividendNftIds.set(185, 5155);
        state.mut().dividendNftIds.set(186, 5156);
        state.mut().dividendNftIds.set(187, 5157);
        state.mut().dividendNftIds.set(188, 5158);
        state.mut().dividendNftIds.set(189, 5159);
        state.mut().dividendNftIds.set(190, 5160);
        state.mut().dividendNftIds.set(191, 5161);
        state.mut().dividendNftIds.set(192, 5162);
        state.mut().dividendNftIds.set(193, 5163);
        state.mut().dividendNftIds.set(194, 5164);
        state.mut().dividendNftIds.set(195, 5170);
        state.mut().dividendNftIds.set(196, 5172);
        state.mut().dividendNftIds.set(197, 5173);
        state.mut().dividendNftIds.set(198, 5174);
        state.mut().dividendNftIds.set(199, 5175);
        state.mut().dividendNftIdCount = 200;

        state.mut().miningRewardRate = QTREAT_MINING_REWARD_DEFAULT;

        state.mut().stakingStartEpoch = 0; // set on first BEGIN_EPOCH
    }

    // ======================== Incoming QU routing (qRWA pattern) ========================
    // Default: plain sends and asset dividends -> dividend fund.
    // Procedure deposits (DepositStakingFund etc.) handle their own accounting,
    // so procedure-transaction transfers are NOT routed here.
    struct POST_INCOMING_TRANSFER_locals { uint64 prev; };
    POST_INCOMING_TRANSFER_WITH_LOCALS()
    {
        if (input.type == TransferType::qpiDistributeDividends)
        {
            // Dividends from SC shares this contract holds -> QTREAT holders.
            state.mut().dividendFund = sadd(state.get().dividendFund, (uint64)input.amount);
            locals.prev = 0;
            state.get().scDividendTracker.get(input.sourceId, locals.prev);
            state.mut().scDividendTracker.set(input.sourceId, sadd(locals.prev, (uint64)input.amount));
            return;
        }
        if (input.type == TransferType::qpiTransfer
            && input.sourceId == id(RANDOM_CONTRACT_INDEX, 0, 0, 0))
        {
            // Refund from a failed raffle entropy purchase -> restore the
            // staking fund (the draw fee was deducted before BuyEntropy).
            state.mut().stakingFund = sadd(state.get().stakingFund, (uint64)input.amount);
            return;
        }
        if (input.type == TransferType::standardTransaction
            || input.type == TransferType::qpiTransfer
            || input.type == TransferType::revenueDonation)
        {
            // Default fund: any QU sent directly to the contract address,
            // including protocol revenue donations.
            state.mut().dividendFund = sadd(state.get().dividendFund, (uint64)input.amount);
        }
        // procedureTransaction / procedureInvocationByOtherContract:
        // accounted for inside the invoked procedure.
    }

    // ======================== Epoch lifecycle ========================

    // Snapshot QTREAT holders at epoch start.
    struct BEGIN_EPOCH_locals { AssetPossessionIterator it; uint64 bal; id h; uint64 existing; sint64 exIdx; uint64 excluded; };
    BEGIN_EPOCH_WITH_LOCALS()
    {
        // Phase clock starts on the first full epoch after deploy.
        if (state.get().stakingStartEpoch == 0)
        {
            state.mut().stakingStartEpoch = qpi.epoch();
        }

        state.mut().beginBalances.reset();
        state.mut().endBalances.reset();
        state.mut().totalHoldersSnapshot = 0;
        if (state.get().qtreatToken.issuer != NULL_ID)
        {
            for (locals.it.begin(state.get().qtreatToken); !locals.it.reachedEnd(); locals.it.next())
            {
                if (locals.it.possessor() == SELF) continue;
                locals.bal = locals.it.numberOfPossessedShares();
                if (locals.bal < QTREAT_MIN_ELIGIBLE_BAL) continue;
                locals.h = locals.it.possessor();
                // Skip admin-excluded addresses (exchange/fundraising wallets).
                // Excluding from the begin snapshot suffices: the dividend
                // payout loop iterates beginBalances only.
                locals.excluded = 0;
                for (locals.exIdx = 0; locals.exIdx < (sint64)QTREAT_MAX_EXCLUDE_ADDRESSES; locals.exIdx++)
                {
                    if (state.get().excludeAddresses.get(locals.exIdx) != NULL_ID
                        && locals.h == state.get().excludeAddresses.get(locals.exIdx))
                    {
                        locals.excluded = 1;
                    }
                }
                if (locals.excluded != 0) continue;
                locals.existing = 0;
                state.get().beginBalances.get(locals.h, locals.existing);
                if (state.mut().beginBalances.set(locals.h, sadd(locals.existing, locals.bal)) != NULL_INDEX)
                {
                    state.mut().totalHoldersSnapshot = sadd(state.get().totalHoldersSnapshot, locals.bal);
                }
            }
        }
    }

    struct END_EPOCH_locals
    {
        AssetPossessionIterator it; uint64 bal; id h; uint64 existing;
        // staking payout
        sint64 sidx; StakerInfo info; uint64 rewardBudget; uint64 reward; uint64 q; uint64 rem;
        uint64 phaseActive; uint64 elapsed;
        // raffle
        RANDOM::BuyEntropy_input entropyIn;
        RANDOM::BuyEntropy_output entropyOut;
        bit_4096 zeroEntropy;
        RaffleSeed seed;
        m256i raffleDigest;
        uint64 eligibleCount; uint64 winnerIdx; uint64 n;
        // unstake auto-release
        sint64 relResult;
        // progressive staking
        sint64 walletBal; uint64 holdings; uint64 totalWeight; uint64 weight;
        // dividend payout
        uint64 amount; sint64 idx; uint64 beginBal; uint64 endBal; uint64 eligible;
        uint64 nftCnt; uint64 totalEligible; sint64 exIdx; uint64 isExcluded;
        uint64 shareholderShare; uint64 perShare; uint64 paidShareholders;
        uint64 reservedDividend; uint64 distributedDividend;
        // QDOGE drip
        uint64 dripRate; uint64 dripDue; sint64 dripIdx; uint64 dripPacked; uint64 dripRarity;
        sint64 dripXfer; sint64 dripManaged; uint64 dripAccounted; StakerInfo dripStakerInfo;
        // mining payout
        AsicRig rig; uint64 miningBudget; uint64 minerReward; uint64 stillOwned; uint32 rigPartId; sint64 pIdx; uint64 verifyOk;
        QBAY::getInfoOfNFTById_input qbayIn; QBAY::getInfoOfNFTById_output qbayOut;
        Entity ent; uint64 contractBalance;
    };
    END_EPOCH_WITH_LOCALS()
    {
        state.mut().beginBalances.cleanupIfNeeded();
        state.mut().endBalances.cleanupIfNeeded();
        state.mut().stakers.cleanupIfNeeded();
        state.mut().generalAssetBalances.cleanupIfNeeded();
        state.mut().scDividendTracker.cleanupIfNeeded();
        state.mut().nftCounts.cleanupIfNeeded();
        state.mut().asicUsedParts.cleanupIfNeeded();

        // ---- 0. Auto-release matured unstakes ----
        // Any pending unstake whose delay has elapsed is released back to QX
        // automatically — the staker only ever sends the RequestUnstake
        // transaction. The 100 QU QX fee was prepaid at request time and is
        // consumed from the contract balance here. On a (transient) release
        // failure the request stays pending and is retried next epoch, and
        // the manual FinalizeUnstake fallback also remains available.
        for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
             locals.sidx != NULL_INDEX;
             locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
        {
            locals.info = state.get().stakers.value(locals.sidx);
            if (locals.info.unstakeAmount == 0) continue;
            if (qpi.epoch() < locals.info.unstakeEpoch + QTREAT_UNSTAKE_DELAY_EPOCHS) continue;

            locals.h = state.get().stakers.key(locals.sidx);
            locals.relResult = qpi.releaseShares(state.get().qdogeToken, locals.h, locals.h,
                (sint64)locals.info.unstakeAmount, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
                QTREAT_QX_TRANSFER_FEE);
            if (locals.relResult < 0) continue; // retry next epoch

            locals.info.unstakeAmount = 0;
            locals.info.unstakeEpoch = 0;
            // Fully exited, no bonus owed: drop the record (mirrors
            // FinalizeUnstake). Removing the current element mid-walk is safe —
            // it only marks a slot the forward scan has already passed.
            if (locals.info.staked == 0 && locals.info.pendingBonus == 0
                && locals.info.bonusAwarded == 0)
            {
                state.mut().stakers.removeByKey(locals.h);
            }
            else
            {
                state.mut().stakers.set(locals.h, locals.info);
            }
        }

        qpi.getEntity(SELF, locals.ent);
        locals.contractBalance = locals.ent.incomingAmount - locals.ent.outgoingAmount;

        // ---- 1. QDOGE staker rewards: 20M QU/epoch, pro-rata, phase-gated ----
        locals.phaseActive = 0;
        if (state.get().stakingStartEpoch != 0)
        {
            locals.elapsed = qpi.epoch() - state.get().stakingStartEpoch;
            if (locals.elapsed < QTREAT_TOTAL_REWARD_EPOCHS) locals.phaseActive = 1;
        }

        if (locals.phaseActive == 1 && state.get().totalStaked > 0)
        {
            locals.rewardBudget = QTREAT_STAKER_REWARD_PER_EPOCH;
            if (locals.rewardBudget > state.get().stakingFund) locals.rewardBudget = state.get().stakingFund;

            // -- Pass A: update growth streaks + bonus counters, sum weights --
            locals.totalWeight = 0;
            for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
                 locals.sidx != NULL_INDEX;
                 locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
            {
                locals.h = state.get().stakers.key(locals.sidx);
                locals.info = state.get().stakers.value(locals.sidx);
                if (locals.info.staked == 0)
                {
                    // Full exit: reset both consecutive counters.
                    if (locals.info.bonusEpochs != 0 || locals.info.growthStreak != 0
                        || locals.info.lastStaked != 0)
                    {
                        locals.info.bonusEpochs = 0;
                        locals.info.growthStreak = 0;
                        locals.info.lastStaked = 0;
                        state.mut().stakers.set(locals.h, locals.info);
                    }
                    continue;
                }

                // -- Progressive growth streak --
                // Growth counts only when the stake grew by the minimum step
                // AND total observable holdings exceed the personal all-time
                // high — so drip-feeding pre-owned tokens never qualifies.
                locals.walletBal = qpi.numberOfPossessedShares(
                    state.get().qdogeToken.assetName, state.get().qdogeToken.issuer,
                    locals.h, locals.h, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX);
                if (locals.walletBal < 0) locals.walletBal = 0;
                locals.holdings = sadd(sadd(locals.info.staked, locals.info.unstakeAmount), (uint64)locals.walletBal);
                if (locals.elapsed < QTREAT_PROGRESSIVE_START_DELAY_EPOCHS)
                {
                    // Quiet period: no streaks yet, but baselines keep
                    // tracking — the high-water mark ratchets with any
                    // holdings growth and lastStaked follows the stake, so
                    // epoch-5 growth is measured against the settled state.
                    locals.info.growthStreak = 0;
                    if (locals.holdings > locals.info.hwmHoldings)
                    {
                        locals.info.hwmHoldings = locals.holdings;
                    }
                }
                else if (locals.info.staked > locals.info.lastStaked
                    && locals.info.staked - locals.info.lastStaked >= QTREAT_PROGRESSIVE_MIN_STEP
                    && locals.holdings > locals.info.hwmHoldings)
                {
                    if (locals.info.growthStreak < QTREAT_PROGRESSIVE_MAX_STREAK)
                    {
                        locals.info.growthStreak += 1;
                    }
                    locals.info.hwmHoldings = locals.holdings;
                }
                else
                {
                    locals.info.growthStreak = 0;
                }
                locals.info.lastStaked = locals.info.staked;

                // -- QTREAT bonus accrual: requires 12 CONSECUTIVE epochs
                //    at >= 50M staked. Any epoch below the threshold resets
                //    the counter to zero. --
                if (locals.info.staked >= QTREAT_BONUS_THRESHOLD)
                {
                    if (locals.info.bonusAwarded < QTREAT_BONUS_MAX_PER_WALLET)
                    {
                        locals.info.bonusEpochs += 1;
                        while (locals.info.bonusEpochs >= QTREAT_BONUS_INTERVAL_EPOCHS
                            && locals.info.bonusAwarded < QTREAT_BONUS_MAX_PER_WALLET)
                        {
                            locals.info.bonusEpochs -= QTREAT_BONUS_INTERVAL_EPOCHS;
                            locals.info.bonusAwarded += 1;
                            locals.info.pendingBonus += 1;
                        }
                    }
                }
                else
                {
                    locals.info.bonusEpochs = 0;
                }

                state.mut().stakers.set(locals.h, locals.info);
                // weight = staked * (1000 + streak * bonus-permille)
                locals.weight = ((uint128)locals.info.staked
                    * (uint128)(1000 + locals.info.growthStreak * QTREAT_PROGRESSIVE_BONUS_PERMILLE)).low;
                locals.totalWeight = sadd(locals.totalWeight, locals.weight);
            }

            // -- Pass B: pay the budget pro-rata by streak-boosted weight --
            if (locals.rewardBudget > 0 && locals.totalWeight > 0)
            {
                for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
                     locals.sidx != NULL_INDEX;
                     locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
                {
                    locals.h = state.get().stakers.key(locals.sidx);
                    locals.info = state.get().stakers.value(locals.sidx);
                    if (locals.info.staked == 0) continue;

                    locals.weight = ((uint128)locals.info.staked
                        * (uint128)(1000 + locals.info.growthStreak * QTREAT_PROGRESSIVE_BONUS_PERMILLE)).low;
                    locals.reward = div((uint128)locals.rewardBudget * (uint128)locals.weight,
                        (uint128)locals.totalWeight).low;
                    if (locals.reward > locals.contractBalance) locals.reward = locals.contractBalance;
                    if (locals.reward == 0) continue;

                    qpi.transfer(locals.h, locals.reward);
                    locals.contractBalance -= locals.reward;
                    state.mut().stakingFund = state.get().stakingFund - locals.reward;
                    state.mut().totalStakingRewardsDistributed =
                        sadd(state.get().totalStakingRewardsDistributed, locals.reward);
                }
            }
        }

        // ---- 1b. Raffle: one random eligible staker wins 1 QTREAT ----
        // Runs each epoch for the first QTREAT_RAFFLE_EPOCHS epochs. Eligible:
        // active stake and NOT marked for unstaking. The draw buys entropy
        // from the RANDOM contract (RandomLottery pattern); the fee comes out
        // of the staking fund and is restored via POST_INCOMING_TRANSFER if
        // RANDOM has no entropy and refunds the purchase.
        if (state.get().stakingStartEpoch != 0
            && qpi.epoch() - state.get().stakingStartEpoch < QTREAT_RAFFLE_EPOCHS
            && state.get().qtreatBonusPool > 0
            && state.get().stakingFund >= QTREAT_RAFFLE_ENTROPY_FEE)
        {
            locals.eligibleCount = 0;
            for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
                 locals.sidx != NULL_INDEX;
                 locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
            {
                locals.info = state.get().stakers.value(locals.sidx);
                if (locals.info.staked > 0 && locals.info.unstakeAmount == 0)
                {
                    locals.eligibleCount++;
                }
            }

            if (locals.eligibleCount > 0)
            {
                state.mut().stakingFund = state.get().stakingFund - QTREAT_RAFFLE_ENTROPY_FEE;
                locals.entropyIn.collateralTier = QTREAT_RAFFLE_COLLATERAL_TIER;
                locals.entropyIn.numberOfBits = QTREAT_RAFFLE_ENTROPY_BITS;
                locals.entropyIn.trustee = id::zero();
                INVOKE_OTHER_CONTRACT_PROCEDURE(RANDOM, BuyEntropy, locals.entropyIn, locals.entropyOut, (sint64)QTREAT_RAFFLE_ENTROPY_FEE);

                // Invoke failed (RANDOM inactive/errored): the fee never left,
                // so restore it. (Zero-entropy refunds come via POST_INCOMING.)
                if (interContractCallError != NoCallError)
                {
                    state.mut().stakingFund = sadd(state.get().stakingFund, QTREAT_RAFFLE_ENTROPY_FEE);
                }

                if (interContractCallError == NoCallError && !(locals.entropyOut.entropy == locals.zeroEntropy))
                {
                    // Hash entropy with draw context before deriving the winner.
                    locals.seed.entropy = locals.entropyOut.entropy;
                    locals.seed.epoch = qpi.epoch();
                    locals.seed.tick = qpi.tick();
                    locals.seed.eligibleCount = locals.eligibleCount;
                    locals.raffleDigest = qpi.K12(locals.seed);
                    locals.winnerIdx = mod(locals.raffleDigest.u64._0, locals.eligibleCount);

                    locals.n = 0;
                    for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
                         locals.sidx != NULL_INDEX;
                         locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
                    {
                        locals.info = state.get().stakers.value(locals.sidx);
                        if (locals.info.staked == 0 || locals.info.unstakeAmount > 0) continue;
                        if (locals.n == locals.winnerIdx)
                        {
                            locals.h = state.get().stakers.key(locals.sidx);
                            // Credit as a pending bonus (claimed via ClaimQtreatBonus).
                            // Raffle wins do NOT count against the 4-token 50M-bonus cap.
                            locals.info.pendingBonus += 1;
                            state.mut().stakers.set(locals.h, locals.info);
                            state.mut().totalRaffleAwarded = state.get().totalRaffleAwarded + 1;
                            state.mut().lastRaffleWinner = locals.h;
                            state.mut().lastRaffleEpoch = qpi.epoch();
                            break;
                        }
                        locals.n++;
                    }
                }
            }
        }

        // ---- 1c. Auto-deliver earned QTREAT bonuses (50M bonus + raffle) ----
        // Two-step per winner: transfer ownership from the contract, then
        // release management to QX so the tokens are freely tradable. The
        // 100 QU QX release fee is paid from the staking fund. If the bonus
        // pool or the fee budget is short, the award stays pending and is
        // retried next epoch (or claimed manually via ClaimQtreatBonus).
        for (locals.sidx = state.get().stakers.nextElementIndex(NULL_INDEX);
             locals.sidx != NULL_INDEX;
             locals.sidx = state.get().stakers.nextElementIndex(locals.sidx))
        {
            locals.info = state.get().stakers.value(locals.sidx);
            if (locals.info.pendingBonus == 0) continue;
            if (state.get().qtreatBonusPool < locals.info.pendingBonus) continue;
            if (state.get().stakingFund < (uint64)QTREAT_QX_TRANSFER_FEE) continue;

            locals.h = state.get().stakers.key(locals.sidx);
            if (qpi.transferShareOwnershipAndPossession(
                    state.get().qtreatToken.assetName, state.get().qtreatToken.issuer,
                    SELF, SELF, (sint64)locals.info.pendingBonus, locals.h) < 0)
            {
                continue; // retry next epoch
            }
            // Ownership is now the winner's. Pool + counters update regardless
            // of the release outcome below.
            state.mut().qtreatBonusPool = state.get().qtreatBonusPool - locals.info.pendingBonus;
            state.mut().totalBonusDelivered = sadd(state.get().totalBonusDelivered, locals.info.pendingBonus);
            state.mut().stakingFund = state.get().stakingFund - (uint64)QTREAT_QX_TRANSFER_FEE;
            // Hand management to QX. If this (rare) step fails, the winner
            // still owns the tokens and can self-release at any time via
            // ReleaseManagedShares.
            qpi.releaseShares(state.get().qtreatToken, locals.h, locals.h,
                (sint64)locals.info.pendingBonus, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
                QTREAT_QX_TRANSFER_FEE);

            locals.info.pendingBonus = 0;
            state.mut().stakers.set(locals.h, locals.info);
        }

        // ---- 1d. ASIC rig re-verification against QBAY ----
        // A rig only survives (and earns) while its registered owner still
        // possesses all four part NFTs in QBAY. Sold parts auto-unregister
        // the rig, freeing them for the buyer — no admin, no challenge.
        // On a transient inter-contract error the rig is left untouched.
        for (locals.sidx = 0; locals.sidx < (sint64)state.get().asicRigHighWater; locals.sidx++)
        {
            locals.rig = state.get().asicRigs.get(locals.sidx);
            if (locals.rig.active == 0) continue;
            locals.stillOwned = 1;
            locals.verifyOk = 1;
            for (locals.pIdx = 0; locals.pIdx < 4; locals.pIdx++)
            {
                if (locals.pIdx == 0) locals.rigPartId = locals.rig.partMotherboard;
                if (locals.pIdx == 1) locals.rigPartId = locals.rig.partChip;
                if (locals.pIdx == 2) locals.rigPartId = locals.rig.partPsu;
                if (locals.pIdx == 3) locals.rigPartId = locals.rig.partFan;
                locals.qbayIn.NFTId = locals.rigPartId;
                CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
                // On error qbayOut is stale; treat as inconclusive and leave
                // the rig untouched (retried next epoch).
                if (interContractCallError != NoCallError) { locals.verifyOk = 0; break; }
                if (locals.qbayOut.possessor != locals.rig.owner)
                {
                    locals.stillOwned = 0;
                }
            }
            if (locals.verifyOk == 1 && locals.stillOwned == 0)
            {
                state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partMotherboard);
                state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partChip);
                state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partPsu);
                state.mut().asicUsedParts.removeByKey((uint64)locals.rig.partFan);
                state.mut().totalAsicCount = state.get().totalAsicCount - 1;
                state.mut().totalMiningWeight = state.get().totalMiningWeight - locals.rig.weight;
                locals.rig.active = 0;
                state.mut().asicRigs.set(locals.sidx, locals.rig);
            }
        }

        // ---- 1e. ASIC miner pool: 20M QU/epoch by rarity weight ----
        // Not phase-gated: pays whenever the mining fund holds QU and at
        // least one verified rig is registered.
        if (state.get().totalMiningWeight > 0 && state.get().miningFund > 0)
        {
            // Re-read live balance: raffle/bonus fees left the earlier
            // snapshot stale.
            qpi.getEntity(SELF, locals.ent);
            locals.contractBalance = locals.ent.incomingAmount - locals.ent.outgoingAmount;
            locals.miningBudget = state.get().miningRewardRate;
            if (locals.miningBudget > state.get().miningFund) locals.miningBudget = state.get().miningFund;

            for (locals.sidx = 0; locals.sidx < (sint64)state.get().asicRigHighWater; locals.sidx++)
            {
                locals.rig = state.get().asicRigs.get(locals.sidx);
                if (locals.rig.active == 0 || locals.rig.weight == 0) continue;

                locals.minerReward = div((uint128)locals.miningBudget * (uint128)locals.rig.weight,
                    (uint128)state.get().totalMiningWeight).low;
                if (locals.minerReward == 0) continue;
                if (locals.minerReward > locals.contractBalance) locals.minerReward = locals.contractBalance;

                qpi.transfer(locals.rig.owner, locals.minerReward);
                locals.contractBalance -= locals.minerReward;
                state.mut().miningFund = state.get().miningFund - locals.minerReward;
                state.mut().totalMiningRewardsDistributed =
                    sadd(state.get().totalMiningRewardsDistributed, locals.minerReward);
                if (locals.contractBalance == 0) break;
            }
        }

        // ---- 1f. Rebuild the dividend-NFT holder tally from QBAY ----
        // The 200 dividend-collection NFT ids are fixed; possession is read
        // live from QBAY, so sales take effect here with no admin calls.
        if (state.get().dividendNftIdCount > 0)
        {
            state.mut().nftCounts.reset();
            state.mut().totalNftCount = 0;
            for (locals.sidx = 0; locals.sidx < (sint64)state.get().dividendNftIdCount; locals.sidx++)
            {
                locals.qbayIn.NFTId = state.get().dividendNftIds.get(locals.sidx);
                CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
                // Skip on QBAY error: stale qbayOut would credit the wrong id.
                if (interContractCallError != NoCallError) continue;
                if (locals.qbayOut.possessor == NULL_ID || locals.qbayOut.possessor == SELF) continue;
                // Keep excluded addresses out of the dividend denominator.
                locals.isExcluded = 0;
                for (locals.exIdx = 0; locals.exIdx < (sint64)QTREAT_MAX_EXCLUDE_ADDRESSES; locals.exIdx++)
                {
                    if (state.get().excludeAddresses.get(locals.exIdx) != NULL_ID
                        && locals.qbayOut.possessor == state.get().excludeAddresses.get(locals.exIdx))
                    {
                        locals.isExcluded = 1;
                    }
                }
                if (locals.isExcluded != 0) continue;
                locals.existing = 0;
                state.get().nftCounts.get(locals.qbayOut.possessor, locals.existing);
                state.mut().nftCounts.set(locals.qbayOut.possessor, locals.existing + 1);
                state.mut().totalNftCount = state.get().totalNftCount + 1;
            }
        }

        // ---- 1g. QDOGE drip to part-NFT possessors (one year from mint) ----
        // Every cataloged part pays its possessor a weekly QDOGE rate by
        // rarity, read live from QBAY — selling a part moves its stream to
        // the buyer automatically. Two passes: tally per possessor, then one
        // combined transfer + QX release per recipient (release fee paid
        // from the mining fund). Unsold parts (admin-possessed) are skipped.
        if (state.get().asicCatalogLocked == 1
            && state.get().dripStartEpoch != 0
            && qpi.epoch() - state.get().dripStartEpoch < QTREAT_DRIP_EPOCHS
            && state.get().dripQdogePool > 0)
        {
            state.mut().dripTally.reset();
            for (locals.dripIdx = state.get().asicCatalog.nextElementIndex(NULL_INDEX);
                 locals.dripIdx != NULL_INDEX;
                 locals.dripIdx = state.get().asicCatalog.nextElementIndex(locals.dripIdx))
            {
                locals.qbayIn.NFTId = (uint32)state.get().asicCatalog.key(locals.dripIdx);
                locals.dripPacked = state.get().asicCatalog.value(locals.dripIdx);
                locals.dripRarity = mod(locals.dripPacked, 8ULL);
                CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
                if (interContractCallError != NoCallError) continue; // skip on error (stale possessor)
                if (locals.qbayOut.possessor == NULL_ID || locals.qbayOut.possessor == SELF) continue;
                if (locals.qbayOut.possessor == state.get().adminAddress) continue; // unsold inventory

                locals.dripRate = QTREAT_DRIP_QDOGE_COMMON;
                if (locals.dripRarity == 1) locals.dripRate = QTREAT_DRIP_QDOGE_UNCOMMON;
                if (locals.dripRarity == 2) locals.dripRate = QTREAT_DRIP_QDOGE_RARE;
                if (locals.dripRarity == 3) locals.dripRate = QTREAT_DRIP_QDOGE_EPIC;
                if (locals.dripRarity == 4) locals.dripRate = QTREAT_DRIP_QDOGE_LEGENDARY;

                locals.existing = 0;
                state.get().dripTally.get(locals.qbayOut.possessor, locals.existing);
                state.mut().dripTally.set(locals.qbayOut.possessor, locals.existing + locals.dripRate);
            }

            for (locals.dripIdx = state.get().dripTally.nextElementIndex(NULL_INDEX);
                 locals.dripIdx != NULL_INDEX;
                 locals.dripIdx = state.get().dripTally.nextElementIndex(locals.dripIdx))
            {
                locals.h = state.get().dripTally.key(locals.dripIdx);
                locals.dripDue = state.get().dripTally.value(locals.dripIdx);
                if (locals.dripDue == 0) continue;
                if (state.get().dripQdogePool < locals.dripDue) continue; // pool short: skip, admin tops up
                if (state.get().miningFund < (uint64)QTREAT_QX_TRANSFER_FEE) break; // fee budget exhausted

                locals.dripXfer = qpi.transferShareOwnershipAndPossession(
                    state.get().qdogeToken.assetName, state.get().qdogeToken.issuer,
                    SELF, SELF, (sint64)locals.dripDue, locals.h);
                if (locals.dripXfer < 0) continue;
                state.mut().dripQdogePool = state.get().dripQdogePool - locals.dripDue;
                state.mut().totalDripQdogeDistributed = sadd(state.get().totalDripQdogeDistributed, locals.dripDue);
                state.mut().miningFund = state.get().miningFund - (uint64)QTREAT_QX_TRANSFER_FEE;
                // Hand the recipient's QDOGE to QX so it is freely tradable
                // (or stakeable). Release the recipient's ENTIRE unaccounted
                // managed surplus (not just this epoch's drip): if a previous
                // release ever failed, this sweeps the residue and keeps the
                // staked == managed invariant self-healing.
                locals.dripManaged = qpi.numberOfPossessedShares(
                    state.get().qdogeToken.assetName, state.get().qdogeToken.issuer,
                    locals.h, locals.h, SELF_INDEX, SELF_INDEX);
                locals.dripStakerInfo.staked = 0; locals.dripStakerInfo.unstakeAmount = 0;
                state.get().stakers.get(locals.h, locals.dripStakerInfo);
                locals.dripAccounted = locals.dripStakerInfo.staked + locals.dripStakerInfo.unstakeAmount;
                if (locals.dripManaged > (sint64)locals.dripAccounted)
                {
                    qpi.releaseShares(state.get().qdogeToken, locals.h, locals.h,
                        locals.dripManaged - (sint64)locals.dripAccounted,
                        QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX,
                        QTREAT_QX_TRANSFER_FEE);
                }
            }
        }

        // ---- 2. End-of-epoch holder snapshot (anti-gaming min()) ----
        if (state.get().qtreatToken.issuer != NULL_ID)
        {
            for (locals.it.begin(state.get().qtreatToken); !locals.it.reachedEnd(); locals.it.next())
            {
                if (locals.it.possessor() == SELF) continue;
                locals.bal = locals.it.numberOfPossessedShares();
                if (locals.bal == 0) continue;
                locals.h = locals.it.possessor();
                locals.existing = 0;
                state.get().endBalances.get(locals.h, locals.existing);
                state.mut().endBalances.set(locals.h, sadd(locals.existing, locals.bal));
            }
        }

        // ---- 3. Dividend fund -> QTREAT holders + NFT holders ----
        // Token weight = min(begin, end) balance; each registered NFT adds
        // the weight of exactly 1 QTREAT token. NFT-only wallets are paid
        // in a second loop over the registry.
        if (state.get().dividendFund == 0) return;
        // Re-read live balance after mining + drip so the payout cap is accurate.
        qpi.getEntity(SELF, locals.ent);
        locals.contractBalance = locals.ent.incomingAmount - locals.ent.outgoingAmount;
        locals.amount = state.get().dividendFund;
        if (locals.amount > locals.contractBalance) locals.amount = locals.contractBalance;
        locals.totalEligible = sadd(state.get().totalHoldersSnapshot, state.get().totalNftCount);
        if (locals.amount == 0 || locals.totalEligible == 0) return;

        // Reserve from the fund; whatever isn't actually distributed (min()
        // shortfall or drained balance) is returned below so it rolls forward
        // instead of being stranded.
        locals.reservedDividend = locals.amount;
        locals.distributedDividend = 0;
        state.mut().dividendFund = state.get().dividendFund - locals.reservedDividend;

        // ---- 3a. Shareholder cut: 5% to the 676 IPO shares ----
        // distributeDividends pays per-share natively; the integer remainder
        // (shareholderShare - perShare*676) stays in the holder pool below.
        locals.shareholderShare = div((uint128)locals.amount * (uint128)QTREAT_DIVIDEND_SHAREHOLDER_PERMILLE, (uint128)1000ULL).low;
        locals.perShare = div(locals.shareholderShare, (uint64)NUMBER_OF_COMPUTORS);
        if (locals.perShare > 0 && qpi.distributeDividends((sint64)locals.perShare))
        {
            locals.paidShareholders = locals.perShare * (uint64)NUMBER_OF_COMPUTORS;
            locals.amount -= locals.paidShareholders;
            locals.distributedDividend = locals.paidShareholders;
            locals.contractBalance = (locals.contractBalance > locals.paidShareholders)
                ? locals.contractBalance - locals.paidShareholders : 0;
            state.mut().totalShareholderDividends = sadd(state.get().totalShareholderDividends, locals.paidShareholders);
        }

        // Pass 1: token holders from the begin snapshot (+ their NFTs).
        for (locals.idx = state.get().beginBalances.nextElementIndex(NULL_INDEX);
             locals.idx != NULL_INDEX;
             locals.idx = state.get().beginBalances.nextElementIndex(locals.idx))
        {
            locals.h = state.get().beginBalances.key(locals.idx);
            locals.beginBal = state.get().beginBalances.value(locals.idx);
            locals.endBal = 0;
            state.get().endBalances.get(locals.h, locals.endBal);
            locals.eligible = (locals.endBal < locals.beginBal) ? locals.endBal : locals.beginBal;
            locals.nftCnt = 0;
            state.get().nftCounts.get(locals.h, locals.nftCnt);
            locals.eligible = sadd(locals.eligible, locals.nftCnt);
            if (locals.eligible == 0) continue;

            locals.q = div(locals.amount, locals.totalEligible);
            locals.rem = mod(locals.amount, locals.totalEligible);
            locals.reward = locals.q * locals.eligible
                + div((uint128)locals.rem * (uint128)locals.eligible, (uint128)locals.totalEligible).low;
            if (locals.reward == 0) continue;
            if (locals.reward > locals.contractBalance) locals.reward = locals.contractBalance;

            qpi.transfer(locals.h, locals.reward);
            locals.contractBalance -= locals.reward;
            locals.distributedDividend = sadd(locals.distributedDividend, locals.reward);
            if (locals.contractBalance == 0) break;
        }

        // Pass 2: NFT-only wallets (no entry in the begin snapshot).
        for (locals.idx = state.get().nftCounts.nextElementIndex(NULL_INDEX);
             locals.idx != NULL_INDEX;
             locals.idx = state.get().nftCounts.nextElementIndex(locals.idx))
        {
            locals.h = state.get().nftCounts.key(locals.idx);
            if (state.get().beginBalances.contains(locals.h)) continue; // paid in pass 1
            locals.isExcluded = 0;
            for (locals.exIdx = 0; locals.exIdx < (sint64)QTREAT_MAX_EXCLUDE_ADDRESSES; locals.exIdx++)
            {
                if (state.get().excludeAddresses.get(locals.exIdx) != NULL_ID
                    && locals.h == state.get().excludeAddresses.get(locals.exIdx))
                {
                    locals.isExcluded = 1;
                }
            }
            if (locals.isExcluded != 0) continue;
            locals.eligible = state.get().nftCounts.value(locals.idx);
            if (locals.eligible == 0) continue;

            locals.q = div(locals.amount, locals.totalEligible);
            locals.rem = mod(locals.amount, locals.totalEligible);
            locals.reward = locals.q * locals.eligible
                + div((uint128)locals.rem * (uint128)locals.eligible, (uint128)locals.totalEligible).low;
            if (locals.reward == 0) continue;
            if (locals.reward > locals.contractBalance) locals.reward = locals.contractBalance;

            qpi.transfer(locals.h, locals.reward);
            locals.contractBalance -= locals.reward;
            locals.distributedDividend = sadd(locals.distributedDividend, locals.reward);
            if (locals.contractBalance == 0) break;
        }

        // Return the undistributed reserve; record only what was paid.
        if (locals.reservedDividend > locals.distributedDividend)
        {
            state.mut().dividendFund = sadd(state.get().dividendFund,
                locals.reservedDividend - locals.distributedDividend);
        }
        state.mut().totalDividendsDistributed =
            sadd(state.get().totalDividendsDistributed, locals.distributedDividend);
    }

    // ======================== Staking entry point ========================
    // The user stakes by calling QX.TransferShareManagementRights with the
    // exact amount they want to stake. PRE validates, POST credits — so the
    // staked amount always equals the managed amount; no over-transfer is
    // possible and no separate Stake procedure is needed.

    struct PRE_ACQUIRE_SHARES_locals { StakerInfo info; };
    PRE_ACQUIRE_SHARES_WITH_LOCALS()
    {
        if (input.otherContractIndex != QTREAT_QX_CONTRACT_INDEX) return; // reject non-QX

        // QDOGE = stake attempt: enforce staking rules here so an invalid
        // transfer FAILS at QX and the user's tokens remain tradable.
        if (input.asset.assetName == state.get().qdogeToken.assetName
            && input.asset.issuer == state.get().qdogeToken.issuer)
        {
            // Exemption: the ADMIN's QDOGE transfers stage the drip pool and
            // are never stakes (so the admin wallet cannot stake).
            if (input.owner == state.get().adminAddress)
            {
                output.allowTransfer = true;
                output.requestedFee = 0;
                return;
            }
            if (input.numberOfShares <= 0) return; // reject
            locals.info.staked = 0; locals.info.unstakeAmount = 0; locals.info.unstakeEpoch = 0;
            locals.info.bonusEpochs = 0; locals.info.bonusAwarded = 0; locals.info.pendingBonus = 0;
            state.get().stakers.get(input.owner, locals.info);
            // The staking program lasts QTREAT_TOTAL_REWARD_EPOCHS epochs
            // from launch; after that, new stakes are rejected at QX so no
            // one locks tokens for a program that pays nothing. Unstaking
            // is unaffected.
            if (state.get().stakingStartEpoch != 0
                && qpi.epoch() - state.get().stakingStartEpoch >= QTREAT_TOTAL_REWARD_EPOCHS) return;
            if (locals.info.unstakeAmount > 0) return;              // reject: unstake pending
            if (locals.info.staked + (uint64)input.numberOfShares < QTREAT_MIN_STAKE) return; // reject: below 10M min
            // Capacity guard: reject NEW stakers when the map is full, so
            // POST_ACQUIRE can never silently fail to credit a stake
            // (existing keys can always be updated, even at capacity).
            if (!state.get().stakers.contains(input.owner)
                && state.get().stakers.population() >= QTREAT_MAX_STAKERS) return;
            output.allowTransfer = true;
            output.requestedFee = 0;
            return;
        }

        // Everything else (QTREAT bonus deposits, dividend-paying assets the
        // admin stages for DepositGeneralAsset): accept from QX, no fee.
        output.allowTransfer = true;
        output.requestedFee = 0;
    }

    // Credit the stake for exactly the QDOGE shares that arrived.
    struct POST_ACQUIRE_SHARES_locals { StakerInfo info; uint64 existed; sint64 wallet; };
    POST_ACQUIRE_SHARES_WITH_LOCALS()
    {
        if (input.otherContractIndex != QTREAT_QX_CONTRACT_INDEX) return;
        if (input.asset.assetName != state.get().qdogeToken.assetName
            || input.asset.issuer != state.get().qdogeToken.issuer) return;
        if (input.owner == state.get().adminAddress) return; // drip staging, not a stake
        if (input.numberOfShares <= 0) return;

        locals.info.staked = 0; locals.info.unstakeAmount = 0; locals.info.unstakeEpoch = 0;
        locals.info.bonusEpochs = 0; locals.info.bonusAwarded = 0; locals.info.pendingBonus = 0;
        locals.info.hwmHoldings = 0; locals.info.lastStaked = 0; locals.info.growthStreak = 0;
        locals.existed = state.get().stakers.get(input.owner, locals.info) ? 1 : 0;
        locals.info.staked = sadd(locals.info.staked, (uint64)input.numberOfShares);
        if (locals.existed == 0)
        {
            // New staker: capture the holdings baseline NOW, including the
            // remaining wallet balance. Tokens the staker already owns can
            // never count as "growth" — only genuinely acquired ones can.
            locals.wallet = qpi.numberOfPossessedShares(
                state.get().qdogeToken.assetName, state.get().qdogeToken.issuer,
                input.owner, input.owner, QTREAT_QX_CONTRACT_INDEX, QTREAT_QX_CONTRACT_INDEX);
            if (locals.wallet < 0) locals.wallet = 0;
            locals.info.hwmHoldings = sadd(locals.info.staked, (uint64)locals.wallet);
        }
        if (state.mut().stakers.set(input.owner, locals.info) == NULL_INDEX)
        {
            // Unreachable: PRE rejects new stakers at capacity and set() reuses
            // removed slots, so it can't fail here. Asset APIs are illegal in
            // this callback anyway, so just don't credit a record-less stake.
            return;
        }
        state.mut().totalStaked = sadd(state.get().totalStaked, (uint64)input.numberOfShares);
    }
};
