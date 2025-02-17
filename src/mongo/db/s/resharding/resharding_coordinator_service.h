/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/future.h"

namespace mongo {

namespace resharding {

struct ParticipantShardsAndChunks {
    std::vector<DonorShardEntry> donorShards;
    std::vector<RecipientShardEntry> recipientShards;
    std::vector<ChunkType> initialChunks;
};

CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation);

void insertCoordDocAndChangeOrigCollEntry(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc);

ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc);

void writeParticipantShardsAndTempCollInfo(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           std::vector<ChunkType> initialChunks,
                                           std::vector<BSONObj> zones);

void writeDecisionPersistedState(OperationContext* opCtx,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 boost::optional<Timestamp> newCollectionTimestamp);

void writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc);

void removeCoordinatorDocAndReshardingFields(OperationContext* opCtx,
                                             const ReshardingCoordinatorDocument& coordinatorDoc);
}  // namespace resharding

class ServiceContext;
class OperationContext;

/**
 * Construct to encapsulate cancellation tokens and related semantics on the ReshardingCoordinator.
 */
class CoordinatorCancellationTokenHolder {
public:
    CoordinatorCancellationTokenHolder(CancellationToken stepdownToken)
        : _stepdownToken(stepdownToken),
          _abortSource(CancellationSource(stepdownToken)),
          _abortToken(_abortSource.token()) {}

    /**
     * Returns whether the any token has been canceled.
     */
    bool isCanceled() {
        return _stepdownToken.isCanceled() || _abortToken.isCanceled();
    }

    /**
     * Returns whether the abort token has been canceled, indicating that the resharding operation
     * was explicitly aborted by an external user.
     */
    bool isAborted() {
        return !_stepdownToken.isCanceled() && _abortToken.isCanceled();
    }

    /**
     * Returns whether the stepdownToken has been canceled, indicating that the shard's underlying
     * replica set node is stepping down or shutting down.
     */
    bool isSteppingOrShuttingDown() {
        return _stepdownToken.isCanceled();
    }

    /**
     * Cancels the source created by this class, in order to indicate to holders of the abortToken
     * that the resharding operation has been aborted.
     */
    void abort() {
        _abortSource.cancel();
    }

    const CancellationToken& getStepdownToken() {
        return _stepdownToken;
    }

    const CancellationToken& getAbortToken() {
        return _abortToken;
    }

private:
    // The token passed in by the PrimaryOnlyService runner that is canceled when this shard's
    // underlying replica set node is stepping down or shutting down.
    CancellationToken _stepdownToken;

    // The source created by inheriting from the stepdown token.
    CancellationSource _abortSource;

    // The token to wait on in cases where a user wants to wait on either a resharding operation
    // being aborted or the replica set node stepping/shutting down.
    CancellationToken _abortToken;
};

class ReshardingCoordinatorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingCoordinatorService"_sd;

    explicit ReshardingCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingCoordinatorService() = default;

    class ReshardingCoordinator;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingCoordinatorService thread pool.
        return ThreadPool::Limits();
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override;

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;
};

