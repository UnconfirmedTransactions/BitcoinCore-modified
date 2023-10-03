// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <policy/policy.h>
#include <script/signingprovider.h>
#include <util/check.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/trace.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

#include <cmath>

using interfaces::FoundBlock;

namespace wallet {
static constexpr size_t OUTPUT_GROUP_MAX_ENTRIES{100};

int GetTxSpendSize(const CWallet& wallet, const CWalletTx& wtx, unsigned int out, bool use_max_sig)
{
    return CalculateMaximumSignedInputSize(wtx.tx->vout[out], &wallet, use_max_sig);
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const SigningProvider* provider, bool use_max_sig)
{
    CMutableTransaction txn;
    txn.vin.push_back(CTxIn(COutPoint()));
    if (!provider || !DummySignInput(*provider, txn.vin[0], txout, use_max_sig)) {
        return -1;
    }
    return GetVirtualTransactionInputSize(txn.vin[0]);
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const CWallet* wallet, bool use_max_sig)
{
    const std::unique_ptr<SigningProvider> provider = wallet->GetSolvingProvider(txout.scriptPubKey);
    return CalculateMaximumSignedInputSize(txout, provider.get(), use_max_sig);
}

// txouts needs to be in the order of tx.vin
TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control)
{
    CMutableTransaction txNew(tx);
    if (!wallet->DummySignTx(txNew, txouts, coin_control)) {
        return TxSize{-1, -1};
    }
    CTransaction ctx(txNew);
    int64_t vsize = GetVirtualTransactionSize(ctx);
    int64_t weight = GetTransactionWeight(ctx);
    return TxSize{vsize, weight};
}

TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const CCoinControl* coin_control)
{
    std::vector<CTxOut> txouts;
    // Look up the inputs. The inputs are either in the wallet, or in coin_control.
    for (const CTxIn& input : tx.vin) {
        const auto mi = wallet->mapWallet.find(input.prevout.hash);
        // Can not estimate size without knowing the input details
        if (mi != wallet->mapWallet.end()) {
            assert(input.prevout.n < mi->second.tx->vout.size());
            txouts.emplace_back(mi->second.tx->vout.at(input.prevout.n));
        } else if (coin_control) {
            CTxOut txout;
            if (!coin_control->GetExternalOutput(input.prevout, txout)) {
                return TxSize{-1, -1};
            }
            txouts.emplace_back(txout);
        } else {
            return TxSize{-1, -1};
        }
    }
    return CalculateMaximumSignedTxSize(tx, wallet, txouts, coin_control);
}

void AvailableCoins(const CWallet& wallet, std::vector<COutput>& vCoins, const CCoinControl* coinControl, std::optional<CFeeRate> feerate, const CAmount& nMinimumAmount, const CAmount& nMaximumAmount, const CAmount& nMinimumSumAmount, const uint64_t nMaximumCount)
{
    AssertLockHeld(wallet.cs_wallet);

    vCoins.clear();
    CAmount nTotal = 0;
    // Either the WALLET_FLAG_AVOID_REUSE flag is not set (in which case we always allow), or we default to avoiding, and only in the case where
    // a coin control object is provided, and has the avoid address reuse flag set to false, do we allow already used addresses
    bool allow_used_addresses = !wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE) || (coinControl && !coinControl->m_avoid_address_reuse);
    const int min_depth = {coinControl ? coinControl->m_min_depth : DEFAULT_MIN_DEPTH};
    const int max_depth = {coinControl ? coinControl->m_max_depth : DEFAULT_MAX_DEPTH};
    const bool only_safe = {coinControl ? !coinControl->m_include_unsafe_inputs : true};

    std::set<uint256> trusted_parents;
    for (const auto& entry : wallet.mapWallet)
    {
        const uint256& wtxid = entry.first;
        const CWalletTx& wtx = entry.second;

        if (wallet.IsTxImmatureCoinBase(wtx))
            continue;

        int nDepth = wallet.GetTxDepthInMainChain(wtx);
        if (nDepth < 0)
            continue;

        // We should not consider coins which aren't at least in our mempool
        // It's possible for these to be conflicted via ancestors which we may never be able to detect
        if (nDepth == 0 && !wtx.InMempool())
            continue;

        bool safeTx = CachedTxIsTrusted(wallet, wtx, trusted_parents);

        // We should not consider coins from transactions that are replacing
        // other transactions.
        //
        // Example: There is a transaction A which is replaced by bumpfee
        // transaction B. In this case, we want to prevent creation of
        // a transaction B' which spends an output of B.
        //
        // Reason: If transaction A were initially confirmed, transactions B
        // and B' would no longer be valid, so the user would have to create
        // a new transaction C to replace B'. However, in the case of a
        // one-block reorg, transactions B' and C might BOTH be accepted,
        // when the user only wanted one of them. Specifically, there could
        // be a 1-block reorg away from the chain where transactions A and C
        // were accepted to another chain where B, B', and C were all
        // accepted.
        if (nDepth == 0 && wtx.mapValue.count("replaces_txid")) {
            safeTx = false;
        }

        // Similarly, we should not consider coins from transactions that
        // have been replaced. In the example above, we would want to prevent
        // creation of a transaction A' spending an output of A, because if
        // transaction B were initially confirmed, conflicting with A and
        // A', we wouldn't want to the user to create a transaction D
        // intending to replace A', but potentially resulting in a scenario
        // where A, A', and D could all be accepted (instead of just B and
        // D, or just A and A' like the user would want).
        if (nDepth == 0 && wtx.mapValue.count("replaced_by_txid")) {
            safeTx = false;
        }

        if (only_safe && !safeTx) {
            continue;
        }

        if (nDepth < min_depth || nDepth > max_depth) {
            continue;
        }

        bool tx_from_me = CachedTxIsFromMe(wallet, wtx, ISMINE_ALL);

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            // Only consider selected coins if add_inputs is false
            if (coinControl && !coinControl->m_add_inputs && !coinControl->IsSelected(COutPoint(entry.first, i))) {
                continue;
            }

            if (wtx.tx->vout[i].nValue < nMinimumAmount || wtx.tx->vout[i].nValue > nMaximumAmount)
                continue;

            if (coinControl && coinControl->HasSelected() && !coinControl->fAllowOtherInputs && !coinControl->IsSelected(COutPoint(entry.first, i)))
                continue;

            if (wallet.IsLockedCoin(entry.first, i))
                continue;

            if (wallet.IsSpent(wtxid, i))
                continue;

            isminetype mine = wallet.IsMine(wtx.tx->vout[i]);

            if (mine == ISMINE_NO) {
                continue;
            }

            if (!allow_used_addresses && wallet.IsSpentKey(wtxid, i)) {
                continue;
            }

            std::unique_ptr<SigningProvider> provider = wallet.GetSolvingProvider(wtx.tx->vout[i].scriptPubKey);

            bool solvable = provider ? IsSolvable(*provider, wtx.tx->vout[i].scriptPubKey) : false;
            bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && (coinControl && coinControl->fAllowWatchOnly && solvable));
            int input_bytes = GetTxSpendSize(wallet, wtx, i, (coinControl && coinControl->fAllowWatchOnly));

            vCoins.emplace_back(COutPoint(wtx.GetHash(), i), wtx.tx->vout.at(i), nDepth, input_bytes, spendable, solvable, safeTx, wtx.GetTxTime(), tx_from_me, feerate);

            // Checks the sum amount of all UTXO's.
            if (nMinimumSumAmount != MAX_MONEY) {
                nTotal += wtx.tx->vout[i].nValue;

                if (nTotal >= nMinimumSumAmount) {
                    return;
                }
            }

            // Checks the maximum number of UTXO's.
            if (nMaximumCount > 0 && vCoins.size() >= nMaximumCount) {
                return;
            }
        }
    }
}

