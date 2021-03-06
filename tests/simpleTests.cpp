/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */
#include <config.h>

#include <deltamain/DeltaMainRewriteStore.hpp>
#include <logstructured/LogstructuredMemoryStore.hpp>

#include "DummyCommitManager.hpp"

#include <crossbow/allocator.hpp>

#include <gtest/gtest.h>

using namespace tell::store;

namespace {

int64_t gTupleLargenumber = 0x7FFFFFFF00000001;
crossbow::string gTupleText1 = crossbow::string("Bacon ipsum dolor amet t-bone chicken prosciutto, cupim ribeye turkey "
        "bresaola leberkas bacon.");
crossbow::string gTupleText2 = crossbow::string("Chuck pork loin ham hock tri-tip pork ball tip drumstick tongue. Jowl "
        "swine short loin, leberkas andouille pancetta strip steak doner ham bresaola.");

template <typename Impl>
class StorageTest : public ::testing::Test {
protected:
    StorageTest()
            : mSchema(TableType::TRANSACTIONAL),
              mTableId(0u) {
        StorageConfig config;
        config.totalMemory = 0x10000000ull;
        config.numScanThreads = 1u;
        config.hashMapCapacity = 0x100000ull;
        mStorage.reset(new Impl(config));

        mSchema.addField(FieldType::INT, "foo", true);
    }

    virtual void SetUp() final override {
        crossbow::allocator _;
        ASSERT_TRUE(mStorage->createTable("testTable", mSchema, mTableId)) << "Creating table failed";
        EXPECT_TRUE(correctTableId("testTable", mTableId));
    }

    ::testing::AssertionResult correctTableId(const crossbow::string& name, uint64_t tId) {
        uint64_t id;
        if (!mStorage->getTable(name, id))
            return ::testing::AssertionFailure() << "Table does not exist";
        else if (tId == id)
            return ::testing::AssertionSuccess();
        else
            return ::testing::AssertionFailure() << "Expected tid " << tId << " got " << id << " instead";
    }

    std::unique_ptr<Impl> mStorage;

    DummyCommitManager mCommitManager;

    Schema mSchema;

