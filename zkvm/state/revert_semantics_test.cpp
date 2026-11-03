// Copyright (C) 2025-26 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// The rollback spec of the guest state: nested acceptance and rejection,
// repeated writes to one field, reverted creation, destruction and warmth,
// then the transaction boundary and the end-of-transaction walks. The
// numbered cases follow the ZisK branch's revert_semantics_test.cpp; the
// pre-state is a hand-built witness so that journal behaviour is isolated
// from the trie.

#include <zkvm/state/state.hpp>
#include <zkvm/state/test_witness.hpp>

#include <category/core/address.hpp>
#include <category/core/byte_string.hpp>
#include <category/core/bytes.hpp>
#include <category/execution/ethereum/core/account.hpp>
#include <category/execution/ethereum/core/receipt.hpp>
#include <category/vm/evm/access_status.hpp>
#include <category/vm/evm/revision.hpp>
#include <category/vm/evm/storage_status.hpp>
#include <category/vm/evm/traits.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace monad;
using namespace monad::zkvm::test;

namespace
{
    using Shanghai = EvmTraits<EvmRevision::Shanghai>;
    using Cancun = EvmTraits<EvmRevision::Cancun>;

    // An address the witness proves absent (every fixture here has a leaf
    // or branch at the root that this hashes away from).
    constexpr auto NOBODY = 0x0000000000000000000000000000000000000009_address;

    Receipt::Log log_from(Address const &address)
    {
        Receipt::Log log{};
        log.address = address;
        return log;
    }
}

// ---------------------------------------------------------------------------
// The branch's numbered cases
// ---------------------------------------------------------------------------

// 1. Parent writes, child accepts, parent rejects. Accepting the child keeps
// its undo records, so rejecting the parent restores the pre-frame value.
TEST(ZkvmRevert, ParentRejectUndoesAcceptedChild)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 7})};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.set_nonce(ADDR, 8);
    st.push();
    st.set_nonce(ADDR, 9);
    st.pop_accept();
    EXPECT_EQ(st.get_nonce(ADDR), 9);
    st.pop_reject();
    EXPECT_EQ(st.get_nonce(ADDR), 7);
}

// 2. Parent writes, child rejects, parent accepts.
TEST(ZkvmRevert, ChildRejectRestoresParentValue)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 7})};
    auto &st = f.state;
    st.push();
    st.set_nonce(ADDR, 8);
    st.push();
    st.set_nonce(ADDR, 9);
    st.pop_reject();
    EXPECT_EQ(st.get_nonce(ADDR), 8);
    st.pop_accept();
    EXPECT_EQ(st.get_nonce(ADDR), 8);
}

// 3. The same field written three times in one frame, then rejected.
TEST(ZkvmRevert, RepeatedWritesRevertToPreFrameValue)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 7})};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.set_nonce(ADDR, 8);
    st.set_nonce(ADDR, 9);
    st.set_nonce(ADDR, 10);
    st.pop_reject();
    EXPECT_EQ(st.get_nonce(ADDR), 7);
}

// 4. An account materialised in the frame, then rejected, is absent again.
// The guest keeps the row (its trie answer is still valid) but the account
// is gone, which is what every reader sees.
TEST(ZkvmRevert, RejectedCreationLeavesNoAccount)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    EXPECT_FALSE(st.account_exists(NOBODY));
    st.push();
    st.add_to_balance(NOBODY, 100);
    EXPECT_TRUE(st.account_exists(NOBODY));
    st.pop_reject();
    EXPECT_FALSE(st.account_exists(NOBODY));
    EXPECT_EQ(st.get_balance(NOBODY), 0);
}

// 5. SELFDESTRUCT then revert.
TEST(ZkvmRevert, RejectedSelfdestructRestoresFlagAndBalance)
{
    Fixture f{two_account_blob(
        ADDR, Account{.balance = 500, .nonce = 1}, OTHER, Account{.nonce = 0})};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.selfdestruct<Shanghai>(ADDR, OTHER);
    EXPECT_TRUE(st.is_destructed(ADDR));
    EXPECT_EQ(st.get_balance(OTHER), 500);
    st.pop_reject();
    EXPECT_FALSE(st.is_destructed(ADDR));
    EXPECT_EQ(st.get_balance(ADDR), 500);
    EXPECT_EQ(st.get_balance(OTHER), 0);
}

