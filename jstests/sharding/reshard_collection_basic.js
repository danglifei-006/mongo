//
// Basic tests for reshardCollection.
// @tags: [
//   requires_fcv_47,
//   uses_atclustertime,
// ]
//

load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

(function() {
'use strict';

const st = new ShardingTest({
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagResharding: true}},
    configOptions: {setParameter: {featureFlagResharding: true}},
    shards: 2,
    shardOptions: {setParameter: {featureFlagResharding: true}},
});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;
const mongosConfig = mongos.getDB('config');
const kNumInitialDocs = 500;

let shardToRSMap = {};
shardToRSMap[st.shard0.shardName] = st.rs0;
shardToRSMap[st.shard1.shardName] = st.rs1;

let shardIdToShardMap = {};
shardIdToShardMap[st.shard0.shardName] = st.shard0;
shardIdToShardMap[st.shard1.shardName] = st.shard1;

const DDLFeatureFlagParam = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1}));
const isDDLFeatureFlagEnabled =
    DDLFeatureFlagParam.featureFlagShardingFullDDLSupportTimestampedVersion.value;

let getUUIDFromCollectionInfo = (dbName, collName, collInfo) => {
    if (collInfo) {
        return extractUUIDFromObject(collInfo.info.uuid);
    }

    const uuidObject = getUUIDFromListCollections(mongos.getDB(dbName), collName);
    return extractUUIDFromObject(uuidObject);
};

let constructTemporaryReshardingCollName = (dbName, collName, collInfo) => {
    const existingUUID = getUUIDFromCollectionInfo(dbName, collName, collInfo);
    return 'system.resharding.' + existingUUID;
};

let getAllShardIdsFromExpectedChunks = (expectedChunks) => {
    let shardIds = new Set();
    expectedChunks.forEach(chunk => {
        shardIds.add(chunk.recipientShardId);
    });
    return shardIds;
};

let verifyChunksMatchExpected = (numExpectedChunks, presetExpectedChunks) => {
    let chunkQuery = {ns: ns};
    if (isDDLFeatureFlagEnabled) {
        let collEntry = mongos.getDB('config').getCollection('collections').findOne({_id: ns});
        chunkQuery = {uuid: collEntry.uuid};
    }

    const reshardedChunks = mongosConfig.chunks.find(chunkQuery).toArray();

    if (presetExpectedChunks) {
        presetExpectedChunks.sort();
    }

    reshardedChunks.sort();
    assert.eq(numExpectedChunks, reshardedChunks.length, tojson(reshardedChunks));

    let shardChunkCounts = {};
    let incChunkCount = key => {
        if (shardChunkCounts.hasOwnProperty(key)) {
            shardChunkCounts[key]++;
        } else {
            shardChunkCounts[key] = 1;
        }
    };

    for (let i = 0; i < numExpectedChunks; i++) {
        incChunkCount(reshardedChunks[i].shard);

        // match exact chunk boundaries for presetExpectedChunks
        if (presetExpectedChunks) {
            assert.eq(presetExpectedChunks[i].recipientShardId, reshardedChunks[i].shard);
            assert.eq(presetExpectedChunks[i].min, reshardedChunks[i].min);
            assert.eq(presetExpectedChunks[i].max, reshardedChunks[i].max);
        }
    }

    // if presetChunks not specified, we only assert that chunks counts are balanced across shards
    if (!presetExpectedChunks) {
        let maxDiff = 0;
        let shards = Object.keys(shardChunkCounts);

        shards.forEach(shard1 => {
            shards.forEach(shard2 => {
                let diff = Math.abs(shardChunkCounts[shard1] - shardChunkCounts[shard2]);
                maxDiff = (diff > maxDiff) ? diff : maxDiff;
            });
        });

        assert.lte(maxDiff, 1, tojson(reshardedChunks));
    }
};

let verifyCollectionExistenceForConn = (collName, expectedToExist, conn) => {
    const doesExist = Boolean(conn.getDB(kDbName)[collName].exists());
    assert.eq(doesExist, expectedToExist);
};

let verifyTemporaryReshardingCollectionExistsWithCorrectOptions = (expectedRecipientShards) => {
    const originalCollInfo = mongos.getDB(kDbName).getCollectionInfos({name: collName})[0];
    assert.neq(originalCollInfo, undefined);

    const tempReshardingCollName =
        constructTemporaryReshardingCollName(kDbName, collName, originalCollInfo);
    verifyCollectionExistenceForConn(tempReshardingCollName, false, mongos);

    expectedRecipientShards.forEach(shardId => {
        const rsPrimary = shardToRSMap[shardId].getPrimary();
        verifyCollectionExistenceForConn(collName, true, rsPrimary);
        verifyCollectionExistenceForConn(tempReshardingCollName, false, rsPrimary);
        ShardedIndexUtil.assertIndexExistsOnShard(
            shardIdToShardMap[shardId], kDbName, collName, {newKey: 1});
    });
};

