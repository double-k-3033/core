using namespace QPI;

// QREWARDS (multi-pool) - open ecosystem loyalty & dividend platform.
//
// Anyone can createPool(): each pool has its own creator/admin and its own asset
// registry. Holding a pool's registered assets earns a weighted "share" of that
// pool's dividends.
//
// DISTRIBUTION MODEL (pure snapshot, qRWA-style):
//   Dividends are NOT tracked as a persistent soulbound balance. Instead, each
//   deposit (minus the 5% fee) accumulates into a per-currency POT for the pool.
//   At END_EPOCH the contract reads LIVE holdings straight from the asset ledger,
//   computes each holder's weight (per-asset points x concentration multiplier x
//   price weight, exactly like previewWeight), and pays out the pot pro-rata by
//   weight. Because every epoch reads the real ledger, a holder who sold cannot be
//   paid (their live balance is 0) and the same tokens can never be counted twice
//   -- there are no stale "ghost" positions. This is why there is no claim/sync
//   step and no persistent per-user balance.
//
// Each pool can pay dividends in up to QREWARDS_MAX_DIV_CURRENCIES currencies
// (slot 0 = QU by default; pool admin can add asset currencies, e.g. QDOGE).
// Pools are fully isolated: a pool can only ever pay out what was deposited to it.
//
// NOTE ON SCALE: END_EPOCH distribution iterates each active pool's registered
// assets' holders. It is designed for a handful of active pools of up to a few
// thousand holders each (QREWARDS_SNAPSHOT_CAP). It is NOT a mass multi-tenant
// sweep of thousands of busy pools in one epoch.
//
// NOTE ON TIMING: distribution uses the END_EPOCH snapshot only. That fully
// prevents stale/ghost over-payment, but does not by itself stop someone buying
// a large position right before the epoch boundary to grab that epoch's dividend
// (and selling after). A min(begin,end) rule would require a BEGIN_EPOCH snapshot;
// see docs/qrewards-build-guide.md.
//
// See docs/qrewards-build-guide.md for the full design rationale.

constexpr uint32 QREWARDS_MAX_POOLS            = 1024;
constexpr uint32 QREWARDS_MAX_ASSETS_PER_POOL  = 64;
constexpr uint32 QREWARDS_MAX_DIV_CURRENCIES   = 4;          // QU + up to 3 assets per pool
constexpr uint32 QREWARDS_MAX_BATCH            = 16;         // assets per registerAssets call
// FIX: 24 is not a power of 2. Any QPI build whose Array<T,N> enforces a power-of-2 capacity
// (a standard requirement -- see qpi.h's static_assert on L && !(L & (L-1))) fails to compile
// addRecipients_input's Array<id,N>/Array<uint64,N> fields at N=24. 32 would exceed the
// documented MAX_INPUT_SIZE=1024 budget for this struct (12 + 40*32 = 1292 bytes), so 16 is the
// largest power-of-2 batch size that still fits (12 + 40*16 = 652 bytes).
constexpr uint32 QREWARDS_MAX_RECIP_BATCH      = 16;         // recipients per addRecipients call (MAX_INPUT_SIZE=1024)
constexpr uint64 QREWARDS_MAX_RECIPIENTS       = 1048576;    // 2^20: shared recipient-list entries across ALL pools
                                                            // (~96MB of the 1GB state budget; ~209 pools at the per-pool cap)
constexpr uint32 QREWARDS_MAX_LIST_PER_POOL    = 5000;      // hard cap on list-mode recipients per pool (< SNAPSHOT_CAP,
                                                            // so every recipient in a list pool is always paid in full each epoch)
constexpr uint64 QREWARDS_MAX_FUNDERS          = 65536;      // sender->pool routing entries for tagged QU transfers
constexpr uint32 QREWARDS_PAGE                 = 256;        // page size for paginated views
constexpr uint64 QREWARDS_SNAPSHOT_CAP         = 16384;      // 2^14: max unique holders per pool in one epoch snapshot
constexpr uint64 QREWARDS_REGISTRY_SIZE        = uint64(QREWARDS_MAX_POOLS) * QREWARDS_MAX_ASSETS_PER_POOL;
constexpr uint64 QREWARDS_BPS                  = 10000;
constexpr uint32 QREWARDS_MAX_WEIGHT_BPS       = 1000000;        // cap weight at 100x
constexpr uint64 QREWARDS_MAX_LIST_WEIGHT      = 1000000000000ULL; // 1e12: per-recipient list weight cap
                                                                   // (keeps pot*weight well within uint128; ample for any pro-rata scheme)
constexpr uint64 QREWARDS_DEFAULT_CREATE_FEE   = 5000000ULL;     // 5M QU to create a pool; protocol-owner tunable
constexpr uint64 QREWARDS_DEFAULT_OPERATING_FEE = 25000ULL;      // 25k QU per pool per epoch (~weekly on mainnet epoch cadence); protocol-owner tunable
// Hardwired ceilings on setPlatformParams: the platformOwner can move either fee anywhere in
// [0, cap] -- i.e. only ever reduce it from here, never raise it -- since the cap is pinned to
// today's default. Without this, a single owner key could set operatingFee arbitrarily high and
// instantly starve every pool's operatingBalance platform-wide, mass-pausing (then mass-
// deactivating) every pool at the next END_EPOCH with no recovery window.
constexpr uint64 QREWARDS_MAX_CREATE_POOL_FEE  = QREWARDS_DEFAULT_CREATE_FEE;
constexpr uint64 QREWARDS_MAX_OPERATING_FEE    = QREWARDS_DEFAULT_OPERATING_FEE;

// NFT-backed weight (QBAY integration): a pool admin can register individual QBAY NFT ids as an
// additional weight source, combined with the fungible-asset registry in holdings mode (not
// available in list mode, which bypasses the whole holdings/weight computation by design).
// QBAY has no notion of "collection" on a minted NFT (confirmed by reading Qbay.h directly --
// mint() uses collectionId only to look up fee/royalty/creator and never stores it on the NFT
// record), so membership can't be derived from QBAY automatically; the admin curates the member
// id list directly here, the same way QTREAT's LoadAsicPart does for its own NFT-backed rewards.
constexpr uint32 QREWARDS_MAX_NFT_PER_POOL     = 128;        // admin-curated NFT ids per pool
constexpr uint64 QREWARDS_NFT_REGISTRY_SIZE    = uint64(QREWARDS_MAX_POOLS) * QREWARDS_MAX_NFT_PER_POOL;
// Re-verifying every registered NFT's current possessor via a cross-contract QBAY call, for
// every pool, every epoch, could force a large pool's snapshot into dozens of external calls in
// a single tick (SnapshotPool must complete within one tick -- see its own comment). Spread the
// re-verification round-robin across N epochs instead: entry i is checked only when
// (i + qpi.epoch()) mod N == 0, so every entry is checked exactly once every N epochs with no
// stored cursor, and a checked entry's weight is scaled by N to keep the long-run average payout
// identical to checking everyone every epoch -- the exact technique QTREAT already uses for its
// own QBAY-backed dividend NFTs (QTREAT_NFT_SNAPSHOT_SPREAD_EPOCHS).
constexpr uint32 QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS = 8;
constexpr uint64 QREWARDS_FEE_SHAREHOLDER_PCT  = 70;             // create/operating fee split: 70% QREWARDS shareholders
constexpr uint64 QREWARDS_FEE_QPAYHUB_PCT      = 15;             // 15% QPAYHUB dividends address (rest burned)
constexpr uint32 QREWARDS_MAX_MISSED_EPOCHS    = 2;              // paused this many epochs, then deactivated
constexpr uint64 QREWARDS_DIVIDEND_FEE_PCT     = 5;              // 5% fee skimmed from every dividend deposit
constexpr uint64 QREWARDS_DIVFEE_QPAYHUB_PCT   = 80;            // of the 5% fee: 80% QPAYHUB (QU) / QPAY wallet (token)
constexpr uint64 QREWARDS_DIVFEE_SHAREHOLDER_PCT = 20;          // of the 5% fee: 20% QREWARDS shareholders
constexpr uint64 QREWARDS_MAX_EXCLUSIONS       = 65536;         // (poolId,address) exclusions, shared
constexpr uint64 QREWARDS_CONTRACT_ASSET_NAME  = 19230739006837329ULL; // packed "QREWARD" (contract shares, issuer NULL_ID)

// Distribution modes (state.distributionMode):
constexpr uint8  QREWARDS_DIST_END_EPOCH       = 0; // (default) distribute every pool in one END_EPOCH tick
constexpr uint8  QREWARDS_DIST_STREAMED        = 1; // stream one pool at a time across END_TICKs over the epoch
// Per-pool distribution target (PoolMeta.targetMode): who the pot is split among.
constexpr uint8  QREWARDS_TARGET_HOLDINGS      = 0; // (default) live holders of the pool's registered assets
constexpr uint8  QREWARDS_TARGET_LIST          = 1; // an explicit admin-uploaded recipient list (pro-rata by weight)
// Streamed-mode pacing (fixed; no setter): ~24h delay before a cycle, batch per tick.
constexpr uint32 QREWARDS_STREAM_DELAY_TICKS   = 86400;  // ~24h at ~1 tick/s: wait this many ticks into the epoch before a cycle
constexpr uint32 QREWARDS_STREAM_BATCH         = 256;    // work units (freezes/payouts) per END_TICK
constexpr uint16 QREWARDS_NO_CYCLE_EPOCH       = 0xFFFF; // sentinel: no cycle has run yet
constexpr uint8  QREWARDS_STREAM_IDLE          = 0;      // streaming sub-state: need to freeze the next pool
constexpr uint8  QREWARDS_STREAM_PAYING        = 1;      // streaming sub-state: paying out the frozen pool