// 6. Storage and transient storage.
TEST(ZkvmRevert, StorageAndTransientRevert)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.set_storage(ADDR, word(5), word(99));
    st.set_transient_storage(ADDR, word(6), word(77));
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(99));
    EXPECT_EQ(st.get_transient_storage(ADDR, word(6)), word(77));
    st.pop_reject();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(42));
    EXPECT_EQ(st.get_transient_storage(ADDR, word(6)), bytes32_t{});
}

// 7. EIP-2929: a key warmed in a frame that reverts is cold again.
TEST(ZkvmRevert, WarmthReverts)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Cold);
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Warm);
    EXPECT_EQ(st.access_storage<Shanghai>(ADDR, word(5)), AccessStatus::Cold);
    EXPECT_EQ(st.access_storage<Shanghai>(ADDR, word(5)), AccessStatus::Warm);
    st.pop_reject();
    st.push();
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Cold);
    EXPECT_EQ(st.access_storage<Shanghai>(ADDR, word(5)), AccessStatus::Cold);
    st.pop_accept();
}

// 8. A child rejection restores the parent's written value, not the
// witness's.
TEST(ZkvmRevert, ChildRejectRestoresParentSlotValue)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    st.push();
    st.set_storage(ADDR, word(5), word(99));
    st.push();
    st.set_storage(ADDR, word(5), word(123));
    st.pop_reject();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(99));
    st.pop_accept();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(99));
}

// 9. Repeated writes to one slot go back to the witness value, not to an
// intermediate one.
TEST(ZkvmRevert, RepeatedSlotWritesRevertToWitness)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.set_storage(ADDR, word(5), word(1));
    st.set_storage(ADDR, word(5), word(2));
    st.set_storage(ADDR, word(5), word(3));
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(3));
    st.pop_reject();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(42));
}

// 10. A parent rejection undoes slot writes from an accepted child, both to
// a shared slot and to one only the child touched.
TEST(ZkvmRevert, ParentRejectUndoesAcceptedChildSlots)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    st.set_nonce(ADDR, st.get_nonce(ADDR));
    st.push();
    st.set_storage(ADDR, word(5), word(7));
    st.push();
    st.set_storage(ADDR, word(5), word(8));
    st.set_storage(ADDR, word(6), word(9));
    st.pop_accept();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(8));
    EXPECT_EQ(st.get_storage(ADDR, word(6)), word(9));
    st.pop_reject();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(42));
    EXPECT_EQ(st.get_storage(ADDR, word(6)), bytes32_t{});
}

// 11. Nested rollback restores balances and reverses the touched transition.
TEST(ZkvmRevert, NestedBalanceAndTouched)
{
    Fixture f{account_blob(ADDR, Account{.balance = 1000, .nonce = 1})};
    auto &st = f.state;
    EXPECT_FALSE(st.is_touched(ADDR));
    st.access_account(ADDR);
    st.push();
    st.add_to_balance(ADDR, 50);
    st.push();
    st.subtract_from_balance(ADDR, 300);
    st.add_to_balance(ADDR, 7);
    EXPECT_EQ(st.get_balance(ADDR), 757);
    st.pop_reject();
    EXPECT_EQ(st.get_balance(ADDR), 1050);
    EXPECT_TRUE(st.is_touched(ADDR));
    st.pop_reject();
    EXPECT_EQ(st.get_balance(ADDR), 1000);
    EXPECT_FALSE(st.is_touched(ADDR));
}