let verifyAllShardingCollectionsRemoved = (tempReshardingCollName) => {
    assert.eq(0, mongos.getDB(kDbName)[tempReshardingCollName].find().itcount());
    assert.eq(0, mongosConfig.reshardingOperations.find({ns}).itcount());
    assert.eq(0, mongosConfig.collections.find({reshardingFields: {$exists: true}}).itcount());
    assert.eq(
        0,
        st.rs0.getPrimary().getDB('config').localReshardingOperations.donor.find({ns}).itcount());
    assert.eq(0,
              st.rs0.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.recipient.find({ns})
                  .itcount());
    assert.eq(
        0,
        st.rs1.getPrimary().getDB('config').localReshardingOperations.donor.find({ns}).itcount());
    assert.eq(0,
              st.rs1.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.recipient.find({ns})
                  .itcount());
};

let assertReshardCollOkWithPreset = (commandObj, presetReshardedChunks) => {
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    let bulk = mongos.getDB(kDbName).getCollection(collName).initializeOrderedBulkOp();
    for (let x = 0; x < kNumInitialDocs; x++) {
        bulk.insert({oldKey: x, newKey: kNumInitialDocs - x});
    }
    assert.commandWorked(bulk.execute());

    commandObj._presetReshardedChunks = presetReshardedChunks;
    const tempReshardingCollName = constructTemporaryReshardingCollName(kDbName, collName);

    assert.commandWorked(mongos.adminCommand(commandObj));

    verifyTemporaryReshardingCollectionExistsWithCorrectOptions(
        getAllShardIdsFromExpectedChunks(presetReshardedChunks));
    verifyChunksMatchExpected(presetReshardedChunks.length, presetReshardedChunks);

    mongos.getDB(kDbName)[collName].drop();
    verifyAllShardingCollectionsRemoved(tempReshardingCollName);
};

let assertReshardCollOk = (commandObj, expectedChunks) => {
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    let bulk = mongos.getDB(kDbName).getCollection(collName).initializeOrderedBulkOp();
    for (let x = 0; x < kNumInitialDocs; x++) {
        bulk.insert({oldKey: x, newKey: kNumInitialDocs - x});
    }
    assert.commandWorked(bulk.execute());

    const tempReshardingCollName = constructTemporaryReshardingCollName(kDbName, collName);

    assert.commandWorked(mongos.adminCommand(commandObj));

    verifyChunksMatchExpected(expectedChunks);

    mongos.getDB(kDbName)[collName].drop();
    verifyAllShardingCollectionsRemoved(tempReshardingCollName);
};

let presetReshardedChunks =
    [{recipientShardId: st.shard1.shardName, min: {newKey: MinKey}, max: {newKey: MaxKey}}];
const existingZoneName = 'x1';

/**
 * Fail cases
 */

jsTest.log('Fail if sharding is disabled.');
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTest.log("Fail if collection is unsharded.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             ErrorCodes.NamespaceNotSharded);

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

jsTest.log("Fail if missing required key.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns}), 40414);

jsTest.log("Fail if unique is specified and is true.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, unique: true}),
    ErrorCodes.BadValue);

jsTest.log("Fail if collation is specified and is not {locale: 'simple'}.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'en_US'}}),
    ErrorCodes.BadValue);

jsTest.log("Fail if both numInitialChunks and _presetReshardedChunks are provided.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: presetReshardedChunks
}),
                             ErrorCodes.BadValue);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are not provided.");
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are provided and use a name which does not exist in authoritative tags.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: 'x', min: {newKey: 5}, max: {newKey: 10}, ns: ns}],
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

jsTestLog("Fail if attempting insert to an unsharded 'system.resharding.' collection");
assert.commandFailedWithCode(mongos.getDB('test').system.resharding.mycoll.insert({_id: 1, a: 1}),
                             ErrorCodes.NamespaceNotSharded);

/**
 * Success cases
 */

mongos.getDB(kDbName)[collName].drop();

jsTest.log("Succeed when correct locale is provided.");
assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'simple'}}, 1);

jsTest.log("Succeed base case.");
assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}}, 1);

jsTest.log("Succeed if unique is specified and is false.");
assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}, unique: false}, 1);

jsTest.log(
    "Succeed if _presetReshardedChunks is provided and test commands are enabled (default).");
assertReshardCollOkWithPreset({reshardCollection: ns, key: {newKey: 1}}, presetReshardedChunks);

presetReshardedChunks = [
    {recipientShardId: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: 0}},
    {recipientShardId: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
];

jsTest.log("Succeed if all optional fields and numInitialChunks are provided with correct values.");
assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
},
                    2);

jsTest.log(
    "Succeed if all optional fields and _presetReshardedChunks are provided with correct values and test commands are enabled (default).");
assertReshardCollOkWithPreset(
    {reshardCollection: ns, key: {newKey: 1}, unique: false, collation: {locale: 'simple'}},
    presetReshardedChunks);

jsTest.log(
    "Succeed if authoritative tags exist in config.tags collection and zones are provided and use an existing zone's name.");
assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: existingZoneName, min: {newKey: 5}, max: {newKey: 10}, ns: ns}]
},
                    3);

jsTest.log("Succeed if resulting chunks all end up in one shard.");
assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    numInitialChunks: 1,
    collation: {locale: 'simple'},
    zones: [{tag: existingZoneName, min: {newKey: MinKey}, max: {newKey: MaxKey}, ns: ns}]
},
                    1);

st.stop();
})();
