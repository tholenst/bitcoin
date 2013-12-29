// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_TXMEMPOOL_H
#define BITCOIN_TXMEMPOOL_H
#include "main.h"
#include "coins.h"
#include "core.h"
#include "sync.h"
#include <boost/signals2.hpp>
/** Fake height value used in CCoins to signify they are only in the memory pool (since 0.8) */
static const unsigned int MEMPOOL_HEIGHT = 0x7FFFFFFF;

class CValidationState;

/*
 * CTxMemPool stores these:
 */
class CTxMemPoolEntry
{
public:
    CTransaction tx;
    int64_t nFee; // Cached to avoid expensive parent-transaction lookups
    size_t nTxSize; // ... and avoid recomputing tx size
    int64_t nTime; // Local time when entering the mempool
    double dPriority; // Priority when entering the mempool
    unsigned int nHeight; // Chain height when entering the mempool

    CTxMemPoolEntry(const CTransaction& _tx, int64_t _nFee,
                    int64_t _nTime, double _dPriority, unsigned int _nHeight);
    CTxMemPoolEntry(const CTxMemPoolEntry& other);

    int64_t GetFee() const { return nFee; }
    size_t GetTxSize() const { return nTxSize; }
    int64_t GetTime() const { return nTime; }
    unsigned int GetHeight() const { return nHeight; }
    CTxMemPoolEntry();
    const CTransaction& GetTx() const { return this->tx; }
    double GetPriority(unsigned int currentHeight) const;
};

/*
 * CTxMemPool stores valid-according-to-the-current-best-chain
 * transactions that may be included in the next block.
 *
 * Transactions are added when they are seen on the network
 * (or created by the local node), but not all transactions seen
 * are added to the pool: if a new transaction double-spends
 * an input of a transaction in the pool, it is dropped,
 * as are non-standard transactions.
 */
class CTxMemPool
{
    friend class CTxMemPoolEntry;
private:
    bool fSanityCheck; // Normally false, true if -checkmempool or -regtest
    unsigned int nTransactionsUpdated;
    std::map<COutPoint, CInPoint> mapNextTx;


    void EraseOrphanTx(uint256);
    void ProcessOrphansAfterAdd(const CTransaction&);
    bool AddInternal(CValidationState &state, const CTransaction &tx, bool fLimitFree,
                     bool* pfMissingInputs, bool fRejectInsaneFee);
public: // NOTE: Should be private, but public for unit tests
    /* The mempool may cache transactions which are not really part of
     * the mempool, but may be later. Currently, these are just the
     * orphans seen, but later it might include double spends.
     */
    std::map<uint256, set<uint256> > mapOrphanTransactionsByPrev;
    std::map<uint256, CTransaction> mapOrphanTransactions;
    bool AddOrphanTx(const CTransaction&);
    unsigned int LimitOrphanTxSize(unsigned int);
public:
    mutable CCriticalSection cs;
    std::map<uint256, CTxMemPoolEntry> mapTx;
    // called when a single transaction is accepted by mempool.add()

    CTxMemPool();
    ~CTxMemPool() {
        mapOrphanTransactions.clear();
    }
    /*
     * If sanity-checking is turned on, check makes sure the pool is
     * consistent (does not contain two transactions that spend the same inputs,
     * all inputs are in the mapNextTx array). If sanity-checking is turned off,
     * check does nothing.
     */
    void check(CCoinsViewCache *pcoins) const;
    void setSanityCheck(bool _fSanityCheck) { fSanityCheck = _fSanityCheck; }

    bool remove(const CTransaction &tx, bool fRecursive = false);
    bool removeConflicts(const CTransaction &tx);
    void clear();
    void queryHashes(std::vector<uint256>& vtxid);
    void pruneSpent(const uint256& hash, CCoins &coins);
    unsigned int GetTransactionsUpdated() const;
    void AddTransactionsUpdated(unsigned int n);

    bool addUnchecked(const uint256& hash, const CTxMemPoolEntry &entry);
    bool add(CValidationState &state, const CTransaction &tx, bool fLimitFree,
             bool fCacheIfInputsMissing=false, bool fRejectInsaneFee=false);

    unsigned long size()
    {
        LOCK(cs);
        return mapTx.size();
    }

    bool exists(uint256 hash, bool includeOrphans = false)
    {
        LOCK(cs);
        return (mapTx.count(hash) != 0) || (includeOrphans && mapOrphanTransactions.count(hash));
    }

    bool lookup(uint256 hash, CTransaction& result) const;
};

/** CCoinsView that brings transactions from a memorypool into view.
    It does not check for spendings by memory pool transactions. */
class CCoinsViewMemPool : public CCoinsViewBacked
{
protected:
    CTxMemPool &mempool;

public:
    CCoinsViewMemPool(CCoinsView &baseIn, CTxMemPool &mempoolIn);
    bool GetCoins(const uint256 &txid, CCoins &coins);
    bool HaveCoins(const uint256 &txid);
};

#endif /* BITCOIN_TXMEMPOOL_H */