// 12. The code hash is restored after rejection.
TEST(ZkvmRevert, CodeHashReverts)
{
    Fixture f{account_blob(
        ADDR, Account{.balance = 1, .code_hash = NULL_HASH, .nonce = 1})};
    auto &st = f.state;
    st.access_account(ADDR);
    st.push();
    st.set_code(ADDR, byte_string_view{CODE.data(), CODE.size()});
    EXPECT_EQ(st.get_code_hash(ADDR), CODE_HASH);
    EXPECT_EQ(st.get_code_size(ADDR), CODE.size());
    st.pop_reject();
    EXPECT_EQ(st.get_code_hash(ADDR), NULL_HASH);
    EXPECT_EQ(st.get_code_size(ADDR), 0u);
}

// 13. Rejecting a materialisation restores absence, not an empty account.
TEST(ZkvmRevert, RejectedMaterialisationIsAbsent)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    EXPECT_FALSE(st.account_exists(NOBODY));
    st.access_account(NOBODY);
    st.push();
    st.set_nonce(NOBODY, 5);
    EXPECT_TRUE(st.account_exists(NOBODY));
    EXPECT_EQ(st.get_nonce(NOBODY), 5);
    st.pop_reject();
    EXPECT_FALSE(st.account_exists(NOBODY));
}

// 14 (MIP-8 page tracking) does not apply: the guest has no page map.

// 15. The incarnation of an existing account moved and then rejected; the
// rest of the account is left alone.
TEST(ZkvmRevert, IncarnationChangeReverts)
{
    Fixture f{account_blob(
        ADDR, Account{.balance = 5, .code_hash = NULL_HASH, .nonce = 0})};
    auto &st = f.state;
    EXPECT_FALSE(st.is_current_incarnation(ADDR));
    st.access_account(ADDR);
    st.push();
    st.set_to_state_incarnation(ADDR);
    EXPECT_TRUE(st.is_current_incarnation(ADDR));
    st.pop_reject();
    EXPECT_FALSE(st.is_current_incarnation(ADDR));
    EXPECT_EQ(st.get_balance(ADDR), 5);
}

// ---------------------------------------------------------------------------
// Guest-specific: storage status, logs, the transaction boundary
// ---------------------------------------------------------------------------

TEST(ZkvmRevert, StorageStatusFollowsEip2200)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    // Witness slot 5 = 42, slot 6 absent.
    EXPECT_EQ(st.set_storage(ADDR, word(6), word(1)), StorageStatus::Added);
    EXPECT_EQ(st.set_storage(ADDR, word(6), word(2)), StorageStatus::Assigned);
    EXPECT_EQ(
        st.set_storage(ADDR, word(6), bytes32_t{}),
        StorageStatus::AddedDeleted);
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(43)), StorageStatus::Modified);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), word(42)),
        StorageStatus::ModifiedRestored);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), bytes32_t{}), StorageStatus::Deleted);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), word(42)),
        StorageStatus::DeletedRestored);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), bytes32_t{}), StorageStatus::Deleted);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), word(7)), StorageStatus::DeletedAdded);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), bytes32_t{}),
        StorageStatus::ModifiedDeleted);
}

// The EIP-2200 original is the transaction-start value: a write in an
// earlier transaction becomes the original of the next.
TEST(ZkvmRevert, TransactionStartValueFollowsWrites)
{
    Fixture f{storage_blob(ADDR, Account{.nonce = 1}, word(5), word(42))};
    auto &st = f.state;
    st.begin_transaction(0);
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(1)), StorageStatus::Modified);
    st.begin_transaction(1);
    // Original 1, current 1: writing 2 is a modification, writing 1 back
    // restores.
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(2)), StorageStatus::Modified);
    EXPECT_EQ(
        st.set_storage(ADDR, word(5), word(1)),
        StorageStatus::ModifiedRestored);
    // A frame that rewrites and is rejected leaves the original alone.
    st.push();
    st.set_storage(ADDR, word(5), word(9));
    st.pop_reject();
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(2)), StorageStatus::Modified);
}