// return codes
constexpr sint32 QREWARDS_SUCCESS            = 0;
constexpr sint32 QREWARDS_NOT_ADMIN          = 1;
constexpr sint32 QREWARDS_INVALID_PARAM      = 2;
constexpr sint32 QREWARDS_REGISTRY_FULL      = 3;
constexpr sint32 QREWARDS_ASSET_NOT_ISSUED   = 4;
constexpr sint32 QREWARDS_INVALID_INDEX      = 5;
constexpr sint32 QREWARDS_POOL_NOT_FOUND     = 6;
constexpr sint32 QREWARDS_POOL_INACTIVE      = 7;
constexpr sint32 QREWARDS_MAX_POOLS_REACHED  = 8;
constexpr sint32 QREWARDS_INSUFFICIENT_FEE   = 9;
constexpr sint32 QREWARDS_NOT_PLATFORM_OWNER = 10;
constexpr sint32 QREWARDS_CURRENCIES_FULL    = 11;
constexpr sint32 QREWARDS_TRANSFER_FAILED    = 12;
constexpr sint32 QREWARDS_POOL_PAUSED        = 13;

// log types
constexpr uint32 QREWARDS_LOG_SUCCESS       = 0;
constexpr uint32 QREWARDS_LOG_POOL_CREATED  = 1;
constexpr uint32 QREWARDS_LOG_DISTRIBUTED   = 2;
constexpr uint32 QREWARDS_LOG_DIVIDEND      = 3;
constexpr uint32 QREWARDS_LOG_ASSET_CHANGED = 4;

struct QREWARDS2
{
};

struct QREWARDS : public ContractBase
{
public:
    struct QRewardsLogger
    {
        uint32 _contractIndex;
        uint32 _type;
        sint8 _terminator;
    };

    // A qualifying asset in a pool: holding it earns a weighted share of the pool.
    struct AssetRule
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;       // quantity of the asset equal to 1 base point
        uint32 weightBps;  // relative value/price multiplier, 10000 = 1.0x
        uint8  kind;       // 0 = fungible token, 1 = contract shares (informational)
        uint8  active;     // 1 = counted, 0 = ignored
    };

    // One admin-curated QBAY NFT id counted toward a pool's weight in holdings mode.
    // weightPoints is flat (not multiplied by any concentration tier the way fungible-asset
    // points are) -- an admin who wants rarity tiers just registers rarer ids with higher points.
    struct NFTRule
    {
        uint32 nftId;         // QBAY global NFT id (QBAY::InfoOfNFT index)
        uint32 weightPoints;  // flat weight contributed while this id is held
        uint8  active;        // 1 = counted, 0 = ignored
    };

    // Spec used by the batch registerAssets.
    struct AssetSpec
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
    };

    // A dividend currency for a pool. assetName==0 && issuer==NULL_ID means QU.
    // `pot` is the accumulated-but-undistributed revenue (carried across epochs
    // if there were no eligible holders, plus per-epoch rounding dust).
    struct DivCurrency
    {
        uint64 assetName;
        id     issuer;
        uint64 pot;           // revenue accumulating this cycle
        uint64 distributable; // revenue rolled over from pot and currently being paid out (streamed mode)
        uint64 lifetime;      // cumulative revenue ever deposited (stat)
        uint8  active;
    };

    struct PoolMeta
    {
        id     admin;
        uint64 label;             // short packed name (optional)
        uint64 operatingBalance;  // QU reserve for the per-epoch operating fee
        uint64 lastTotalWeight;   // total weight at the last distribution (for front ends)
        uint32 numAssets;
        uint32 numNfts;           // admin-curated QBAY NFT ids counted toward holdings-mode weight
        uint32 missedEpochs;      // consecutive epochs the operating fee could not be paid
        uint32 listVersion;       // bumped by clearRecipients; only entries of the current version count
        uint32 listCount;         // live recipient-list entries for this pool (enforces QREWARDS_MAX_LIST_PER_POOL)
        uint8  numCurrencies;
        uint8  active;            // 1 = pool exists (0 after deactivation)
        uint8  paused;            // 1 = underfunded, not accruing/distributing
        uint8  targetMode;        // 0 = by holdings (default), 1 = by explicit recipient list
        Array<DivCurrency, QREWARDS_MAX_DIV_CURRENCIES> currencies;
    };

    // Composite key helper (excluded set + recipient list).
    struct KeyProto
    {
        uint64 poolId;
        id     wallet;
    };

    // One entry of a pool's explicit recipient list (used when targetMode == LIST). Keyed in the
    // shared `recipients` map by K12(poolId, wallet); the value carries poolId/wallet/weight so the
    // map can be filtered and enumerated by pool, and `version` lets clearRecipients invalidate a
    // whole list in O(1) (stale-version entries are ignored and reclaimed on cleanup/overwrite).
    struct RecipientEntry
    {
        id     wallet;
        uint64 poolId;
        uint64 weight;
        uint32 version;
    };

    struct StateData
    {
        // Excluded (poolId,address): key = K12(poolId, address) -> 1. Excluded holders
        // earn nothing (weight forced to 0), so they neither receive nor dilute.
        HashMap<id, uint8, QREWARDS_MAX_EXCLUSIONS> excluded;
        // Scratch holder->weight map, rebuilt per pool during END_EPOCH distribution.
        HashMap<id, uint64, QREWARDS_SNAPSHOT_CAP> snapshot;
        // Explicit recipient lists for list-mode pools: key = K12(poolId, wallet) -> RecipientEntry.
        HashMap<id, RecipientEntry, QREWARDS_MAX_RECIPIENTS> recipients;
        uint64 pendingDivFeeQU; // QU dividend-fee skim, flushed 80/20 at END_EPOCH
        Array<PoolMeta, QREWARDS_MAX_POOLS> poolMeta;
        Array<AssetRule, QREWARDS_REGISTRY_SIZE> registry;           // pool p asset i at p*MAX_ASSETS+i
        Array<NFTRule, QREWARDS_NFT_REGISTRY_SIZE> nftRegistry;       // pool p NFT i at p*MAX_NFT_PER_POOL+i
        // sourceId -> poolId: a plain QU transfer from sourceId is credited to that pool's QU currency.
        HashMap<id, uint64, QREWARDS_MAX_FUNDERS> fundingRoute;
        uint32 numPools;
        id platformOwner;
        // Receives the 15% QPAYHUB share of CREATE/OPERATING fees AND the 80% QPAYHUB
        // share of the 5% QU DIVIDEND fee. QPAYHUB (qubic/core PR #1015) is
        // CONTRACT_INDEX 29, so set this to id(29, 0, 0, 0): a plain qpi.transfer there
        // is auto-credited to QPAYHUB's feePool.
        id qpayhubAddress;
        // Receives the QPAY share (80%) of the 5% TOKEN dividend fee (QPAYHUB's feePool
        // is QU-only, so token fees go here instead). Initialized to the QPAY token
        // dividends wallet; protocol-owner tunable.
        id qpayTokenDividendsAddress;
        uint64 createPoolFee;
        uint64 operatingFee;   // per-pool per-epoch

        // --- Streamed distribution (dormant unless distributionMode == QREWARDS_DIST_STREAMED) ---
        uint8  distributionMode;     // 0 = END_EPOCH (default), 1 = streamed across END_TICKs
        uint32 streamDelayTicks;     // ticks into the epoch before a cycle may start
        uint32 streamBatchSize;      // work units per END_TICK
        uint32 epochStartTick;       // qpi.tick() captured at BEGIN_EPOCH
        uint16 lastCycleEpoch;       // epoch in which the last cycle was started (NO_CYCLE_EPOCH = none)
        uint8  cycleActive;          // 1 = a distribution cycle is in progress
        uint8  streamState;          // IDLE (freeze next pool) / PAYING (pay current pool)
        uint8  streamCurrencyStarted;// 1 = streamFrozenAmt captured for the current currency
        uint32 streamPoolCursor;     // pool being processed this cycle
        uint8  streamCurrencyCursor; // currency index within the current pool
        sint64 streamPayCursor;      // snapshot element index last paid (resume via nextElementIndex)
        // Frozen exact total weight of the current pool, stored as hi/lo (uint128 has no default
        // constructor, so it is not used directly as a state field; reassembled where needed).
        uint64 streamTotalWeightHi;
        uint64 streamTotalWeightLo;
        uint64 streamFrozenAmt;      // frozen distributable amount of the current currency
        uint64 streamPaidSum;        // amount paid so far for the current currency
    };

