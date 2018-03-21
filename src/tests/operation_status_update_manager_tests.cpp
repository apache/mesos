// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>

#include <gtest/gtest.h>

#include <mesos/v1/mesos.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>

#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include <stout/os/ftruncate.hpp>

#include "common/protobuf_utils.hpp"

#include "slave/constants.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#include "status_update_manager/operation.hpp"

using lambda::function;

using process::Clock;
using process::Future;
using process::Owned;
using process::Promise;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

// This class will be the target of the forward callbacks passed to the
// operation status update managers in this test suite.
//
// It uses gmock, so that we can easily set expectations and check how
// often/which status updates are forwarded.
class MockUpdateOperationStatusMessageProcessor
{
public:
  MOCK_METHOD1(update, void(const UpdateOperationStatusMessage&));
};


class OperationStatusUpdateManagerTest : public MesosTest
{
protected:
  OperationStatusUpdateManagerTest()
    : statusUpdateManager(new OperationStatusUpdateManager())
  {
    Clock::pause();

    const function<void(const UpdateOperationStatusMessage&)> forward =
      [&](const UpdateOperationStatusMessage& update) {
        statusUpdateProcessor.update(update);
      };

    statusUpdateManager->initialize(
        forward, OperationStatusUpdateManagerTest::getPath);
  }

  void TearDown() override
  {
    Clock::resume();
    statusUpdateManager.reset();

    MesosTest::TearDown();
  }

  UpdateOperationStatusMessage createUpdateOperationStatusMessage(
      const id::UUID& uuid,
      const id::UUID& operationUuid,
      const OperationState& state,
      const Option<FrameworkID>& frameworkId = None())
  {
    UpdateOperationStatusMessage statusUpdate;

    statusUpdate.mutable_operation_uuid()->CopyFrom(
        protobuf::createUUID(operationUuid));

    if (frameworkId.isSome()) {
      statusUpdate.mutable_framework_id()->CopyFrom(frameworkId.get());
    }

    OperationStatus* status = statusUpdate.mutable_status();
    status->set_state(state);
    status->mutable_uuid()->CopyFrom(protobuf::createUUID(uuid));

    return statusUpdate;
  }

  void resetStatusUpdateManager()
  {
    statusUpdateManager.reset(new OperationStatusUpdateManager());

    const function<void(const UpdateOperationStatusMessage&)> forward =
      [&](const UpdateOperationStatusMessage& update) {
        statusUpdateProcessor.update(update);
      };

    statusUpdateManager->initialize(
        forward, OperationStatusUpdateManagerTest::getPath);
  }

  static const string getPath(const id::UUID& operationUuid)
  {
    return path::join(os::getcwd(), "streams", operationUuid.toString());
  }

  Owned<OperationStatusUpdateManager> statusUpdateManager;
  MockUpdateOperationStatusMessageProcessor statusUpdateProcessor;
};


// This test verifies that the status update manager will not retry a terminal
// status update after it has been acknowledged.
TEST_F(OperationStatusUpdateManagerTest, UpdateAndAck)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a terminal update, so `acknowledgement`
  // should return `false`.
  AWAIT_EXPECT_FALSE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Advance the clock, the status update has been acknowledged, so it shouldn't
  // trigger a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that the status update manager will not retry a
// non-terminal status update after it has been acknowledged.
TEST_F(OperationStatusUpdateManagerTest, UpdateAndAckNonTerminalUpdate)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a non-terminal update, so `acknowledgement`
  // should return `true`.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Advance the clock, the status update has been acknowledged, so it shouldn't
  // trigger a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that the status update manager resends status updates
// until they are acknowledged.
TEST_F(OperationStatusUpdateManagerTest, ResendUnacknowledged)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate1;
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate2;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate1))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate2));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate1);

  EXPECT_FALSE(forwardedStatusUpdate2.isReady());

  // Advance the clock to trigger a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Verify that the status update is forwarded again.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate2);

  // Acknowledge the update, this is a terminal update, so `acknowledgement`
  // should return `false`.
  AWAIT_EXPECT_FALSE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Advance the clock, the status update has been acknowledged, so it shouldn't
  // trigger a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that after the updates belonging to a framework are
