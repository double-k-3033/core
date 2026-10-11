using namespace QPI;

// QREWARDS (multi-pool) - open ecosystem loyalty & dividend platform.
//
// Anyone can createPool(): each pool has its own creator/admin, its own asset registry and its
// own dividend pots. Deposits (minus a 5% fee) accumulate into per-currency pots and are paid
// out automatically once per epoch -- there is no claim step.
//
// LOCK-AND-JOIN MODEL:
//   A holder LOCKS registered assets by moving their share management rights to QREWARDS
//   (QX.TransferShareManagementRights -> QREWARDS; ownership and possession never change) and
//   calls join(pool) once (small fee, burned into the contract's execution-fee reserve). Only
//   locked shares count. Each epoch the distribution engine walks the pool's MEMBERS -- never the
//   asset ledger -- reading each member's locked balance with one O(1) lookup per asset. Locked
//   shares can only leave QREWARDS management through TransferShareManagementRights, which is
//   refused while a distribution cycle runs, so the same shares cannot be shuttled between members
//   to be measured or paid twice. Outside cycles holders unlock freely; a member whose locked
//   weight is 0 at the next cycle is pruned. Token dividends paid in a registered asset arrive
//   already locked. In list mode the members are an admin-uploaded weighted wallet list.
//
// BOUNDED WORK (no unbounded loops anywhere):
//   * Distribution runs only in END_TICK, at most streamBatchSize work units per tick. Every unit
//     is O(1): one member (<= MAX_ASSETS_PER_POOL lookups and <= MAX_DIV_CURRENCIES transfers) or
//     one pass over a pool's NFT registry (<= MAX_NFT_PER_POOL QBAY lookups). A tick therefore
//     never does more than a fixed amount of work or more than a fixed number of QU transfers.
//   * END_EPOCH only collects operating fees (one O(1) step per pool) and flushes fees.
//   * Member lookups use an index map kept at <= 50% load and keyed by K12(pool, wallet), so
//     join/leave/exclude are O(1) and cannot be degraded by grinding raw wallet ids.
//
// PAYOUT RULE: a cycle first MEASURES every member (weight w1, summed into W), then PAYS each
//   member pot * min(w1, w2) / W where w2 is re-read at payment time. Locked shares cannot move
//   between members during a cycle (unlocking is refused), and a member whose locked balance fell
//   since MEASURE (e.g. by depositing it) is paid on the lower figure, so the sum of payouts can
//   never exceed the frozen pot; any remainder carries to the next cycle.
//
// NEVER PAYS CONTRACTS: contract ids (and NULL_ID) can neither join, be listed, be a pool admin,
//   nor receive shareholder dividends. Paying a contract runs its POST_INCOMING_TRANSFER, which
//   aborts the payer -- and with no contract timeout halts the node -- if that contract is in an
//   error state or not yet constructed. For the same reason QREWARDS itself defines no
//   POST_INCOMING_TRANSFER: QU sent to it before construction (or after a failed IPO) cannot trap
//   the sender. Pools are funded with depositDividend.

constexpr uint32 QREWARDS_MAX_POOLS               = 1024;
constexpr uint32 QREWARDS_MAX_ASSETS_PER_POOL     = 64;
constexpr uint32 QREWARDS_MAX_DIV_CURRENCIES      = 4;          // QU + up to 3 assets per pool
constexpr uint32 QREWARDS_MAX_BATCH               = 16;         // assets per registerAssets call
constexpr uint32 QREWARDS_MAX_RECIP_BATCH         = 16;         // recipients per addRecipients call (MAX_INPUT_SIZE=1024)
constexpr uint64 QREWARDS_MAX_MEMBERS             = 524288;     // 2^19 member/recipient/exclusion entries shared by all pools
constexpr uint64 QREWARDS_MEMBER_INDEX_SIZE       = 1048576;    // 2^20 index slots: <= 50% load keeps every lookup O(1)
constexpr uint32 QREWARDS_MAX_MEMBERS_PER_POOL    = 16384;      // joined holders or listed recipients per pool
constexpr uint32 QREWARDS_MAX_EXCLUSIONS_PER_POOL = 64;
constexpr uint32 QREWARDS_NONE                    = 0xFFFFFFFF; // null slot in the member lists
constexpr uint32 QREWARDS_PAGE                    = 256;        // page size for paginated views
constexpr uint64 QREWARDS_REGISTRY_SIZE           = uint64(QREWARDS_MAX_POOLS) * QREWARDS_MAX_ASSETS_PER_POOL;
constexpr uint64 QREWARDS_BPS                     = 10000;
constexpr uint32 QREWARDS_MAX_WEIGHT_BPS          = 1000000;    // cap weight at 100x
constexpr uint64 QREWARDS_MAX_LIST_WEIGHT         = 1000000000000ULL; // 1e12: per-recipient list weight cap
constexpr uint64 QREWARDS_DEFAULT_CREATE_FEE      = 5000000ULL; // 5M QU to create a pool
constexpr uint64 QREWARDS_DEFAULT_OPERATING_FEE   = 25000ULL;   // 25k QU per pool per epoch
constexpr uint64 QREWARDS_DEFAULT_JOIN_FEE        = 10000ULL;   // per joined member / listed recipient, burned into the fee reserve
// The platformOwner can move each fee within [min, max]. The maxima are pinned to the defaults, so
// fees can only ever be lowered (a single owner key cannot price every pool out). The minima keep
// pool slots and member slots from becoming free to squat.
constexpr uint64 QREWARDS_MIN_CREATE_POOL_FEE     = 1000000ULL;
constexpr uint64 QREWARDS_MAX_CREATE_POOL_FEE     = QREWARDS_DEFAULT_CREATE_FEE;
constexpr uint64 QREWARDS_MAX_OPERATING_FEE       = QREWARDS_DEFAULT_OPERATING_FEE;
constexpr uint64 QREWARDS_MIN_JOIN_FEE            = 1000ULL;
constexpr uint64 QREWARDS_MAX_JOIN_FEE            = QREWARDS_DEFAULT_JOIN_FEE;

// NFT-backed weight (QBAY integration): a pool admin can register individual QBAY NFT ids as an
// additional weight source in holdings mode. QBAY has no notion of "collection" on a minted NFT,
// so the admin curates the id list. Every registered NFT is re-checked against QBAY in every
// cycle (no rotating schedule to game), and an NFT whose QBAY auction is still running does not
// count (QBAY moves possession to the current top bidder while the auction is open).
constexpr uint32 QREWARDS_MAX_NFT_PER_POOL        = 128;
constexpr uint64 QREWARDS_NFT_REGISTRY_SIZE       = uint64(QREWARDS_MAX_POOLS) * QREWARDS_MAX_NFT_PER_POOL;
constexpr uint32 QREWARDS_QBAY_MAX_NFT_ID         = 2097152;    // QBAY_MAX_NUMBER_NFT: larger ids alias in QBAY's array
constexpr uint16 QREWARDS_QX_CONTRACT_INDEX       = 1;          // only QX may hand share management rights to QREWARDS

constexpr uint64 QREWARDS_FEE_SHAREHOLDER_PCT     = 70;         // create/operating fee split: 70% QREWARDS shareholders
constexpr uint64 QREWARDS_FEE_QPAYHUB_PCT         = 15;         // 15% QPAYHUB (rest burned)
constexpr uint32 QREWARDS_MAX_MISSED_EPOCHS       = 2;          // paused this many epochs, then closed
constexpr uint64 QREWARDS_DIVIDEND_FEE_PCT        = 5;          // 5% fee skimmed from every dividend deposit
constexpr uint64 QREWARDS_DIVFEE_QPAYHUB_PCT      = 80;         // of the 5% fee: 80% QPAYHUB (QU) / QPAY wallet (token)
constexpr uint64 QREWARDS_DIVFEE_SHAREHOLDER_PCT  = 20;         // of the 5% fee: 20% QREWARDS shareholders
constexpr uint64 QREWARDS_CONTRACT_ASSET_NAME     = 19230739006837329ULL; // packed "QREWARD" (contract shares, issuer NULL_ID)

// Pool lifecycle (PoolMeta.status).
constexpr uint8  QREWARDS_POOL_FREE               = 0;          // slot unused / released for reuse
constexpr uint8  QREWARDS_POOL_ACTIVE             = 1;
constexpr uint8  QREWARDS_POOL_CLOSING            = 2;          // operating fee lapsed: pays out its pots to members, then is released
// Per-pool distribution target (PoolMeta.targetMode), fixed once the pool has members or deposits.
constexpr uint8  QREWARDS_TARGET_HOLDINGS         = 0;          // members are joined holders, weighted by locked holdings
constexpr uint8  QREWARDS_TARGET_LIST             = 1;          // members are an admin-uploaded weighted wallet list
// Member-entry status (MemberEntry.status).
constexpr uint8  QREWARDS_ENTRY_FREE              = 0;
constexpr uint8  QREWARDS_ENTRY_MEMBER            = 1;          // joined holder (holdings mode)
constexpr uint8  QREWARDS_ENTRY_LISTED            = 2;          // admin-listed recipient (list mode)
constexpr uint8  QREWARDS_ENTRY_EXCLUDED          = 3;          // barred by the admin (holdings mode)

// Distribution engine (END_TICK). One cycle per epoch, after a fixed delay into the epoch.
constexpr uint32 QREWARDS_STREAM_DELAY_TICKS      = 86400;      // ~24h at ~1 tick/s
constexpr uint32 QREWARDS_STREAM_BATCH            = 256;        // O(1) work units per END_TICK
constexpr uint16 QREWARDS_NO_CYCLE_EPOCH          = 0xFFFF;
constexpr uint8  QREWARDS_PHASE_IDLE              = 0;          // pick the next pool, freeze its pots
constexpr uint8  QREWARDS_PHASE_NFT_MEASURE       = 1;          // credit registered NFTs to their member possessors
constexpr uint8  QREWARDS_PHASE_MEASURE           = 2;          // one member per unit: record w1, sum W, prune w1 == 0
constexpr uint8  QREWARDS_PHASE_NFT_VERIFY        = 3;          // drop NFT weight whose possessor changed since NFT_MEASURE
constexpr uint8  QREWARDS_PHASE_PAY               = 4;          // one member per unit: pay pot * min(w1, w2) / W
constexpr uint8  QREWARDS_PHASE_FINISH            = 5;          // carry the remainder back into the pots
constexpr uint8  QREWARDS_PHASE_FREE              = 6;          // closing pool: release one member entry per unit