TEST(ZkvmRevert, TransactionBoundaryResetsScope)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    st.begin_transaction(0);
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Cold);
    EXPECT_EQ(st.access_storage<Shanghai>(ADDR, word(5)), AccessStatus::Cold);
    st.touch(ADDR);
    st.set_transient_storage(ADDR, word(1), word(2));
    st.set_nonce(ADDR, 4);
    EXPECT_TRUE(st.is_touched(ADDR));
    st.begin_transaction(1);
    // Warmth, touched and transient storage are per transaction; the nonce
    // is not.
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Cold);
    EXPECT_EQ(st.access_storage<Shanghai>(ADDR, word(5)), AccessStatus::Cold);
    EXPECT_FALSE(st.is_touched(ADDR));
    EXPECT_EQ(st.get_transient_storage(ADDR, word(1)), bytes32_t{});
    EXPECT_EQ(st.get_nonce(ADDR), 4);
    EXPECT_TRUE(st.is_current_incarnation(NOBODY) == false);
}

TEST(ZkvmRevert, LogsFollowFrames)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    st.push();
    st.store_log(log_from(ADDR));
    st.push();
    st.store_log(log_from(OTHER));
    st.pop_reject();
    EXPECT_EQ(st.logs().size(), 1u);
    st.push();
    st.store_log(log_from(OTHER));
    st.pop_accept();
    st.pop_accept();
    ASSERT_EQ(st.logs().size(), 2u);
    EXPECT_EQ(st.logs()[0].address, ADDR);
    EXPECT_EQ(st.logs()[1].address, OTHER);
    auto const taken = st.take_logs();
    EXPECT_EQ(taken.size(), 2u);
    EXPECT_TRUE(st.logs().empty());
    st.begin_transaction(0);
}

// ---------------------------------------------------------------------------
// Guest-specific: the end-of-transaction walks
// ---------------------------------------------------------------------------

TEST(ZkvmRevert, DestructSuicidesDeleteAll)
{
    Fixture f{two_account_blob(
        ADDR, Account{.balance = 500, .nonce = 1}, OTHER, Account{.nonce = 0})};
    auto &st = f.state;
    st.begin_transaction(0);
    st.push();
    st.selfdestruct<EvmTraits<EvmRevision::London>>(ADDR, OTHER);
    st.pop_accept();
    // Pre-Cancun: a witness account is destroyed too.
    st.destruct_suicides<EvmTraits<EvmRevision::London>>();
    EXPECT_FALSE(st.account_exists(ADDR));
    EXPECT_EQ(st.get_balance(OTHER), 500);
}

TEST(ZkvmRevert, DestructSuicidesOnlyCreatedInTransaction)
{
    Fixture f{two_account_blob(
        ADDR, Account{.balance = 500, .nonce = 1}, OTHER, Account{.nonce = 0})};
    auto &st = f.state;
    st.begin_transaction(0);
    st.push();
    // The witness account survives under EIP-6780; the balance still moves.
    st.selfdestruct<Cancun>(ADDR, OTHER);
    // An account created in this transaction does not.
    st.create_contract(NOBODY);
    st.add_to_balance(NOBODY, 7);
    st.set_storage(NOBODY, word(1), word(1));
    st.selfdestruct<Cancun>(NOBODY, OTHER);
    st.pop_accept();
    st.destruct_suicides<Cancun>();
    EXPECT_TRUE(st.account_exists(ADDR));
    EXPECT_EQ(st.get_balance(ADDR), 0);
    EXPECT_FALSE(st.account_exists(NOBODY));
    EXPECT_EQ(st.get_balance(OTHER), 507);
}

TEST(ZkvmRevert, DestructTouchedDeadRemovesEmptyAccounts)
{
    Fixture f{two_account_blob(
        ADDR, Account{.balance = 1}, OTHER, Account{.nonce = 0})};
    auto &st = f.state;
    st.begin_transaction(0);
    // OTHER is empty and gets touched by a zero transfer; ADDR has balance.
    st.add_to_balance(OTHER, 0);
    st.add_to_balance(ADDR, 0);
    // NOBODY is touched without ever being materialised.
    st.touch(NOBODY);
    st.destruct_touched_dead();
    EXPECT_FALSE(st.account_exists(OTHER));
    EXPECT_TRUE(st.account_exists(ADDR));
    EXPECT_FALSE(st.account_exists(NOBODY));
}

