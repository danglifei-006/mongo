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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/repl/tenant_migration_donor_service.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_statistics.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterPersistingInitialDonorStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingDataSyncState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeFetchingKeys);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeEnteringFutureChain);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterFetchingAndStoringKeys);

const std::string kTTLIndexName = "TenantMigrationDonorTTLIndex";
const std::string kExternalKeysTTLIndexName = "ExternalKeysTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

const int kMaxRecipientKeyDocsFindAttempts = 10;

bool shouldStopCreatingTTLIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopInsertingDonorStateDoc(Status status, const CancellationToken& token) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress ||
        token.isCanceled();
}

bool shouldStopUpdatingDonorStateDoc(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopSendingRecipientCommand(Status status, const CancellationToken& token) {
    return status.isOK() ||
        !(ErrorCodes::isRetriableError(status) ||
          status == ErrorCodes::FailedToSatisfyReadPreference) ||
        token.isCanceled();
}

bool shouldStopFetchingRecipientClusterTimeKeyDocs(Status status, const CancellationToken& token) {
    // TODO (SERVER-54926): Convert HostUnreachable error in
    // _fetchAndStoreRecipientClusterTimeKeyDocs to specific error.
    return status.isOK() || !ErrorCodes::isRetriableError(status) ||
        status.code() == ErrorCodes::HostUnreachable || token.isCanceled();
}

void checkIfReceivedDonorAbortMigration(const CancellationToken& serviceToken,
                                        const CancellationToken& instanceToken) {
    // If only the instance token was canceled, then we must have gotten donorAbortMigration.
    uassert(ErrorCodes::TenantMigrationAborted,
            "Migration aborted due to receiving donorAbortMigration.",
            !instanceToken.isCanceled() || serviceToken.isCanceled());
}

template <class Promise>
void setPromiseFromStatusIfNotReady(WithLock lk, Promise& promise, Status status) {
    if (promise.getFuture().isReady()) {
        return;
    }

    if (status.isOK()) {
        promise.emplaceValue();
    } else {
        promise.setError(status);
    }
}

template <class Promise>
void setPromiseErrorIfNotReady(WithLock lk, Promise& promise, Status status) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.setError(status);
}

template <class Promise>
void setPromiseOkIfNotReady(WithLock lk, Promise& promise) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue();
}

}  // namespace

// Note this index is required on both the donor and recipient in a tenant migration, since each
// will copy cluster time keys from the other. The donor service is set up on all mongods on stepup
// to primary, so this index will be created on both donors and recipients.
ExecutorFuture<void> TenantMigrationDonorService::createStateDocumentTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("expireAt" << 1) << "name" << kTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopCreatingTTLIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::createExternalKeysTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               const auto nss = NamespaceString::kExternalKeysCollectionNamespace;

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("ttlExpiresAt" << 1) << "name"
                                                 << kExternalKeysTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopCreatingTTLIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return createStateDocumentTTLIndex(executor, token).then([this, executor, token] {
        return createExternalKeysTTLIndex(executor, token);
    });
}

TenantMigrationDonorService::Instance::Instance(ServiceContext* const serviceContext,
                                                const TenantMigrationDonorService* donorService,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _donorService(donorService),
      _stateDoc(tenant_migration_access_blocker::parseDonorStateDocument(initialState)),
      _instanceName(kServiceName + "-" + _stateDoc.getTenantId()),
      _recipientUri(
          uassertStatusOK(MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()))),
      _tenantId(_stateDoc.getTenantId()),
      _recipientConnectionString(_stateDoc.getRecipientConnectionString()),
      _readPreference(_stateDoc.getReadPreference()),
      _migrationUuid(_stateDoc.getId()),
      _donorCertificateForRecipient(_stateDoc.getDonorCertificateForRecipient()),
      _recipientCertificateForDonor(_stateDoc.getRecipientCertificateForDonor()),
      _sslMode(repl::tenantMigrationDisableX509Auth ? transport::kGlobalSSLMode
                                                    : transport::kEnableSSL) {
    _recipientCmdExecutor = _makeRecipientCmdExecutor();
    _recipientCmdExecutor->startup();

    if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
        // The migration was resumed on stepup.

        _durableState.state = _stateDoc.getState();
        if (_stateDoc.getAbortReason()) {
            auto abortReasonBson = _stateDoc.getAbortReason().get();
            auto code = abortReasonBson["code"].Int();
            auto errmsg = abortReasonBson["errmsg"].String();
            _durableState.abortReason = Status(ErrorCodes::Error(code), errmsg);
            _abortReason = _durableState.abortReason;
        }

        _initialDonorStateDurablePromise.emplaceValue();

        if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted ||
            _stateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted) {
            _decisionPromise.emplaceValue();
        }
    }
}