// return codes
constexpr sint32 QREWARDS_SUCCESS                 = 0;
constexpr sint32 QREWARDS_NOT_ADMIN               = 1;
constexpr sint32 QREWARDS_INVALID_PARAM           = 2;
constexpr sint32 QREWARDS_REGISTRY_FULL           = 3;
constexpr sint32 QREWARDS_ASSET_NOT_ISSUED        = 4;
constexpr sint32 QREWARDS_INVALID_INDEX           = 5;
constexpr sint32 QREWARDS_POOL_NOT_FOUND          = 6;
constexpr sint32 QREWARDS_POOL_INACTIVE           = 7;
constexpr sint32 QREWARDS_MAX_POOLS_REACHED       = 8;
constexpr sint32 QREWARDS_INSUFFICIENT_FEE        = 9;
constexpr sint32 QREWARDS_NOT_PLATFORM_OWNER      = 10;
constexpr sint32 QREWARDS_CURRENCIES_FULL         = 11;
constexpr sint32 QREWARDS_TRANSFER_FAILED         = 12;
constexpr sint32 QREWARDS_POOL_PAUSED             = 13;
constexpr sint32 QREWARDS_NOT_MEMBER              = 14;
constexpr sint32 QREWARDS_ALREADY_MEMBER          = 15;
constexpr sint32 QREWARDS_POOL_FULL               = 16;
constexpr sint32 QREWARDS_EXCLUDED                = 17;
constexpr sint32 QREWARDS_NO_WEIGHT               = 18;
constexpr sint32 QREWARDS_CONTRACT_NOT_ALLOWED    = 19;
constexpr sint32 QREWARDS_WRONG_MODE              = 20;
constexpr sint32 QREWARDS_POOL_BUSY               = 21;