protected:
    /**************************************/
    /************UTIL FUNCTIONS************/
    /**************************************/
    inline static uint64 concentrationMultiplier(uint64 basePoints)
    {
        if (basePoints >= 200) return 20000;
        if (basePoints >= 50)  return 15000;
        if (basePoints >= 10)  return 12500;
        if (basePoints >= 1)   return 10000;
        return 0;
    }

    // Saturating uint128 -> uint64 (for the informational lastTotalWeight field).
    inline static uint64 sat64(uint128 v)
    {
        return (v.high != 0) ? 0xFFFFFFFFFFFFFFFFULL : v.low;
    }

    // One asset's weight contribution: pts * mult * weightBps / BPS^2, computed in 128-bit
    // and saturated to UINT64_MAX so the multiplication can never overflow 64 bits. (mult
    // <= 20000 and weightBps <= 1e6, so pts > ~9.2e8 would otherwise wrap uint64.)
    inline static uint64 weightContribution(uint64 pts, uint64 mult, uint64 weightBps)
    {
        return sat64(div<uint128>(uint128(pts) * uint128(mult) * uint128(weightBps),
                                  uint128(QREWARDS_BPS * QREWARDS_BPS)));
    }

    // Saturating a + b in uint64 (a holder's summed weight; capping a numerator can only
    // under-pay, never over-draw, so this is safe).
    inline static uint64 satAdd64(uint64 a, uint64 b)
    {
        return (a + b < a) ? 0xFFFFFFFFFFFFFFFFULL : a + b;
    }

    // Live weight of one user in one pool: sum over the pool's active registered
    // assets of (held/unit) x concentrationMultiplier x weightBps / BPS^2.
    // Excluded (poolId,user) earns 0. Reads holdings straight from the ledger.
    struct ComputeEntitlement_input { uint64 poolId; id user; };
    struct ComputeEntitlement_output { uint64 weight; };
    struct ComputeEntitlement_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id exKey;
        uint8 exFlag;
        uint32 i;
        uint64 base;
        AssetRule rule;
        Asset asset;
        sint64 held;
        uint64 pts;
        uint64 mult;
        uint64 add;
        uint64 nftBase;
        NFTRule nftRule;
        QBAY::getInfoOfNFTById_input qbayIn;
        QBAY::getInfoOfNFTById_output qbayOut;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ComputeEntitlement)
    {
        output.weight = 0;
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.user;
        locals.exKey = qpi.K12(locals.proto);
        locals.exFlag = 0;
        if (state.get().excluded.get(locals.exKey, locals.exFlag) && locals.exFlag)
        {
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (!locals.rule.active || locals.rule.unit == 0) continue;
            locals.asset.assetName = locals.rule.assetName;
            locals.asset.issuer = locals.rule.issuer;
            locals.held = qpi.numberOfShares(locals.asset,
                AssetOwnershipSelect::byOwner(input.user),
                AssetPossessionSelect::byPossessor(input.user));
            if (locals.held <= 0) continue;
            locals.pts = div((uint64)locals.held, locals.rule.unit);
            if (locals.pts == 0) continue;
            locals.mult = concentrationMultiplier(locals.pts);
            locals.add = weightContribution(locals.pts, locals.mult, (uint64)locals.rule.weightBps);
            output.weight = satAdd64(output.weight, locals.add);
        }

        // NFT-backed weight: bounded by this pool's own numNfts (<= QREWARDS_MAX_NFT_PER_POOL),
        // so checking every entry live (no round-robin spread) is fine for a single-user,
        // read-only query -- the spread exists only to bound SnapshotPool's bulk per-tick cost.
        locals.nftBase = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            locals.nftRule = state.get().nftRegistry.get(locals.nftBase + locals.i);
            if (!locals.nftRule.active) continue;
            locals.qbayIn.NFTId = locals.nftRule.nftId;
            CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
            if (interContractCallError != NoCallError) continue;
            if (locals.qbayOut.possessor != input.user) continue;
            output.weight = satAdd64(output.weight, locals.nftRule.weightPoints);
        }
    }

    // Split a QU fee (create/operating): 70% to QREWARDS shareholders, 15% to the
    // QPAYHUB address, 15% burned. (No QPAYHUB address -> its share is burned too.)
    struct DistributeFee_input { uint64 amount; };
    struct DistributeFee_output { };
    struct DistributeFee_locals { uint64 shTotal; uint64 qpayhub; uint64 perShare; uint64 actualSh; uint64 burnAmt; };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributeFee)
    {
        if (input.amount == 0) return;
        locals.shTotal = div(input.amount * QREWARDS_FEE_SHAREHOLDER_PCT, 100ULL);
        locals.qpayhub = div(input.amount * QREWARDS_FEE_QPAYHUB_PCT, 100ULL);
        locals.perShare = div(locals.shTotal, (uint64)NUMBER_OF_COMPUTORS);
        locals.actualSh = locals.perShare * (uint64)NUMBER_OF_COMPUTORS;
        if (locals.perShare > 0)
        {
            qpi.distributeDividends((sint64)locals.perShare);
        }
        if (locals.qpayhub > 0)
        {
            if (state.get().qpayhubAddress != NULL_ID)
            {
                qpi.transfer(state.get().qpayhubAddress, (sint64)locals.qpayhub);
            }
            else
            {
                locals.qpayhub = 0; // no address set -> burn this share instead
            }
        }
        locals.burnAmt = input.amount - locals.actualSh - locals.qpayhub;
        if (locals.burnAmt > 0)
        {
            qpi.burn((sint64)locals.burnAmt);
        }
    }

    // Split a QU dividend fee: 20% to QREWARDS shareholders, 80% (+ rounding) to the
    // QPAYHUB dividends account (auto-credited QU transfer). Burned if unset.
    struct DistributeDivFee_input { uint64 amount; };
    struct DistributeDivFee_output { };
    struct DistributeDivFee_locals { uint64 shTotal; uint64 perShare; uint64 actualSh; uint64 toQpayhub; };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributeDivFee)
    {
        if (input.amount == 0) return;
        locals.shTotal = div(input.amount * QREWARDS_DIVFEE_SHAREHOLDER_PCT, 100ULL);
        locals.perShare = div(locals.shTotal, (uint64)NUMBER_OF_COMPUTORS);
        locals.actualSh = locals.perShare * (uint64)NUMBER_OF_COMPUTORS;
        if (locals.perShare > 0)
        {
            qpi.distributeDividends((sint64)locals.perShare);
        }
        locals.toQpayhub = input.amount - locals.actualSh;
        if (locals.toQpayhub > 0)
        {
            if (state.get().qpayhubAddress != NULL_ID)
            {
                qpi.transfer(state.get().qpayhubAddress, (sint64)locals.toQpayhub);
            }
            else
            {
                qpi.burn((sint64)locals.toQpayhub);
            }
        }
    }

    // Distribute an asset (token) held by SELF pro-rata to QREWARDS shareholders.
    // distributeDividends() is QU-only, so for the 20% shareholder leg of a *token*
    // dividend fee we iterate the contract share asset and transfer per-share.
    // `distributed` is the amount actually sent out (<= amount; dust/failed transfers
    // stay with SELF and are folded back by the caller to the QPAY wallet).
    struct DistributeTokenToShareholders_input { uint64 assetName; id issuer; uint64 amount; };
    struct DistributeTokenToShareholders_output { uint64 distributed; };
    struct DistributeTokenToShareholders_locals { Asset shareAsset; AssetPossessionIterator iter; uint64 perShare; sint64 held; uint64 send; };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributeTokenToShareholders)
    {
        output.distributed = 0;
        if (input.amount == 0) return;
        locals.perShare = div(input.amount, (uint64)NUMBER_OF_COMPUTORS);
        if (locals.perShare == 0) return;
        locals.shareAsset.assetName = QREWARDS_CONTRACT_ASSET_NAME;
        locals.shareAsset.issuer = NULL_ID;
        locals.iter.begin(locals.shareAsset);
        while (!locals.iter.reachedEnd())
        {
            locals.held = locals.iter.numberOfPossessedShares();
            if (locals.held > 0)
            {
                locals.send = locals.perShare * (uint64)locals.held;
                if (qpi.transferShareOwnershipAndPossession(input.assetName, input.issuer,
                        SELF, SELF, (sint64)locals.send, locals.iter.possessor()) >= 0)
                {
                    output.distributed += locals.send;
                }
            }
            locals.iter.next();
        }
    }

    // Build the live holder -> weight snapshot for a pool into state.snapshot, and return
    // the total weight. Reads holdings straight from the asset ledger; must run within one
    // tick (the ledger iterator cannot be resumed across ticks). Excluded addresses and SELF
    // are skipped. Shared by END_EPOCH distribution and the streamed engine's freeze step.
    struct SnapshotPool_input { uint64 poolId; };
    struct SnapshotPool_output { uint128 totalWeight; }; // exact sum of stored per-holder weights
    struct SnapshotPool_locals
    {
        PoolMeta meta;
        uint64 base;
        uint32 i;
        AssetRule rule;
        Asset asset;
        AssetPossessionIterator iter;
        id holder;
        sint64 held;
        KeyProto proto;
        id exKey;
        uint8 exFlag;
        uint64 pts;
        uint64 mult;
        uint64 add;
        uint64 existing;
        uint64 stored;
        sint64 ridx;
        RecipientEntry rentry;
        uint64 nftBase;
        NFTRule nftRule;
        QBAY::getInfoOfNFTById_input qbayIn;
        QBAY::getInfoOfNFTById_output qbayOut;
        uint64 nftAdd;
    };
    PRIVATE_PROCEDURE_WITH_LOCALS(SnapshotPool)
    {
        output.totalWeight = 0;
        state.mut().snapshot.reset();
        locals.meta = state.get().poolMeta.get(input.poolId);

        // LIST mode: the "snapshot" is the admin's explicit recipient list (current version,
        // weight > 0), not the live ledger. Each wallet appears once (deduped by key).
        if (locals.meta.targetMode == QREWARDS_TARGET_LIST)
        {
            for (locals.ridx = state.get().recipients.nextElementIndex(NULL_INDEX);
                 locals.ridx != NULL_INDEX;
                 locals.ridx = state.get().recipients.nextElementIndex(locals.ridx))
            {
                locals.rentry = state.get().recipients.value(locals.ridx);
                if (locals.rentry.poolId != input.poolId) continue;
                if (locals.rentry.version != locals.meta.listVersion) continue;
                if (locals.rentry.weight == 0) continue;
                if (state.mut().snapshot.set(locals.rentry.wallet, locals.rentry.weight) != NULL_INDEX)
                    output.totalWeight = output.totalWeight + (uint128)locals.rentry.weight;
            }
            return;
        }

        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (!locals.rule.active || locals.rule.unit == 0) continue;
            locals.asset.assetName = locals.rule.assetName;
            locals.asset.issuer = locals.rule.issuer;
            locals.iter.begin(locals.asset);
            while (!locals.iter.reachedEnd())
            {
                locals.held = locals.iter.numberOfPossessedShares();
                locals.holder = locals.iter.possessor();
                if (locals.held > 0 && locals.holder != SELF)
                {
                    setMemory(locals.proto, 0);
                    locals.proto.poolId = input.poolId;
                    locals.proto.wallet = locals.holder;
                    locals.exKey = qpi.K12(locals.proto);
                    locals.exFlag = 0;
                    if (!(state.get().excluded.get(locals.exKey, locals.exFlag) && locals.exFlag))
                    {
                        locals.pts = div((uint64)locals.held, locals.rule.unit);
                        if (locals.pts > 0)
                        {
                            locals.mult = concentrationMultiplier(locals.pts);
                            locals.add = weightContribution(locals.pts, locals.mult, (uint64)locals.rule.weightBps);
                            if (locals.add > 0)
                            {
                                locals.existing = 0;
                                state.get().snapshot.get(locals.holder, locals.existing);
                                locals.stored = satAdd64(locals.existing, locals.add); // per-holder cap
                                // Only count the weight if it is actually stored (an existing key
                                // updates in place; a new key is dropped iff the map is full).
                                if (state.mut().snapshot.set(locals.holder, locals.stored) != NULL_INDEX)
                                {
                                    // totalWeight is the EXACT uint128 sum of stored per-holder
                                    // weights, so sum(payouts) can never exceed the pot.
                                    output.totalWeight = output.totalWeight + (uint128)(locals.stored - locals.existing);
                                }
                            }
                        }
                    }
                }
                locals.iter.next();
            }
        }

        // NFT-backed weight: round-robin spread across QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS
        // epochs (see the constant's comment) instead of re-verifying every registered id's
        // live possessor via a cross-contract QBAY call every single epoch. Entry i is only
        // checked when (i + qpi.epoch()) mod N == 0; a checked entry's weight is scaled by N
        // to keep the long-run average payout identical to checking everyone every epoch.
        locals.nftBase = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            if (mod((uint64)locals.i + qpi.epoch(), (uint64)QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS) != 0) continue;
            locals.nftRule = state.get().nftRegistry.get(locals.nftBase + locals.i);
            if (!locals.nftRule.active) continue;
            locals.qbayIn.NFTId = locals.nftRule.nftId;
            CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
            if (interContractCallError != NoCallError) continue;
            if (locals.qbayOut.possessor == NULL_ID || locals.qbayOut.possessor == SELF) continue;

            setMemory(locals.proto, 0);
            locals.proto.poolId = input.poolId;
            locals.proto.wallet = locals.qbayOut.possessor;
            locals.exKey = qpi.K12(locals.proto);
            locals.exFlag = 0;
            if (state.get().excluded.get(locals.exKey, locals.exFlag) && locals.exFlag) continue;

            locals.nftAdd = (uint64)locals.nftRule.weightPoints * QREWARDS_NFT_SNAPSHOT_SPREAD_EPOCHS;
            locals.existing = 0;
            state.get().snapshot.get(locals.qbayOut.possessor, locals.existing);
            locals.stored = satAdd64(locals.existing, locals.nftAdd);
            if (state.mut().snapshot.set(locals.qbayOut.possessor, locals.stored) != NULL_INDEX)
            {
                output.totalWeight = output.totalWeight + (uint128)(locals.stored - locals.existing);
            }
        }
    }

    // END_EPOCH distribution (distributionMode == QREWARDS_DIST_END_EPOCH): snapshot the pool
    // and pay every currency's pot pro-rata by weight, all in this tick. Rounding dust stays
    // in pot and carries. Fine for a handful of pools; use streamed mode to scale out.
    struct DistributePool_input { uint64 poolId; };
    struct DistributePool_output { };
    struct DistributePool_locals
    {
        PoolMeta meta;
        SnapshotPool_input spi;
        SnapshotPool_output spo;
        uint128 totalWeight;
        uint32 k;
        DivCurrency cur;
        sint64 idx;
        id holder;
        uint64 weight;
        uint64 payout;
        uint64 distributed;
        uint64 frozen;
        uint128 prod;
        uint128 pay128;
    };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributePool)
    {
        locals.meta = state.get().poolMeta.get(input.poolId);
        // (A list-mode pool may have no registered assets, so don't skip on numAssets==0;
        //  an empty snapshot just yields totalWeight 0 and carries the pot.)
        if (!locals.meta.active || locals.meta.paused) return;

        locals.spi.poolId = input.poolId;
        CALL(SnapshotPool, locals.spi, locals.spo);
        locals.totalWeight = locals.spo.totalWeight;

        locals.meta.lastTotalWeight = sat64(locals.totalWeight);
        if (!locals.totalWeight) // == 0
        {
            state.mut().poolMeta.set(input.poolId, locals.meta); // no eligible holders: pots carry
            return;
        }

        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            // Fold in any `distributable` too: it is normally 0 in END_EPOCH mode, but if the owner
            // switched streamed -> END_EPOCH mid-cycle, a pool may have amounts already rolled from
            // pot into distributable; draining both here prevents those funds from being stranded.
            if (locals.cur.pot == 0 && locals.cur.distributable == 0) continue;
            // Checks-effects-interactions: freeze the amount and persist pot = 0 BEFORE paying, so
            // the stored state never shows funds that are mid-transfer. Payouts are computed from the
            // frozen amount; the undistributed remainder (rounding dust + any failed sends) is
            // returned to the pot after the loop. (Mirrors the streamed engine's roll-then-pay.)
            locals.frozen = locals.cur.pot + locals.cur.distributable;
            locals.cur.pot = 0;
            locals.cur.distributable = 0;
            locals.meta.currencies.set(locals.k, locals.cur);
            state.mut().poolMeta.set(input.poolId, locals.meta);

            locals.distributed = 0;
            for (locals.idx = state.get().snapshot.nextElementIndex(NULL_INDEX);
                 locals.idx != NULL_INDEX;
                 locals.idx = state.get().snapshot.nextElementIndex(locals.idx))
            {
                locals.holder = state.get().snapshot.key(locals.idx);
                locals.weight = state.get().snapshot.value(locals.idx);
                if (locals.weight == 0) continue;
                locals.prod = (uint128)locals.frozen * (uint128)locals.weight;
                locals.pay128 = div<uint128>(locals.prod, locals.totalWeight);
                locals.payout = locals.pay128.low; // <= frozen, fits uint64
                if (locals.payout == 0) continue;
                if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
                {
                    if (qpi.transfer(locals.holder, (sint64)locals.payout) >= 0)
                        locals.distributed += locals.payout;
                }
                else
                {
                    if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                            SELF, SELF, (sint64)locals.payout, locals.holder) >= 0)
                        locals.distributed += locals.payout;
                }
            }
            locals.cur.pot = locals.frozen - locals.distributed; // carry rounding dust / failed sends
            locals.meta.currencies.set(locals.k, locals.cur);
            state.mut().poolMeta.set(input.poolId, locals.meta);
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
    }

    // Streamed distribution (distributionMode == QREWARDS_DIST_STREAMED): advance one
    // distribution cycle by up to `streamBatchSize` work units, called from END_TICK. A cycle
    // walks pools in order; for each pool it rolls pot->distributable and freezes the holder
    // snapshot in one work unit, then pays the frozen amounts out over subsequent work units.
    // Cursors live in state, so a cycle resumes exactly across ticks and even across epoch
    // boundaries (it is never reset mid-cycle), which makes it idempotent: no holder is paid
    // twice and no funds are lost (undistributed amounts stay in pot/distributable and carry).
    struct StreamDistribute_input { };
    struct StreamDistribute_output { };
    struct StreamDistribute_locals
    {
        uint32 budget;
        PoolMeta meta;
        SnapshotPool_input spi;
        SnapshotPool_output spo;
        uint32 k;
        DivCurrency cur;
        sint64 idx;
        id holder;
        uint64 weight;
        uint64 payout;
        uint128 prod;
        uint128 pay128;
    };
    PRIVATE_PROCEDURE_WITH_LOCALS(StreamDistribute)
    {
        locals.budget = state.get().streamBatchSize;
        while (locals.budget > 0)
        {
            locals.budget--;
            if (state.get().streamPoolCursor >= state.get().numPools)
            {
                state.mut().cycleActive = 0; // cycle complete
                return;
            }

            if (state.get().streamState == QREWARDS_STREAM_IDLE)
            {
                // Start a pool: roll pot -> distributable, then freeze its snapshot (one unit).
                // (A list-mode pool may have no registered assets; don't skip on numAssets==0.)
                locals.meta = state.get().poolMeta.get(state.get().streamPoolCursor);
                if (!locals.meta.active || locals.meta.paused)
                {
                    state.mut().streamPoolCursor = state.get().streamPoolCursor + 1;
                    continue;
                }
                for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
                {
                    locals.cur = locals.meta.currencies.get(locals.k);
                    locals.cur.distributable += locals.cur.pot;
                    locals.cur.pot = 0;
                    locals.meta.currencies.set(locals.k, locals.cur);
                }
                locals.spi.poolId = state.get().streamPoolCursor;
                CALL(SnapshotPool, locals.spi, locals.spo);
                locals.meta.lastTotalWeight = sat64(locals.spo.totalWeight);
                state.mut().poolMeta.set(state.get().streamPoolCursor, locals.meta);
                state.mut().streamTotalWeightHi = locals.spo.totalWeight.high;
                state.mut().streamTotalWeightLo = locals.spo.totalWeight.low;
                state.mut().streamState = QREWARDS_STREAM_PAYING;
                state.mut().streamCurrencyCursor = 0;
                state.mut().streamCurrencyStarted = 0;
                state.mut().streamPayCursor = NULL_INDEX;
                continue;
            }

            // PAYING
            locals.meta = state.get().poolMeta.get(state.get().streamPoolCursor);
            if (state.get().streamCurrencyCursor >= locals.meta.numCurrencies
                || (state.get().streamTotalWeightHi == 0 && state.get().streamTotalWeightLo == 0))
            {
                state.mut().streamPoolCursor = state.get().streamPoolCursor + 1;
                state.mut().streamState = QREWARDS_STREAM_IDLE;
                continue;
            }
            locals.cur = locals.meta.currencies.get(state.get().streamCurrencyCursor);
            if (!state.get().streamCurrencyStarted)
            {
                state.mut().streamFrozenAmt = locals.cur.distributable;
                state.mut().streamPaidSum = 0;
                state.mut().streamPayCursor = NULL_INDEX;
                state.mut().streamCurrencyStarted = 1;
                if (locals.cur.distributable == 0)
                {
                    // nothing to pay in this currency; advance
                    state.mut().streamCurrencyCursor = state.get().streamCurrencyCursor + 1;
                    state.mut().streamCurrencyStarted = 0;
                }
                continue;
            }
            locals.idx = state.get().snapshot.nextElementIndex(state.get().streamPayCursor);
            if (locals.idx == NULL_INDEX)
            {
                // currency done: carry the undistributed dust, advance to next currency
                locals.cur.distributable = state.get().streamFrozenAmt - state.get().streamPaidSum;
                locals.meta.currencies.set(state.get().streamCurrencyCursor, locals.cur);
                state.mut().poolMeta.set(state.get().streamPoolCursor, locals.meta);
                state.mut().streamCurrencyCursor = state.get().streamCurrencyCursor + 1;
                state.mut().streamCurrencyStarted = 0;
                continue;
            }
            locals.holder = state.get().snapshot.key(locals.idx);
            locals.weight = state.get().snapshot.value(locals.idx);
            state.mut().streamPayCursor = locals.idx;
            if (locals.weight == 0) continue;
            locals.prod = (uint128)state.get().streamFrozenAmt * (uint128)locals.weight;
            locals.pay128 = div<uint128>(locals.prod, uint128(state.get().streamTotalWeightHi, state.get().streamTotalWeightLo));
            locals.payout = locals.pay128.low;
            if (locals.payout == 0) continue;
            if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
            {
                if (qpi.transfer(locals.holder, (sint64)locals.payout) >= 0)
                    state.mut().streamPaidSum = state.get().streamPaidSum + locals.payout;
            }
            else
            {
                if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                        SELF, SELF, (sint64)locals.payout, locals.holder) >= 0)
                    state.mut().streamPaidSum = state.get().streamPaidSum + locals.payout;
            }
        }
    }

