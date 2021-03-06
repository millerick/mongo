/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TransactionRouterTest : public ShardingTestFixture {
protected:
    void setUp() {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    }
};

TEST_F(TransactionRouterTest, BasicStartTxn) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTest, CannotContiueTxnWithoutStarting) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(operationContext(), txnNum, false),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, NewParticipantMustAttachTxnAndReadConcern) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    ShardId shard1("a");
    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }

    ShardId shard2("b");

    expectedNewObj = BSON("insert"
                          << "test"
                          << "startTransaction"
                          << true
                          << "readConcern"
                          << BSON("level"
                                  << "snapshot")
                          << "autocommit"
                          << false
                          << "txnNumber"
                          << txnNum);

    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTest, StartingNewTxnShouldClearState) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        participant.markAsCommandSent();
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }

    TxnNumber txnNum2{5};
    sessionState.beginOrContinueTxn(operationContext(), txnNum2, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum2);

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }
}

TEST_F(TransactionRouterTest, FirstParticipantIsCoordinator) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    ASSERT_FALSE(sessionState.getCoordinatorId());

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        ASSERT(participant.isCoordinator());
        ASSERT(sessionState.getCoordinatorId());
        ASSERT_EQ(*sessionState.getCoordinatorId(), shard1);
    }

    ShardId shard2("b");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        ASSERT_FALSE(participant.isCoordinator());
        ASSERT(sessionState.getCoordinatorId());
        ASSERT_EQ(*sessionState.getCoordinatorId(), shard1);
    }

    TxnNumber txnNum2{5};
    sessionState.beginOrContinueTxn(operationContext(), txnNum2, true);

    ASSERT_FALSE(sessionState.getCoordinatorId());

    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        ASSERT(participant.isCoordinator());
        ASSERT(sessionState.getCoordinatorId());
        ASSERT_EQ(*sessionState.getCoordinatorId(), shard2);
    }
}

TEST_F(TransactionRouterTest, DoesNotAttachTxnNumIfAlreadyThere) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "txnNumber"
                                  << txnNum
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false);

    ShardId shard1("a");
    auto& participant = sessionState.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"
                                                           << "txnNumber"
                                                           << txnNum));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

DEATH_TEST_F(TransactionRouterTest, CrashesIfCmdHasDifferentTxnNumber, "invariant") {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    ShardId shard1("a");
    auto& participant = sessionState.getOrCreateParticipant(shard1);
    participant.attachTxnFieldsIfNeeded(BSON("insert"
                                             << "test"
                                             << "txnNumber"
                                             << TxnNumber(10)));
}

TEST_F(TransactionRouterTest, AttachTxnValidatesReadConcernIfAlreadyOnCmd) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true);

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"
                                                               << "readConcern"
                                                               << BSON("level"
                                                                       << "snapshot")));
        ASSERT_BSONOBJ_EQ(BSON("insert"
                               << "test"
                               << "readConcern"
                               << BSON("level"
                                       << "snapshot")
                               << "startTransaction"
                               << true
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTest, CannotSpecifyReadConcernAfterFirstStatement) {
    TxnNumber txnNum{3};

    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    ASSERT_THROWS_CODE(
        sessionState.beginOrContinueTxn(operationContext(), txnNum, false /* startTransaction */),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(TransactionRouterTest, UpconvertToSnapshotIfNoReadConcernLevelGiven) {
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

    TxnNumber txnNum{3};
    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    ShardId shard1("a");
    auto& participant = sessionState.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTest, UpconvertToSnapshotIfNoReadConcernLevelButHasAfterClusterTime) {
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(LogicalTime(Timestamp(10, 1)), boost::none);

    TxnNumber txnNum{3};
    TransactionRouter sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          // TODO SERVER-36237: afterClusterTime should be replaced
                                          // by an atClusterTime at least as large.
                                          << "afterClusterTime"
                                          << Timestamp(10, 1))
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    ShardId shard1("a");
    auto& participant = sessionState.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTest, CannotUpconvertIfLevelOtherThanSnapshotWasGiven) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter sessionState({});
        sessionState.checkOut();
        ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(
                               operationContext(), txnNum, true /* startTransaction */),
                           DBException,
                           ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTest, CannotUpconvertIfLevelOtherThanSnapshotWasGivenWithAfterClusterTime) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(LogicalTime(Timestamp(10, 1)), readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter sessionState({});
        sessionState.checkOut();
        ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(
                               operationContext(), txnNum, true /* startTransaction */),
                           DBException,
                           ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTest, CannotUpconvertWithAfterOpTime) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::OpTime(Timestamp(10, 1), 2), readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter sessionState({});
        sessionState.checkOut();
        ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(
                               operationContext(), txnNum, true /* startTransaction */),
                           DBException,
                           ErrorCodes::InvalidOptions);
    }

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::OpTime(Timestamp(10, 1), 2), boost::none);

    {

        TxnNumber txnNum{3};
        TransactionRouter sessionState({});
        sessionState.checkOut();
        ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(
                               operationContext(), txnNum, true /* startTransaction */),
                           DBException,
                           ErrorCodes::InvalidOptions);
    }
}

}  // unnamed namespace
}  // namespace mongo