void AvailableCoinsListUnspent(const CWallet& wallet, std::vector<COutput>& vCoins, const CCoinControl* coinControl, const CAmount& nMinimumAmount, const CAmount& nMaximumAmount, const CAmount& nMinimumSumAmount, const uint64_t nMaximumCount)
{
    AvailableCoins(wallet, vCoins, coinControl, /*feerate=*/ std::nullopt, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount);
}

CAmount GetAvailableBalance(const CWallet& wallet, const CCoinControl* coinControl)
{
    LOCK(wallet.cs_wallet);

    CAmount balance = 0;
    std::vector<COutput> vCoins;
    AvailableCoinsListUnspent(wallet, vCoins, coinControl);
    for (const COutput& out : vCoins) {
        if (out.spendable) {
            balance += out.txout.nValue;
        }
    }
    return balance;
}

const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const CTransaction& tx, int output)
{
    AssertLockHeld(wallet.cs_wallet);
    const CTransaction* ptx = &tx;
    int n = output;
    while (OutputIsChange(wallet, ptx->vout[n]) && ptx->vin.size() > 0) {
        const COutPoint& prevout = ptx->vin[0].prevout;
        auto it = wallet.mapWallet.find(prevout.hash);
        if (it == wallet.mapWallet.end() || it->second.tx->vout.size() <= prevout.n ||
            !wallet.IsMine(it->second.tx->vout[prevout.n])) {
            break;
        }
        ptx = it->second.tx.get();
        n = prevout.n;
    }
    return ptx->vout[n];
}

const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const COutPoint& outpoint)
{
    AssertLockHeld(wallet.cs_wallet);
    return FindNonChangeParentOutput(wallet, *wallet.GetWalletTx(outpoint.hash)->tx, outpoint.n);
}

std::map<CTxDestination, std::vector<COutput>> ListCoins(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    std::map<CTxDestination, std::vector<COutput>> result;
    std::vector<COutput> availableCoins;

    AvailableCoinsListUnspent(wallet, availableCoins);

    for (const COutput& coin : availableCoins) {
        CTxDestination address;
        if ((coin.spendable || (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && coin.solvable)) &&
            ExtractDestination(FindNonChangeParentOutput(wallet, coin.outpoint).scriptPubKey, address)) {
            result[address].emplace_back(std::move(coin));
        }
    }

    std::vector<COutPoint> lockedCoins;
    wallet.ListLockedCoins(lockedCoins);
    // Include watch-only for LegacyScriptPubKeyMan wallets without private keys
    const bool include_watch_only = wallet.GetLegacyScriptPubKeyMan() && wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    const isminetype is_mine_filter = include_watch_only ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
    for (const COutPoint& output : lockedCoins) {
        auto it = wallet.mapWallet.find(output.hash);
        if (it != wallet.mapWallet.end()) {
            const auto& wtx = it->second;
            int depth = wallet.GetTxDepthInMainChain(wtx);
            if (depth >= 0 && output.n < wtx.tx->vout.size() &&
                wallet.IsMine(wtx.tx->vout[output.n]) == is_mine_filter
            ) {
                CTxDestination address;
                if (ExtractDestination(FindNonChangeParentOutput(wallet, *wtx.tx, output.n).scriptPubKey, address)) {
                    result[address].emplace_back(
                        COutPoint(wtx.GetHash(), output.n), wtx.tx->vout.at(output.n), depth, GetTxSpendSize(wallet, wtx, output.n), /*spendable=*/ true, /*solvable=*/ true, /*safe=*/ false, wtx.GetTxTime(), CachedTxIsFromMe(wallet, wtx, ISMINE_ALL));
                }
            }
        }
    }

    return result;
}