TenantMigrationDonorService::Instance::~Instance() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_initialDonorStateDurablePromise.getFuture().isReady());
    invariant(_receiveDonorForgetMigrationPromise.getFuture().isReady());

    // Unlike the TenantMigrationDonorService's scoped task executor which is shut down on stepdown
    // and joined on stepup, _recipientCmdExecutor is only shut down and joined when the Instance
    // is destroyed. This is safe since ThreadPoolTaskExecutor::shutdown() only cancels the
    // outstanding work on the task executor which the cancellation token will already do, and the
    // Instance will be destroyed on stepup so this is equivalent to joining the task executor on
    // stepup.
    _recipientCmdExecutor->shutdown();
    _recipientCmdExecutor->join();
}

std::shared_ptr<executor::ThreadPoolTaskExecutor>
TenantMigrationDonorService::Instance::_makeRecipientCmdExecutor() {
    ThreadPool::Options threadPoolOptions(_getRecipientCmdThreadPoolLimits());
    threadPoolOptions.threadNamePrefix = _instanceName + "-";
    threadPoolOptions.poolName = _instanceName + "ThreadPool";
    threadPoolOptions.onCreateThread = [this](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization(&cc());

        // Ideally, we should also associate the client created by _recipientCmdExecutor with the
        // TenantMigrationDonorService to make the opCtxs created by the task executor get
        // registered in the TenantMigrationDonorService, and killed on stepdown. But that would
        // require passing the pointer to the TenantMigrationService into the Instance and making
        // constructInstance not const so we can set the client's decoration here. Right now there
        // is no need for that since the task executor is only used with scheduleRemoteCommand and
        // no opCtx will be created (the cancellation token is responsible for canceling the
        // outstanding work on the task executor).
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();

    auto connPoolOptions = executor::ConnectionPool::Options();
    if (_donorCertificateForRecipient) {
        invariant(!repl::tenantMigrationDisableX509Auth);
        invariant(_recipientCertificateForDonor);
        invariant(_sslMode == transport::kEnableSSL);
#ifdef MONGO_CONFIG_SSL
        uassert(ErrorCodes::IllegalOperation,
                "Cannot run tenant migration with x509 authentication as SSL is not enabled",
                getSSLGlobalParams().sslMode.load() != SSLParams::SSLMode_disabled);
        auto donorSSLClusterPEMPayload =
            _donorCertificateForRecipient->getCertificate().toString() + "\n" +
            _donorCertificateForRecipient->getPrivateKey().toString();
        connPoolOptions.transientSSLParams = TransientSSLParams{
            _recipientUri.connectionString(), std::move(donorSSLClusterPEMPayload)};
#else
        // If SSL is not supported, the donorStartMigration command should have failed certificate
        // field validation.
        MONGO_UNREACHABLE;
#endif
    } else {
        invariant(repl::tenantMigrationDisableX509Auth);
        invariant(!_recipientCertificateForDonor);
        invariant(_sslMode == transport::kGlobalSSLMode);
    }

    return std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(
            _instanceName + "-Network", nullptr, std::move(hookList), connPoolOptions));
}

boost::optional<BSONObj> TenantMigrationDonorService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    stdx::lock_guard<Latch> lg(_mutex);

    // Ignore connMode and sessionMode because tenant migrations are not associated with
    // sessions and they run in a background thread pool.
    BSONObjBuilder bob;
    bob.append("desc", "tenant donor migration");
    bob.append("migrationCompleted", _completionPromise.getFuture().isReady());
    _migrationUuid.appendToBuilder(&bob, "instanceID"_sd);
    bob.append("tenantId", _tenantId);
    bob.append("recipientConnectionString", _recipientConnectionString);
    bob.append("readPreference", _readPreference.toInnerBSON());
    bob.append("receivedCancellation", _abortMigrationSource.token().isCanceled());
    bob.append("lastDurableState", _durableState.state);
    if (_stateDoc.getMigrationStart()) {
        bob.appendDate("migrationStart", *_stateDoc.getMigrationStart());
    }
    if (_stateDoc.getExpireAt()) {
        bob.appendDate("expireAt", *_stateDoc.getExpireAt());
    }
    if (_stateDoc.getStartMigrationDonorTimestamp()) {
        bob.append("startMigrationDonorTimestamp", *_stateDoc.getStartMigrationDonorTimestamp());
    }
    if (_stateDoc.getBlockTimestamp()) {
        bob.append("blockTimestamp", *_stateDoc.getBlockTimestamp());
    }
    if (_stateDoc.getCommitOrAbortOpTime()) {
        _stateDoc.getCommitOrAbortOpTime()->append(&bob, "commitOrAbortOpTime");
    }
    if (_stateDoc.getAbortReason()) {
        bob.append("abortReason", *_stateDoc.getAbortReason());
    }
    return bob.obj();
}