// cleaned up from the status update manager, the status update manager stops
// resending them.
TEST_F(OperationStatusUpdateManagerTest, Cleanup)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  FrameworkID frameworkId;
  frameworkId.set_value("frameworkId");

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid,
        operationUuid,
        OperationState::OPERATION_FINISHED,
        frameworkId);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Cleanup the framework.
  statusUpdateManager->cleanup(frameworkId);

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that the status update manager is able to recover
// checkpointed status updates, and that it resends the recovered updates that
// haven't been acknowledged.
TEST_F(OperationStatusUpdateManagerTest, RecoverCheckpointedStream)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate1;
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate2;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate1))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate2));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate1);

  resetStatusUpdateManager();

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  EXPECT_FALSE(forwardedStatusUpdate2.isReady());

  // Recover the checkpointed stream.
  Future<OperationStatusUpdateManagerState> state =
    statusUpdateManager->recover({operationUuid}, true);

  AWAIT_READY(state);

  EXPECT_EQ(0u, state->errors);
  EXPECT_TRUE(state->streams.contains(operationUuid));
  EXPECT_SOME(state->streams.at(operationUuid));

  const Option<UpdateOperationStatusMessage> recoveredUpdate =
    state->streams.at(operationUuid)->updates.front();

  ASSERT_SOME(recoveredUpdate);
  EXPECT_EQ(statusUpdate, recoveredUpdate.get());

  // The stream should NOT be terminated.
  EXPECT_FALSE(state->streams.at(operationUuid)->terminated);

  // Check that the status update is resent.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate2);
}


// This test verifies that the status update manager returns a `Failure` when
// trying to recover a stream that isn't checkpointed.
TEST_F(OperationStatusUpdateManagerTest, RecoverNotCheckpointedStream)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a non-checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, false));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Verify that the stream file is NOT created.
  EXPECT_TRUE(!os::exists(getPath(operationUuid)));

  resetStatusUpdateManager();

  // Trying to recover the non-checkpointed stream should fail.
  AWAIT_EXPECT_FAILED(statusUpdateManager->recover({operationUuid}, true));
}


// This test verifies that the status update manager  doesn't return a
// `Failure` when trying to recover a stream from an empty file.
//
// This can happen when the checkpointing failed between opening the file and
// writing the first update. In this case `recover()` should succeed, but return
// `None` as the operation's state.
TEST_F(OperationStatusUpdateManagerTest, RecoverEmptyFile)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  resetStatusUpdateManager();

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Truncate the status updates to zero bytes, simulating a failure in
  // checkpointing, where the file was created, but the status update manager
  // crashed before any data was written to disk.
  Try<int_fd> fd = os::open(getPath(operationUuid), O_RDWR);
  ASSERT_SOME(os::ftruncate(fd.get(), 0));
  os::close(fd.get());

  // The recovery of the empty stream should not fail, but the stream state
  // should be empty.
  Future<OperationStatusUpdateManagerState> state =
    statusUpdateManager->recover({operationUuid}, false);

  AWAIT_READY(state);

  EXPECT_EQ(0u, state->errors);
  EXPECT_TRUE(state->streams.contains(operationUuid));
  EXPECT_NONE(state->streams.at(operationUuid));
}


// This test verifies that the status update manager doesn't return a `Failure`
// when trying to recover a stream from a parent directory that doesn't contain
// the status updates file.
//
// This can happen when the checkpointing failed after creating the parent
// directory, but before the file was opened. In this case `recover()` should
// succeed, but return `None` as the operation's state.
TEST_F(OperationStatusUpdateManagerTest, RecoverEmptyDirectory)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  resetStatusUpdateManager();

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Leave the parent directory, but remove the status updates file, simulating
  // a failure in checkpointing, where the parent directory was created, but the
  // checkpointing process crashed before opening the status updates file.
  ASSERT_SOME(os::rm(getPath(operationUuid)));

  // The recovery of the empty stream should not fail, but the stream state
  // should be empty.
  Future<OperationStatusUpdateManagerState> state =
    statusUpdateManager->recover({operationUuid}, false);

  AWAIT_READY(state);

  EXPECT_EQ(0u, state->errors);
  EXPECT_TRUE(state->streams.contains(operationUuid));
  EXPECT_NONE(state->streams.at(operationUuid));
}