public:
    /**************************************/
    /********PROCEDURES (state-changing)***/
    /**************************************/

    struct createPool_input { uint64 label; };
    struct createPool_output { sint32 returnCode; uint64 poolId; };
    struct createPool_locals { PoolMeta meta; DivCurrency qu; DistributeFee_input dfi; DistributeFee_output dfo; QRewardsLogger log; };
    PUBLIC_PROCEDURE_WITH_LOCALS(createPool)
    {
        if ((uint64)qpi.invocationReward() < state.get().createPoolFee)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_INSUFFICIENT_FEE;
            return;
        }
        if (state.get().numPools >= QREWARDS_MAX_POOLS)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_MAX_POOLS_REACHED;
            return;
        }
        // 70/15/15 split of the create fee; any excess seeds the pool's operating balance.
        locals.dfi.amount = state.get().createPoolFee;
        CALL(DistributeFee, locals.dfi, locals.dfo);

        setMemory(locals.meta, 0);
        locals.meta.admin = qpi.invocator();
        locals.meta.label = input.label;
        locals.meta.operatingBalance = (uint64)qpi.invocationReward() - state.get().createPoolFee;
        locals.meta.active = 1;
        locals.meta.paused = 0;
        locals.meta.missedEpochs = 0;
        // currency slot 0 = QU by default
        setMemory(locals.qu, 0);
        locals.qu.assetName = 0;
        locals.qu.issuer = NULL_ID;
        locals.qu.active = 1;
        locals.meta.currencies.set(0, locals.qu);
        locals.meta.numCurrencies = 1;

        output.poolId = state.get().numPools;
        state.mut().poolMeta.set(output.poolId, locals.meta);
        state.mut().numPools = state.get().numPools + 1;

        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_POOL_CREATED, 0 };
        LOG_INFO(locals.log);
    }

    struct addDividendCurrency_input { uint64 poolId; uint64 assetName; id issuer; };
    struct addDividendCurrency_output { sint32 returnCode; uint32 currencyIndex; };
    struct addDividendCurrency_locals { PoolMeta meta; DivCurrency cur; };
    PUBLIC_PROCEDURE_WITH_LOCALS(addDividendCurrency)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (locals.meta.numCurrencies >= QREWARDS_MAX_DIV_CURRENCIES) { output.returnCode = QREWARDS_CURRENCIES_FULL; return; }
        if (input.assetName == 0 || !qpi.isAssetIssued(input.issuer, input.assetName))
        {
            output.returnCode = QREWARDS_ASSET_NOT_ISSUED;
            return;
        }
        setMemory(locals.cur, 0);
        locals.cur.assetName = input.assetName;
        locals.cur.issuer = input.issuer;
        locals.cur.active = 1;
        locals.meta.currencies.set(locals.meta.numCurrencies, locals.cur);
        output.currencyIndex = locals.meta.numCurrencies;
        locals.meta.numCurrencies++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct registerAsset_input { uint64 poolId; uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; };
    struct registerAsset_output { sint32 returnCode; uint32 index; };
    struct registerAsset_locals { PoolMeta meta; AssetRule rule; QRewardsLogger log; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS)
        { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }
        if (!qpi.isAssetIssued(input.issuer, input.assetName)) { output.returnCode = QREWARDS_ASSET_NOT_ISSUED; return; }

        locals.rule.assetName = input.assetName;
        locals.rule.issuer = input.issuer;
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = 1;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + locals.meta.numAssets, locals.rule);
        output.index = locals.meta.numAssets;
        locals.meta.numAssets++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_ASSET_CHANGED, 0 };
        LOG_INFO(locals.log);
    }

    // Batch register up to QREWARDS_MAX_BATCH assets at once. Invalid specs are
    // skipped; output.added reports how many were registered.
    struct registerAssets_input { uint64 poolId; uint32 count; Array<AssetSpec, QREWARDS_MAX_BATCH> specs; };
    struct registerAssets_output { sint32 returnCode; uint32 added; };
    struct registerAssets_locals { PoolMeta meta; AssetSpec spec; AssetRule rule; uint32 i; uint32 n; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAssets)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.added = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }

        locals.n = (input.count > QREWARDS_MAX_BATCH) ? QREWARDS_MAX_BATCH : input.count;
        for (locals.i = 0; locals.i < locals.n; locals.i++)
        {
            if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) break;
            locals.spec = input.specs.get(locals.i);
            if (locals.spec.unit == 0 || locals.spec.weightBps == 0 || locals.spec.weightBps > QREWARDS_MAX_WEIGHT_BPS) continue;
            if (!qpi.isAssetIssued(locals.spec.issuer, locals.spec.assetName)) continue;
            locals.rule.assetName = locals.spec.assetName;
            locals.rule.issuer = locals.spec.issuer;
            locals.rule.unit = locals.spec.unit;
            locals.rule.weightBps = locals.spec.weightBps;
            locals.rule.kind = locals.spec.kind;
            locals.rule.active = 1;
            state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + locals.meta.numAssets, locals.rule);
            locals.meta.numAssets++;
            output.added++;
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Admin-curated NFT-backed weight (QBAY integration, holdings mode only -- see the
    // QREWARDS_MAX_NFT_PER_POOL comment for why QBAY can't tell QREWARDS which NFTs belong
    // together on its own). Registering confirms the id is a real, currently-possessed QBAY NFT
    // (catches admin typos early) but does NOT lock in the possessor -- ComputeEntitlement and
    // SnapshotPool both re-check the live possessor every time weight is computed.
    struct registerNFT_input { uint64 poolId; uint32 nftId; uint32 weightPoints; };
    struct registerNFT_output { sint32 returnCode; uint32 index; };
    struct registerNFT_locals
    {
        PoolMeta meta; NFTRule rule; uint64 base; uint32 i;
        QBAY::getInfoOfNFTById_input qbayIn; QBAY::getInfoOfNFTById_output qbayOut;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerNFT)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.weightPoints == 0) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        if (locals.meta.numNfts >= QREWARDS_MAX_NFT_PER_POOL) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }

        locals.base = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            if (state.get().nftRegistry.get(locals.base + locals.i).nftId == input.nftId)
            { output.returnCode = QREWARDS_INVALID_PARAM; return; } // already registered
        }

        locals.qbayIn.NFTId = input.nftId;
        CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
        if (interContractCallError != NoCallError || locals.qbayOut.possessor == NULL_ID)
        { output.returnCode = QREWARDS_ASSET_NOT_ISSUED; return; }

        locals.rule.nftId = input.nftId;
        locals.rule.weightPoints = input.weightPoints;
        locals.rule.active = 1;
        output.index = locals.meta.numNfts;
        state.mut().nftRegistry.set(locals.base + output.index, locals.rule);
        locals.meta.numNfts++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct updateNFT_input { uint64 poolId; uint32 index; uint32 weightPoints; uint8 active; };
    struct updateNFT_output { sint32 returnCode; };
    struct updateNFT_locals { PoolMeta meta; NFTRule rule; };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateNFT)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numNfts) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.weightPoints == 0) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().nftRegistry.get(input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL + input.index);
        locals.rule.weightPoints = input.weightPoints;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().nftRegistry.set(input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct updateAsset_input { uint64 poolId; uint32 index; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; };
    struct updateAsset_output { sint32 returnCode; };
    struct updateAsset_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS)
        { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct updateWeight_input { uint64 poolId; uint32 index; uint32 weightBps; };
    struct updateWeight_output { sint32 returnCode; };
    struct updateWeight_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateWeight)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.weightBps = input.weightBps;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct setPoolAdmin_input { uint64 poolId; id newAdmin; };
    struct setPoolAdmin_output { sint32 returnCode; };
    struct setPoolAdmin_locals { PoolMeta meta; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setPoolAdmin)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        locals.meta.admin = input.newAdmin;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Exclude (or re-include) an address in a pool. Excluded addresses earn nothing
    // (weight forced to 0), so they neither receive dividends nor dilute others.
    struct setExcludedAddress_input { uint64 poolId; id address; bit excluded; };
    struct setExcludedAddress_output { sint32 returnCode; };
    struct setExcludedAddress_locals { PoolMeta meta; KeyProto proto; id key; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setExcludedAddress)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.address;
        locals.key = qpi.K12(locals.proto);
        if (input.excluded) state.mut().excluded.set(locals.key, 1);
        else state.mut().excluded.removeByKey(locals.key);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // --- Explicit recipient-list distribution (targetMode == LIST) ---
    // A pool admin can switch a pool to pay its dividends to an uploaded wallet list (pro-rata by
    // weight, default equal) instead of to asset holders. The pot still accumulates from deposits
    // and is distributed automatically each epoch by the normal engine; only the recipient set
    // changes. The list is stored and built up in batches.

    // Switch a pool between by-holdings (0, default) and by-list (1) distribution. Admin only.
    struct setTargetMode_input { uint64 poolId; uint8 mode; };
    struct setTargetMode_output { sint32 returnCode; };
    struct setTargetMode_locals { PoolMeta meta; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setTargetMode)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        locals.meta.targetMode = (input.mode == QREWARDS_TARGET_LIST) ? QREWARDS_TARGET_LIST : QREWARDS_TARGET_HOLDINGS;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Add/update (weight > 0) or remove (weight == 0) recipients in a pool's list. For an equal
    // split, pass the same weight (e.g. 1) for every wallet. Deduped by wallet; tagged with the
    // pool's current listVersion. Up to QREWARDS_MAX_RECIP_BATCH per call; call repeatedly for
    // longer lists. A pool's list is hard-capped at QREWARDS_MAX_LIST_PER_POOL (5,000): once full,
    // further NEW wallets are skipped (output.added < count), but existing entries can still be
    // updated or removed. Since the cap is below QREWARDS_SNAPSHOT_CAP, every listed recipient is
    // always paid in full each epoch (nobody is truncated).
    struct addRecipients_input
    {
        uint64 poolId;
        uint32 count;
        Array<id, QREWARDS_MAX_RECIP_BATCH>     wallets;
        Array<uint64, QREWARDS_MAX_RECIP_BATCH> weights;
    };
    struct addRecipients_output { sint32 returnCode; uint32 added; uint32 rejected; uint32 listCount; };
    struct addRecipients_locals { PoolMeta meta; uint32 n; uint32 i; id w; uint64 wt; KeyProto proto; id key; RecipientEntry e; bit existed; };
    PUBLIC_PROCEDURE_WITH_LOCALS(addRecipients)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.added = 0;
        output.rejected = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        locals.n = (input.count > QREWARDS_MAX_RECIP_BATCH) ? QREWARDS_MAX_RECIP_BATCH : input.count;
        for (locals.i = 0; locals.i < locals.n; locals.i++)
        {
            locals.w = input.wallets.get(locals.i);
            locals.wt = input.weights.get(locals.i);
            // Clamp the weight: relative values only need to be modest, and this keeps the
            // pot * weight product (computed in uint128 at distribution) far from overflow.
            if (locals.wt > QREWARDS_MAX_LIST_WEIGHT) locals.wt = QREWARDS_MAX_LIST_WEIGHT;
            setMemory(locals.proto, 0);
            locals.proto.poolId = input.poolId;
            locals.proto.wallet = locals.w;
            locals.key = qpi.K12(locals.proto);
            locals.existed = state.get().recipients.contains(locals.key);
            if (locals.wt == 0)
            {
                if (locals.existed)
                {
                    state.mut().recipients.removeByKey(locals.key); // weight 0 -> remove
                    if (locals.meta.listCount > 0) locals.meta.listCount--;
                }
                continue;
            }
            // Enforce the per-pool cap on NEW wallets only (updates to existing entries are fine).
            if (!locals.existed && locals.meta.listCount >= QREWARDS_MAX_LIST_PER_POOL)
            {
                output.rejected++;
                continue;
            }
            setMemory(locals.e, 0);
            locals.e.wallet = locals.w;
            locals.e.poolId = input.poolId;
            locals.e.weight = locals.wt;
            locals.e.version = locals.meta.listVersion;
            if (state.mut().recipients.set(locals.key, locals.e) != NULL_INDEX)
            {
                if (!locals.existed) locals.meta.listCount++;
                output.added++;
            }
            else
            {
                output.rejected++; // shared map globally full
            }
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.listCount = locals.meta.listCount;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Clear a pool's entire recipient list. Physically removes the pool's entries so their slots in
    // the shared, finite `recipients` map are reclaimed (a bare version bump would leave them as live
    // occupants forever -> a slow leak toward lockout). listVersion is still bumped so any entry not
    // yet compacted is ignored immediately. The scan is O(map size) but admin-initiated and rare.
    struct clearRecipients_input { uint64 poolId; };
    struct clearRecipients_output { sint32 returnCode; uint32 removed; };
    struct clearRecipients_locals { PoolMeta meta; sint64 idx; };
    PUBLIC_PROCEDURE_WITH_LOCALS(clearRecipients)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.removed = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        for (locals.idx = state.get().recipients.nextElementIndex(NULL_INDEX);
             locals.idx != NULL_INDEX;
             locals.idx = state.get().recipients.nextElementIndex(locals.idx))
        {
            if (state.get().recipients.value(locals.idx).poolId == input.poolId)
            {
                state.mut().recipients.removeByIndex(locals.idx); // marks slot reusable; safe mid-iteration
                output.removed++;
            }
        }
        state.mut().recipients.cleanupIfNeeded();
        locals.meta.listVersion++;
        locals.meta.listCount = 0; // list emptied
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Deposit a dividend into one pool/currency. Added to the currency's pot and paid
    // to live holders at END_EPOCH.
    //  - QU currency (slot where assetName==0): QU is taken from the invocation reward.
    //  - asset currency: `amount` of the asset is pulled from the caller into the contract.
    struct depositDividend_input { uint64 poolId; uint32 currencyIndex; uint64 amount; };
    struct depositDividend_output { sint32 returnCode; };
    struct depositDividend_locals
    {
        PoolMeta meta;
        DivCurrency cur;
        uint64 rev;
        uint64 fee;
        bit isQU;
        uint64 qpayPart;
        DistributeTokenToShareholders_input dtsi;
        DistributeTokenToShareholders_output dtso;
        QRewardsLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(depositDividend)
    {
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active || locals.meta.paused || input.currencyIndex >= locals.meta.numCurrencies)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = (!locals.meta.active) ? QREWARDS_POOL_INACTIVE
                               : (locals.meta.paused ? QREWARDS_POOL_PAUSED : QREWARDS_INVALID_INDEX);
            return;
        }
        locals.cur = locals.meta.currencies.get(input.currencyIndex);

        if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
        {
            // QU currency: use the invocation reward.
            locals.rev = (uint64)qpi.invocationReward();
            locals.isQU = 1;
        }
        else
        {
            // Asset currency: refund any QU, pull `amount` of the asset from the caller.
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            if (input.amount == 0) { output.returnCode = QREWARDS_SUCCESS; return; }
            if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                    qpi.invocator(), qpi.invocator(), (sint64)input.amount, SELF) < 0)
            {
                output.returnCode = QREWARDS_TRANSFER_FAILED;
                return;
            }
            locals.rev = input.amount;
            locals.isQU = 0;
        }

        if (locals.rev == 0) { output.returnCode = QREWARDS_SUCCESS; return; }

        // 5% dividend fee, split 80% / 20% (20% -> QREWARDS shareholders for both).
        //  - QU  : whole fee accrues to pendingDivFeeQU; split 80/20 at END_EPOCH.
        //  - Token: distributed inline. 20% to shareholders in the token itself; the rest
        //           (80% + dust + any failed shareholder sends) to the QPAY wallet. No QPAY
        //           wallet set -> no fee taken on tokens.
        locals.fee = div(locals.rev * QREWARDS_DIVIDEND_FEE_PCT, 100ULL);
        if (locals.fee > 0)
        {
            if (locals.isQU)
            {
                state.mut().pendingDivFeeQU += locals.fee;
                locals.rev -= locals.fee;
            }
            else if (state.get().qpayTokenDividendsAddress != NULL_ID)
            {
                locals.dtsi.assetName = locals.cur.assetName;
                locals.dtsi.issuer = locals.cur.issuer;
                locals.dtsi.amount = div(locals.fee * QREWARDS_DIVFEE_SHAREHOLDER_PCT, 100ULL);
                CALL(DistributeTokenToShareholders, locals.dtsi, locals.dtso);
                locals.qpayPart = locals.fee - locals.dtso.distributed;
                if (locals.qpayPart == 0
                    || qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                           SELF, SELF, (sint64)locals.qpayPart, state.get().qpayTokenDividendsAddress) >= 0)
                {
                    locals.rev -= locals.fee;
                }
                else
                {
                    locals.rev -= locals.dtso.distributed;
                }
            }
            // else (token with no QPAY dividends address): no fee taken.
        }

        locals.cur.lifetime += locals.rev;
        locals.cur.pot += locals.rev;
        locals.meta.currencies.set(input.currencyIndex, locals.cur);
        state.mut().poolMeta.set(input.poolId, locals.meta);

        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_DIVIDEND, 0 };
        LOG_INFO(locals.log);
    }

    // Top up a pool's operating balance (the per-epoch operating fee is drawn from it).
    // Open to anyone; QU is taken from the invocation reward.
    struct depositOperating_input { uint64 poolId; };
    struct depositOperating_output { sint32 returnCode; uint64 operatingBalance; };
    struct depositOperating_locals { PoolMeta meta; };
    PUBLIC_PROCEDURE_WITH_LOCALS(depositOperating)
    {
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) // a deactivated pool cannot be revived
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_INACTIVE;
            return;
        }
        if (qpi.invocationReward() > 0)
        {
            locals.meta.operatingBalance += (uint64)qpi.invocationReward();
            state.mut().poolMeta.set(input.poolId, locals.meta);
        }
        output.operatingBalance = locals.meta.operatingBalance;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct setPlatformParams_input { uint64 createPoolFee; uint64 operatingFee; };
    struct setPlatformParams_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setPlatformParams)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().platformOwner) { output.returnCode = QREWARDS_NOT_PLATFORM_OWNER; return; }
        if (input.createPoolFee > QREWARDS_MAX_CREATE_POOL_FEE || input.operatingFee > QREWARDS_MAX_OPERATING_FEE)
        {
            output.returnCode = QREWARDS_INVALID_PARAM;
            return;
        }
        state.mut().createPoolFee = input.createPoolFee;
        state.mut().operatingFee = input.operatingFee;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // The platform owner (and the fee destinations) are hardcoded in INITIALIZE with no setters,
    // so they can never be repointed or transferred after deployment. A compromised owner key
    // therefore cannot hand the role to anyone or redirect fees; rotating the owner requires a
    // code change + redeploy, which is deliberate.

    // Register (or clear) a funding route: a plain QU transfer from the caller is
    // auto-credited to poolId's QU currency via POST_INCOMING_TRANSFER. Lets even
    // lower-index contracts fund a pool with a plain qpi.transfer (no procedure call).
    struct setFundingRoute_input { uint64 poolId; bit clear; };
    struct setFundingRoute_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setFundingRoute)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.clear)
        {
            state.mut().fundingRoute.removeByKey(qpi.invocator());
            output.returnCode = QREWARDS_SUCCESS;
            return;
        }
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        state.mut().fundingRoute.set(qpi.invocator(), input.poolId);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct TransferShareManagementRights_input { Asset asset; sint64 numberOfShares; uint32 newManagingContractIndex; };
    struct TransferShareManagementRights_output { sint64 transferredNumberOfShares; };
    struct TransferShareManagementRights_locals { sint64 result; };
    PUBLIC_PROCEDURE_WITH_LOCALS(TransferShareManagementRights)
    {
        if (qpi.numberOfPossessedShares(input.asset.assetName, input.asset.issuer,
                qpi.invocator(), qpi.invocator(), SELF_INDEX, SELF_INDEX) < input.numberOfShares)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            return;
        }
        locals.result = qpi.releaseShares(input.asset, qpi.invocator(), qpi.invocator(),
            input.numberOfShares, input.newManagingContractIndex, input.newManagingContractIndex,
            qpi.invocationReward());
        if (locals.result < 0)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        else
        {
            output.transferredNumberOfShares = input.numberOfShares;
        }
    }

    // Protocol owner: switch distribution between END_EPOCH (0, default) and streamed (1).
    // Takes effect from the next cycle; an in-progress streamed cycle finishes first. The
    // streamed 24h delay and batch size are fixed (QREWARDS_STREAM_DELAY_TICKS / _BATCH).
    struct setDistributionMode_input { uint8 mode; };
    struct setDistributionMode_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setDistributionMode)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().platformOwner) { output.returnCode = QREWARDS_NOT_PLATFORM_OWNER; return; }
        state.mut().distributionMode = (input.mode == QREWARDS_DIST_STREAMED) ? QREWARDS_DIST_STREAMED : QREWARDS_DIST_END_EPOCH;
        output.returnCode = QREWARDS_SUCCESS;
    }

    /**************************************/
    /********FUNCTIONS (read-only)*********/
    /**************************************/

    // Live weight of a user in a pool (what the END_EPOCH snapshot would record now).
    // Divide by getPool().lastTotalWeight for an approximate share of the next payout.
    struct previewWeight_input { uint64 poolId; id user; };
    struct previewWeight_output { uint64 weight; sint32 returnCode; };
    struct previewWeight_locals { ComputeEntitlement_input cei; ComputeEntitlement_output ceo; };
    PUBLIC_FUNCTION_WITH_LOCALS(previewWeight)
    {
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.cei.poolId = input.poolId;
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        output.weight = locals.ceo.weight;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPool_input { uint64 poolId; };
    struct getPool_output
    {
        id admin;
        uint64 label;
        uint64 operatingBalance;
        uint64 lastTotalWeight;
        uint32 numAssets;
        uint32 missedEpochs;
        uint8 numCurrencies;
        uint8 active;
        uint8 paused;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyAssetName;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyPot;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyLifetime;
        sint32 returnCode;
    };
    struct getPool_locals { PoolMeta meta; uint32 k; DivCurrency cur; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPool)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        output.admin = locals.meta.admin;
        output.label = locals.meta.label;
        output.operatingBalance = locals.meta.operatingBalance;
        output.lastTotalWeight = locals.meta.lastTotalWeight;
        output.numAssets = locals.meta.numAssets;
        output.missedEpochs = locals.meta.missedEpochs;
        output.numCurrencies = locals.meta.numCurrencies;
        output.active = locals.meta.active;
        output.paused = locals.meta.paused;
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            output.currencyAssetName.set(locals.k, locals.cur.assetName); // 0 = QU
            output.currencyPot.set(locals.k, locals.cur.pot);
            output.currencyLifetime.set(locals.k, locals.cur.lifetime);
        }
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPoolAsset_input { uint64 poolId; uint32 index; };
    struct getPoolAsset_output { uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; sint32 returnCode; };
    struct getPoolAsset_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolAsset)
    {
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        output.assetName = locals.rule.assetName;
        output.issuer = locals.rule.issuer;
        output.unit = locals.rule.unit;
        output.weightBps = locals.rule.weightBps;
        output.kind = locals.rule.kind;
        output.active = locals.rule.active;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Return every asset in a pool in one call.
    struct getAllPoolAssets_input { uint64 poolId; };
    struct getAllPoolAssets_output
    {
        Array<AssetRule, QREWARDS_MAX_ASSETS_PER_POOL> assets;
        uint32 count;
        sint32 returnCode;
    };
    struct getAllPoolAssets_locals { PoolMeta meta; uint32 i; uint64 base; AssetRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getAllPoolAssets)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            output.assets.set(locals.i, locals.rule);
        }
        output.count = locals.meta.numAssets;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPoolNFT_input { uint64 poolId; uint32 index; };
    struct getPoolNFT_output { uint32 nftId; uint32 weightPoints; uint8 active; sint32 returnCode; };
    struct getPoolNFT_locals { PoolMeta meta; NFTRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolNFT)
    {
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (input.index >= locals.meta.numNfts) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        locals.rule = state.get().nftRegistry.get(input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL + input.index);
        output.nftId = locals.rule.nftId;
        output.weightPoints = locals.rule.weightPoints;
        output.active = locals.rule.active;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Return every registered NFT id in a pool in one call (so a depositor can audit the NFT-
    // backed weight source the same way getAllPoolAssets lets them audit the asset registry).
    struct getAllPoolNFTs_input { uint64 poolId; };
    struct getAllPoolNFTs_output
    {
        Array<NFTRule, QREWARDS_MAX_NFT_PER_POOL> nfts;
        uint32 count;
        sint32 returnCode;
    };
    struct getAllPoolNFTs_locals { PoolMeta meta; uint32 i; uint64 base; NFTRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getAllPoolNFTs)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            locals.rule = state.get().nftRegistry.get(locals.base + locals.i);
            output.nfts.set(locals.i, locals.rule);
        }
        output.count = locals.meta.numNfts;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPlatform_input { };
    struct getPlatform_output { id platformOwner; id qpayhubAddress; id qpayTokenDividendsAddress; uint64 createPoolFee; uint64 operatingFee; uint32 numPools; uint8 distributionMode; uint32 streamDelayTicks; uint32 streamBatchSize; uint8 cycleActive; };
    PUBLIC_FUNCTION(getPlatform)
    {
        output.platformOwner = state.get().platformOwner;
        output.qpayhubAddress = state.get().qpayhubAddress;
        output.qpayTokenDividendsAddress = state.get().qpayTokenDividendsAddress;
        output.createPoolFee = state.get().createPoolFee;
        output.operatingFee = state.get().operatingFee;
        output.numPools = state.get().numPools;
        output.distributionMode = state.get().distributionMode;
        output.streamDelayTicks = state.get().streamDelayTicks;
        output.streamBatchSize = state.get().streamBatchSize;
        output.cycleActive = state.get().cycleActive;
    }

    struct isExcluded_input { uint64 poolId; id address; };
    struct isExcluded_output { bit excluded; };
    struct isExcluded_locals { KeyProto proto; id key; uint8 flag; };
    PUBLIC_FUNCTION_WITH_LOCALS(isExcluded)
    {
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.address;
        locals.key = qpi.K12(locals.proto);
        locals.flag = 0;
        output.excluded = (state.get().excluded.get(locals.key, locals.flag) && locals.flag) ? 1 : 0;
    }

    struct getFundingRoute_input { id source; };
    struct getFundingRoute_output { uint64 poolId; bit isSet; };
    struct getFundingRoute_locals { uint64 p; };
    PUBLIC_FUNCTION_WITH_LOCALS(getFundingRoute)
    {
        output.poolId = 0;
        output.isSet = 0;
        if (state.get().fundingRoute.get(input.source, locals.p))
        {
            output.poolId = locals.p;
            output.isSet = 1;
        }
    }

    // Paginated list of pools owned by an admin. `offset` skips that many matches;
    // `count` is how many ids this page returned; `totalMatched` is the full total.
    struct getPoolsByAdmin_input { id admin; uint32 offset; };
    struct getPoolsByAdmin_output
    {
        Array<uint64, QREWARDS_PAGE> poolIds;
        uint32 count;
        uint32 totalMatched;
    };
    struct getPoolsByAdmin_locals { uint32 i; uint32 matched; PoolMeta meta; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolsByAdmin)
    {
        setMemory(output, 0);
        locals.matched = 0;
        for (locals.i = 0; locals.i < state.get().numPools; locals.i++)
        {
            locals.meta = state.get().poolMeta.get(locals.i);
            if (locals.meta.admin == input.admin)
            {
                if (locals.matched >= input.offset && output.count < QREWARDS_PAGE)
                {
                    output.poolIds.set(output.count, locals.i);
                    output.count++;
                }
                locals.matched++;
            }
        }
        output.totalMatched = locals.matched;
    }

    /**************************************/
    /************REGISTRATION**************/
    /**************************************/
    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(getPool, 1);
        REGISTER_USER_FUNCTION(getPoolAsset, 2);
        REGISTER_USER_FUNCTION(getAllPoolAssets, 3);
        REGISTER_USER_FUNCTION(getPlatform, 4);
        REGISTER_USER_FUNCTION(getFundingRoute, 5);
        REGISTER_USER_FUNCTION(getPoolsByAdmin, 6);
        REGISTER_USER_FUNCTION(isExcluded, 7);
        REGISTER_USER_FUNCTION(previewWeight, 8);
        REGISTER_USER_FUNCTION(getPoolNFT, 9);
        REGISTER_USER_FUNCTION(getAllPoolNFTs, 10);

        REGISTER_USER_PROCEDURE(createPool, 1);
        REGISTER_USER_PROCEDURE(registerAsset, 2);
        REGISTER_USER_PROCEDURE(updateAsset, 3);
        REGISTER_USER_PROCEDURE(updateWeight, 4);
        REGISTER_USER_PROCEDURE(setPoolAdmin, 5);
        REGISTER_USER_PROCEDURE(depositDividend, 6);
        REGISTER_USER_PROCEDURE(depositOperating, 7);
        REGISTER_USER_PROCEDURE(setPlatformParams, 8);
        REGISTER_USER_PROCEDURE(TransferShareManagementRights, 9);
        REGISTER_USER_PROCEDURE(registerAssets, 10);
        REGISTER_USER_PROCEDURE(addDividendCurrency, 11);
        REGISTER_USER_PROCEDURE(setFundingRoute, 12);
        REGISTER_USER_PROCEDURE(setExcludedAddress, 13);
        REGISTER_USER_PROCEDURE(setDistributionMode, 14);
        REGISTER_USER_PROCEDURE(setTargetMode, 15);
        REGISTER_USER_PROCEDURE(addRecipients, 16);
        REGISTER_USER_PROCEDURE(clearRecipients, 17);
        REGISTER_USER_PROCEDURE(registerNFT, 18);
        REGISTER_USER_PROCEDURE(updateNFT, 19);
    }

    INITIALIZE()
    {
        // Platform owner is HARDCODED (not a first-caller bootstrap) so nobody can claim the role
        // at deploy time and redirect fees. Owner = the QPay wallet
        // (QPAYNOWSWZMGHFEAEVJXGZAVSHABAZDDBDIHTEBOPCOGHRGBCYCUZOHCVLXG). It can call only
        // setPlatformParams (fees) and setDistributionMode (END_EPOCH<->streamed). There is no
        // ownership-transfer function: the owner is fixed for the life of the deployment.
        state.mut().platformOwner = ID(_Q, _P, _A, _Y, _N, _O, _W, _S, _W, _Z, _M, _G, _H, _F, _E, _A, _E, _V, _J, _X, _G, _Z, _A, _V, _S, _H, _A, _B, _A, _Z, _D, _D, _B, _D, _I, _H, _T, _E, _B, _O, _P, _C, _O, _G, _H, _R, _G, _B, _C, _Y, _C, _U, _Z, _O, _H, _C);
        // QPAYHUB contract address = id(29, 0, 0, 0) (receives the 15% create/operating-fee
        // share and the 80% QU dividend-fee leg; its POST_INCOMING_TRANSFER auto-credits plain
        // QU to its feePool). QPAYHUB is CONTRACT_INDEX 29 on the live chain.
        state.mut().qpayhubAddress = id(29, 0, 0, 0);
        // Destination for the 80% leg of the 5% TOKEN dividend fee: the QRaffle charity
        // address (DPQRLSZSSCXIYFIQGBFBXXISDDEBEGQNWNTQUEIFSCUWGHVXJPLFGMYD...), a plain
        // wallet that simply receives the tokens. Hardcoded, no setter (immutable).
        // (The 20% shareholder leg is unchanged.)
        state.mut().qpayTokenDividendsAddress = ID(_D, _P, _Q, _R, _L, _S, _Z, _S, _S, _C, _X, _I, _Y, _F, _I, _Q, _G, _B, _F, _B, _X, _X, _I, _S, _D, _D, _E, _B, _E, _G, _Q, _N, _W, _N, _T, _Q, _U, _E, _I, _F, _S, _C, _U, _W, _G, _H, _V, _X, _J, _P, _L, _F, _G, _M, _Y, _D);
        state.mut().createPoolFee = QREWARDS_DEFAULT_CREATE_FEE;
        state.mut().operatingFee = QREWARDS_DEFAULT_OPERATING_FEE;
        state.mut().pendingDivFeeQU = 0;
        state.mut().numPools = 0;

        // Streamed distribution: dormant by default (END_EPOCH mode). Protocol owner flips it on.
        state.mut().distributionMode = QREWARDS_DIST_END_EPOCH;
        state.mut().streamDelayTicks = QREWARDS_STREAM_DELAY_TICKS;
        state.mut().streamBatchSize = QREWARDS_STREAM_BATCH;
        state.mut().epochStartTick = 0;
        state.mut().lastCycleEpoch = QREWARDS_NO_CYCLE_EPOCH;
        state.mut().cycleActive = 0;
        state.mut().streamState = QREWARDS_STREAM_IDLE;
        state.mut().streamCurrencyStarted = 0;
        state.mut().streamPoolCursor = 0;
        state.mut().streamCurrencyCursor = 0;
        state.mut().streamPayCursor = NULL_INDEX;
        state.mut().streamTotalWeightHi = 0;
        state.mut().streamTotalWeightLo = 0;
        state.mut().streamFrozenAmt = 0;
        state.mut().streamPaidSum = 0;
    }

    BEGIN_EPOCH()
    {
        // Anchor the epoch so streamed mode can wait streamDelayTicks before starting a cycle.
        state.mut().epochStartTick = qpi.tick();
    }

    struct END_EPOCH_locals
    {
        uint32 i;
        uint64 fee;
        uint64 collected;
        PoolMeta meta;
        DistributeFee_input dfi;
        DistributeFee_output dfo;
        DistributeDivFee_input ddfi;
        DistributeDivFee_output ddfo;
        DistributePool_input dpi;
        DistributePool_output dpo;
        uint32 k;
        DivCurrency cur;
        uint64 refundAmt;
    };
    END_EPOCH_WITH_LOCALS()
    {
        // 1) Draw the per-epoch operating fee from each active pool; underfunded pools
        //    are paused, and deactivated after being paused more than QREWARDS_MAX_MISSED_EPOCHS.
        locals.fee = state.get().operatingFee;
        locals.collected = 0;
        for (locals.i = 0; locals.i < state.get().numPools; locals.i++)
        {
            locals.meta = state.get().poolMeta.get(locals.i);
            if (!locals.meta.active) continue;
            if (locals.fee == 0 || locals.meta.operatingBalance >= locals.fee)
            {
                locals.meta.operatingBalance -= locals.fee;
                locals.collected += locals.fee;
                locals.meta.paused = 0;
                locals.meta.missedEpochs = 0;
            }
            else
            {
                locals.meta.paused = 1;
                locals.meta.missedEpochs++;
                if (locals.meta.missedEpochs > QREWARDS_MAX_MISSED_EPOCHS)
                {
                    locals.meta.active = 0; // paused > MAX epochs -> deactivate
                    // Refund the deactivated pool's remaining funds to its admin so nothing is
                    // stranded: each currency's (pot + distributable), plus the leftover
                    // operating balance. Tokens are managed by SELF, so they transfer out.
                    for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
                    {
                        locals.cur = locals.meta.currencies.get(locals.k);
                        locals.refundAmt = locals.cur.pot + locals.cur.distributable;
                        if (locals.refundAmt > 0)
                        {
                            if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
                                qpi.transfer(locals.meta.admin, (sint64)locals.refundAmt);
                            else
                                qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                                    SELF, SELF, (sint64)locals.refundAmt, locals.meta.admin);
                            locals.cur.pot = 0;
                            locals.cur.distributable = 0;
                            locals.meta.currencies.set(locals.k, locals.cur);
                        }
                    }
                    if (locals.meta.operatingBalance > 0)
                    {
                        qpi.transfer(locals.meta.admin, (sint64)locals.meta.operatingBalance);
                        locals.meta.operatingBalance = 0;
                    }
                }
            }
            state.mut().poolMeta.set(locals.i, locals.meta);
        }
        if (locals.collected > 0)
        {
            locals.dfi.amount = locals.collected;
            CALL(DistributeFee, locals.dfi, locals.dfo);
        }

        // 2) Flush the accrued 5% QU dividend fee: 80% QPAYHUB / 20% QREWARDS shareholders.
        if (state.get().pendingDivFeeQU > 0)
        {
            locals.ddfi.amount = state.get().pendingDivFeeQU;
            CALL(DistributeDivFee, locals.ddfi, locals.ddfo);
            state.mut().pendingDivFeeQU = 0;
        }

        // 3) Distribute pots. In END_EPOCH mode, pay every active pool now (one tick). In
        //    streamed mode, END_TICK does it across the epoch, so skip here.
        if (state.get().distributionMode == QREWARDS_DIST_END_EPOCH)
        {
            for (locals.i = 0; locals.i < state.get().numPools; locals.i++)
            {
                locals.dpi.poolId = locals.i;
                CALL(DistributePool, locals.dpi, locals.dpo);
            }
            state.mut().snapshot.reset();
        }

        state.mut().fundingRoute.cleanupIfNeeded();
        state.mut().excluded.cleanupIfNeeded();
        state.mut().recipients.cleanupIfNeeded();
    }

    // Streamed-mode driver. Once per epoch, after streamDelayTicks have elapsed, start a
    // distribution cycle; thereafter advance it a batch per tick until every pool is paid.
    // Dormant (does nothing) in the default END_EPOCH mode.
    struct END_TICK_locals { StreamDistribute_input sdi; StreamDistribute_output sdo; };
    END_TICK_WITH_LOCALS()
    {
        if (state.get().distributionMode != QREWARDS_DIST_STREAMED) return;
        if (!state.get().cycleActive)
        {
            // Start at most one cycle per epoch, once we're far enough into it.
            if (state.get().lastCycleEpoch != qpi.epoch()
                && (qpi.tick() - state.get().epochStartTick) >= state.get().streamDelayTicks)
            {
                state.mut().cycleActive = 1;
                state.mut().streamPoolCursor = 0;
                state.mut().streamState = QREWARDS_STREAM_IDLE;
                state.mut().lastCycleEpoch = qpi.epoch();
            }
        }
        if (state.get().cycleActive)
        {
            CALL(StreamDistribute, locals.sdi, locals.sdo);
        }
    }

    // Credit plain incoming QU to a routed pool's QU pot (slot 0).
    // Only standard wallet transfers and contract qpi.transfers are routed;
    // procedure/other-contract-invocation transfers are handled by depositDividend,
    // so they are skipped here to avoid double counting.
    struct POST_INCOMING_TRANSFER_locals
    {
        uint64 poolId;
        PoolMeta meta;
        DivCurrency cur;
        uint64 rev;
        uint64 fee;
    };
    POST_INCOMING_TRANSFER_WITH_LOCALS()
    {
        if (input.type != TransferType::standardTransaction && input.type != TransferType::qpiTransfer) return;
        if (input.amount <= 0) return;
        if (!state.get().fundingRoute.get(input.sourceId, locals.poolId)) return;
        if (locals.poolId >= state.get().numPools) return;
        locals.meta = state.get().poolMeta.get(locals.poolId);
        if (!locals.meta.active || locals.meta.paused || locals.meta.numCurrencies == 0) return;

        locals.cur = locals.meta.currencies.get(0); // slot 0 is always QU
        locals.rev = (uint64)input.amount;
        // 5% dividend fee -> pendingDivFeeQU (flushed 80/20 at END_EPOCH); 95% to the pot.
        locals.fee = div(locals.rev * QREWARDS_DIVIDEND_FEE_PCT, 100ULL);
        if (locals.fee > 0)
        {
            state.mut().pendingDivFeeQU += locals.fee;
            locals.rev -= locals.fee;
        }
        locals.cur.lifetime += locals.rev;
        locals.cur.pot += locals.rev;
        locals.meta.currencies.set(0, locals.cur);
        state.mut().poolMeta.set(locals.poolId, locals.meta);
    }

    PRE_ACQUIRE_SHARES()
    {
        output.allowTransfer = true;
    }
};