class ReshardingCoordinatorService::ReshardingCoordinator final
    : public PrimaryOnlyService::TypedInstance<ReshardingCoordinator> {
public:
    explicit ReshardingCoordinator(const ReshardingCoordinatorService* coordinatorService,
                                   const BSONObj& state);
    ~ReshardingCoordinator();

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override {}

    /**
     * Attempts to cancel the underlying resharding operation using the abort token.
     */
    void abort();

    /**
     * Replace in-memory representation of the CoordinatorDoc
     */
    void installCoordinatorDoc(OperationContext* opCtx, const ReshardingCoordinatorDocument& doc);

    /**
     * Returns a Future that will be resolved when all work associated with this Instance has
     * completed running.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override;

    std::shared_ptr<ReshardingCoordinatorObserver> getObserver();

private:
    struct ChunksAndZones {
        std::vector<ChunkType> initialChunks;
        std::vector<TagsType> newZones;
    };

    /**
     * Runs resharding up through preparing to persist the decision.
     */
    ExecutorFuture<ReshardingCoordinatorDocument> _runUntilReadyToPersistDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) noexcept;

    /**
     * Runs resharding through persisting the decision until cleanup.
     */
    ExecutorFuture<void> _persistDecisionAndFinishReshardOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const ReshardingCoordinatorDocument& updatedCoordinatorDoc) noexcept;

    /**
     * Runs cleanup logic that only applies to abort.
     */
    void _onAbort(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                  const Status& status);

    /**
     * Does the following writes:
     * 1. Inserts the coordinator document into config.reshardingOperations
     * 2. Adds reshardingFields to the config.collections entry for the original collection
     *
     * Transitions to 'kInitializing'.
     */
    void _insertCoordDocAndChangeOrigCollEntry();

    /**
     * Calculates the participant shards and target chunks under the new shard key, then does the
     * following writes:
     * 1. Updates the coordinator state to 'kPreparingToDonate'.
     * 2. Updates reshardingFields to reflect the state change on the original collection entry.
     * 3. Inserts an entry into config.collections for the temporary collection
     * 4. Inserts entries into config.chunks for ranges based on the new shard key
     * 5. Upserts entries into config.tags for any zones associated with the new shard key
     *
     * Transitions to 'kPreparingToDonate'.
     */
    void _calculateParticipantsAndChunksThenWriteToDisk();

    /**
     * Waits on _reshardingCoordinatorObserver to notify that all donors have picked a
     * minFetchTimestamp and are ready to donate. Transitions to 'kCloning'.
     */
    ExecutorFuture<void> _awaitAllDonorsReadyToDonate(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Waits on _reshardingCoordinatorObserver to notify that all recipients have finished
     * cloning. Transitions to 'kApplying'.
     */
    ExecutorFuture<void> _awaitAllRecipientsFinishedCloning(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Waits on _reshardingCoordinatorObserver to notify that all recipients have finished
     * applying oplog entries. Transitions to 'kBlockingWrites'.
     */
    ExecutorFuture<void> _awaitAllRecipientsFinishedApplying(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Waits on _reshardingCoordinatorObserver to notify that all recipients have entered
     * strict-consistency.
     */
    ExecutorFuture<ReshardingCoordinatorDocument> _awaitAllRecipientsInStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Does the following writes:
     * 1. Updates the config.collections entry for the new sharded collection
     * 2. Updates config.chunks entries for the new sharded collection
     * 3. Updates config.tags for the new sharded collection
     *
     * Transitions to 'kDecisionPersisted'.
     */
    Future<void> _persistDecision(const ReshardingCoordinatorDocument& updatedDoc);

    /**
     * Waits on _reshardingCoordinatorObserver to notify that:
     * 1. All recipient shards have renamed the temporary collection to the original collection
     *    namespace, and
     * 2. All donor shards that were not also recipient shards have dropped the original
     *    collection.
     *
     * Transitions to 'kDone'.
     */
    ExecutorFuture<void> _awaitAllParticipantShardsRenamedOrDroppedOriginalCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Updates the entry for this resharding operation in config.reshardingOperations and the
     * catalog entries for the original and temporary namespaces in config.collections.
     */
    void _updateCoordinatorDocStateAndCatalogEntries(
        CoordinatorStateEnum nextState,
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<Timestamp> fetchTimestamp = boost::none,
        boost::optional<ReshardingApproxCopySize> approxCopySize = boost::none,
        boost::optional<Status> abortReason = boost::none);

    /**
     * Sends '_flushReshardingStateChange' to all recipient shards.
     *
     * When the coordinator is in a state before 'kDecisionPersisted', refreshes the temporary
     * namespace. When the coordinator is in a state at or after 'kDecisionPersisted', refreshes the
     * original namespace.
     */
    void _tellAllRecipientsToRefresh(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Sends '_flushReshardingStateChange' for the original namespace to all donor shards.
     */
    void _tellAllDonorsToRefresh(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Sends '_flushReshardingStateChange' for the original namespace to all participant shards.
     */
    void _tellAllParticipantsToRefresh(
        const NamespaceString& nss, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // The unique key for a given resharding operation. InstanceID is an alias for BSONObj. The
    // value of this is the UUID that will be used as the collection UUID for the new sharded
    // collection. The object looks like: {_id: 'reshardingUUID'}
    const InstanceID _id;

    // The primary-only service instance corresponding to the coordinator instance. Not owned.
    const ReshardingCoordinatorService* const _coordinatorService;

    // Observes writes that indicate state changes for this resharding operation and notifies
    // 'this' when all donors/recipients have entered some state so that 'this' can transition
    // states.
    std::shared_ptr<ReshardingCoordinatorObserver> _reshardingCoordinatorObserver;

    // The updated coordinator state document.
    ReshardingCoordinatorDocument _coordinatorDoc;

    // Holds the cancellation tokens relevant to the ReshardingCoordinator.
    std::unique_ptr<CoordinatorCancellationTokenHolder> _ctHolder;

    // ThreadPool used by CancelableOperationContext.
    // CancelableOperationContext must have a thread that is always available to it to mark its
    // opCtx as killed when the cancelToken has been cancelled.
    const std::shared_ptr<ThreadPool> _markKilledExecutor;
    boost::optional<CancelableOperationContextFactory> _factory;

    // Protects promises below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReshardingCoordinatorService::_mutex");

    // Promise that is resolved when the chain of work kicked off by run() has completed.
    SharedPromise<void> _completionPromise;

    // Callback handle for scheduled work to handle critical section timeout.
    boost::optional<executor::TaskExecutor::CallbackHandle> _criticalSectionTimeoutCbHandle;
};

}  // namespace mongo