std::vector<OutputGroup> GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const CoinEligibilityFilter& filter, bool positive_only)
{
    std::vector<OutputGroup> groups_out;

    if (!coin_sel_params.m_avoid_partial_spends) {
        // Allowing partial spends  means no grouping. Each COutput gets its own OutputGroup.
        for (const COutput& output : outputs) {
            // Skip outputs we cannot spend
            if (!output.spendable) continue;

            size_t ancestors, descendants;
            wallet.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);

            // Make an OutputGroup containing just this output
            OutputGroup group{coin_sel_params};
            group.Insert(output, ancestors, descendants, positive_only);

            // Check the OutputGroup's eligibility. Only add the eligible ones.
            if (positive_only && group.GetSelectionAmount() <= 0) continue;
            if (group.m_outputs.size() > 0 && group.EligibleForSpending(filter)) groups_out.push_back(group);
        }
        return groups_out;
    }

    // We want to combine COutputs that have the same scriptPubKey into single OutputGroups
    // except when there are more than OUTPUT_GROUP_MAX_ENTRIES COutputs grouped in an OutputGroup.
    // To do this, we maintain a map where the key is the scriptPubKey and the value is a vector of OutputGroups.
    // For each COutput, we check if the scriptPubKey is in the map, and if it is, the COutput is added
    // to the last OutputGroup in the vector for the scriptPubKey. When the last OutputGroup has
    // OUTPUT_GROUP_MAX_ENTRIES COutputs, a new OutputGroup is added to the end of the vector.
    std::map<CScript, std::vector<OutputGroup>> spk_to_groups_map;
    for (const auto& output : outputs) {
        // Skip outputs we cannot spend
        if (!output.spendable) continue;

        size_t ancestors, descendants;
        wallet.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);
        CScript spk = output.txout.scriptPubKey;

        std::vector<OutputGroup>& groups = spk_to_groups_map[spk];

        if (groups.size() == 0) {
            // No OutputGroups for this scriptPubKey yet, add one
            groups.emplace_back(coin_sel_params);
        }

        // Get the last OutputGroup in the vector so that we can add the COutput to it
        // A pointer is used here so that group can be reassigned later if it is full.
        OutputGroup* group = &groups.back();

        // Check if this OutputGroup is full. We limit to OUTPUT_GROUP_MAX_ENTRIES when using -avoidpartialspends
        // to avoid surprising users with very high fees.
        if (group->m_outputs.size() >= OUTPUT_GROUP_MAX_ENTRIES) {
            // The last output group is full, add a new group to the vector and use that group for the insertion
            groups.emplace_back(coin_sel_params);
            group = &groups.back();
        }

        // Add the output to group
        group->Insert(output, ancestors, descendants, positive_only);
    }

    // Now we go through the entire map and pull out the OutputGroups
    for (const auto& spk_and_groups_pair: spk_to_groups_map) {
        const std::vector<OutputGroup>& groups_per_spk= spk_and_groups_pair.second;

        // Go through the vector backwards. This allows for the first item we deal with being the partial group.
        for (auto group_it = groups_per_spk.rbegin(); group_it != groups_per_spk.rend(); group_it++) {
            const OutputGroup& group = *group_it;

            // Don't include partial groups if there are full groups too and we don't want partial groups
            if (group_it == groups_per_spk.rbegin() && groups_per_spk.size() > 1 && !filter.m_include_partial_groups) {
                continue;
            }

            // Check the OutputGroup's eligibility. Only add the eligible ones.
            if (positive_only && group.GetSelectionAmount() <= 0) continue;
            if (group.m_outputs.size() > 0 && group.EligibleForSpending(filter)) groups_out.push_back(group);
        }
    }

    return groups_out;
}

std::optional<SelectionResult> AttemptSelection(const CWallet& wallet, const CAmount& nTargetValue, const CoinEligibilityFilter& eligibility_filter, std::vector<COutput> coins,
                               const CoinSelectionParams& coin_selection_params)
{
    // Vector of results. We will choose the best one based on waste.
    std::vector<SelectionResult> results;

    // Note that unlike KnapsackSolver, we do not include the fee for creating a change output as BnB will not create a change output.
    std::vector<OutputGroup> positive_groups = GroupOutputs(wallet, coins, coin_selection_params, eligibility_filter, true /* positive_only */);
    if (auto bnb_result{SelectCoinsBnB(positive_groups, nTargetValue, coin_selection_params.m_cost_of_change)}) {
        results.push_back(*bnb_result);
    }

    // The knapsack solver has some legacy behavior where it will spend dust outputs. We retain this behavior, so don't filter for positive only here.
    std::vector<OutputGroup> all_groups = GroupOutputs(wallet, coins, coin_selection_params, eligibility_filter, false /* positive_only */);
    // While nTargetValue includes the transaction fees for non-input things, it does not include the fee for creating a change output.
    // So we need to include that for KnapsackSolver as well, as we are expecting to create a change output.
    if (auto knapsack_result{KnapsackSolver(all_groups, nTargetValue + coin_selection_params.m_change_fee,
                                            coin_selection_params.m_min_change_target, coin_selection_params.rng_fast)}) {
        knapsack_result->ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
        results.push_back(*knapsack_result);
    }

    // Include change for SRD as we want to avoid making really small change if the selection just
    // barely meets the target. Just use the lower bound change target instead of the randomly
    // generated one, since SRD will result in a random change amount anyway; avoid making the
    // target needlessly large.
    const CAmount srd_target = nTargetValue + coin_selection_params.m_change_fee + CHANGE_LOWER;
    if (auto srd_result{SelectCoinsSRD(positive_groups, srd_target, coin_selection_params.rng_fast)}) {
        srd_result->ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
        results.push_back(*srd_result);
    }

    if (results.size() == 0) {
        // No solution found
        return std::nullopt;
    }

    // Choose the result with the least waste
    // If the waste is the same, choose the one which spends more inputs.
    auto& best_result = *std::min_element(results.begin(), results.end());
    return best_result;
}

