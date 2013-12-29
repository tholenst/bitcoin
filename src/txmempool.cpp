// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "core.h"
#include "txmempool.h"

using namespace std;

CTxMemPoolEntry::CTxMemPoolEntry()
{
    nHeight = MEMPOOL_HEIGHT;
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTransaction& _tx, int64_t _nFee,
                                 int64_t _nTime, double _dPriority,
                                 unsigned int _nHeight):
    tx(_tx), nFee(_nFee), nTime(_nTime), dPriority(_dPriority), nHeight(_nHeight)
{
    nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTxMemPoolEntry& other)
{
    *this = other;
}

double
CTxMemPoolEntry::GetPriority(unsigned int currentHeight) const
{
    int64_t nValueIn = tx.GetValueOut()+nFee;
    double deltaPriority = ((double)(currentHeight-nHeight)*nValueIn)/nTxSize;
    double dResult = dPriority + deltaPriority;
    return dResult;
}

CTxMemPool::CTxMemPool()
{
    // Sanity checks off by default for performance, because otherwise
    // accepting transactions becomes O(N^2) where N is the number
    // of transactions in the pool
    fSanityCheck = false;
}

void CTxMemPool::pruneSpent(const uint256 &hashTx, CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

unsigned int CTxMemPool::GetTransactionsUpdated() const
{
    LOCK(cs);
    return nTransactionsUpdated;
}

void CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
    LOCK(cs);
    nTransactionsUpdated += n;
}


bool CTxMemPool::addUnchecked(const uint256& hash, const CTxMemPoolEntry &entry)
{
    // Add to memory pool without checking anything.
    // Used by main.cpp add(), which DOES do
    // all the appropriate checks.
    LOCK(cs);
    {
        mapTx[hash] = entry;
        const CTransaction& tx = mapTx[hash].GetTx();
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = CInPoint(&tx, i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(const CTransaction &tx, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (fRecursive) {
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                if (it != mapNextTx.end())
                    remove(*it->second.ptx, true);
            }
        }
        if (mapTx.count(hash))
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
    return true;
}

bool CTxMemPool::removeConflicts(const CTransaction &tx)
{
    // Remove transactions which depend on inputs of tx, recursively
    LOCK(cs);
    BOOST_FOREACH(const CTxIn &txin, tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
                remove(txConflict, true);
        }
    }
    return true;
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::check(CCoinsViewCache *pcoins) const
{
    if (!fSanityCheck)
        return;

    LogPrint("mempool", "Checking mempool with %u transactions and %u inputs\n", (unsigned int)mapTx.size(), (unsigned int)mapNextTx.size());

    LOCK(cs);
    for (std::map<uint256, CTxMemPoolEntry>::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        unsigned int i = 0;
        const CTransaction& tx = it->second.GetTx();
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            // Check that every mempool transaction's inputs refer to available coins, or other mempool tx's.
            std::map<uint256, CTxMemPoolEntry>::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end()) {
                const CTransaction& tx2 = it2->second.GetTx();
                assert(tx2.vout.size() > txin.prevout.n && !tx2.vout[txin.prevout.n].IsNull());
            } else {
                CCoins &coins = pcoins->GetCoins(txin.prevout.hash);
                assert(coins.IsAvailable(txin.prevout.n));
            }
            // Check whether its inputs are marked in mapNextTx.
            std::map<COutPoint, CInPoint>::const_iterator it3 = mapNextTx.find(txin.prevout);
            assert(it3 != mapNextTx.end());
            assert(it3->second.ptx == &tx);
            assert(it3->second.n == i);
            i++;
        }
    }
    for (std::map<COutPoint, CInPoint>::const_iterator it = mapNextTx.begin(); it != mapNextTx.end(); it++) {
        uint256 hash = it->second.ptx->GetHash();
        map<uint256, CTxMemPoolEntry>::const_iterator it2 = mapTx.find(hash);
        const CTransaction& tx = it2->second.GetTx();
        assert(it2 != mapTx.end());
        assert(&tx == it->second.ptx);
        assert(tx.vin.size() > it->second.n);
        assert(it->first == it->second.ptx->vin[it->second.n].prevout);
    }
}

void CTxMemPool::queryHashes(vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTxMemPoolEntry>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}

bool CTxMemPool::lookup(uint256 hash, CTransaction& result) const
{
    LOCK(cs);
    map<uint256, CTxMemPoolEntry>::const_iterator i = mapTx.find(hash);
    if (i == mapTx.end()) return false;
    result = i->second.GetTx();
    return true;
}

CCoinsViewMemPool::CCoinsViewMemPool(CCoinsView &baseIn, CTxMemPool &mempoolIn) : CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool CCoinsViewMemPool::GetCoins(const uint256 &txid, CCoins &coins) {
    if (base->GetCoins(txid, coins))
        return true;
    CTransaction tx;
    if (mempool.lookup(txid, tx)) {
        coins = CCoins(tx, MEMPOOL_HEIGHT);
        return true;
    }
    return false;
}

bool CCoinsViewMemPool::HaveCoins(const uint256 &txid) {
    return mempool.exists(txid) || base->HaveCoins(txid);
}

bool CTxMemPool::add(CValidationState &state, const CTransaction &tx, bool fLimitFree,
                     bool fCacheIfInputsMissing, bool fRejectInsaneFee)
{
    bool fMissingInputs = false;
    bool result;
    result = AddInternal(state, tx, fLimitFree, &fMissingInputs, fRejectInsaneFee);
    if (fCacheIfInputsMissing && fMissingInputs) {
        AddOrphanTx(tx);
        // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
        unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
        if (nEvicted > 0)
            LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
    }
    return result;
}