    uint64_t mTableId;
};

using StorageTestImplementations = ::testing::Types<DeltaMainRewriteRowStore, DeltaMainRewriteColumnStore,
        LogstructuredMemoryStore>;
TYPED_TEST_CASE(StorageTest, StorageTestImplementations);

TYPED_TEST(StorageTest, insert_and_get) {
    crossbow::allocator _; // needed to free memory
    Record record(this->mSchema);

    // Force GC - since we did not do anything yet, this should
    // just return. We can not really test whether it worked
    // correctly, but at least it must not segfault
    this->mStorage->forceGC();
    auto tx = this->mCommitManager.startTx();
    {
        size_t size;
        std::unique_ptr<char[]> rec(record.create(GenericTuple({
                std::make_pair<crossbow::string, boost::any>("foo", 12)
        }), size));
        auto res = this->mStorage->insert(this->mTableId, 1, size, rec.get(), tx);
        ASSERT_TRUE(!res) << "This insert must not fail!";
    }
    {
        std::unique_ptr<char[]> dest;
        auto res = this->mStorage->get(this->mTableId, 1, tx, [&tx, &dest]
                (size_t size, uint64_t version, bool isNewest) {
            EXPECT_EQ(tx->version(), version) << "Tuple has not the version of the snapshot descriptor";
            EXPECT_TRUE(isNewest) << "There should not be any versioning at this point";
            dest.reset(new char[size]);
            return dest.get();
        });
        ASSERT_TRUE(!res) << "Tuple not found";
    }
    tx.commit();
    this->mStorage->forceGC();
}

TYPED_TEST(StorageTest, concurrent_transactions) {
    Record record(this->mSchema);

    // Start transaction 1
    auto tx1 = this->mCommitManager.startTx();

    // Transaction 1 can insert a new tuple
    {
        crossbow::allocator _;
        size_t size;
        std::unique_ptr<char[]> rec(record.create(GenericTuple({
                std::make_pair<crossbow::string, boost::any>("foo", 12)
        }), size));
        auto res = this->mStorage->insert(this->mTableId, 1, size, rec.get(), tx1);
        ASSERT_TRUE(!res) << "This insert must not fail!";
    }

    // Start transaction 2
    auto tx2 = this->mCommitManager.startTx();

    // Transaction 2 can not read the tuple written by transaction 1
    {
        crossbow::allocator _;
        std::unique_ptr<char[]> dest;
        auto res = this->mStorage->get(this->mTableId, 1, tx2, [&dest] (size_t size, uint64_t version, bool isNewest) {
            dest.reset(new char[size]);
            return dest.get();
        });
        EXPECT_EQ(error::not_in_snapshot, res) << "Tuple found for uncommited version";
    }

    // Transaction 2 can not insert a new tuple already written by transaction 1
    {
        crossbow::allocator _;
        size_t size;
        std::unique_ptr<char[]> rec(record.create(GenericTuple({
                std::make_pair<crossbow::string, boost::any>("foo", 13)
        }), size));
        auto res = this->mStorage->insert(this->mTableId, 1, size, rec.get(), tx2);
        EXPECT_FALSE(!res) << "Insert succeeded despite tuple already existing in different version";
    }

    // Transaction 2 can not update the tuple written by transaction 1
    {
        crossbow::allocator _;
        size_t size;
        std::unique_ptr<char[]> rec(record.create(GenericTuple({
                std::make_pair<crossbow::string, boost::any>("foo", 13)
        }), size));
        auto res = this->mStorage->update(this->mTableId, 1, size, rec.get(), tx2);
        EXPECT_FALSE(!res) << "Update succeeded despite tuple already existing in different version";
    }

    // Commit Transaction 1
    tx1.commit();

    // Start transaction 3 able to read transaction 1 but not 2
    auto tx3 = this->mCommitManager.startTx();

    // Transaction 3 can read the tuple written by transaction 1
    {
        crossbow::allocator _;
        std::unique_ptr<char[]> dest;
        auto res = this->mStorage->get(this->mTableId, 1, tx3, [&tx1, &dest]
                (size_t size, uint64_t version, bool isNewest) {
            EXPECT_EQ(tx1->version(), version) << "Version does not match the version of the first transaction";
            EXPECT_TRUE(isNewest) << "Tuple should be the newest";
            dest.reset(new char[size]);
            return dest.get();
        });
        EXPECT_TRUE(!res) << "Tuple not found for commited version";
    }

    // Transaction 3 updates tuple written by transaction 1
    {
        crossbow::allocator _;
        size_t size;
        std::unique_ptr<char[]> rec(record.create(GenericTuple({
                std::make_pair<crossbow::string, boost::any>("foo", 13)
        }), size));
        auto res = this->mStorage->update(this->mTableId, 1, size, rec.get(), tx3);
        EXPECT_TRUE(!res) << "Update not successful";
    }

    // Transaction 2 should not be able to see all versions
    {
        crossbow::allocator _;
        std::unique_ptr<char[]> dest;
        auto res = this->mStorage->get(this->mTableId, 1, tx2, [&tx1, &dest]
                (size_t size, uint64_t version, bool isNewest) {
            dest.reset(new char[size]);
            return dest.get();
        });
        EXPECT_EQ(error::not_in_snapshot, res) << "Tuple found for uncommited version";
    }
}

template <typename Impl>
class HeavyStorageTest : public ::testing::Test {
public:
    HeavyStorageTest()
            : mSchema(TableType::TRANSACTIONAL),
              mTableId(0),
              mTupleSize(0),
              mGo(false) {
        StorageConfig config;
        config.totalMemory = 0x100000000ull;
        config.numScanThreads = 1u;
        config.hashMapCapacity = 0x2000000ull;
        mStorage.reset(new Impl(config));
    }