// log types
constexpr uint32 QREWARDS_LOG_SUCCESS             = 0;
constexpr uint32 QREWARDS_LOG_POOL_CREATED        = 1;
constexpr uint32 QREWARDS_LOG_DISTRIBUTED         = 2;
constexpr uint32 QREWARDS_LOG_DIVIDEND            = 3;
constexpr uint32 QREWARDS_LOG_ASSET_CHANGED       = 4;
constexpr uint32 QREWARDS_LOG_MEMBER_JOINED       = 5;

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

    // One admin-curated QBAY NFT id counted toward a pool's weight in holdings mode. weightPoints
    // is flat (no concentration tier); rarer ids can simply be registered with more points.
    // measuredSlot/measuredCycle remember which member the NFT was credited to in the current
    // cycle so NFT_VERIFY can withdraw it if the NFT moved before payment.
    struct NFTRule
    {
        uint32 nftId;         // QBAY global NFT id (< QREWARDS_QBAY_MAX_NFT_ID)
        uint32 weightPoints;  // flat weight contributed while this id is held
        uint32 measuredSlot;
        uint32 measuredCycle;
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
    struct DivCurrency
    {
        uint64 assetName;
        id     issuer;
        uint64 pot;           // revenue accumulating for the next cycle (plus carried remainders)
        uint64 distributable; // revenue frozen for the cycle currently paying this pool
        uint64 lifetime;      // cumulative revenue ever deposited (stat)
        uint8  active;
    };

    struct PoolMeta
    {
        id     admin;
        uint64 label;             // short packed name (optional)
        uint64 operatingBalance;  // QU reserve for the per-epoch operating fee
        uint64 lastTotalWeight;   // total measured weight of the last completed cycle (for front ends)
        uint32 numAssets;
        uint32 numNfts;
        uint32 missedEpochs;      // consecutive epochs the operating fee could not be paid
        uint32 memberCount;       // MEMBER/LISTED entries
        uint32 excludedCount;     // EXCLUDED entries
        uint32 headSlot;          // first entry of this pool's member list (QREWARDS_NONE if empty)
        uint32 generation;        // bumped every time the slot is (re)used by createPool
        uint8  numCurrencies;
        uint8  status;            // QREWARDS_POOL_*
        uint8  paused;            // 1 = underfunded, not distributing
        uint8  targetMode;        // QREWARDS_TARGET_*
        Array<DivCurrency, QREWARDS_MAX_DIV_CURRENCIES> currencies;
    };

    // One entry of the shared member slab. Each pool links its entries into a doubly linked list
    // (prev/next) so insertion and removal are O(1); free slots are chained through `next`.
    struct MemberEntry
    {
        id     wallet;
        uint64 listWeight;      // list mode: admin-set weight
        uint64 measuredWeight;  // w1 recorded in MEASURE of cycle measureCycle
        uint64 nftMeasured;     // NFT points credited in NFT_MEASURE of cycle nftCycle
        uint64 nftVerified;     // nftMeasured minus NFTs that moved before payment
        uint32 prev;
        uint32 next;
        uint32 measureCycle;
        uint32 nftCycle;
        uint16 poolId;
        uint8  status;          // QREWARDS_ENTRY_*
    };

    // Composite key for the member index: K12(poolId, wallet).
    struct KeyProto
    {
        uint64 poolId;
        id     wallet;
    };

    struct StateData
    {
        Array<MemberEntry, QREWARDS_MAX_MEMBERS> members;
        HashMap<uint64, uint32, QREWARDS_MEMBER_INDEX_SIZE> memberIndex; // K12(poolId, wallet).u64._0 -> slot
        uint32 memberHighWater;  // slots below this have been handed out at least once
        uint32 memberFreeHead;   // released slots, chained through MemberEntry.next
        uint32 totalEntries;
        uint64 pendingDivFeeQU;  // QU dividend-fee skim, flushed 80/20 at END_EPOCH
        Array<PoolMeta, QREWARDS_MAX_POOLS> poolMeta;
        Array<AssetRule, QREWARDS_REGISTRY_SIZE> registry;       // pool p asset i at p*MAX_ASSETS+i
        Array<NFTRule, QREWARDS_NFT_REGISTRY_SIZE> nftRegistry;   // pool p NFT i at p*MAX_NFT_PER_POOL+i
        Array<uint32, QREWARDS_MAX_POOLS> freePools;             // released pool slots, reused by createPool
        uint32 numFreePools;
        uint32 numPools;          // pool slots ever handed out (ids are < numPools)
        id platformOwner;
        // Receives the 15% QPAYHUB share of create/operating fees and the 80% QPAYHUB share of the
        // 5% QU dividend fee: the QPAYHUB contract (CONTRACT_INDEX 29), a live contract whose
        // POST_INCOMING_TRANSFER credits plain QU to its feePool.
        id qpayhubAddress;
        // Receives the 80% leg of the 5% TOKEN dividend fee (QPAYHUB's feePool is QU-only): the
        // QRaffle charity address, a plain wallet. Hardcoded in INITIALIZE, no setter.
        id qpayTokenDividendsAddress;
        uint64 createPoolFee;
        uint64 operatingFee;      // per pool per epoch
        uint64 joinFee;           // per joined member / listed recipient

        // --- Distribution engine ---
        uint32 streamDelayTicks;     // ticks into the epoch before a cycle may start
        uint32 streamBatchSize;      // work units per END_TICK
        uint32 epochStartTick;       // qpi.tick() captured at BEGIN_EPOCH
        uint16 lastCycleEpoch;       // epoch in which the last cycle was started
        uint8  cycleActive;
        uint8  streamPhase;
        uint32 cycleId;              // incremented per cycle; 0 never matches a live cycle
        uint32 streamPoolCursor;     // pool being processed
        uint32 streamMemberCursor;   // next member slot to visit in MEASURE/PAY
        uint64 streamTotalWeightHi;  // W of the current pool, as hi/lo (uint128 has no default ctor)
        uint64 streamTotalWeightLo;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> streamPaid; // paid so far per currency of the current pool
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

    // Saturating uint128 -> uint64.
    inline static uint64 sat64(uint128 v)
    {
        return (v.high != 0) ? 0xFFFFFFFFFFFFFFFFULL : v.low;
    }

    // One asset's weight contribution: pts * mult * weightBps / BPS^2, computed in 128-bit
    // and saturated to UINT64_MAX so the multiplication can never overflow 64 bits.
    inline static uint64 weightContribution(uint64 pts, uint64 mult, uint64 weightBps)
    {
        return sat64(div<uint128>(uint128(pts) * uint128(mult) * uint128(weightBps),
                                  uint128(QREWARDS_BPS * QREWARDS_BPS)));
    }

    // Saturating a + b in uint64 (capping a weight can only under-pay, never over-draw).
    inline static uint64 satAdd64(uint64 a, uint64 b)
    {
        return (a + b < a) ? 0xFFFFFFFFFFFFFFFFULL : a + b;
    }

    inline static uint64 min64(uint64 a, uint64 b)
    {
        return (a < b) ? a : b;
    }

    // NULL_ID and every contract id -- current or future -- have their upper 192 bits zero. Such
    // ids are never paid: a contract's POST_INCOMING_TRANSFER can abort the payer.
    inline static bit isReservedId(id x)
    {
        return x.u64._1 == 0 && x.u64._2 == 0 && x.u64._3 == 0;
    }

    // Order-preserving key of a qpi.year()/month()/... date (year is 0..99 since 2000).
    inline static uint64 dateKey(uint64 y, uint64 mo, uint64 d, uint64 h, uint64 mi, uint64 s)
    {
        return ((((y * 16 + mo) * 32 + d) * 32 + h) * 64 + mi) * 64 + s;
    }

    // A QBAY NFT counts only when no auction is running on it: QBAY hands possession to the
    // current top bidder while an auction is open and never resets statusOfAuction, judging
    // auctions by their end date instead (see QBAY::getNumberOfNFTForUser).
    inline static bit nftSettled(uint8 statusOfAuction, uint64 endKey, uint64 nowKey)
    {
        return statusOfAuction == 0 || nowKey > endKey;
    }

    // Look up the entry of (poolId, wallet). `taken` is set if the index key is occupied by a
    // different entry (a 64-bit key collision): such a wallet cannot be added to that pool.
    struct FindEntry_input { uint64 poolId; id wallet; };
    struct FindEntry_output { uint32 slot; uint64 key; uint8 status; bit taken; };
    struct FindEntry_locals { KeyProto proto; id hash; uint32 found; MemberEntry e; };
    PRIVATE_FUNCTION_WITH_LOCALS(FindEntry)
    {
        output.slot = QREWARDS_NONE;
        output.status = QREWARDS_ENTRY_FREE;
        output.taken = 0;
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.wallet;
        locals.hash = qpi.K12(locals.proto);
        output.key = locals.hash.u64._0;
        if (state.get().memberIndex.get(output.key, locals.found))
        {
            locals.e = state.get().members.get(locals.found);
            if (locals.e.status != QREWARDS_ENTRY_FREE && (uint64)locals.e.poolId == input.poolId && locals.e.wallet == input.wallet)
            {
                output.slot = locals.found;
                output.status = locals.e.status;
            }
            else
            {
                output.taken = 1;
            }
        }
    }

    // Locked asset weight of `wallet` in a pool: sum over the pool's active registered assets of
    // (held/unit) x concentrationMultiplier x weightBps / BPS^2, where `held` is the wallet's
    // shares managed by QREWARDS (locked). Shares under any other management do not count: they
    // could move between members mid-cycle. O(numAssets) <= O(QREWARDS_MAX_ASSETS_PER_POOL) lookups.
    struct ComputeAssetWeight_input { uint64 poolId; id wallet; };
    struct ComputeAssetWeight_output { uint64 weight; };
    struct ComputeAssetWeight_locals
    {
        PoolMeta meta;
        uint32 i;
        uint64 base;
        AssetRule rule;
        sint64 held;
        uint64 pts;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ComputeAssetWeight)
    {
        output.weight = 0;
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (!locals.rule.active || locals.rule.unit == 0) continue;
            locals.held = qpi.numberOfPossessedShares(locals.rule.assetName, locals.rule.issuer,
                input.wallet, input.wallet, SELF_INDEX, SELF_INDEX);
            if (locals.held <= 0) continue;
            locals.pts = div((uint64)locals.held, locals.rule.unit);
            if (locals.pts == 0) continue;
            output.weight = satAdd64(output.weight,
                weightContribution(locals.pts, concentrationMultiplier(locals.pts), (uint64)locals.rule.weightBps));
        }
    }

    // Live NFT weight of `wallet` in a pool: points of registered NFTs it possesses with no
    // running auction. O(numNfts) <= O(QREWARDS_MAX_NFT_PER_POOL) QBAY lookups. Used by join and
    // previewWeight; the engine credits NFTs per pool in NFT_MEASURE instead.
    struct ComputeNftWeight_input { uint64 poolId; id wallet; };
    struct ComputeNftWeight_output { uint64 weight; };
    struct ComputeNftWeight_locals
    {
        PoolMeta meta;
        uint32 i;
        uint64 base;
        uint64 nowKey;
        NFTRule rule;
        QBAY::getInfoOfNFTById_input qbayIn;
        QBAY::getInfoOfNFTById_output qbayOut;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ComputeNftWeight)
    {
        output.weight = 0;
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.numNfts == 0) return;
        locals.nowKey = dateKey(qpi.year(), qpi.month(), qpi.day(), qpi.hour(), qpi.minute(), qpi.second());
        locals.base = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            locals.rule = state.get().nftRegistry.get(locals.base + locals.i);
            if (!locals.rule.active) continue;
            locals.qbayIn.NFTId = locals.rule.nftId;
            CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
            if (interContractCallError != NoCallError) continue;
            if (locals.qbayOut.possessor != input.wallet) continue;
            if (!nftSettled(locals.qbayOut.statusOfAuction,
                    dateKey(locals.qbayOut.yearAuctionEnded, locals.qbayOut.monthAuctionEnded, locals.qbayOut.dayAuctionEnded,
                            locals.qbayOut.hourAuctionEnded, locals.qbayOut.minuteAuctionEnded, locals.qbayOut.secondAuctionEnded),
                    locals.nowKey)) continue;
            output.weight = satAdd64(output.weight, (uint64)locals.rule.weightPoints);
        }
    }

    // Insert an entry for (poolId, wallet) at the head of the pool's member list. O(1). Fails
    // (slot = NONE) if the index key is already present or the shared slab is full. Callers that
    // hold a PoolMeta copy must re-read it afterwards (counts and headSlot change here).
    struct AddEntry_input { uint64 poolId; id wallet; uint8 status; uint64 listWeight; uint64 key; };
    struct AddEntry_output { uint32 slot; };
    struct AddEntry_locals { PoolMeta meta; MemberEntry e; MemberEntry n; uint32 slot; };
    PRIVATE_PROCEDURE_WITH_LOCALS(AddEntry)
    {
        output.slot = QREWARDS_NONE;
        if (state.get().memberIndex.contains(input.key)) return;
        if (state.get().memberFreeHead != QREWARDS_NONE)
        {
            locals.slot = state.get().memberFreeHead;
            state.mut().memberFreeHead = state.get().members.get(locals.slot).next;
        }
        else if (state.get().memberHighWater < QREWARDS_MAX_MEMBERS)
        {
            locals.slot = state.get().memberHighWater;
            state.mut().memberHighWater = state.get().memberHighWater + 1;
        }
        else
        {
            return;
        }
        if (state.mut().memberIndex.set(input.key, locals.slot) == NULL_INDEX)
        {
            // Cannot happen below 100% index load; give the slot back to the free list.
            setMemory(locals.e, 0);
            locals.e.next = state.get().memberFreeHead;
            state.mut().members.set(locals.slot, locals.e);
            state.mut().memberFreeHead = locals.slot;
            return;
        }

        locals.meta = state.get().poolMeta.get(input.poolId);
        setMemory(locals.e, 0);
        locals.e.wallet = input.wallet;
        locals.e.poolId = (uint16)input.poolId;
        locals.e.status = input.status;
        locals.e.listWeight = input.listWeight;
        locals.e.prev = QREWARDS_NONE;
        locals.e.next = locals.meta.headSlot;
        if (locals.meta.headSlot != QREWARDS_NONE)
        {
            locals.n = state.get().members.get(locals.meta.headSlot);
            locals.n.prev = locals.slot;
            state.mut().members.set(locals.meta.headSlot, locals.n);
        }
        state.mut().members.set(locals.slot, locals.e);
        locals.meta.headSlot = locals.slot;
        if (input.status == QREWARDS_ENTRY_EXCLUDED) locals.meta.excludedCount++;
        else locals.meta.memberCount++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        state.mut().totalEntries = state.get().totalEntries + 1;
        output.slot = locals.slot;
    }

    // Unlink and release an entry. O(1). If the distribution engine's cursor points at it, the
    // cursor advances to the next entry, so removals between or during engine steps are safe.
    struct RemoveEntry_input { uint32 slot; };
    struct RemoveEntry_output { };
    struct RemoveEntry_locals { PoolMeta meta; MemberEntry e; MemberEntry n; KeyProto proto; id hash; };
    PRIVATE_PROCEDURE_WITH_LOCALS(RemoveEntry)
    {
        locals.e = state.get().members.get(input.slot);
        if (locals.e.status == QREWARDS_ENTRY_FREE) return;
        locals.meta = state.get().poolMeta.get(locals.e.poolId);
        if (locals.e.prev != QREWARDS_NONE)
        {
            locals.n = state.get().members.get(locals.e.prev);
            locals.n.next = locals.e.next;
            state.mut().members.set(locals.e.prev, locals.n);
        }
        else
        {
            locals.meta.headSlot = locals.e.next;
        }
        if (locals.e.next != QREWARDS_NONE)
        {
            locals.n = state.get().members.get(locals.e.next);
            locals.n.prev = locals.e.prev;
            state.mut().members.set(locals.e.next, locals.n);
        }
        if (locals.e.status == QREWARDS_ENTRY_EXCLUDED)
        {
            if (locals.meta.excludedCount > 0) locals.meta.excludedCount--;
        }
        else if (locals.meta.memberCount > 0)
        {
            locals.meta.memberCount--;
        }
        state.mut().poolMeta.set(locals.e.poolId, locals.meta);

        if (state.get().cycleActive && state.get().streamPoolCursor == (uint32)locals.e.poolId
            && state.get().streamMemberCursor == input.slot)
        {
            state.mut().streamMemberCursor = locals.e.next;
        }

        setMemory(locals.proto, 0);
        locals.proto.poolId = locals.e.poolId;
        locals.proto.wallet = locals.e.wallet;
        locals.hash = qpi.K12(locals.proto);
        state.mut().memberIndex.removeByKey(locals.hash.u64._0);

        setMemory(locals.e, 0);
        locals.e.next = state.get().memberFreeHead;
        state.mut().members.set(input.slot, locals.e);
        state.mut().memberFreeHead = input.slot;
        if (state.get().totalEntries > 0) state.mut().totalEntries = state.get().totalEntries - 1;
    }

    // Pay `amount` QU pro-rata to QREWARDS shareholders, skipping contract possessors (paying a
    // contract runs its POST_INCOMING_TRANSFER, which can abort this contract). Replaces
    // qpi.distributeDividends, which pays every possessor including contracts. Bounded by the 676
    // contract shares. `paid` is what actually left the contract; the caller handles the rest.
    struct PayShareholdersQU_input { uint64 amount; };
    struct PayShareholdersQU_output { uint64 paid; };
    struct PayShareholdersQU_locals { Asset shareAsset; AssetPossessionIterator iter; uint64 perShare; sint64 held; uint64 send; };
    PRIVATE_PROCEDURE_WITH_LOCALS(PayShareholdersQU)
    {
        output.paid = 0;
        locals.perShare = div(input.amount, (uint64)NUMBER_OF_COMPUTORS);
        if (locals.perShare == 0) return;
        locals.shareAsset.assetName = QREWARDS_CONTRACT_ASSET_NAME;
        locals.shareAsset.issuer = NULL_ID;
        locals.iter.begin(locals.shareAsset);
        while (!locals.iter.reachedEnd())
        {
            locals.held = locals.iter.numberOfPossessedShares();
            if (locals.held > 0 && !isReservedId(locals.iter.possessor()))
            {
                locals.send = locals.perShare * (uint64)locals.held;
                if (qpi.transfer(locals.iter.possessor(), (sint64)locals.send) >= 0)
                {
                    output.paid += locals.send;
                }
            }
            locals.iter.next();
        }
    }

    // Split a QU fee (create/operating): 70% to QREWARDS shareholders, 15% to QPAYHUB, the rest
    // (15% + rounding + shares held by contracts) burned into this contract's fee reserve.
    struct DistributeFee_input { uint64 amount; };
    struct DistributeFee_output { };
    struct DistributeFee_locals { PayShareholdersQU_input psi; PayShareholdersQU_output pso; uint64 qpayhub; uint64 burnAmt; };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributeFee)
    {
        if (input.amount == 0) return;
        locals.psi.amount = div(input.amount * QREWARDS_FEE_SHAREHOLDER_PCT, 100ULL);
        CALL(PayShareholdersQU, locals.psi, locals.pso);
        locals.qpayhub = div(input.amount * QREWARDS_FEE_QPAYHUB_PCT, 100ULL);
        if (locals.qpayhub > 0)
        {
            if (state.get().qpayhubAddress == NULL_ID
                || qpi.transfer(state.get().qpayhubAddress, (sint64)locals.qpayhub) < 0)
            {
                locals.qpayhub = 0; // not sent -> burned with the rest
            }
        }
        locals.burnAmt = input.amount - locals.pso.paid - locals.qpayhub;
        if (locals.burnAmt > 0)
        {
            qpi.burn((sint64)locals.burnAmt);
        }
    }

    // Split a QU dividend fee: 20% to QREWARDS shareholders, the rest (80% + rounding + shares
    // held by contracts) to QPAYHUB. Burned if QPAYHUB cannot be paid.
    struct DistributeDivFee_input { uint64 amount; };
    struct DistributeDivFee_output { };
    struct DistributeDivFee_locals { PayShareholdersQU_input psi; PayShareholdersQU_output pso; uint64 toQpayhub; };
    PRIVATE_PROCEDURE_WITH_LOCALS(DistributeDivFee)
    {
        if (input.amount == 0) return;
        locals.psi.amount = div(input.amount * QREWARDS_DIVFEE_SHAREHOLDER_PCT, 100ULL);
        CALL(PayShareholdersQU, locals.psi, locals.pso);
        locals.toQpayhub = input.amount - locals.pso.paid;
        if (locals.toQpayhub > 0)
        {
            if (state.get().qpayhubAddress == NULL_ID
                || qpi.transfer(state.get().qpayhubAddress, (sint64)locals.toQpayhub) < 0)
            {
                qpi.burn((sint64)locals.toQpayhub);
            }
        }
    }

    // Distribute an asset (token) held by SELF pro-rata to QREWARDS shareholders, skipping
    // contract possessors (they could never move QREWARDS-managed shares). `distributed` is the
    // amount actually sent out; the caller folds the rest to the QPAY wallet.
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
            if (locals.held > 0 && !isReservedId(locals.iter.possessor()))
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

    // One O(1) unit of the distribution engine (see the phase constants). END_TICK calls this at
    // most streamBatchSize times per tick. A unit is: skipping/freezing one pool, one member
    // (<= MAX_ASSETS_PER_POOL lookups, <= MAX_DIV_CURRENCIES transfers), one pass over a pool's NFT
    // registry (<= MAX_NFT_PER_POOL QBAY lookups), finishing a pool, or releasing one entry.
    struct StreamStep_input { };
    struct StreamStep_output { };
    struct StreamStep_locals
    {
        PoolMeta meta;
        MemberEntry e;
        NFTRule rule;
        DivCurrency cur;
        uint32 pool;
        uint32 slot;
        uint32 k;
        uint32 i;
        uint64 nftBase;
        uint64 w;
        uint64 eff;
        uint64 amt;
        uint64 carry;
        uint64 nowKey;
        bit anyFunds;
        bit holdsNft;
        uint128 totalWeight;
        ComputeAssetWeight_input cawi;
        ComputeAssetWeight_output cawo;
        FindEntry_input fei;
        FindEntry_output feo;
        RemoveEntry_input rei;
        RemoveEntry_output reo;
        QBAY::getInfoOfNFTById_input qbayIn;
        QBAY::getInfoOfNFTById_output qbayOut;
    };
    PRIVATE_PROCEDURE_WITH_LOCALS(StreamStep)
    {
        locals.pool = state.get().streamPoolCursor;

        if (state.get().streamPhase == QREWARDS_PHASE_IDLE)
        {
            if (locals.pool >= state.get().numPools)
            {
                state.mut().cycleActive = 0; // cycle complete
                return;
            }
            locals.meta = state.get().poolMeta.get(locals.pool);
            if (locals.meta.status == QREWARDS_POOL_FREE
                || (locals.meta.status == QREWARDS_POOL_ACTIVE && locals.meta.paused))
            {
                state.mut().streamPoolCursor = locals.pool + 1;
                return;
            }
            locals.anyFunds = 0;
            for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
            {
                locals.cur = locals.meta.currencies.get(locals.k);
                if (locals.cur.pot > 0 || locals.cur.distributable > 0) locals.anyFunds = 1;
            }
            if (locals.meta.status == QREWARDS_POOL_CLOSING && !locals.anyFunds)
            {
                state.mut().streamPhase = QREWARDS_PHASE_FREE;
                return;
            }
            if (!locals.anyFunds || locals.meta.memberCount == 0)
            {
                state.mut().streamPoolCursor = locals.pool + 1; // nothing to pay / nobody to pay
                return;
            }
            // Freeze the pots for this cycle; later deposits accumulate in pot for the next one.
            for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
            {
                locals.cur = locals.meta.currencies.get(locals.k);
                locals.cur.distributable += locals.cur.pot;
                locals.cur.pot = 0;
                locals.meta.currencies.set(locals.k, locals.cur);
                state.mut().streamPaid.set(locals.k, 0);
            }
            state.mut().poolMeta.set(locals.pool, locals.meta);
            state.mut().streamTotalWeightHi = 0;
            state.mut().streamTotalWeightLo = 0;
            // Members inserted after this point land before the cursor and are not measured
            // this cycle, so joining mid-cycle cannot capture an already frozen pot.
            state.mut().streamMemberCursor = locals.meta.headSlot;
            state.mut().streamPhase = (locals.meta.targetMode == QREWARDS_TARGET_HOLDINGS && locals.meta.numNfts > 0)
                ? QREWARDS_PHASE_NFT_MEASURE : QREWARDS_PHASE_MEASURE;
            return;
        }

        locals.meta = state.get().poolMeta.get(locals.pool);
        locals.nftBase = (uint64)locals.pool * (uint64)QREWARDS_MAX_NFT_PER_POOL;

        if (state.get().streamPhase == QREWARDS_PHASE_NFT_MEASURE)
        {
            locals.nowKey = dateKey(qpi.year(), qpi.month(), qpi.day(), qpi.hour(), qpi.minute(), qpi.second());
            for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
            {
                locals.rule = state.get().nftRegistry.get(locals.nftBase + locals.i);
                locals.rule.measuredCycle = 0;
                if (locals.rule.active)
                {
                    locals.qbayIn.NFTId = locals.rule.nftId;
                    CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
                    if (interContractCallError == NoCallError && locals.qbayOut.possessor != NULL_ID
                        && nftSettled(locals.qbayOut.statusOfAuction,
                               dateKey(locals.qbayOut.yearAuctionEnded, locals.qbayOut.monthAuctionEnded, locals.qbayOut.dayAuctionEnded,
                                       locals.qbayOut.hourAuctionEnded, locals.qbayOut.minuteAuctionEnded, locals.qbayOut.secondAuctionEnded),
                               locals.nowKey))
                    {
                        locals.fei.poolId = locals.pool;
                        locals.fei.wallet = locals.qbayOut.possessor;
                        CALL(FindEntry, locals.fei, locals.feo);
                        if (locals.feo.slot != QREWARDS_NONE && locals.feo.status == QREWARDS_ENTRY_MEMBER)
                        {
                            locals.e = state.get().members.get(locals.feo.slot);
                            if (locals.e.nftCycle != state.get().cycleId)
                            {
                                locals.e.nftMeasured = 0;
                                locals.e.nftVerified = 0;
                                locals.e.nftCycle = state.get().cycleId;
                            }
                            locals.e.nftMeasured = satAdd64(locals.e.nftMeasured, (uint64)locals.rule.weightPoints);
                            locals.e.nftVerified = satAdd64(locals.e.nftVerified, (uint64)locals.rule.weightPoints);
                            state.mut().members.set(locals.feo.slot, locals.e);
                            locals.rule.measuredSlot = locals.feo.slot;
                            locals.rule.measuredCycle = state.get().cycleId;
                        }
                    }
                }
                state.mut().nftRegistry.set(locals.nftBase + locals.i, locals.rule);
            }
            state.mut().streamPhase = QREWARDS_PHASE_MEASURE;
            return;
        }

        if (state.get().streamPhase == QREWARDS_PHASE_MEASURE)
        {
            locals.slot = state.get().streamMemberCursor;
            if (locals.slot == QREWARDS_NONE)
            {
                state.mut().streamMemberCursor = locals.meta.headSlot;
                state.mut().streamPhase = (locals.meta.targetMode == QREWARDS_TARGET_HOLDINGS && locals.meta.numNfts > 0)
                    ? QREWARDS_PHASE_NFT_VERIFY : QREWARDS_PHASE_PAY;
                return;
            }
            locals.e = state.get().members.get(locals.slot);
            locals.w = 0;
            if (locals.e.status == QREWARDS_ENTRY_MEMBER)
            {
                locals.cawi.poolId = locals.pool;
                locals.cawi.wallet = locals.e.wallet;
                CALL(ComputeAssetWeight, locals.cawi, locals.cawo);
                locals.w = locals.cawo.weight;
                if (locals.e.nftCycle == state.get().cycleId) locals.w = satAdd64(locals.w, locals.e.nftMeasured);
                if (locals.w == 0)
                {
                    // Holds nothing that counts any more: prune (frees the slot, advances the cursor).
                    locals.rei.slot = locals.slot;
                    CALL(RemoveEntry, locals.rei, locals.reo);
                    return;
                }
            }
            else if (locals.e.status == QREWARDS_ENTRY_LISTED)
            {
                locals.w = locals.e.listWeight;
            }
            if (locals.w > 0)
            {
                locals.e.measuredWeight = locals.w;
                locals.e.measureCycle = state.get().cycleId;
                state.mut().members.set(locals.slot, locals.e);
                locals.totalWeight = uint128(state.get().streamTotalWeightHi, state.get().streamTotalWeightLo) + uint128(locals.w);
                state.mut().streamTotalWeightHi = locals.totalWeight.high;
                state.mut().streamTotalWeightLo = locals.totalWeight.low;
            }
            state.mut().streamMemberCursor = locals.e.next;
            return;
        }

        if (state.get().streamPhase == QREWARDS_PHASE_NFT_VERIFY)
        {
            locals.nowKey = dateKey(qpi.year(), qpi.month(), qpi.day(), qpi.hour(), qpi.minute(), qpi.second());
            for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
            {
                locals.rule = state.get().nftRegistry.get(locals.nftBase + locals.i);
                if (locals.rule.measuredCycle != state.get().cycleId) continue;
                locals.e = state.get().members.get(locals.rule.measuredSlot);
                // A slot freed and reused since NFT_MEASURE has nftCycle 0 and is left alone.
                if (locals.e.status != QREWARDS_ENTRY_MEMBER || (uint32)locals.e.poolId != locals.pool
                    || locals.e.nftCycle != state.get().cycleId) continue;
                locals.qbayIn.NFTId = locals.rule.nftId;
                CALL_OTHER_CONTRACT_FUNCTION(QBAY, getInfoOfNFTById, locals.qbayIn, locals.qbayOut);
                locals.holdsNft = (interContractCallError == NoCallError && locals.qbayOut.possessor == locals.e.wallet
                    && nftSettled(locals.qbayOut.statusOfAuction,
                           dateKey(locals.qbayOut.yearAuctionEnded, locals.qbayOut.monthAuctionEnded, locals.qbayOut.dayAuctionEnded,
                                   locals.qbayOut.hourAuctionEnded, locals.qbayOut.minuteAuctionEnded, locals.qbayOut.secondAuctionEnded),
                           locals.nowKey));
                if (!locals.holdsNft)
                {
                    locals.e.nftVerified = (locals.e.nftVerified > (uint64)locals.rule.weightPoints)
                        ? locals.e.nftVerified - (uint64)locals.rule.weightPoints : 0;
                    state.mut().members.set(locals.rule.measuredSlot, locals.e);
                }
            }
            state.mut().streamMemberCursor = locals.meta.headSlot;
            state.mut().streamPhase = QREWARDS_PHASE_PAY;
            return;
        }

        if (state.get().streamPhase == QREWARDS_PHASE_PAY)
        {
            locals.slot = state.get().streamMemberCursor;
            if (locals.slot == QREWARDS_NONE)
            {
                state.mut().streamPhase = QREWARDS_PHASE_FINISH;
                return;
            }
            locals.e = state.get().members.get(locals.slot);
            state.mut().streamMemberCursor = locals.e.next;
            if (locals.e.measureCycle != state.get().cycleId) return; // joined after MEASURE
            if (isReservedId(locals.e.wallet)) return;                  // never pay contracts
            if (locals.e.status == QREWARDS_ENTRY_MEMBER)
            {
                locals.cawi.poolId = locals.pool;
                locals.cawi.wallet = locals.e.wallet;
                CALL(ComputeAssetWeight, locals.cawi, locals.cawo);
                locals.w = locals.cawo.weight;
                if (locals.e.nftCycle == state.get().cycleId) locals.w = satAdd64(locals.w, locals.e.nftVerified);
            }
            else if (locals.e.status == QREWARDS_ENTRY_LISTED)
            {
                locals.w = locals.e.listWeight;
            }
            else
            {
                return; // excluded since MEASURE
            }
            locals.eff = min64(locals.e.measuredWeight, locals.w);
            if (locals.eff == 0) return;
            locals.totalWeight = uint128(state.get().streamTotalWeightHi, state.get().streamTotalWeightLo);
            if (!locals.totalWeight) return;
            for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
            {
                locals.cur = locals.meta.currencies.get(locals.k);
                if (locals.cur.distributable == 0) continue;
                // eff <= w1 and sum(w1) == W, so this never exceeds distributable; the clamp to the
                // unpaid remainder keeps the pot solvent even so.
                locals.amt = div<uint128>(uint128(locals.cur.distributable) * uint128(locals.eff), locals.totalWeight).low;
                locals.amt = min64(locals.amt, locals.cur.distributable - state.get().streamPaid.get(locals.k));
                if (locals.amt == 0) continue;
                if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
                {
                    if (qpi.transfer(locals.e.wallet, (sint64)locals.amt) >= 0)
                        state.mut().streamPaid.set(locals.k, state.get().streamPaid.get(locals.k) + locals.amt);
                }
                else
                {
                    if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                            SELF, SELF, (sint64)locals.amt, locals.e.wallet) >= 0)
                        state.mut().streamPaid.set(locals.k, state.get().streamPaid.get(locals.k) + locals.amt);
                }
            }
            return;
        }

        if (state.get().streamPhase == QREWARDS_PHASE_FINISH)
        {
            locals.totalWeight = uint128(state.get().streamTotalWeightHi, state.get().streamTotalWeightLo);
            locals.anyFunds = 0;
            for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
            {
                locals.cur = locals.meta.currencies.get(locals.k);
                locals.carry = locals.cur.distributable - state.get().streamPaid.get(locals.k);
                locals.cur.distributable = 0;
                // A closing pool's final cycle paid its members; whatever is left (rounding dust and
                // the share of members whose weight fell mid-cycle) is burned / sent to the QPAY wallet
                // rather than carried forever. Without any measured weight, it waits for members.
                if (locals.meta.status == QREWARDS_POOL_CLOSING && locals.totalWeight && locals.carry > 0)
                {
                    if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
                    {
                        if (qpi.burn((sint64)locals.carry) >= 0) locals.carry = 0;
                    }
                    else if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                                 SELF, SELF, (sint64)locals.carry, state.get().qpayTokenDividendsAddress) >= 0)
                    {
                        locals.carry = 0;
                    }
                }
                locals.cur.pot += locals.carry;
                if (locals.cur.pot > 0) locals.anyFunds = 1;
                locals.meta.currencies.set(locals.k, locals.cur);
            }
            locals.meta.lastTotalWeight = sat64(locals.totalWeight);
            state.mut().poolMeta.set(locals.pool, locals.meta);
            if (locals.meta.status == QREWARDS_POOL_CLOSING && !locals.anyFunds)
            {
                state.mut().streamPhase = QREWARDS_PHASE_FREE;
                return;
            }
            state.mut().streamPhase = QREWARDS_PHASE_IDLE;
            state.mut().streamPoolCursor = locals.pool + 1;
            return;
        }

        if (state.get().streamPhase == QREWARDS_PHASE_FREE)
        {
            if (locals.meta.headSlot != QREWARDS_NONE)
            {
                locals.rei.slot = locals.meta.headSlot;
                CALL(RemoveEntry, locals.rei, locals.reo);
                return;
            }
            // All entries released: refund the operating balance to the admin and free the slot.
            if (locals.meta.operatingBalance > 0)
            {
                if (isReservedId(locals.meta.admin))
                    qpi.burn((sint64)locals.meta.operatingBalance);
                else
                    qpi.transfer(locals.meta.admin, (sint64)locals.meta.operatingBalance);
            }
            locals.meta.operatingBalance = 0;
            locals.meta.status = QREWARDS_POOL_FREE;
            locals.meta.paused = 0;
            locals.meta.numAssets = 0;
            locals.meta.numNfts = 0;
            locals.meta.numCurrencies = 0;
            locals.meta.memberCount = 0;
            locals.meta.excludedCount = 0;
            locals.meta.headSlot = QREWARDS_NONE;
            state.mut().poolMeta.set(locals.pool, locals.meta);
            state.mut().freePools.set(state.get().numFreePools, locals.pool);
            state.mut().numFreePools = state.get().numFreePools + 1;
            state.mut().streamPhase = QREWARDS_PHASE_IDLE;
            state.mut().streamPoolCursor = locals.pool + 1;
            return;
        }
    }