std::optional<SelectionResult> SelectCoins(const CWallet& wallet, const std::vector<COutput>& vAvailableCoins, const CAmount& nTargetValue, const CCoinControl& coin_control, const CoinSelectionParams& coin_selection_params)
{
    std::vector<COutput> vCoins(vAvailableCoins);
    CAmount value_to_select = nTargetValue;

    OutputGroup preset_inputs(coin_selection_params);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coin_control.HasSelected() && !coin_control.fAllowOtherInputs)
    {
        for (const COutput& out : vCoins) {
            if (!out.spendable) continue;
            /* Set ancestors and descendants to 0 as these don't matter for preset inputs as no actual selection is being done.
             * positive_only is set to false because we want to include all preset inputs, even if they are dust.
             */
            preset_inputs.Insert(out, /*ancestors=*/ 0, /*descendants=*/ 0, /*positive_only=*/ false);
        }
        SelectionResult result(nTargetValue, SelectionAlgorithm::MANUAL);
        result.AddInput(preset_inputs);
        if (result.GetSelectedValue() < nTargetValue) return std::nullopt;
        result.ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
        return result;
    }

    // calculate value from preset inputs and store them
    std::set<COutPoint> preset_coins;

    std::vector<COutPoint> vPresetInputs;
    coin_control.ListSelected(vPresetInputs);
    for (const COutPoint& outpoint : vPresetInputs) {
        int input_bytes = -1;
        CTxOut txout;
        std::map<uint256, CWalletTx>::const_iterator it = wallet.mapWallet.find(outpoint.hash);
        if (it != wallet.mapWallet.end()) {
            const CWalletTx& wtx = it->second;
            // Clearly invalid input, fail
            if (wtx.tx->vout.size() <= outpoint.n) {
                return std::nullopt;
            }
            input_bytes = GetTxSpendSize(wallet, wtx, outpoint.n, false);
            txout = wtx.tx->vout.at(outpoint.n);
        } else {
            // The input is external. We did not find the tx in mapWallet.
            if (!coin_control.GetExternalOutput(outpoint, txout)) {
                return std::nullopt;
            }
            input_bytes = CalculateMaximumSignedInputSize(txout, &coin_control.m_external_provider, /*use_max_sig=*/true);
        }
        // If available, override calculated size with coin control specified size
        if (coin_control.HasInputWeight(outpoint)) {
            input_bytes = GetVirtualTransactionSize(coin_control.GetInputWeight(outpoint), 0, 0);
        }

        if (input_bytes == -1) {
            return std::nullopt; // Not solvable, can't estimate size for fee
        }

        /* Set some defaults for depth, spendable, solvable, safe, time, and from_me as these don't matter for preset inputs since no selection is being done. */
        COutput output(outpoint, txout, /*depth=*/ 0, input_bytes, /*spendable=*/ true, /*solvable=*/ true, /*safe=*/ true, /*time=*/ 0, /*from_me=*/ false, coin_selection_params.m_effective_feerate);
        if (coin_selection_params.m_subtract_fee_outputs) {
            value_to_select -= output.txout.nValue;
        } else {
            value_to_select -= output.GetEffectiveValue();
        }
        preset_coins.insert(outpoint);
        /* Set ancestors and descendants to 0 as they don't matter for preset inputs since no actual selection is being done.
         * positive_only is set to false because we want to include all preset inputs, even if they are dust.
         */
        preset_inputs.Insert(output, /*ancestors=*/ 0, /*descendants=*/ 0, /*positive_only=*/ false);
    }

    // remove preset inputs from vCoins so that Coin Selection doesn't pick them.
    for (std::vector<COutput>::iterator it = vCoins.begin(); it != vCoins.end() && coin_control.HasSelected();)
    {
        if (preset_coins.count(it->outpoint))
            it = vCoins.erase(it);
        else
            ++it;
    }

    unsigned int limit_ancestor_count = 0;
    unsigned int limit_descendant_count = 0;
    wallet.chain().getPackageLimits(limit_ancestor_count, limit_descendant_count);
    const size_t max_ancestors = (size_t)std::max<int64_t>(1, limit_ancestor_count);
    const size_t max_descendants = (size_t)std::max<int64_t>(1, limit_descendant_count);
    const bool fRejectLongChains = gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS);

    // form groups from remaining coins; note that preset coins will not
    // automatically have their associated (same address) coins included
    if (coin_control.m_avoid_partial_spends && vCoins.size() > OUTPUT_GROUP_MAX_ENTRIES) {
        // Cases where we have 101+ outputs all pointing to the same destination may result in
        // privacy leaks as they will potentially be deterministically sorted. We solve that by
        // explicitly shuffling the outputs before processing
        Shuffle(vCoins.begin(), vCoins.end(), coin_selection_params.rng_fast);
    }

    // Coin Selection attempts to select inputs from a pool of eligible UTXOs to fund the
    // transaction at a target feerate. If an attempt fails, more attempts may be made using a more
    // permissive CoinEligibilityFilter.
    std::optional<SelectionResult> res = [&] {
        // Pre-selected inputs already cover the target amount.
        if (value_to_select <= 0) return std::make_optional(SelectionResult(nTargetValue, SelectionAlgorithm::MANUAL));

        // If possible, fund the transaction with confirmed UTXOs only. Prefer at least six
        // confirmations on outputs received from other wallets and only spend confirmed change.
        if (auto r1{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(1, 6, 0), vCoins, coin_selection_params)}) return r1;
        if (auto r2{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(1, 1, 0), vCoins, coin_selection_params)}) return r2;

        // Fall back to using zero confirmation change (but with as few ancestors in the mempool as
        // possible) if we cannot fund the transaction otherwise.
        if (wallet.m_spend_zero_conf_change) {
            if (auto r3{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, 2), vCoins, coin_selection_params)}) return r3;
            if (auto r4{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, std::min((size_t)4, max_ancestors/3), std::min((size_t)4, max_descendants/3)),
                                   vCoins, coin_selection_params)}) {
                return r4;
            }
            if (auto r5{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, max_ancestors/2, max_descendants/2),
                                   vCoins, coin_selection_params)}) {
                return r5;
            }
            // If partial groups are allowed, relax the requirement of spending OutputGroups (groups
            // of UTXOs sent to the same address, which are obviously controlled by a single wallet)
            // in their entirety.
            if (auto r6{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, max_ancestors-1, max_descendants-1, true /* include_partial_groups */),
                                   vCoins, coin_selection_params)}) {
                return r6;
            }
            // Try with unsafe inputs if they are allowed. This may spend unconfirmed outputs
            // received from other wallets.
            if (coin_control.m_include_unsafe_inputs) {
                if (auto r7{AttemptSelection(wallet, value_to_select,
                    CoinEligibilityFilter(0 /* conf_mine */, 0 /* conf_theirs */, max_ancestors-1, max_descendants-1, true /* include_partial_groups */),
                    vCoins, coin_selection_params)}) {
                    return r7;
                }
            }
            // Try with unlimited ancestors/descendants. The transaction will still need to meet
            // mempool ancestor/descendant policy to be accepted to mempool and broadcasted, but
            // OutputGroups use heuristics that may overestimate ancestor/descendant counts.
            if (!fRejectLongChains) {
                if (auto r8{AttemptSelection(wallet, value_to_select,
                                      CoinEligibilityFilter(0, 1, std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), true /* include_partial_groups */),
                                      vCoins, coin_selection_params)}) {
                    return r8;
                }
            }
        }
        // Coin Selection failed.
        return std::optional<SelectionResult>();
    }();

    if (!res) return std::nullopt;

    // Add preset inputs to result
    res->AddInput(preset_inputs);
    if (res->m_algo == SelectionAlgorithm::MANUAL) {
        res->ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
    }

    return res;
}