bool CTxMemPool::AddInternal(CValidationState &state, const CTransaction &tx, bool fLimitFree,
                             bool* pfMissingInputs, bool fRejectInsaneFee)
{
    if (pfMissingInputs) 
        *pfMissingInputs = false;

    if (!CheckTransaction(tx, state))
        return error("mempool.add : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("mempool.add : coinbase as individual tx"),
                         REJECT_INVALID, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    if (Params().NetworkID() == CChainParams::MAIN && !IsStandardTx(tx, reason))
        return state.DoS(0,
                         error("mempool.add : nonstandard transaction: %s", reason.c_str()),
                         REJECT_NONSTANDARD, reason);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (this->exists(hash))
        return false;

    // Check for conflicts with in-memory transactions
    {
        LOCK(this->cs); // protect this->mapNextTx
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            COutPoint outpoint = tx.vin[i].prevout;
            if (this->mapNextTx.count(outpoint))
            {
                // Disable replacement feature for now
                return false;
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(dummy);

        {
            LOCK(this->cs);
            CCoinsViewMemPool viewMemPool(*pcoinsTip, *this);
            view.SetBackend(viewMemPool);

            // do we already have it?
            if (view.HaveCoins(hash))
                return false;

            // do all inputs exist?
            // Note that this does not check for the presence of actual outputs (see the next check for that),
            // only helps filling in pfMissingInputs (to determine missing vs spent).
            BOOST_FOREACH(const CTxIn txin, tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    return false;
                }
            }

            // are the actual inputs available?
            if (!view.HaveInputs(tx))
                return state.Invalid(error("mempool.add : inputs already spent"),
                                     REJECT_DUPLICATE, "inputs spent");

            // Bring the best block into scope
            view.GetBestBlock();

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (Params().NetworkID() == CChainParams::MAIN && !AreInputsStandard(tx, view))
            return error("mempool.add : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64_t nValueIn = view.GetValueIn(tx);
        int64_t nValueOut = tx.GetValueOut();
        int64_t nFees = nValueIn-nValueOut;
        double dPriority = view.GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);
        if (fLimitFree && nFees < txMinFee)
            return state.DoS(0, error("mempool.add : not enough fees %s, %"PRId64" < %"PRId64,
                                      hash.ToString().c_str(), nFees, txMinFee),
                             REJECT_INSUFFICIENTFEE, "insufficient fee");

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < CTransaction::nMinRelayTxFee)
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
                return state.DoS(0, error("mempool.add : free transaction rejected by rate limiter"),
                                 REJECT_INSUFFICIENTFEE, "insufficient priority");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        if (fRejectInsaneFee && nFees > CTransaction::nMinRelayTxFee * 10000)
            return error("mempool.add : insane fees %s, %"PRId64" > %"PRId64,
                         hash.ToString().c_str(),
                         nFees, CTransaction::nMinRelayTxFee * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!CheckInputs(tx, state, view, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
        {
            return error("mempool.add : ConnectInputs failed %s", hash.ToString().c_str());
        }
        // Store transaction in memory
        this->addUnchecked(hash, entry);
    }
    
    ::g_signals.SyncTransaction(hash, tx, NULL);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool CTxMemPool::AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString().c_str());
        return false;
    }

    mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %"PRIszu")\n", hash.ToString().c_str(),
             mapOrphanTransactions.size());
    return true;
}

void CTxMemPool::EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CTransaction& tx = mapOrphanTransactions[hash];
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    mapOrphanTransactions.erase(hash);
}

unsigned int CTxMemPool::LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

void CTxMemPool::ProcessOrphansAfterAdd(const CTransaction& tx) {
    vector<uint256> vWorkQueue;
    vector<uint256> vEraseQueue;
    vWorkQueue.push_back(tx.GetHash());
    vEraseQueue.push_back(tx.GetHash());
    
    // Recursively process any orphan transactions that depended on this one
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
             mi != mapOrphanTransactionsByPrev[hashPrev].end();
             ++mi)
        {
            const uint256& orphanHash = *mi;
            const CTransaction& orphanTx = mapOrphanTransactions[orphanHash];
            bool fMissingInputs2 = false;
            // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
            // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
            // anyone relaying LegitTxX banned)
            CValidationState stateDummy;
            
            if (mempool.AddInternal(stateDummy, orphanTx, true, &fMissingInputs2, false))
            {
                LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString().c_str());
                RelayTransaction(orphanTx, orphanHash);
                mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
                vWorkQueue.push_back(orphanHash);
                vEraseQueue.push_back(orphanHash);
            }
            else if (!fMissingInputs2)
            {
                // invalid or too-little-fee orphan
                vEraseQueue.push_back(orphanHash);
                LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString().c_str());
            }
            mempool.check(pcoinsTip);
        }
    }
    
    BOOST_FOREACH(uint256 hash, vEraseQueue)
        EraseOrphanTx(hash);
    
}

void CTxMemPool::blockchainupdate(std::vector<CTransaction>& vDelete, std::list<CTransaction>& vResurrect) 
{
    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect) {
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!add(stateDummy, tx, false, NULL))
            remove(tx, true);
    }
    
    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete) {
        remove(tx);
        removeConflicts(tx);
    }
}