public:
    /**************************************/
    /********PROCEDURES (state-changing)***/
    /**************************************/

    // Create a pool. The invocation reward must cover createPoolFee (split 70/15/15); any excess
    // seeds the pool's operating balance. targetMode is chosen here, atomically with creation, so
    // a pool can never be switched to list mode under its depositors. Released pool slots are
    // reused (generation is bumped). Contracts cannot create pools.
    struct createPool_input { uint64 label; uint8 targetMode; };
    struct createPool_output { sint32 returnCode; uint64 poolId; };
    struct createPool_locals
    {
        PoolMeta meta;
        DivCurrency qu;
        DistributeFee_input dfi;
        DistributeFee_output dfo;
        uint32 slot;
        uint32 generation;
        QRewardsLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(createPool)
    {
        if (isReservedId(qpi.invocator()))
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_CONTRACT_NOT_ALLOWED;
            return;
        }
        if ((uint64)qpi.invocationReward() < state.get().createPoolFee)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_INSUFFICIENT_FEE;
            return;
        }
        if (input.targetMode != QREWARDS_TARGET_HOLDINGS && input.targetMode != QREWARDS_TARGET_LIST)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_INVALID_PARAM;
            return;
        }
        if (state.get().numFreePools > 0)
        {
            state.mut().numFreePools = state.get().numFreePools - 1;
            locals.slot = state.get().freePools.get(state.get().numFreePools);
        }
        else if (state.get().numPools < QREWARDS_MAX_POOLS)
        {
            locals.slot = state.get().numPools;
            state.mut().numPools = state.get().numPools + 1;
        }
        else
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_MAX_POOLS_REACHED;
            return;
        }
        locals.generation = state.get().poolMeta.get(locals.slot).generation + 1;

        locals.dfi.amount = state.get().createPoolFee;
        CALL(DistributeFee, locals.dfi, locals.dfo);

        setMemory(locals.meta, 0);
        locals.meta.admin = qpi.invocator();
        locals.meta.label = input.label;
        locals.meta.operatingBalance = (uint64)qpi.invocationReward() - state.get().createPoolFee;
        locals.meta.status = QREWARDS_POOL_ACTIVE;
        locals.meta.targetMode = input.targetMode;
        locals.meta.headSlot = QREWARDS_NONE;
        locals.meta.generation = locals.generation;
        // currency slot 0 = QU
        setMemory(locals.qu, 0);
        locals.qu.active = 1;
        locals.meta.currencies.set(0, locals.qu);
        locals.meta.numCurrencies = 1;
        state.mut().poolMeta.set(locals.slot, locals.meta);

        output.poolId = locals.slot;
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_POOL_CREATED, 0 };
        LOG_INFO(locals.log);
    }

    struct addDividendCurrency_input { uint64 poolId; uint64 assetName; id issuer; };
    struct addDividendCurrency_output { sint32 returnCode; uint32 currencyIndex; };
    struct addDividendCurrency_locals { PoolMeta meta; DivCurrency cur; uint32 k; };
    PUBLIC_PROCEDURE_WITH_LOCALS(addDividendCurrency)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (locals.meta.numCurrencies >= QREWARDS_MAX_DIV_CURRENCIES) { output.returnCode = QREWARDS_CURRENCIES_FULL; return; }
        if (input.assetName == 0 || !qpi.isAssetIssued(input.issuer, input.assetName))
        {
            output.returnCode = QREWARDS_ASSET_NOT_ISSUED;
            return;
        }
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            if (locals.cur.assetName == input.assetName && locals.cur.issuer == input.issuer)
            {
                output.returnCode = QREWARDS_INVALID_PARAM; // already a currency of this pool
                return;
            }
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

    // Register a qualifying asset. The same asset cannot be registered twice in a pool.
    struct registerAsset_input { uint64 poolId; uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; };
    struct registerAsset_output { sint32 returnCode; uint32 index; };
    struct registerAsset_locals { PoolMeta meta; AssetRule rule; uint64 base; uint32 i; QRewardsLogger log; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS)
        { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }
        if (!qpi.isAssetIssued(input.issuer, input.assetName)) { output.returnCode = QREWARDS_ASSET_NOT_ISSUED; return; }
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (locals.rule.assetName == input.assetName && locals.rule.issuer == input.issuer)
            { output.returnCode = QREWARDS_INVALID_PARAM; return; } // duplicate
        }

        locals.rule.assetName = input.assetName;
        locals.rule.issuer = input.issuer;
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = 1;
        state.mut().registry.set(locals.base + locals.meta.numAssets, locals.rule);
        output.index = locals.meta.numAssets;
        locals.meta.numAssets++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_ASSET_CHANGED, 0 };
        LOG_INFO(locals.log);
    }

    // Batch register up to QREWARDS_MAX_BATCH assets at once. Invalid or duplicate specs are
    // skipped; output.added reports how many were registered.
    struct registerAssets_input { uint64 poolId; uint32 count; Array<AssetSpec, QREWARDS_MAX_BATCH> specs; };
    struct registerAssets_output { sint32 returnCode; uint32 added; };
    struct registerAssets_locals { PoolMeta meta; AssetSpec spec; AssetRule rule; uint64 base; uint32 i; uint32 j; uint32 n; bit duplicate; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAssets)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.added = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }

        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        locals.n = (input.count > QREWARDS_MAX_BATCH) ? QREWARDS_MAX_BATCH : input.count;
        for (locals.i = 0; locals.i < locals.n; locals.i++)
        {
            if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) break;
            locals.spec = input.specs.get(locals.i);
            if (locals.spec.unit == 0 || locals.spec.weightBps == 0 || locals.spec.weightBps > QREWARDS_MAX_WEIGHT_BPS) continue;
            if (!qpi.isAssetIssued(locals.spec.issuer, locals.spec.assetName)) continue;
            locals.duplicate = 0;
            for (locals.j = 0; locals.j < locals.meta.numAssets; locals.j++)
            {
                locals.rule = state.get().registry.get(locals.base + locals.j);
                if (locals.rule.assetName == locals.spec.assetName && locals.rule.issuer == locals.spec.issuer) locals.duplicate = 1;
            }
            if (locals.duplicate) continue;
            locals.rule.assetName = locals.spec.assetName;
            locals.rule.issuer = locals.spec.issuer;
            locals.rule.unit = locals.spec.unit;
            locals.rule.weightBps = locals.spec.weightBps;
            locals.rule.kind = locals.spec.kind;
            locals.rule.active = 1;
            state.mut().registry.set(locals.base + locals.meta.numAssets, locals.rule);
            locals.meta.numAssets++;
            output.added++;
        }
        if (output.added > 0) state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Remove a registered asset (swap-remove, so registry slots are reused).
    struct removeAsset_input { uint64 poolId; uint32 index; };
    struct removeAsset_output { sint32 returnCode; };
    struct removeAsset_locals { PoolMeta meta; uint64 base; };
    PUBLIC_PROCEDURE_WITH_LOCALS(removeAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        locals.meta.numAssets--;
        if (input.index != locals.meta.numAssets)
        {
            state.mut().registry.set(locals.base + input.index, state.get().registry.get(locals.base + locals.meta.numAssets));
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Register a QBAY NFT id as a weight source (holdings mode). The id must be below QBAY's NFT
    // capacity (larger ids alias in QBAY's array), not already registered, and currently minted.
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.weightPoints == 0 || input.nftId >= QREWARDS_QBAY_MAX_NFT_ID) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
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

        setMemory(locals.rule, 0);
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numNfts) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.weightPoints == 0) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().nftRegistry.get(input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL + input.index);
        locals.rule.weightPoints = input.weightPoints;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().nftRegistry.set(input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Remove a registered NFT (swap-remove, so registry slots are reused).
    struct removeNFT_input { uint64 poolId; uint32 index; };
    struct removeNFT_output { sint32 returnCode; };
    struct removeNFT_locals { PoolMeta meta; uint64 base; };
    PUBLIC_PROCEDURE_WITH_LOCALS(removeNFT)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numNfts) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        locals.base = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        locals.meta.numNfts--;
        if (input.index != locals.meta.numNfts)
        {
            state.mut().nftRegistry.set(locals.base + input.index, state.get().nftRegistry.get(locals.base + locals.meta.numNfts));
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.weightBps = input.weightBps;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Hand the pool to a new admin. NULL_ID and contract ids are rejected: the admin receives the
    // operating-balance refund when the pool closes, and contracts are never paid.
    struct setPoolAdmin_input { uint64 poolId; id newAdmin; };
    struct setPoolAdmin_output { sint32 returnCode; };
    struct setPoolAdmin_locals { PoolMeta meta; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setPoolAdmin)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status == QREWARDS_POOL_FREE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (isReservedId(input.newAdmin)) { output.returnCode = QREWARDS_CONTRACT_NOT_ALLOWED; return; }
        locals.meta.admin = input.newAdmin;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Exclude (or re-include) an address in a holdings-mode pool. An excluded address cannot join;
    // a current member is converted in place and earns nothing from then on. Capped at
    // QREWARDS_MAX_EXCLUSIONS_PER_POOL per pool, so one pool cannot exhaust shared capacity.
    struct setExcludedAddress_input { uint64 poolId; id address; bit excluded; };
    struct setExcludedAddress_output { sint32 returnCode; };
    struct setExcludedAddress_locals
    {
        PoolMeta meta; MemberEntry e;
        FindEntry_input fei; FindEntry_output feo;
        AddEntry_input aei; AddEntry_output aeo;
        RemoveEntry_input rei; RemoveEntry_output reo;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(setExcludedAddress)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (locals.meta.targetMode != QREWARDS_TARGET_HOLDINGS) { output.returnCode = QREWARDS_WRONG_MODE; return; }
        if (isReservedId(input.address)) { output.returnCode = QREWARDS_INVALID_PARAM; return; } // can never join anyway

        locals.fei.poolId = input.poolId;
        locals.fei.wallet = input.address;
        CALL(FindEntry, locals.fei, locals.feo);
        output.returnCode = QREWARDS_SUCCESS;
        if (input.excluded)
        {
            if (locals.feo.status == QREWARDS_ENTRY_EXCLUDED) return;
            if (locals.meta.excludedCount >= QREWARDS_MAX_EXCLUSIONS_PER_POOL) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }
            if (locals.feo.slot != QREWARDS_NONE)
            {
                // Current member: flip in place. Stays linked; the engine skips EXCLUDED entries.
                locals.e = state.get().members.get(locals.feo.slot);
                locals.e.status = QREWARDS_ENTRY_EXCLUDED;
                state.mut().members.set(locals.feo.slot, locals.e);
                locals.meta.memberCount--;
                locals.meta.excludedCount++;
                state.mut().poolMeta.set(input.poolId, locals.meta);
                return;
            }
            if (locals.feo.taken) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }
            locals.aei.poolId = input.poolId;
            locals.aei.wallet = input.address;
            locals.aei.status = QREWARDS_ENTRY_EXCLUDED;
            locals.aei.listWeight = 0;
            locals.aei.key = locals.feo.key;
            CALL(AddEntry, locals.aei, locals.aeo);
            if (locals.aeo.slot == QREWARDS_NONE) output.returnCode = QREWARDS_REGISTRY_FULL;
        }
        else if (locals.feo.status == QREWARDS_ENTRY_EXCLUDED)
        {
            locals.rei.slot = locals.feo.slot;
            CALL(RemoveEntry, locals.rei, locals.reo);
        }
    }

    // Change a pool's target mode. Only allowed during setup -- before the pool has ever received
    // a deposit and while it has no entries -- so an admin can never switch a funded holdings pool
    // to a self-serving list. (The mode can also be chosen directly in createPool.)
    struct setTargetMode_input { uint64 poolId; uint8 mode; };
    struct setTargetMode_output { sint32 returnCode; };
    struct setTargetMode_locals { PoolMeta meta; DivCurrency cur; uint32 k; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setTargetMode)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.mode != QREWARDS_TARGET_HOLDINGS && input.mode != QREWARDS_TARGET_LIST) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        if (locals.meta.memberCount > 0 || locals.meta.excludedCount > 0) { output.returnCode = QREWARDS_POOL_BUSY; return; }
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            if (locals.cur.lifetime > 0 || locals.cur.pot > 0 || locals.cur.distributable > 0)
            { output.returnCode = QREWARDS_POOL_BUSY; return; }
        }
        locals.meta.targetMode = input.mode;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // --- List mode: an admin-uploaded weighted wallet list instead of joined holders ---

    // Add/update (weight > 0) or remove (weight == 0) recipients. Every NEW recipient costs
    // joinFee, taken from the invocation reward (unused reward is refunded) and burned into the
    // fee reserve, like a join. NULL_ID and contract ids are rejected. Capped per pool at
    // QREWARDS_MAX_MEMBERS_PER_POOL.
    struct addRecipients_input
    {
        uint64 poolId;
        uint32 count;
        Array<id, QREWARDS_MAX_RECIP_BATCH>     wallets;
        Array<uint64, QREWARDS_MAX_RECIP_BATCH> weights;
    };
    struct addRecipients_output { sint32 returnCode; uint32 added; uint32 rejected; uint32 listCount; };
    struct addRecipients_locals
    {
        PoolMeta meta; MemberEntry e;
        uint32 n; uint32 i; id w; uint64 wt; uint64 budget; uint64 charged;
        FindEntry_input fei; FindEntry_output feo;
        AddEntry_input aei; AddEntry_output aeo;
        RemoveEntry_input rei; RemoveEntry_output reo;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(addRecipients)
    {
        output.added = 0;
        output.rejected = 0;
        output.listCount = 0;
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE || qpi.invocator() != locals.meta.admin
            || locals.meta.targetMode != QREWARDS_TARGET_LIST)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = (locals.meta.status != QREWARDS_POOL_ACTIVE) ? QREWARDS_POOL_INACTIVE
                              : ((qpi.invocator() != locals.meta.admin) ? QREWARDS_NOT_ADMIN : QREWARDS_WRONG_MODE);
            return;
        }
        locals.budget = (uint64)qpi.invocationReward();
        locals.charged = 0;
        locals.n = (input.count > QREWARDS_MAX_RECIP_BATCH) ? QREWARDS_MAX_RECIP_BATCH : input.count;
        for (locals.i = 0; locals.i < locals.n; locals.i++)
        {
            locals.w = input.wallets.get(locals.i);
            locals.wt = input.weights.get(locals.i);
            if (locals.wt > QREWARDS_MAX_LIST_WEIGHT) locals.wt = QREWARDS_MAX_LIST_WEIGHT;
            if (isReservedId(locals.w)) { output.rejected++; continue; }
            locals.fei.poolId = input.poolId;
            locals.fei.wallet = locals.w;
            CALL(FindEntry, locals.fei, locals.feo);
            if (locals.wt == 0)
            {
                if (locals.feo.slot != QREWARDS_NONE)
                {
                    locals.rei.slot = locals.feo.slot;
                    CALL(RemoveEntry, locals.rei, locals.reo);
                }
                continue;
            }
            if (locals.feo.slot != QREWARDS_NONE)
            {
                locals.e = state.get().members.get(locals.feo.slot);
                locals.e.listWeight = locals.wt;
                state.mut().members.set(locals.feo.slot, locals.e);
                output.added++;
                continue;
            }
            if (state.get().poolMeta.get(input.poolId).memberCount >= QREWARDS_MAX_MEMBERS_PER_POOL
                || locals.budget < state.get().joinFee || locals.feo.taken)
            {
                output.rejected++;
                continue;
            }
            locals.aei.poolId = input.poolId;
            locals.aei.wallet = locals.w;
            locals.aei.status = QREWARDS_ENTRY_LISTED;
            locals.aei.listWeight = locals.wt;
            locals.aei.key = locals.feo.key;
            CALL(AddEntry, locals.aei, locals.aeo);
            if (locals.aeo.slot == QREWARDS_NONE) { output.rejected++; continue; }
            locals.budget -= state.get().joinFee;
            locals.charged += state.get().joinFee;
            output.added++;
        }
        if (locals.charged > 0) qpi.burn((sint64)locals.charged);
        if (locals.budget > 0) qpi.transfer(qpi.invocator(), (sint64)locals.budget);
        output.listCount = state.get().poolMeta.get(input.poolId).memberCount;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Remove every recipient of a list-mode pool. Bounded by QREWARDS_MAX_MEMBERS_PER_POOL.
    struct clearRecipients_input { uint64 poolId; };
    struct clearRecipients_output { sint32 returnCode; uint32 removed; };
    struct clearRecipients_locals { PoolMeta meta; RemoveEntry_input rei; RemoveEntry_output reo; };
    PUBLIC_PROCEDURE_WITH_LOCALS(clearRecipients)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.removed = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (locals.meta.status != QREWARDS_POOL_ACTIVE) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (locals.meta.targetMode != QREWARDS_TARGET_LIST) { output.returnCode = QREWARDS_WRONG_MODE; return; }
        while (state.get().poolMeta.get(input.poolId).headSlot != QREWARDS_NONE)
        {
            locals.rei.slot = state.get().poolMeta.get(input.poolId).headSlot;
            CALL(RemoveEntry, locals.rei, locals.reo);
            output.removed++;
        }
        output.returnCode = QREWARDS_SUCCESS;
    }

    // --- Holdings mode: join to earn ---

    // Join a holdings-mode pool. Costs joinFee (burned into the fee reserve; any excess reward is
    // refunded). The caller must currently hold something that counts (locked assets or NFTs,
    // weight > 0), must not be excluded, and must not be a contract. When the pool is full, the
    // caller names a current member with strictly lower weight to displace, so low-weight entries can never
    // crowd out real holders. A closing pool still accepts joins while it has pots to pay out.
    struct join_input { uint64 poolId; id displace; };
    struct join_output { sint32 returnCode; };
    struct join_locals
    {
        PoolMeta meta;
        DivCurrency cur;
        uint32 k;
        bit anyFunds;
        uint64 weight;
        uint64 victimWeight;
        FindEntry_input fei;
        FindEntry_output feo;
        FindEntry_input vfi;
        FindEntry_output vfo;
        ComputeAssetWeight_input cawi;
        ComputeAssetWeight_output cawo;
        ComputeNftWeight_input cnwi;
        ComputeNftWeight_output cnwo;
        AddEntry_input aei;
        AddEntry_output aeo;
        RemoveEntry_input rei;
        RemoveEntry_output reo;
        QRewardsLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(join)
    {
        if (isReservedId(qpi.invocator()))
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_CONTRACT_NOT_ALLOWED;
            return;
        }
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.anyFunds = 0;
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            if (locals.cur.pot > 0 || locals.cur.distributable > 0) locals.anyFunds = 1;
        }
        if (locals.meta.status == QREWARDS_POOL_FREE || (locals.meta.status == QREWARDS_POOL_CLOSING && !locals.anyFunds))
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_INACTIVE;
            return;
        }
        if (locals.meta.targetMode != QREWARDS_TARGET_HOLDINGS)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_WRONG_MODE;
            return;
        }
        if ((uint64)qpi.invocationReward() < state.get().joinFee)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_INSUFFICIENT_FEE;
            return;
        }
        locals.fei.poolId = input.poolId;
        locals.fei.wallet = qpi.invocator();
        CALL(FindEntry, locals.fei, locals.feo);
        if (locals.feo.slot != QREWARDS_NONE || locals.feo.taken)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = (locals.feo.status == QREWARDS_ENTRY_EXCLUDED) ? QREWARDS_EXCLUDED
                              : (locals.feo.taken ? QREWARDS_REGISTRY_FULL : QREWARDS_ALREADY_MEMBER);
            return;
        }
        locals.cawi.poolId = input.poolId;
        locals.cawi.wallet = qpi.invocator();
        CALL(ComputeAssetWeight, locals.cawi, locals.cawo);
        locals.cnwi.poolId = input.poolId;
        locals.cnwi.wallet = qpi.invocator();
        CALL(ComputeNftWeight, locals.cnwi, locals.cnwo);
        locals.weight = satAdd64(locals.cawo.weight, locals.cnwo.weight);
        if (locals.weight == 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_NO_WEIGHT;
            return;
        }

        if (locals.meta.memberCount >= QREWARDS_MAX_MEMBERS_PER_POOL)
        {
            locals.vfi.poolId = input.poolId;
            locals.vfi.wallet = input.displace;
            CALL(FindEntry, locals.vfi, locals.vfo);
            locals.victimWeight = 0xFFFFFFFFFFFFFFFFULL;
            if (locals.vfo.slot != QREWARDS_NONE && locals.vfo.status == QREWARDS_ENTRY_MEMBER)
            {
                locals.cawi.wallet = input.displace;
                CALL(ComputeAssetWeight, locals.cawi, locals.cawo);
                locals.cnwi.wallet = input.displace;
                CALL(ComputeNftWeight, locals.cnwi, locals.cnwo);
                locals.victimWeight = satAdd64(locals.cawo.weight, locals.cnwo.weight);
            }
            if (locals.victimWeight >= locals.weight)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.returnCode = QREWARDS_POOL_FULL;
                return;
            }
            locals.rei.slot = locals.vfo.slot;
            CALL(RemoveEntry, locals.rei, locals.reo);
        }

        locals.aei.poolId = input.poolId;
        locals.aei.wallet = qpi.invocator();
        locals.aei.status = QREWARDS_ENTRY_MEMBER;
        locals.aei.listWeight = 0;
        locals.aei.key = locals.feo.key;
        CALL(AddEntry, locals.aei, locals.aeo);
        if (locals.aeo.slot == QREWARDS_NONE)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_REGISTRY_FULL;
            return;
        }
        qpi.burn((sint64)state.get().joinFee);
        if ((uint64)qpi.invocationReward() > state.get().joinFee)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - (sint64)state.get().joinFee);
        }
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_MEMBER_JOINED, 0 };
        LOG_INFO(locals.log);
    }

    // Leave a holdings-mode pool (frees the slot; the join fee is not refunded).
    struct leave_input { uint64 poolId; };
    struct leave_output { sint32 returnCode; };
    struct leave_locals { FindEntry_input fei; FindEntry_output feo; RemoveEntry_input rei; RemoveEntry_output reo; };
    PUBLIC_PROCEDURE_WITH_LOCALS(leave)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.fei.poolId = input.poolId;
        locals.fei.wallet = qpi.invocator();
        CALL(FindEntry, locals.fei, locals.feo);
        if (locals.feo.slot == QREWARDS_NONE || locals.feo.status != QREWARDS_ENTRY_MEMBER)
        {
            output.returnCode = QREWARDS_NOT_MEMBER;
            return;
        }
        locals.rei.slot = locals.feo.slot;
        CALL(RemoveEntry, locals.rei, locals.reo);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Deposit a dividend into one pool/currency. Added to the currency's pot and paid to the
    // pool's members in the next distribution cycle.
    //  - QU currency (assetName==0): QU is taken from the invocation reward.
    //  - asset currency: `amount` of the asset is pulled from the caller into the contract (the
    //    caller first transfers management rights of those shares to QREWARDS via QX).
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE || locals.meta.paused || input.currencyIndex >= locals.meta.numCurrencies)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = (locals.meta.status != QREWARDS_POOL_ACTIVE) ? QREWARDS_POOL_INACTIVE
                               : (locals.meta.paused ? QREWARDS_POOL_PAUSED : QREWARDS_INVALID_INDEX);
            return;
        }
        locals.cur = locals.meta.currencies.get(input.currencyIndex);

        if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
        {
            locals.rev = (uint64)qpi.invocationReward();
            locals.isQU = 1;
        }
        else
        {
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
        //           (80% + dust + any shares not sent) to the QPAY wallet.
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
    // Open to anyone; QU is taken from the invocation reward. A closing pool cannot be revived.
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
        if (locals.meta.status != QREWARDS_POOL_ACTIVE)
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

    // Platform owner: tune the fees within their hardwired [min, max] ranges.
    struct setPlatformParams_input { uint64 createPoolFee; uint64 operatingFee; uint64 joinFee; };
    struct setPlatformParams_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setPlatformParams)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().platformOwner) { output.returnCode = QREWARDS_NOT_PLATFORM_OWNER; return; }
        if (input.createPoolFee < QREWARDS_MIN_CREATE_POOL_FEE || input.createPoolFee > QREWARDS_MAX_CREATE_POOL_FEE
            || input.operatingFee > QREWARDS_MAX_OPERATING_FEE
            || input.joinFee < QREWARDS_MIN_JOIN_FEE || input.joinFee > QREWARDS_MAX_JOIN_FEE)
        {
            output.returnCode = QREWARDS_INVALID_PARAM;
            return;
        }
        state.mut().createPoolFee = input.createPoolFee;
        state.mut().operatingFee = input.operatingFee;
        state.mut().joinFee = input.joinFee;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // The platform owner and the fee destinations are hardcoded in INITIALIZE with no setters,
    // so they can never be repointed after deployment.

    // Unlock: move QREWARDS-managed shares to another contract's management (e.g. back to QX to
    // trade them). Refused while a distribution cycle runs, so locked shares cannot move between
    // members during a cycle; any other time it is open.
    struct TransferShareManagementRights_input { Asset asset; sint64 numberOfShares; uint32 newManagingContractIndex; };
    struct TransferShareManagementRights_output { sint64 transferredNumberOfShares; };
    struct TransferShareManagementRights_locals { sint64 result; };
    PUBLIC_PROCEDURE_WITH_LOCALS(TransferShareManagementRights)
    {
        if (state.get().cycleActive)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            return;
        }
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

    /**************************************/
    /********FUNCTIONS (read-only)*********/
    /**************************************/

    // Current weight of a wallet in a pool -- exactly what the next MEASURE would record if the
    // wallet is (or joins as) a member. List mode: the wallet's listed weight. Excluded: 0.
    struct previewWeight_input { uint64 poolId; id user; };
    struct previewWeight_output { uint64 weight; sint32 returnCode; uint8 entryStatus; };
    struct previewWeight_locals
    {
        PoolMeta meta;
        FindEntry_input fei; FindEntry_output feo;
        ComputeAssetWeight_input cawi; ComputeAssetWeight_output cawo;
        ComputeNftWeight_input cnwi; ComputeNftWeight_output cnwo;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(previewWeight)
    {
        output.weight = 0;
        output.entryStatus = QREWARDS_ENTRY_FREE;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.fei.poolId = input.poolId;
        locals.fei.wallet = input.user;
        CALL(FindEntry, locals.fei, locals.feo);
        output.entryStatus = locals.feo.status;
        output.returnCode = QREWARDS_SUCCESS;
        if (locals.meta.targetMode == QREWARDS_TARGET_LIST)
        {
            if (locals.feo.status == QREWARDS_ENTRY_LISTED) output.weight = state.get().members.get(locals.feo.slot).listWeight;
            return;
        }
        if (locals.feo.status == QREWARDS_ENTRY_EXCLUDED || isReservedId(input.user)) return;
        locals.cawi.poolId = input.poolId;
        locals.cawi.wallet = input.user;
        CALL(ComputeAssetWeight, locals.cawi, locals.cawo);
        locals.cnwi.poolId = input.poolId;
        locals.cnwi.wallet = input.user;
        CALL(ComputeNftWeight, locals.cnwi, locals.cnwo);
        output.weight = satAdd64(locals.cawo.weight, locals.cnwo.weight);
    }

    struct getPool_input { uint64 poolId; };
    struct getPool_output
    {
        id admin;
        uint64 label;
        uint64 operatingBalance;
        uint64 lastTotalWeight;
        uint32 numAssets;
        uint32 numNfts;
        uint32 missedEpochs;
        uint32 memberCount;
        uint32 excludedCount;
        uint32 generation;
        uint8 numCurrencies;
        uint8 status;
        uint8 paused;
        uint8 targetMode;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyAssetName;
        Array<id, QREWARDS_MAX_DIV_CURRENCIES> currencyIssuer;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyPot;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyDistributable;
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
        output.numNfts = locals.meta.numNfts;
        output.missedEpochs = locals.meta.missedEpochs;
        output.memberCount = locals.meta.memberCount;
        output.excludedCount = locals.meta.excludedCount;
        output.generation = locals.meta.generation;
        output.numCurrencies = locals.meta.numCurrencies;
        output.status = locals.meta.status;
        output.paused = locals.meta.paused;
        output.targetMode = locals.meta.targetMode;
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            output.currencyAssetName.set(locals.k, locals.cur.assetName); // 0 = QU
            output.currencyIssuer.set(locals.k, locals.cur.issuer);
            output.currencyPot.set(locals.k, locals.cur.pot);
            output.currencyDistributable.set(locals.k, locals.cur.distributable);
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
    struct getAllPoolAssets_locals { PoolMeta meta; uint32 i; uint64 base; };
    PUBLIC_FUNCTION_WITH_LOCALS(getAllPoolAssets)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            output.assets.set(locals.i, state.get().registry.get(locals.base + locals.i));
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

    // Return every registered NFT id in a pool in one call.
    struct getAllPoolNFTs_input { uint64 poolId; };
    struct getAllPoolNFTs_output
    {
        Array<NFTRule, QREWARDS_MAX_NFT_PER_POOL> nfts;
        uint32 count;
        sint32 returnCode;
    };
    struct getAllPoolNFTs_locals { PoolMeta meta; uint32 i; uint64 base; };
    PUBLIC_FUNCTION_WITH_LOCALS(getAllPoolNFTs)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_NFT_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numNfts; locals.i++)
        {
            output.nfts.set(locals.i, state.get().nftRegistry.get(locals.base + locals.i));
        }
        output.count = locals.meta.numNfts;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPlatform_input { };
    struct getPlatform_output
    {
        id platformOwner;
        id qpayhubAddress;
        id qpayTokenDividendsAddress;
        uint64 createPoolFee;
        uint64 operatingFee;
        uint64 joinFee;
        uint32 numPools;
        uint32 numFreePools;
        uint32 totalEntries;
        uint32 streamDelayTicks;
        uint32 streamBatchSize;
        uint32 cycleId;
        uint32 streamPoolCursor;
        uint8 cycleActive;
        uint8 streamPhase;
    };
    PUBLIC_FUNCTION(getPlatform)
    {
        output.platformOwner = state.get().platformOwner;
        output.qpayhubAddress = state.get().qpayhubAddress;
        output.qpayTokenDividendsAddress = state.get().qpayTokenDividendsAddress;
        output.createPoolFee = state.get().createPoolFee;
        output.operatingFee = state.get().operatingFee;
        output.joinFee = state.get().joinFee;
        output.numPools = state.get().numPools;
        output.numFreePools = state.get().numFreePools;
        output.totalEntries = state.get().totalEntries;
        output.streamDelayTicks = state.get().streamDelayTicks;
        output.streamBatchSize = state.get().streamBatchSize;
        output.cycleId = state.get().cycleId;
        output.streamPoolCursor = state.get().streamPoolCursor;
        output.cycleActive = state.get().cycleActive;
        output.streamPhase = state.get().streamPhase;
    }

    struct isExcluded_input { uint64 poolId; id address; };
    struct isExcluded_output { bit excluded; };
    struct isExcluded_locals { FindEntry_input fei; FindEntry_output feo; };
    PUBLIC_FUNCTION_WITH_LOCALS(isExcluded)
    {
        locals.fei.poolId = input.poolId;
        locals.fei.wallet = input.address;
        CALL(FindEntry, locals.fei, locals.feo);
        output.excluded = (locals.feo.status == QREWARDS_ENTRY_EXCLUDED) ? 1 : 0;
    }

    // Membership record of a wallet in a pool (status QREWARDS_ENTRY_FREE = not a member).
    struct getMember_input { uint64 poolId; id wallet; };
    struct getMember_output { uint8 status; uint64 listWeight; uint64 measuredWeight; uint32 measureCycle; };
    struct getMember_locals { FindEntry_input fei; FindEntry_output feo; MemberEntry e; };
    PUBLIC_FUNCTION_WITH_LOCALS(getMember)
    {
        setMemory(output, 0);
        locals.fei.poolId = input.poolId;
        locals.fei.wallet = input.wallet;
        CALL(FindEntry, locals.fei, locals.feo);
        if (locals.feo.slot == QREWARDS_NONE) return;
        locals.e = state.get().members.get(locals.feo.slot);
        output.status = locals.e.status;
        output.listWeight = locals.e.listWeight;
        output.measuredWeight = locals.e.measuredWeight;
        output.measureCycle = locals.e.measureCycle;
    }

    // Page through a pool's entries (members, listed recipients and exclusions) in list order.
    // `offset` skips that many entries; bounded by the per-pool caps.
    struct getPoolMembers_input { uint64 poolId; uint32 offset; };
    struct getPoolMembers_output
    {
        Array<id, QREWARDS_PAGE> wallets;
        Array<uint8, QREWARDS_PAGE> statuses;
        uint32 count;
        uint32 total;
    };
    struct getPoolMembers_locals { PoolMeta meta; MemberEntry e; uint32 slot; uint32 skipped; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolMembers)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) return;
        locals.meta = state.get().poolMeta.get(input.poolId);
        output.total = locals.meta.memberCount + locals.meta.excludedCount;
        locals.skipped = 0;
        locals.slot = locals.meta.headSlot;
        while (locals.slot != QREWARDS_NONE && output.count < QREWARDS_PAGE)
        {
            locals.e = state.get().members.get(locals.slot);
            if (locals.skipped >= input.offset)
            {
                output.wallets.set(output.count, locals.e.wallet);
                output.statuses.set(output.count, locals.e.status);
                output.count++;
            }
            else
            {
                locals.skipped++;
            }
            locals.slot = locals.e.next;
        }
    }

    // Paginated list of live pools owned by an admin. `offset` skips that many matches;
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
            if (locals.meta.status != QREWARDS_POOL_FREE && locals.meta.admin == input.admin)
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
    // Retired ids (do not reuse): function 5 (getFundingRoute), procedures 12 (setFundingRoute)
    // and 14 (setDistributionMode).
    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(getPool, 1);
        REGISTER_USER_FUNCTION(getPoolAsset, 2);
        REGISTER_USER_FUNCTION(getAllPoolAssets, 3);
        REGISTER_USER_FUNCTION(getPlatform, 4);
        REGISTER_USER_FUNCTION(getPoolsByAdmin, 6);
        REGISTER_USER_FUNCTION(isExcluded, 7);
        REGISTER_USER_FUNCTION(previewWeight, 8);
        REGISTER_USER_FUNCTION(getPoolNFT, 9);
        REGISTER_USER_FUNCTION(getAllPoolNFTs, 10);
        REGISTER_USER_FUNCTION(getMember, 11);
        REGISTER_USER_FUNCTION(getPoolMembers, 12);

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
        REGISTER_USER_PROCEDURE(setExcludedAddress, 13);
        REGISTER_USER_PROCEDURE(setTargetMode, 15);
        REGISTER_USER_PROCEDURE(addRecipients, 16);
        REGISTER_USER_PROCEDURE(clearRecipients, 17);
        REGISTER_USER_PROCEDURE(registerNFT, 18);
        REGISTER_USER_PROCEDURE(updateNFT, 19);
        REGISTER_USER_PROCEDURE(join, 20);
        REGISTER_USER_PROCEDURE(leave, 21);
        REGISTER_USER_PROCEDURE(removeAsset, 22);
        REGISTER_USER_PROCEDURE(removeNFT, 23);
    }

    INITIALIZE()
    {
        // Platform owner is HARDCODED (not a first-caller bootstrap) so nobody can claim the role
        // at deploy time. Owner = the QPay wallet
        // (QPAYNOWSWZMGHFEAEVJXGZAVSHABAZDDBDIHTEBOPCOGHRGBCYCUZOHCVLXG). It can only tune the
        // fees within their hardwired ranges, and there is no ownership-transfer function.
        state.mut().platformOwner = ID(_Q, _P, _A, _Y, _N, _O, _W, _S, _W, _Z, _M, _G, _H, _F, _E, _A, _E, _V, _J, _X, _G, _Z, _A, _V, _S, _H, _A, _B, _A, _Z, _D, _D, _B, _D, _I, _H, _T, _E, _B, _O, _P, _C, _O, _G, _H, _R, _G, _B, _C, _Y, _C, _U, _Z, _O, _H, _C);
        // QPAYHUB contract address = id(29, 0, 0, 0) (CONTRACT_INDEX 29 on the live chain).
        state.mut().qpayhubAddress = id(29, 0, 0, 0);
        // Destination for the 80% leg of the 5% TOKEN dividend fee: the QRaffle charity address
        // (DPQRLSZSSCXIYFIQGBFBXXISDDEBEGQNWNTQUEIFSCUWGHVXJPLFGMYD...), a plain wallet.
        state.mut().qpayTokenDividendsAddress = ID(_D, _P, _Q, _R, _L, _S, _Z, _S, _S, _C, _X, _I, _Y, _F, _I, _Q, _G, _B, _F, _B, _X, _X, _I, _S, _D, _D, _E, _B, _E, _G, _Q, _N, _W, _N, _T, _Q, _U, _E, _I, _F, _S, _C, _U, _W, _G, _H, _V, _X, _J, _P, _L, _F, _G, _M, _Y, _D);
        state.mut().createPoolFee = QREWARDS_DEFAULT_CREATE_FEE;
        state.mut().operatingFee = QREWARDS_DEFAULT_OPERATING_FEE;
        state.mut().joinFee = QREWARDS_DEFAULT_JOIN_FEE;
        state.mut().memberFreeHead = QREWARDS_NONE;
        state.mut().memberHighWater = 0;

        state.mut().streamDelayTicks = QREWARDS_STREAM_DELAY_TICKS;
        state.mut().streamBatchSize = QREWARDS_STREAM_BATCH;
        state.mut().lastCycleEpoch = QREWARDS_NO_CYCLE_EPOCH;
        state.mut().streamPhase = QREWARDS_PHASE_IDLE;
        state.mut().streamMemberCursor = QREWARDS_NONE;
    }

    BEGIN_EPOCH()
    {
        // Anchor the epoch so a cycle starts streamDelayTicks into it.
        state.mut().epochStartTick = qpi.tick();
    }

    // O(numPools): collect the per-epoch operating fee (pools that cannot pay are paused, and
    // closed after QREWARDS_MAX_MISSED_EPOCHS), then flush the QU dividend fee. No distribution
    // happens here -- a closing pool pays its remaining pots to its members through the engine.
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
    };
    END_EPOCH_WITH_LOCALS()
    {
        locals.fee = state.get().operatingFee;
        locals.collected = 0;
        for (locals.i = 0; locals.i < state.get().numPools; locals.i++)
        {
            locals.meta = state.get().poolMeta.get(locals.i);
            if (locals.meta.status != QREWARDS_POOL_ACTIVE) continue;
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
                    locals.meta.status = QREWARDS_POOL_CLOSING;
                    locals.meta.paused = 0;
                }
            }
            state.mut().poolMeta.set(locals.i, locals.meta);
        }
        if (locals.collected > 0)
        {
            locals.dfi.amount = locals.collected;
            CALL(DistributeFee, locals.dfi, locals.dfo);
        }

        if (state.get().pendingDivFeeQU > 0)
        {
            locals.ddfi.amount = state.get().pendingDivFeeQU;
            state.mut().pendingDivFeeQU = 0;
            CALL(DistributeDivFee, locals.ddfi, locals.ddfo);
        }

        state.mut().memberIndex.cleanupIfNeeded();
    }

    // Distribution driver: once per epoch, streamDelayTicks into it, start a cycle; then advance
    // it by at most streamBatchSize O(1) units per tick until every pool has been processed.
    struct END_TICK_locals { uint32 budget; StreamStep_input ssi; StreamStep_output sso; };
    END_TICK_WITH_LOCALS()
    {
        if (!state.get().cycleActive)
        {
            if (state.get().lastCycleEpoch == qpi.epoch()
                || qpi.tick() - state.get().epochStartTick < state.get().streamDelayTicks)
            {
                return;
            }
            state.mut().cycleActive = 1;
            state.mut().cycleId = state.get().cycleId + 1;
            state.mut().streamPoolCursor = 0;
            state.mut().streamPhase = QREWARDS_PHASE_IDLE;
            state.mut().lastCycleEpoch = qpi.epoch();
        }
        for (locals.budget = state.get().streamBatchSize; locals.budget > 0 && state.get().cycleActive; locals.budget--)
        {
            CALL(StreamStep, locals.ssi, locals.sso);
        }
    }

    // Only QX may hand share management rights to QREWARDS (the token-deposit flow). Shares
    // pushed from any other contract would be stranded under QREWARDS management.
    PRE_ACQUIRE_SHARES()
    {
        output.allowTransfer = (input.otherContractIndex == QREWARDS_QX_CONTRACT_INDEX);
    }
};