static bool IsCurrentForAntiFeeSniping(interfaces::Chain& chain, const uint256& block_hash)
{
    if (chain.isInitialBlockDownload()) {
        return false;
    }
    constexpr int64_t MAX_ANTI_FEE_SNIPING_TIP_AGE = 8 * 60 * 60; // in seconds
    int64_t block_time;
    CHECK_NONFATAL(chain.findBlock(block_hash, FoundBlock().time(block_time)));
    if (block_time < (GetTime() - MAX_ANTI_FEE_SNIPING_TIP_AGE)) {
        return false;
    }
    return true;
}

/**
 * Set a height-based locktime for new transactions (uses the height of the
 * current chain tip unless we are not synced with the current chain
 */
static void DiscourageFeeSniping(CMutableTransaction& tx, FastRandomContext& rng_fast,
                                 interfaces::Chain& chain, const uint256& block_hash, int block_height)
{
    // All inputs must be added by now
    assert(!tx.vin.empty());
    // Discourage fee sniping.
    //
    // For a large miner the value of the transactions in the best block and
    // the mempool can exceed the cost of deliberately attempting to mine two
    // blocks to orphan the current best block. By setting nLockTime such that
    // only the next block can include the transaction, we discourage this
    // practice as the height restricted and limited blocksize gives miners
    // considering fee sniping fewer options for pulling off this attack.
    //
    // A simple way to think about this is from the wallet's point of view we
    // always want the blockchain to move forward. By setting nLockTime this
    // way we're basically making the statement that we only want this
    // transaction to appear in the next block; we don't want to potentially
    // encourage reorgs by allowing transactions to appear at lower heights
    // than the next block in forks of the best chain.
    //
    // Of course, the subsidy is high enough, and transaction volume low
    // enough, that fee sniping isn't a problem yet, but by implementing a fix
    // now we ensure code won't be written that makes assumptions about
    // nLockTime that preclude a fix later.
    if (IsCurrentForAntiFeeSniping(chain, block_hash)) {
        tx.nLockTime = block_height;

        // Secondly occasionally randomly pick a nLockTime even further back, so
        // that transactions that are delayed after signing for whatever reason,
        // e.g. high-latency mix networks and some CoinJoin implementations, have
        // better privacy.
        if (rng_fast.randrange(10) == 0) {
            tx.nLockTime = std::max(0, int(tx.nLockTime) - int(rng_fast.randrange(100)));
        }
    } else {
        // If our chain is lagging behind, we can't discourage fee sniping nor help
        // the privacy of high-latency transactions. To avoid leaking a potentially
        // unique "nLockTime fingerprint", set nLockTime to a constant.
        tx.nLockTime = 0;
    }
    // Sanity check all values
    assert(tx.nLockTime < LOCKTIME_THRESHOLD); // Type must be block height
    assert(tx.nLockTime <= uint64_t(block_height));
    for (const auto& in : tx.vin) {
        // Can not be FINAL for locktime to work
        assert(in.nSequence != CTxIn::SEQUENCE_FINAL);
        // May be MAX NONFINAL to disable both BIP68 and BIP125
        if (in.nSequence == CTxIn::MAX_SEQUENCE_NONFINAL) continue;
        // May be MAX BIP125 to disable BIP68 and enable BIP125
        if (in.nSequence == MAX_BIP125_RBF_SEQUENCE) continue;
        // The wallet does not support any other sequence-use right now.
        assert(false);
    }
}