// Re-creation hides the slots of the earlier incarnation: the CREATE2,
// SSTORE, SELFDESTRUCT, CREATE2, SSTORE sequence across two transactions.
TEST(ZkvmRevert, RecreatedAccountStartsWithEmptyStorage)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    st.begin_transaction(0);
    st.push();
    st.create_contract(NOBODY);
    EXPECT_EQ(st.set_storage(NOBODY, word(5), word(7)), StorageStatus::Added);
    st.selfdestruct<Cancun>(NOBODY, ADDR);
    st.pop_accept();
    st.destruct_suicides<Cancun>();
    EXPECT_FALSE(st.account_exists(NOBODY));

    st.begin_transaction(1);
    st.push();
    st.create_contract(NOBODY);
    EXPECT_EQ(st.get_storage(NOBODY, word(5)), bytes32_t{});
    // A fresh store, charged as such: the original is zero, not 7.
    EXPECT_EQ(st.set_storage(NOBODY, word(5), word(9)), StorageStatus::Added);
    EXPECT_EQ(st.get_storage(NOBODY, word(5)), word(9));
    st.pop_reject();
    // The rejected re-creation leaves the account absent again.
    EXPECT_FALSE(st.account_exists(NOBODY));
}

// A re-creation inside a frame that is rejected brings the old incarnation's
// slots back, with their transaction-start values.
TEST(ZkvmRevert, RejectedRecreationRestoresSlots)
{
    Fixture f{account_blob(ADDR, Account{.nonce = 0})};
    auto &st = f.state;
    st.begin_transaction(0);
    // A witness account with nonce 0 and no code may be CREATEd over
    // (EIP-684 allows it); it keeps its balance and loses its storage.
    st.set_storage(ADDR, word(5), word(7));
    st.push();
    st.create_contract(ADDR);
    EXPECT_EQ(st.get_storage(ADDR, word(5)), bytes32_t{});
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(9)), StorageStatus::Added);
    st.pop_reject();
    EXPECT_EQ(st.get_storage(ADDR, word(5)), word(7));
    EXPECT_FALSE(st.is_current_incarnation(ADDR));
    // The original is still the witness value (zero), so this is a plain
    // assignment of an added slot.
    EXPECT_EQ(st.set_storage(ADDR, word(5), word(8)), StorageStatus::Assigned);
}

// ---------------------------------------------------------------------------
// Misuse aborts
// ---------------------------------------------------------------------------

TEST(ZkvmRevertDeathTest, FrameMisuseAborts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Fixture f{account_blob(ADDR, Account{.nonce = 1})};
    auto &st = f.state;
    EXPECT_DEATH(st.pop_accept(), "pop without push");
    EXPECT_DEATH(st.pop_reject(), "pop without push");
    st.push();
    EXPECT_DEATH(st.begin_transaction(0), "frame open");
    EXPECT_DEATH(st.destruct_touched_dead(), "inside a frame");
    st.pop_accept();
    st.begin_transaction(0);
    EXPECT_DEATH(st.begin_transaction(0), "out of order");
}

TEST(ZkvmRevertDeathTest, WritesToUnprovenAccountsAbort)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Fixture f{branch_blob(OTHER, Account{.nonce = 3}, ADDR)};
    auto &st = f.state;
    // Warmth and touch need no account; anything that reads it aborts.
    EXPECT_EQ(st.access_account(ADDR), AccessStatus::Cold);
    st.touch(ADDR);
    EXPECT_DEATH(st.set_nonce(ADDR, 1), "not in the witness");
    EXPECT_DEATH(st.add_to_balance(ADDR, 1), "not in the witness");
    EXPECT_DEATH(st.create_contract(ADDR), "not in the witness");
    EXPECT_DEATH(st.destruct_touched_dead(), "not in the witness");
}