Status TenantMigrationDonorService::Instance::checkIfOptionsConflict(
    const TenantMigrationDonorDocument& stateDoc) {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(stateDoc.getId() == _migrationUuid);

    if (stateDoc.getTenantId() == _tenantId &&
        stateDoc.getRecipientConnectionString() == _recipientConnectionString &&
        stateDoc.getReadPreference().equals(_readPreference) &&
        stateDoc.getDonorCertificateForRecipient() == _donorCertificateForRecipient &&
        stateDoc.getRecipientCertificateForDonor() == _recipientCertificateForDonor) {
        return Status::OK();
    }

    return Status(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _migrationUuid.toBSON() << "\" with different options "
                                << tenant_migration_util::redactStateDoc(_stateDoc.toBSON()));
}

TenantMigrationDonorService::Instance::DurableState
TenantMigrationDonorService::Instance::getDurableState(OperationContext* opCtx) {
    // Wait for the insert of the state doc to become majority-committed.
    _initialDonorStateDurablePromise.getFuture().get(opCtx);

    stdx::lock_guard<Latch> lg(_mutex);
    return _durableState;
}

void TenantMigrationDonorService::Instance::onReceiveDonorAbortMigration() {
    _abortMigrationSource.cancel();

    stdx::lock_guard<Latch> lg(_mutex);
    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
}

void TenantMigrationDonorService::Instance::onReceiveDonorForgetMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    setPromiseOkIfNotReady(lg, _receiveDonorForgetMigrationPromise);
}

void TenantMigrationDonorService::Instance::interrupt(Status status) {
    stdx::lock_guard<Latch> lg(_mutex);
    // Resolve any unresolved promises to avoid hanging.
    setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);
    setPromiseErrorIfNotReady(lg, _receiveDonorForgetMigrationPromise, status);
    setPromiseErrorIfNotReady(lg, _completionPromise, status);
    setPromiseErrorIfNotReady(lg, _decisionPromise, status);
    setPromiseErrorIfNotReady(lg, _migrationCancelablePromise, status);

    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_insertStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kUninitialized);
    _stateDoc.setState(TenantMigrationDonorStateEnum::kAbortingIndexBuilds);

    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _migrationUuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return BSON("$setOnInsert" << _stateDoc.toBSON());
                       }();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       // '$setOnInsert' update operator can never modify an existing on-disk state
                       // doc.
                       invariant(!updateResult.numDocsModified);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_updateStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const TenantMigrationDonorStateEnum nextState,
    const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    const auto originalStateDocBson = _stateDoc.toBSON();

    return AsyncTry([this, self = shared_from_this(), executor, nextState, originalStateDocBson] {
               boost::optional<repl::OpTime> updateOpTime;

               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       WriteUnitOfWork wuow(opCtx);

                       const auto originalRecordId = Helpers::findOne(opCtx,
                                                                      collection.getCollection(),
                                                                      originalStateDocBson,
                                                                      false /* requireIndex */);
                       const auto originalSnapshot = Snapshotted<BSONObj>(
                           opCtx->recoveryUnit()->getSnapshotId(), originalStateDocBson);
                       invariant(!originalRecordId.isNull());

                       // Reserve an opTime for the write.
                       auto oplogSlot =
                           repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                       {
                           stdx::lock_guard<Latch> lg(_mutex);

                           // Update the state.
                           _stateDoc.setState(nextState);
                           switch (nextState) {
                               case TenantMigrationDonorStateEnum::kDataSync: {
                                   _stateDoc.setStartMigrationDonorTimestamp(
                                       oplogSlot.getTimestamp());
                                   break;
                               }
                               case TenantMigrationDonorStateEnum::kBlocking: {
                                   _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());

                                   auto mtab = tenant_migration_access_blocker::
                                       getTenantMigrationDonorAccessBlocker(_serviceContext,
                                                                            _tenantId);
                                   invariant(mtab);

                                   mtab->startBlockingWrites();
                                   opCtx->recoveryUnit()->onRollback(
                                       [mtab] { mtab->rollBackStartBlocking(); });
                                   break;
                               }
                               case TenantMigrationDonorStateEnum::kCommitted:
                                   _stateDoc.setCommitOrAbortOpTime(oplogSlot);
                                   break;
                               case TenantMigrationDonorStateEnum::kAborted: {
                                   _stateDoc.setCommitOrAbortOpTime(oplogSlot);

                                   invariant(_abortReason);
                                   BSONObjBuilder bob;
                                   _abortReason.get().serializeErrorToBSON(&bob);
                                   _stateDoc.setAbortReason(bob.obj());
                                   break;
                               }
                               default:
                                   MONGO_UNREACHABLE;
                           }
                       }

                       const auto updatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       }();

                       CollectionUpdateArgs args;
                       args.criteria = BSON("_id" << _migrationUuid);
                       args.oplogSlot = oplogSlot;
                       args.update = updatedStateDocBson;

                       collection->updateDocument(opCtx,
                                                  originalRecordId,
                                                  originalSnapshot,
                                                  updatedStateDocBson,
                                                  false,
                                                  nullptr /* OpDebug* */,
                                                  &args);

                       wuow.commit();

                       updateOpTime = oplogSlot;
                   });

               invariant(updateOpTime);
               return updateOpTime.get();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<repl::OpTime>