static std::optional<CreatedTransactionResult> CreateTransactionInternal(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        int change_pos,
        bilingual_str& error,
        const CCoinControl& coin_control,
        FeeCalculation& fee_calc_out,
        bool sign) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    // out variables, to be packed into returned result structure
    CTransactionRef tx;
    CAmount nFeeRet;
    int nChangePosInOut = change_pos;

    FastRandomContext rng_fast;
    CMutableTransaction txNew; // The resulting transaction that we make

    CoinSelectionParams coin_selection_params{rng_fast}; // Parameters for coin selection, init with dummy
    coin_selection_params.m_avoid_partial_spends = coin_control.m_avoid_partial_spends;

    // Set the long term feerate estimate to the wallet's consolidate feerate
    coin_selection_params.m_long_term_feerate = wallet.m_consolidate_feerate;

    CAmount recipients_sum = 0;
    const OutputType change_type = wallet.TransactionChangeType(coin_control.m_change_type ? *coin_control.m_change_type : wallet.m_default_change_type, vecSend);
    ReserveDestination reservedest(&wallet, change_type);
    unsigned int outputs_to_subtract_fee_from = 0; // The number of outputs which we are subtracting the fee from
    for (const auto& recipient : vecSend) {
        recipients_sum += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount) {
            outputs_to_subtract_fee_from++;
            coin_selection_params.m_subtract_fee_outputs = true;
        }
    }
    coin_selection_params.m_change_target = GenerateChangeTarget(std::floor(recipients_sum / vecSend.size()), rng_fast);

    // Create change script that will be used if we need change
    CScript scriptChange;

    // coin control: send change to custom address
    if (!std::get_if<CNoDestination>(&coin_control.destChange)) {
        scriptChange = GetScriptForDestination(coin_control.destChange);
    } else { // no coin control: send change to newly generated address
        // Note: We use a new key here to keep it from being obvious which side is the change.
        //  The drawback is that by not reusing a previous key, the change may be lost if a
        //  backup is restored, if the backup doesn't have the new private key for the change.
        //  If we reused the old key, it would be possible to add code to look for and
        //  rediscover unknown transactions that were written with keys of ours to recover
        //  post-backup change.

        // Reserve a new key pair from key pool. If it fails, provide a dummy
        // destination in case we don't need change.
        CTxDestination dest;
        bilingual_str dest_err;
        if (!reservedest.GetReservedDestination(dest, true, dest_err)) {
            error = _("Transaction needs a change address, but we can't generate it.") + Untranslated(" ") + dest_err;
        }
        scriptChange = GetScriptForDestination(dest);
        // A valid destination implies a change script (and
        // vice-versa). An empty change script will abort later, if the
        // change keypool ran out, but change is required.
        CHECK_NONFATAL(IsValidDestination(dest) != scriptChange.empty());
    }
    CTxOut change_prototype_txout(0, scriptChange);
    coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);

    // Get size of spending the change output
    int change_spend_size = CalculateMaximumSignedInputSize(change_prototype_txout, &wallet);
    // If the wallet doesn't know how to sign change output, assume p2sh-p2wpkh
    // as lower-bound to allow BnB to do it's thing
    if (change_spend_size == -1) {
        coin_selection_params.change_spend_size = DUMMY_NESTED_P2WPKH_INPUT_SIZE;
    } else {
        coin_selection_params.change_spend_size = (size_t)change_spend_size;
    }

    // Set discard feerate
    coin_selection_params.m_discard_feerate = GetDiscardRate(wallet);

    // Get the fee rate to use effective values in coin selection
    FeeCalculation feeCalc;
    coin_selection_params.m_effective_feerate = GetMinimumFeeRate(wallet, coin_control, &feeCalc);
    // Do not, ever, assume that it's fine to change the fee rate if the user has explicitly
    // provided one
    if (coin_control.m_feerate && coin_selection_params.m_effective_feerate > *coin_control.m_feerate) {
        error = strprintf(_("Fee rate (%s) is lower than the minimum fee rate setting (%s)"), coin_control.m_feerate->ToString(FeeEstimateMode::SAT_VB), coin_selection_params.m_effective_feerate.ToString(FeeEstimateMode::SAT_VB));
        return std::nullopt;
    }
    if (feeCalc.reason == FeeReason::FALLBACK && !wallet.m_allow_fallback_fee) {
        // eventually allow a fallback fee
        error = _("Fee estimation failed. Fallbackfee is disabled. Wait a few blocks or enable -fallbackfee.");
        return std::nullopt;
    }

    // Calculate the cost of change
    // Cost of change is the cost of creating the change output + cost of spending the change output in the future.
    // For creating the change output now, we use the effective feerate.
    // For spending the change output in the future, we use the discard feerate for now.
    // So cost of change = (change output size * effective feerate) + (size of spending change output * discard feerate)
    coin_selection_params.m_change_fee = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.change_output_size);
    coin_selection_params.m_cost_of_change = coin_selection_params.m_discard_feerate.GetFee(coin_selection_params.change_spend_size) + coin_selection_params.m_change_fee;

    // vouts to the payees
    if (!coin_selection_params.m_subtract_fee_outputs) {
        coin_selection_params.tx_noinputs_size = 11; // Static vsize overhead + outputs vsize. 4 nVersion, 4 nLocktime, 1 input count, 1 output count, 1 witness overhead (dummy, flag, stack size)
    }
    for (const auto& recipient : vecSend)
    {
        CTxOut txout(recipient.nAmount, recipient.scriptPubKey);

        // Include the fee cost for outputs.
        if (!coin_selection_params.m_subtract_fee_outputs) {
            coin_selection_params.tx_noinputs_size += ::GetSerializeSize(txout, PROTOCOL_VERSION);
        }

        if (IsDust(txout, wallet.chain().relayDustFee()))
        {
            error = _("Transaction amount too small");
            return std::nullopt;
        }
        txNew.vout.push_back(txout);
    }

    // Include the fees for things that aren't inputs, excluding the change output
    const CAmount not_input_fees = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.tx_noinputs_size);
    CAmount selection_target = recipients_sum + not_input_fees;

    // Get available coins
    std::vector<COutput> vAvailableCoins;
    AvailableCoins(wallet, vAvailableCoins, &coin_control, coin_selection_params.m_effective_feerate, 1, MAX_MONEY, MAX_MONEY, 0);

    // Choose coins to use
    std::optional<SelectionResult> result = SelectCoins(wallet, vAvailableCoins, /*nTargetValue=*/selection_target, coin_control, coin_selection_params);
    if (!result) {
        error = _("Insufficient funds");
        return std::nullopt;
    }
    TRACE5(coin_selection, selected_coins, wallet.GetName().c_str(), GetAlgorithmName(result->m_algo).c_str(), result->m_target, result->GetWaste(), result->GetSelectedValue());

    // Always make a change output
    // We will reduce the fee from this change output later, and remove the output if it is too small.
    const CAmount change_and_fee = result->GetSelectedValue() - recipients_sum;
    assert(change_and_fee >= 0);
    CTxOut newTxOut(change_and_fee, scriptChange);

    if (nChangePosInOut == -1) {
        // Insert change txn at random position:
        nChangePosInOut = rng_fast.randrange(txNew.vout.size() + 1);
    }
    else if ((unsigned int)nChangePosInOut > txNew.vout.size())
    {
        error = _("Transaction change output index out of range");
        return std::nullopt;
    }

    assert(nChangePosInOut != -1);
    auto change_position = txNew.vout.insert(txNew.vout.begin() + nChangePosInOut, newTxOut);

    // Shuffle selected coins and fill in final vin
    std::vector<COutput> selected_coins = result->GetShuffledInputVector();

    // The sequence number is set to non-maxint so that DiscourageFeeSniping
    // works.
    //
    // BIP125 defines opt-in RBF as any nSequence < maxint-1, so
    // we use the highest possible value in that range (maxint-2)
    // to avoid conflicting with other possible uses of nSequence,
    // and in the spirit of "smallest possible change from prior
    // behavior."
    const uint32_t nSequence{coin_control.m_signal_bip125_rbf.value_or(wallet.m_signal_rbf) ? MAX_BIP125_RBF_SEQUENCE : CTxIn::MAX_SEQUENCE_NONFINAL};
    for (const auto& coin : selected_coins) {
        txNew.vin.push_back(CTxIn(coin.outpoint, CScript(), nSequence));
    }
    DiscourageFeeSniping(txNew, rng_fast, wallet.chain(), wallet.GetLastBlockHash(), wallet.GetLastBlockHeight());

    // Calculate the transaction fee
    TxSize tx_sizes = CalculateMaximumSignedTxSize(CTransaction(txNew), &wallet, &coin_control);
    int nBytes = tx_sizes.vsize;
    if (nBytes == -1) {
        error = _("Missing solving data for estimating transaction size");
        return std::nullopt;
    }
    nFeeRet = coin_selection_params.m_effective_feerate.GetFee(nBytes);

    // Subtract fee from the change output if not subtracting it from recipient outputs
    CAmount fee_needed = nFeeRet;
    if (!coin_selection_params.m_subtract_fee_outputs) {
        change_position->nValue -= fee_needed;
    }

    // We want to drop the change to fees if:
    // 1. The change output would be dust
    // 2. The change is within the (almost) exact match window, i.e. it is less than or equal to the cost of the change output (cost_of_change)
    CAmount change_amount = change_position->nValue;
    if (IsDust(*change_position, coin_selection_params.m_discard_feerate) || change_amount <= coin_selection_params.m_cost_of_change)
    {
        nChangePosInOut = -1;
        change_amount = 0;
        txNew.vout.erase(change_position);

        // Because we have dropped this change, the tx size and required fee will be different, so let's recalculate those
        tx_sizes = CalculateMaximumSignedTxSize(CTransaction(txNew), &wallet, &coin_control);
        nBytes = tx_sizes.vsize;
        fee_needed = coin_selection_params.m_effective_feerate.GetFee(nBytes);
    }

    // The only time that fee_needed should be less than the amount available for fees (in change_and_fee - change_amount) is when
    // we are subtracting the fee from the outputs. If this occurs at any other time, it is a bug.
    assert(coin_selection_params.m_subtract_fee_outputs || fee_needed <= change_and_fee - change_amount);

    // Update nFeeRet in case fee_needed changed due to dropping the change output
    if (fee_needed <= change_and_fee - change_amount) {
        nFeeRet = change_and_fee - change_amount;
    }

    // Reduce output values for subtractFeeFromAmount
    if (coin_selection_params.m_subtract_fee_outputs) {
        CAmount to_reduce = fee_needed + change_amount - change_and_fee;
        int i = 0;
        bool fFirst = true;
        for (const auto& recipient : vecSend)
        {
            if (i == nChangePosInOut) {
                ++i;
            }
            CTxOut& txout = txNew.vout[i];

            if (recipient.fSubtractFeeFromAmount)
            {
                txout.nValue -= to_reduce / outputs_to_subtract_fee_from; // Subtract fee equally from each selected recipient

                if (fFirst) // first receiver pays the remainder not divisible by output count
                {
                    fFirst = false;
                    txout.nValue -= to_reduce % outputs_to_subtract_fee_from;
                }

                // Error if this output is reduced to be below dust
                if (IsDust(txout, wallet.chain().relayDustFee())) {
                    if (txout.nValue < 0) {
                        error = _("The transaction amount is too small to pay the fee");
                    } else {
                        error = _("The transaction amount is too small to send after the fee has been deducted");
                    }
                    return std::nullopt;
                }
            }
            ++i;
        }
        nFeeRet = fee_needed;
    }

    // Give up if change keypool ran out and change is required
    if (scriptChange.empty() && nChangePosInOut != -1) {
        return std::nullopt;
    }

    if (sign && !wallet.SignTransaction(txNew)) {
        error = _("Signing transaction failed");
        return std::nullopt;
    }

    // Return the constructed transaction data.
    tx = MakeTransactionRef(std::move(txNew));

    // Limit size
    if ((sign && GetTransactionWeight(*tx) > MAX_STANDARD_TX_WEIGHT) ||
        (!sign && tx_sizes.weight > MAX_STANDARD_TX_WEIGHT))
    {
        error = _("Transaction too large");
        return std::nullopt;
    }

    if (nFeeRet > wallet.m_default_max_tx_fee) {
        error = TransactionErrorString(TransactionError::MAX_FEE_EXCEEDED);
        return std::nullopt;
    }

    if (gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the mempool's chain limits
        if (!wallet.chain().checkChainLimits(tx)) {
            error = _("Transaction has too long of a mempool chain");
            return std::nullopt;
        }
    }

    // Before we return success, we assume any change key will be used to prevent
    // accidental re-use.
    reservedest.KeepDestination();
    fee_calc_out = feeCalc;

    wallet.WalletLogPrintf("Fee Calculation: Fee:%d Bytes:%u Tgt:%d (requested %d) Reason:\"%s\" Decay %.5f: Estimation: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out) Fail: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out)\n",
              nFeeRet, nBytes, feeCalc.returnedTarget, feeCalc.desiredTarget, StringForFeeReason(feeCalc.reason), feeCalc.est.decay,
              feeCalc.est.pass.start, feeCalc.est.pass.end,
              (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) > 0.0 ? 100 * feeCalc.est.pass.withinTarget / (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) : 0.0,
              feeCalc.est.pass.withinTarget, feeCalc.est.pass.totalConfirmed, feeCalc.est.pass.inMempool, feeCalc.est.pass.leftMempool,
              feeCalc.est.fail.start, feeCalc.est.fail.end,
              (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) > 0.0 ? 100 * feeCalc.est.fail.withinTarget / (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) : 0.0,
              feeCalc.est.fail.withinTarget, feeCalc.est.fail.totalConfirmed, feeCalc.est.fail.inMempool, feeCalc.est.fail.leftMempool);
    return CreatedTransactionResult(tx, nFeeRet, nChangePosInOut);
}