// This test verifies that the status update manager is able to recover a
// terminated stream, and that the stream's state reflects that it is
// terminated.
TEST_F(OperationStatusUpdateManagerTest, RecoverTerminatedStream)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a terminal update, so `acknowledgement`
  // should return `false`.
  AWAIT_EXPECT_FALSE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  resetStatusUpdateManager();

  // Recover the checkpointed stream.
  Future<OperationStatusUpdateManagerState> state =
    statusUpdateManager->recover({operationUuid}, true);

  AWAIT_ASSERT_READY(state);

  EXPECT_EQ(0u, state->errors);
  EXPECT_TRUE(state->streams.contains(operationUuid));
  EXPECT_SOME(state->streams.at(operationUuid));

  // The stream should be terminated.
  EXPECT_TRUE(state->streams.at(operationUuid)->terminated);

  const Option<UpdateOperationStatusMessage> recoveredUpdate =
    state->streams.at(operationUuid)->updates.front();

  ASSERT_SOME(recoveredUpdate);
  EXPECT_EQ(statusUpdate, recoveredUpdate.get());

  // Advance the clock, the status update has been acknowledged, so it shouldn't
  // trigger a retry.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that the status update manager silently ignores duplicate
// updates.
TEST_F(OperationStatusUpdateManagerTest, IgnoreDuplicateUpdate)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a non-terminal update, so `acknowledgement`
  // should return `true`.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Check that a duplicated update is ignored.
  AWAIT_EXPECT_READY(statusUpdateManager->update(statusUpdate, true));

  // Advance the clock enough to trigger a retry if the update hasn't been
  // acknowledged.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that after recovery the status update manager still
// silently ignores duplicate updates.
TEST_F(OperationStatusUpdateManagerTest, IgnoreDuplicateUpdateAfterRecover)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a non-terminal update, so `acknowledgement`
  // should return `true`.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  resetStatusUpdateManager();

  // Recover the checkpointed stream.
  AWAIT_ASSERT_READY(statusUpdateManager->recover({operationUuid}, true));

  // Check that a duplicated update is ignored.
  AWAIT_EXPECT_READY(statusUpdateManager->update(statusUpdate, true));

  // Advance the clock enough to trigger a retry if the update hasn't been
  // acknowledged.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that the status update manager rejects duplicated
// acknowledgements.
TEST_F(OperationStatusUpdateManagerTest, RejectDuplicateAck)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a non-terminal update, so `acknowledgement`
  // should return `true`.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Advance the clock enough to trigger a retry if the update hasn't been
  // ignored.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Try to acknowledge the same update, the operation should fail.
  AWAIT_EXPECT_FAILED(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));
}


// This test verifies that after recovery the status update manager still
// rejects duplicated acknowledgements.
TEST_F(OperationStatusUpdateManagerTest, RejectDuplicateAckAfterRecover)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  // Acknowledge the update, this is a non-terminal update, so `acknowledgement`
  // should return `true`.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  resetStatusUpdateManager();

  // Recover the checkpointed stream.
  AWAIT_ASSERT_READY(statusUpdateManager->recover({operationUuid}, true));

  // Try to acknowledge the same update, the operation should fail.
  AWAIT_EXPECT_FAILED(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid));

  // Advance the clock enough to trigger a retry if the update hasn't been
  // ignored.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that non-strict recovery of a status updates file
// containing a corrupted record at the end succeeds.
TEST_F(OperationStatusUpdateManagerTest, NonStrictRecoveryCorruptedFile)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate1;
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate2;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate1))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate2));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate1);

  resetStatusUpdateManager();

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // The file should contain only `UpdateOperationStatusRecord` messages.
  // Write a `UpdateOperationStatusMessage` to the end of the file, so that the
  // recovery process encounters an error.
  Try<int_fd> fd = os::open(getPath(operationUuid), O_APPEND | O_RDWR);
  ASSERT_SOME(fd);
  ASSERT_SOME(::protobuf::write(fd.get(), statusUpdate));
  ASSERT_SOME(os::close(fd.get()));

  EXPECT_FALSE(forwardedStatusUpdate2.isReady());

  // The non-strict recovery of the corrupted stream should not fail, but the
  // state should reflect that the status update file contained a corrupted
  // record.
  Future<OperationStatusUpdateManagerState> state =
    statusUpdateManager->recover({operationUuid}, false);

  AWAIT_READY(state);

  EXPECT_EQ(1u, state->errors);
  EXPECT_TRUE(state->streams.contains(operationUuid));
  EXPECT_SOME(state->streams.at(operationUuid));

  // Check that the status update could be recovered.
  const Option<UpdateOperationStatusMessage> recoveredUpdate =
    state->streams.at(operationUuid)->updates.front();

  ASSERT_SOME(recoveredUpdate);
  EXPECT_EQ(statusUpdate, recoveredUpdate.get());

  // Check that the status update is resent.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate2);
}