TenantMigrationDonorService::Instance::_markStateDocAsGarbageCollectable(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    stdx::lock_guard<Latch> lg(_mutex);

    _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable.pauseWhileSet(opCtx);

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx,
                   "TenantMigrationDonorMarkStateDocAsGarbageCollectable",
                   _stateDocumentsNS.ns(),
                   [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _migrationUuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       }();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_waitForMajorityWriteConcern(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, repl::OpTime opTime) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime), CancellationToken::uncancelable())
        .thenRunOn(**executor)
        .then([this, self = shared_from_this()] {
            stdx::lock_guard<Latch> lg(_mutex);
            _durableState.state = _stateDoc.getState();
            switch (_durableState.state) {
                case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
                    setPromiseOkIfNotReady(lg, _initialDonorStateDurablePromise);
                    break;
                case TenantMigrationDonorStateEnum::kDataSync:
                case TenantMigrationDonorStateEnum::kBlocking:
                case TenantMigrationDonorStateEnum::kCommitted:
                    break;
                case TenantMigrationDonorStateEnum::kAborted:
                    invariant(_abortReason);
                    _durableState.abortReason = _abortReason;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendCommandToRecipient(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const BSONObj& cmdObj,
    const CancellationToken& token) {
    return AsyncTry(
               [this, self = shared_from_this(), executor, recipientTargeterRS, cmdObj, token] {
                   return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, token)
                       .thenRunOn(**executor)
                       .then([this, self = shared_from_this(), executor, cmdObj, token](
                                 auto recipientHost) {
                           executor::RemoteCommandRequest request(
                               std::move(recipientHost),
                               NamespaceString::kAdminDb.toString(),
                               std::move(cmdObj),
                               rpc::makeEmptyMetadata(),
                               nullptr);
                           request.sslMode = _sslMode;

                           return (_recipientCmdExecutor)
                               ->scheduleRemoteCommand(std::move(request), token)
                               .then([this,
                                      self = shared_from_this()](const auto& response) -> Status {
                                   if (!response.isOK()) {
                                       return response.status;
                                   }
                                   auto commandStatus = getStatusFromCommandResult(response.data);
                                   commandStatus.addContext(
                                       "Tenant migration recipient command failed");
                                   return commandStatus;
                               });
                       });
               })
        .until([token](Status status) { return shouldStopSendingRecipientCommand(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    const auto cmdObj = [&] {
        auto donorConnString =
            repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();

        RecipientSyncData request;
        request.setDbName(NamespaceString::kAdminDb);

        MigrationRecipientCommonData commonData(
            _migrationUuid, donorConnString.toString(), _tenantId, _readPreference);
        commonData.setRecipientCertificateForDonor(_recipientCertificateForDonor);
        request.setMigrationRecipientCommonData(commonData);

        stdx::lock_guard<Latch> lg(_mutex);

        invariant(_stateDoc.getStartMigrationDonorTimestamp());
        request.setStartMigrationDonorTimestamp(*_stateDoc.getStartMigrationDonorTimestamp());
        request.setReturnAfterReachingDonorTimestamp(_stateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }();

    return _sendCommandToRecipient(executor, recipientTargeterRS, cmdObj, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientForgetMigrationCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& token) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto donorConnString =
        repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();

    RecipientForgetMigration request;
    request.setDbName(NamespaceString::kAdminDb);

    MigrationRecipientCommonData commonData(
        _migrationUuid, donorConnString.toString(), _tenantId, _readPreference);
    commonData.setRecipientCertificateForDonor(_recipientCertificateForDonor);
    request.setMigrationRecipientCommonData(commonData);

    return _sendCommandToRecipient(executor, recipientTargeterRS, request.toBSON(BSONObj()), token);
}

SemiFuture<void> TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& serviceToken) noexcept {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_stateDoc.getMigrationStart()) {
            _stateDoc.setMigrationStart(_serviceContext->getFastClockSource()->now());
        }
    }

    pauseTenantMigrationBeforeEnteringFutureChain.pauseWhileSet();

    _abortMigrationSource = CancellationSource(serviceToken);
    {
        stdx::lock_guard<Latch> lg(_mutex);
        setPromiseOkIfNotReady(lg, _migrationCancelablePromise);
    }
    auto recipientTargeterRS = std::make_shared<RemoteCommandTargeterRS>(
        _recipientUri.getSetName(), _recipientUri.getServers());
    auto scopedOutstandingMigrationCounter =
        TenantMigrationStatistics::get(_serviceContext)->getScopedOutstandingDonatingCount();

    return ExecutorFuture(**executor)
        .then([this, self = shared_from_this(), executor, serviceToken] {
            return _enterAbortingIndexBuildsState(
                executor, serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), serviceToken] {
            _abortIndexBuilds(serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            return _fetchAndStoreRecipientClusterTimeKeyDocs(
                executor, recipientTargeterRS, serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), executor, serviceToken] {
            return _enterDataSyncState(executor, serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            return _waitForRecipientToBecomeConsistentAndEnterBlockingState(
                executor, recipientTargeterRS, serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            return _waitForRecipientToReachBlockTimestampAndEnterCommittedState(
                executor, recipientTargeterRS, serviceToken, _abortMigrationSource.token());
        })
        .onError([this, self = shared_from_this(), executor, serviceToken](Status status) {
            return _handleErrorOrEnterAbortedState(executor, serviceToken, status);
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _migrationUuid,
                  "tenantId"_attr = _tenantId,
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);
            if (!_stateDoc.getExpireAt()) {
                // Avoid double counting tenant migration statistics after failover.
                // Double counting may still happen if the failover to the same primary
                // happens after this block and before the state doc GC is persisted.
                if (_abortReason) {
                    TenantMigrationStatistics::get(_serviceContext)
                        ->incTotalFailedMigrationsDonated();
                } else {
                    TenantMigrationStatistics::get(_serviceContext)
                        ->incTotalSuccessfulMigrationsDonated();
                }
            }
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            return _waitForForgetMigrationThenMarkMigrationGarbageCollectable(
                executor, recipientTargeterRS, serviceToken);
        })
        .onCompletion([this,
                       self = shared_from_this(),
                       scopedCounter{std::move(scopedOutstandingMigrationCounter)}](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);

            LOGV2(4920400,
                  "Marked migration state as garbage collectable",
                  "migrationId"_attr = _migrationUuid,
                  "expireAt"_attr = _stateDoc.getExpireAt(),
                  "status"_attr = status);

            setPromiseFromStatusIfNotReady(lg, _completionPromise, status);
        })
        .semi();
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_enterAbortingIndexBuildsState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& serviceToken,
    const CancellationToken& instanceToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
            return ExecutorFuture(**executor);
        }
    }

    // Enter "abortingIndexBuilds" state.
    return _insertStateDoc(executor, instanceToken)
        .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
            // use its base PrimaryOnlyService's cancellation source to pass tokens
            // in calls to WaitForMajorityService::waitUntilMajority.
            return _waitForMajorityWriteConcern(executor, std::move(opTime));
        })
        .then([this, self = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            pauseTenantMigrationAfterPersistingInitialDonorStateDoc.pauseWhileSet(opCtx);
        });
}