std::optional<CreatedTransactionResult> CreateTransaction(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        int change_pos,
        bilingual_str& error,
        const CCoinControl& coin_control,
        FeeCalculation& fee_calc_out,
        bool sign)
{
    if (vecSend.empty()) {
        error = _("Transaction must have at least one recipient");
        return std::nullopt;
    }

    if (std::any_of(vecSend.cbegin(), vecSend.cend(), [](const auto& recipient){ return recipient.nAmount < 0; })) {
        error = _("Transaction amounts must not be negative");
        return std::nullopt;
    }

    LOCK(wallet.cs_wallet);

    std::optional<CreatedTransactionResult> txr_ungrouped = CreateTransactionInternal(wallet, vecSend, change_pos, error, coin_control, fee_calc_out, sign);
    TRACE4(coin_selection, normal_create_tx_internal, wallet.GetName().c_str(), txr_ungrouped.has_value(),
           txr_ungrouped.has_value() ? txr_ungrouped->fee : 0, txr_ungrouped.has_value() ? txr_ungrouped->change_pos : 0);
    if (!txr_ungrouped) return std::nullopt;
    // try with avoidpartialspends unless it's enabled already
    if (txr_ungrouped->fee > 0 /* 0 means non-functional fee rate estimation */ && wallet.m_max_aps_fee > -1 && !coin_control.m_avoid_partial_spends) {
        TRACE1(coin_selection, attempting_aps_create_tx, wallet.GetName().c_str());
        CCoinControl tmp_cc = coin_control;
        tmp_cc.m_avoid_partial_spends = true;
        bilingual_str error2; // fired and forgotten; if an error occurs, we discard the results
        std::optional<CreatedTransactionResult> txr_grouped = CreateTransactionInternal(wallet, vecSend, change_pos, error2, tmp_cc, fee_calc_out, sign);
        // if fee of this alternative one is within the range of the max fee, we use this one
        const bool use_aps{txr_grouped.has_value() ? (txr_grouped->fee <= txr_ungrouped->fee + wallet.m_max_aps_fee) : false};
        TRACE5(coin_selection, aps_create_tx_internal, wallet.GetName().c_str(), use_aps, txr_grouped.has_value(),
               txr_grouped.has_value() ? txr_grouped->fee : 0, txr_grouped.has_value() ? txr_grouped->change_pos : 0);
        if (txr_grouped) {
            wallet.WalletLogPrintf("Fee non-grouped = %lld, grouped = %lld, using %s\n",
                txr_ungrouped->fee, txr_grouped->fee, use_aps ? "grouped" : "non-grouped");
            if (use_aps) return txr_grouped;
        }
    }
    return txr_ungrouped;
}