// This test verifies that strict recovery of a status updates file containing
// a corrupted record at the end fails.
TEST_F(OperationStatusUpdateManagerTest, StrictRecoveryCorruptedFile)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate));

  const id::UUID operationUuid = id::UUID::random();
  const id::UUID statusUuid = id::UUID::random();

  UpdateOperationStatusMessage statusUpdate =
    createUpdateOperationStatusMessage(
        statusUuid, operationUuid, OperationState::OPERATION_FINISHED);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate, true));

  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(statusUpdate.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate);

  resetStatusUpdateManager();

  // Advance the clock enough to trigger a retry if the update hasn't been
  // cleaned up.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // The file should contain only `UpdateOperationStatusMessageRecord` messages.
  // Write a `UpdateOperationStatusMessage` to the end of the file, so that the
  // recovery process encounters an error.
  Try<int_fd> fd = os::open(getPath(operationUuid), O_APPEND | O_RDWR);
  ASSERT_SOME(fd);

  ASSERT_SOME(::protobuf::write(fd.get(), statusUpdate));
  ASSERT_SOME(os::close(fd.get()));

  // The strict recovery of the corrupted stream should fail.
  AWAIT_ASSERT_FAILED(statusUpdateManager->recover({operationUuid}, true));
}


// This test verifies that the status update manager correctly fills in the
// latest status when (re)sending status updates.
TEST_F(OperationStatusUpdateManagerTest, UpdateLatestWhenResending)
{
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate1;
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate2;
  Future<UpdateOperationStatusMessage> forwardedStatusUpdate3;
  EXPECT_CALL(statusUpdateProcessor, update(_))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate1))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate2))
    .WillOnce(FutureArg<0>(&forwardedStatusUpdate3));

  const id::UUID operationUuid = id::UUID::random();

  const id::UUID statusUuid1 = id::UUID::random();
  UpdateOperationStatusMessage statusUpdate1 =
    createUpdateOperationStatusMessage(
        statusUuid1, operationUuid, OperationState::OPERATION_PENDING);

  // Send a checkpointed operation status update.
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate1, true));

  // The status update manager should fill in the `latest_status` field with the
  // status update we just sent.
  UpdateOperationStatusMessage expectedStatusUpdate(statusUpdate1);
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(
      statusUpdate1.status());

  // Verify that the status update is forwarded.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate1);

  EXPECT_FALSE(forwardedStatusUpdate2.isReady());

  // Send another status update.
  const id::UUID statusUuid2 = id::UUID::random();
  UpdateOperationStatusMessage statusUpdate2 =
    createUpdateOperationStatusMessage(
        statusUuid2, operationUuid, OperationState::OPERATION_PENDING);
  AWAIT_ASSERT_READY(statusUpdateManager->update(statusUpdate2, true));

  // Advance the clock to trigger a retry of the first update.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Now that another status update was sent, the status update manager should
  // fill in the `latest_status` field with this new status update.
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(
      statusUpdate2.status());

  // Verify that the status update is forwarded again.
  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate2);

  EXPECT_FALSE(forwardedStatusUpdate3.isReady());

  // Acknowledge the first update, it is NOT a terminal update, so
  // `acknowledgement` should return `true`. The status update manager
  // should now send the second status update.
  AWAIT_EXPECT_TRUE(
      statusUpdateManager->acknowledgement(operationUuid, statusUuid1));

  // The status update manager should then forward the latest status update.
  expectedStatusUpdate = statusUpdate2;
  expectedStatusUpdate.mutable_latest_status()->CopyFrom(
    statusUpdate2.status());

  AWAIT_EXPECT_EQ(expectedStatusUpdate, forwardedStatusUpdate3);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