void TenantMigrationDonorService::Instance::_abortIndexBuilds(
    const CancellationToken& serviceToken, const CancellationToken& instanceToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return;
        }
    }

    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

    // Before starting data sync, abort any in-progress index builds.  No new index
    // builds can start while we are doing this because the mtab prevents it.
    {
        auto opCtxHolder = cc().makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        auto* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
        indexBuildsCoordinator->abortTenantIndexBuilds(opCtx, _tenantId, "tenant migration");
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_fetchAndStoreRecipientClusterTimeKeyDocs(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& serviceToken,
    const CancellationToken& instanceToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return ExecutorFuture(**executor);
        }
    }

    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

    return AsyncTry([this,
                     self = shared_from_this(),
                     executor,
                     recipientTargeterRS,
                     serviceToken,
                     instanceToken] {
               return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, instanceToken)
                   .thenRunOn(**executor)
                   .then([this, self = shared_from_this(), executor, serviceToken, instanceToken](
                             HostAndPort host) {
                       pauseTenantMigrationBeforeFetchingKeys.pauseWhileSet();

                       const auto nss = NamespaceString::kKeysCollectionNamespace;

                       const auto cmdObj = [&] {
                           FindCommandRequest request(NamespaceStringOrUUID{nss});
                           request.setReadConcern(
                               repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern)
                                   .toBSONInner());
                           return request.toBSON(BSONObj());
                       }();

                       std::vector<ExternalKeysCollectionDocument> keyDocs;
                       boost::optional<Status> fetchStatus;

                       auto fetcherCallback =
                           [this, self = shared_from_this(), &keyDocs, &fetchStatus](
                               const Fetcher::QueryResponseStatus& dataStatus,
                               Fetcher::NextAction* nextAction,
                               BSONObjBuilder* getMoreBob) {
                               // Throw out any accumulated results on error
                               if (!dataStatus.isOK()) {
                                   fetchStatus = dataStatus.getStatus();
                                   keyDocs.clear();
                                   return;
                               }

                               const auto& data = dataStatus.getValue();
                               for (const BSONObj& doc : data.documents) {
                                   keyDocs.push_back(
                                       tenant_migration_util::makeExternalClusterTimeKeyDoc(
                                           _migrationUuid, doc.getOwned()));
                               }
                               fetchStatus = Status::OK();

                               if (!getMoreBob) {
                                   return;
                               }
                               getMoreBob->append("getMore", data.cursorId);
                               getMoreBob->append("collection", data.nss.coll());
                           };

                       auto fetcher = std::make_shared<Fetcher>(
                           _recipientCmdExecutor.get(),
                           host,
                           nss.db().toString(),
                           cmdObj,
                           fetcherCallback,
                           kPrimaryOnlyReadPreference.toContainingBSON(),
                           executor::RemoteCommandRequest::kNoTimeout, /* findNetworkTimeout */
                           executor::RemoteCommandRequest::kNoTimeout, /* getMoreNetworkTimeout */
                           RemoteCommandRetryScheduler::makeRetryPolicy<
                               ErrorCategory::RetriableError>(
                               kMaxRecipientKeyDocsFindAttempts,
                               executor::RemoteCommandRequest::kNoTimeout),
                           _sslMode);

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);
                           uassert(ErrorCodes::Interrupted,
                                   "Donor service interrupted",
                                   !serviceToken.isCanceled());
                           _recipientKeysFetcher = fetcher;
                       }

                       uassertStatusOK(fetcher->schedule());
                       fetcher->join();

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           _recipientKeysFetcher.reset();
                       }

                       if (!fetchStatus) {
                           // The callback never got invoked.
                           uasserted(5340400, "Internal error running cursor callback in command");
                       }
                       uassertStatusOK(fetchStatus.get());

                       return keyDocs;
                   })
                   .then([this, self = shared_from_this(), executor, serviceToken, instanceToken](
                             auto keyDocs) {
                       checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

                       return tenant_migration_util::storeExternalClusterTimeKeyDocs(
                           std::move(keyDocs));
                   })
                   .then([this, self = shared_from_this(), serviceToken, instanceToken](
                             repl::OpTime lastKeyOpTime) {
                       checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

                       pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate.pauseWhileSet();

                       auto votingMembersWriteConcern =
                           WriteConcernOptions(repl::ReplSetConfig::kConfigAllWriteConcernName,
                                               WriteConcernOptions::SyncMode::NONE,
                                               WriteConcernOptions::kNoTimeout);
                       auto writeConcernFuture = repl::ReplicationCoordinator::get(_serviceContext)
                                                     ->awaitReplicationAsyncNoWTimeout(
                                                         lastKeyOpTime, votingMembersWriteConcern);
                       return future_util::withCancellation(std::move(writeConcernFuture),
                                                            instanceToken);
                   });
           })
        .until([instanceToken](Status status) {
            return shouldStopFetchingRecipientClusterTimeKeyDocs(status, instanceToken);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_enterDataSyncState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& serviceToken,
    const CancellationToken& instanceToken) {
    pauseTenantMigrationAfterFetchingAndStoringKeys.pauseWhileSet();
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
            return ExecutorFuture(**executor);
        }
    }

    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

    pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState.pauseWhileSet();

    // Enter "dataSync" state.
    return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kDataSync, instanceToken)
        .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
            // use its base PrimaryOnlyService's cancellation source to pass tokens
            // in calls to WaitForMajorityService::waitUntilMajority.
            return _waitForMajorityWriteConcern(executor, std::move(opTime));
        });
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForRecipientToBecomeConsistentAndEnterBlockingState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& serviceToken,
    const CancellationToken& instanceToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kDataSync) {
            return ExecutorFuture(**executor);
        }
    }

    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

    return _sendRecipientSyncDataCommand(executor, recipientTargeterRS, instanceToken)
        .then([this, self = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            pauseTenantMigrationBeforeLeavingDataSyncState.pauseWhileSet(opCtx);
        })
        .then([this, self = shared_from_this(), executor, serviceToken, instanceToken] {
            checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

            // Enter "blocking" state.
            return _updateStateDoc(
                       executor, TenantMigrationDonorStateEnum::kBlocking, instanceToken)
                .then([this, self = shared_from_this(), executor, serviceToken, instanceToken](
                          repl::OpTime opTime) {
                    // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                    // use its base PrimaryOnlyService's cancellation source to pass tokens
                    // in calls to WaitForMajorityService::waitUntilMajority.
                    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                });
        });
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForRecipientToReachBlockTimestampAndEnterCommittedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& serviceToken,
    const CancellationToken& instanceToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }
    }

    checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        invariant(_stateDoc.getBlockTimestamp());
    }

    // Source to cancel the timeout if the operation completed in time.
    CancellationSource cancelTimeoutSource;

    auto deadlineReachedFuture =
        (*executor)->sleepFor(Milliseconds(repl::tenantMigrationBlockingStateTimeoutMS.load()),
                              cancelTimeoutSource.token());
    std::vector<ExecutorFuture<void>> futures;

    futures.push_back(std::move(deadlineReachedFuture));
    futures.push_back(_sendRecipientSyncDataCommand(executor, recipientTargeterRS, instanceToken));

    return whenAny(std::move(futures))
        .thenRunOn(**executor)
        .then([this, cancelTimeoutSource, self = shared_from_this()](auto result) mutable {
            const auto& [status, idx] = result;

            if (idx == 0) {
                LOGV2(5290301,
                      "Tenant migration blocking stage timeout expired",
                      "timeoutMs"_attr = repl::tenantMigrationGarbageCollectionDelayMS.load());
                // Deadline reached, cancel the pending '_sendRecipientSyncDataCommand()'...
                _abortMigrationSource.cancel();
                // ...and return error.
                uasserted(ErrorCodes::ExceededTimeLimit, "Blocking state timeout expired");
            } else if (idx == 1) {
                // '_sendRecipientSyncDataCommand()' finished first, cancel the timeout.
                cancelTimeoutSource.cancel();
                return status;
            }
            MONGO_UNREACHABLE;
        })
        .then([this, self = shared_from_this()]() -> void {
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            pauseTenantMigrationBeforeLeavingBlockingState.executeIf(
                [&](const BSONObj& data) {
                    if (!data.hasField("blockTimeMS")) {
                        pauseTenantMigrationBeforeLeavingBlockingState.pauseWhileSet(opCtx);
                    } else {
                        const auto blockTime = Milliseconds{data.getIntField("blockTimeMS")};
                        LOGV2(5010400,
                              "Keep migration in blocking state",
                              "blockTime"_attr = blockTime);
                        opCtx->sleepFor(blockTime);
                    }
                },
                [&](const BSONObj& data) {
                    return !data.hasField("tenantId") || _tenantId == data["tenantId"].str();
                });

            if (MONGO_unlikely(abortTenantMigrationBeforeLeavingBlockingState.shouldFail())) {
                uasserted(ErrorCodes::InternalError, "simulate a tenant migration error");
            }
        })
        .then([this, self = shared_from_this(), executor, serviceToken, instanceToken] {
            checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

            // Enter "commit" state.
            return _updateStateDoc(
                       executor, TenantMigrationDonorStateEnum::kCommitted, serviceToken)
                .then(
                    [this, self = shared_from_this(), executor, serviceToken](repl::OpTime opTime) {
                        // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                        // use its base PrimaryOnlyService's cancellation source to pass tokens
                        // in calls to WaitForMajorityService::waitUntilMajority.
                        return _waitForMajorityWriteConcern(executor, std::move(opTime))
                            .then([this, self = shared_from_this()] {
                                stdx::lock_guard<Latch> lg(_mutex);
                                // If interrupt is called at some point during execution, it is
                                // possible that interrupt() will fulfill the promise before we
                                // do.
                                setPromiseOkIfNotReady(lg, _decisionPromise);
                            });
                    });
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_handleErrorOrEnterAbortedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& serviceToken,
    Status status) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
            // The migration was resumed on stepup and it was already aborted.
            return ExecutorFuture(**executor);
        }
    }

    auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
        _serviceContext, _tenantId);
    if (status == ErrorCodes::ConflictingOperationInProgress || !mtab) {
        stdx::lock_guard<Latch> lg(_mutex);
        // Fulfill the promise since the state doc failed to insert.
        setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);

        return ExecutorFuture(**executor);
    } else if (status == ErrorCodes::PrimarySteppedDown) {
        // The node started stepping down while the instance was waiting for key docs to
        // to replicate. Do not abort the migration since the migration can safely resume
        // when the new primary steps up.
        stdx::lock_guard<Latch> lg(_mutex);
        setPromiseErrorIfNotReady(lg, _initialDonorStateDurablePromise, status);

        return ExecutorFuture(**executor);
    } else {
        // Enter "abort" state.
        _abortReason.emplace(status);
        return _updateStateDoc(executor, TenantMigrationDonorStateEnum::kAborted, serviceToken)
            .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                return _waitForMajorityWriteConcern(executor, std::move(opTime))
                    .then([this, self = shared_from_this()] {
                        stdx::lock_guard<Latch> lg(_mutex);
                        // If interrupt is called at some point during execution, it is
                        // possible that interrupt() will fulfill the promise before we do.
                        setPromiseOkIfNotReady(lg, _decisionPromise);
                    });
            });
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_waitForForgetMigrationThenMarkMigrationGarbageCollectable(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancellationToken& serviceToken) {
    auto expiredAt = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        return _stateDoc.getExpireAt();
    }();

    if (expiredAt) {
        // The migration state has already been marked as garbage collectable. Set the
        // donorForgetMigration promise here since the Instance's destructor has an
        // invariant that _receiveDonorForgetMigrationPromise is ready.
        onReceiveDonorForgetMigration();
        return ExecutorFuture(**executor);
    }

    // Wait for the donorForgetMigration command.
    // If donorAbortMigration has already canceled work, the abortMigrationSource would be
    // canceled and continued usage of the source would lead to incorrect behavior. Thus, we
    // need to use the serviceToken after the migration has reached a decision state in
    // order to continue work, such as sending donorForgetMigration, successfully.
    return std::move(_receiveDonorForgetMigrationPromise.getFuture())
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            return _sendRecipientForgetMigrationCommand(
                executor, recipientTargeterRS, serviceToken);
        })
        .then([this, self = shared_from_this(), executor, serviceToken] {
            // Note marking the keys as garbage collectable is not atomic with marking the
            // state document garbage collectable, so an interleaved failover can lead the
            // keys to be deleted before the state document has an expiration date. This is
            // acceptable because the decision to forget a migration is not reversible.
            return tenant_migration_util::markExternalKeysAsGarbageCollectable(
                _serviceContext,
                executor,
                _donorService->getInstanceCleanupExecutor(),
                _migrationUuid,
                serviceToken);
        })
        .then([this, self = shared_from_this(), executor, serviceToken] {
            return _markStateDocAsGarbageCollectable(executor, serviceToken);
        })
        .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime));
        });
}

}  // namespace mongo