    virtual void SetUp() {
        mSchema.addField(FieldType::INT, "number", true);
        mSchema.addField(FieldType::TEXT, "text1", true);
        mSchema.addField(FieldType::BIGINT, "largenumber", true);
        mSchema.addField(FieldType::TEXT, "text2", true);

        Record record(mSchema);
        for (decltype(mTuple.size()) i = 0; i < mTuple.size(); ++i) {
            GenericTuple insertTuple({
                    std::make_pair<crossbow::string, boost::any>("number", static_cast<int32_t>(i)),
                    std::make_pair<crossbow::string, boost::any>("text1", gTupleText1),
                    std::make_pair<crossbow::string, boost::any>("largenumber", gTupleLargenumber),
                    std::make_pair<crossbow::string, boost::any>("text2", gTupleText2)
            });
            mTuple[i].reset(record.create(insertTuple, mTupleSize));
        }

        ASSERT_TRUE(mStorage->createTable("testTable", mSchema, mTableId));
    }

    void run(uint64_t startKey, uint64_t endKey) {
        while (!mGo.load()) {
        }

        Record record(mSchema);
        auto transaction = mCommitManager.startTx();

        for (auto key = startKey; key < endKey; ++key) {
            auto ec = mStorage->insert(mTableId, key, mTupleSize, mTuple[key % mTuple.size()].get(), transaction);
            ASSERT_FALSE(ec);

            std::unique_ptr<char[]> dest;
            ec = mStorage->get(mTableId, key, transaction, [&transaction, &dest]
                    (size_t size, uint64_t version, bool isNewest) {
                EXPECT_EQ(version, transaction->version());
                EXPECT_TRUE(isNewest);
                dest.reset(new char[size]);
                return dest.get();
            });
            ASSERT_FALSE(ec);

            auto numberData = getTupleData(dest.get(), record, "number");
            EXPECT_EQ(static_cast<int32_t>(key % mTuple.size()), *reinterpret_cast<const int32_t*>(numberData));

            auto text1OffsetData = reinterpret_cast<const uint32_t*>(getTupleData(dest.get(), record, "text1"));
            auto text1Offset = text1OffsetData[0];
            EXPECT_EQ(gTupleText1, crossbow::string(dest.get() + text1Offset, text1OffsetData[1] - text1Offset));

            auto largenumberData = getTupleData(dest.get(), record, "largenumber");
            EXPECT_EQ(gTupleLargenumber, *reinterpret_cast<const int64_t*>(largenumberData));

            auto text2OffsetData = reinterpret_cast<const uint32_t*>(getTupleData(dest.get(), record, "text2"));
            auto text2Offset = text2OffsetData[0];
            EXPECT_EQ(gTupleText2, crossbow::string(dest.get() + text2Offset, text2OffsetData[1] - text2Offset));
        }

        transaction.commit();
    }

    const char* getTupleData(const char* data, Record& record, const crossbow::string& name) {
        Record::id_t recordField;
        if (!record.idOf(name, recordField)) {
            LOG_ERROR("%1% field not found", name);
        }
        bool fieldIsNull = false;
        auto fieldData = record.data(data, recordField, fieldIsNull);
        return fieldData;
    }

protected:
    std::function<void()> runFunction(uint64_t startRange, uint64_t endRange) {
        return std::bind(&HeavyStorageTest<Impl>::run, this, startRange, endRange);
    }

    std::unique_ptr<Impl> mStorage;
    DummyCommitManager mCommitManager;

    Schema mSchema;

    uint64_t mTableId;

    uint64_t mTupleSize;
    std::array<std::unique_ptr<char[]>, 4> mTuple;

    std::atomic<bool> mGo;
};

TYPED_TEST_CASE(HeavyStorageTest, StorageTestImplementations);

TYPED_TEST(HeavyStorageTest, DISABLED_heavy) {
    std::array<std::thread, 3> threads = {{
        std::thread(this->runFunction(0, 2500000)),
        std::thread(this->runFunction(2500000, 5000000)),
        std::thread(this->runFunction(5000000, 7500000))
    }};

    this->mGo.store(true);
    this->run(7500000, 10000000);

    for (auto& t : threads) {
        t.join();
    }
}

}