bool FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& nFeeRet, int& nChangePosInOut, bilingual_str& error, bool lockUnspents, const std::set<int>& setSubtractFeeFromOutputs, CCoinControl coinControl)
{
    std::vector<CRecipient> vecSend;

    // Turn the txout set into a CRecipient vector.
    for (size_t idx = 0; idx < tx.vout.size(); idx++) {
        const CTxOut& txOut = tx.vout[idx];
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, setSubtractFeeFromOutputs.count(idx) == 1};
        vecSend.push_back(recipient);
    }

    coinControl.fAllowOtherInputs = true;

    for (const CTxIn& txin : tx.vin) {
        coinControl.Select(txin.prevout);
    }

    // Acquire the locks to prevent races to the new locked unspents between the
    // CreateTransaction call and LockCoin calls (when lockUnspents is true).
    LOCK(wallet.cs_wallet);

    FeeCalculation fee_calc_out;
    std::optional<CreatedTransactionResult> txr = CreateTransaction(wallet, vecSend, nChangePosInOut, error, coinControl, fee_calc_out, false);
    if (!txr) return false;
    CTransactionRef tx_new = txr->tx;
    nFeeRet = txr->fee;
    nChangePosInOut = txr->change_pos;

    if (nChangePosInOut != -1) {
        tx.vout.insert(tx.vout.begin() + nChangePosInOut, tx_new->vout[nChangePosInOut]);
    }

    // Copy output sizes from new transaction; they may have had the fee
    // subtracted from them.
    for (unsigned int idx = 0; idx < tx.vout.size(); idx++) {
        tx.vout[idx].nValue = tx_new->vout[idx].nValue;
    }

    // Add new txins while keeping original txin scriptSig/order.
    for (const CTxIn& txin : tx_new->vin) {
        if (!coinControl.IsSelected(txin.prevout)) {
            tx.vin.push_back(txin);

        }
        if (lockUnspents) {
            wallet.LockCoin(txin.prevout);
        }

    }

    return true;
}
} // namespace wallet