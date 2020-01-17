//
// file_appender_test.cc
// Copyright (C) 2017 4paradigm.com
// Author vagrant
// Date 2017-04-21
//

#include "replica/log_replicator.h"
#include "replica/replicate_node.h"
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include "proto/tablet.pb.h"
#include "logging.h"
#include "thread_pool.h"
#include <brpc/server.h>
#include "storage/mem_table.h"
#include "storage/segment.h"
#include "storage/ticket.h"
#include "timer.h"

using ::baidu::common::ThreadPool;
using ::rtidb::storage::Table;
using ::rtidb::storage::MemTable;
using ::rtidb::storage::Ticket;
using ::rtidb::storage::TableIterator;
using ::rtidb::storage::DataBlock;
using ::google::protobuf::RpcController;
using ::google::protobuf::Closure;
using ::baidu::common::INFO;
using ::baidu::common::DEBUG;

namespace rtidb {
namespace replica {

const std::vector<std::string> g_endpoints;    

class MockTabletImpl : public ::rtidb::api::TabletServer {

public:
    MockTabletImpl(const ReplicatorRole& role,
                   const std::string& path,
                   const std::vector<std::string>& endpoints,
                   std::shared_ptr<MemTable> table): role_(role),
    path_(path), endpoints_(endpoints), 
    replicator_(path_, endpoints_, role_, table, &follower_) {
    }

    ~MockTabletImpl() {
    }
    bool Init() {
        //table_ = new Table("test", 1, 1, 8, 0, false, g_endpoints);
        //table_->Init();
        return replicator_.Init();
    }

    void Put(RpcController* controller,
             const ::rtidb::api::PutRequest* request,
             ::rtidb::api::PutResponse* response,
             Closure* done) {}

    void Scan(RpcController* controller,
              const ::rtidb::api::ScanRequest* request,
              ::rtidb::api::ScanResponse* response,
              Closure* done) {}

    void CreateTable(RpcController* controller,
            const ::rtidb::api::CreateTableRequest* request,
            ::rtidb::api::CreateTableResponse* response,
            Closure* done) {}

    void DropTable(RpcController* controller,
            const ::rtidb::api::DropTableRequest* request,
            ::rtidb::api::DropTableResponse* response,
            Closure* done) {}

    void AppendEntries(RpcController* controller,
            const ::rtidb::api::AppendEntriesRequest* request,
            ::rtidb::api::AppendEntriesResponse* response,
            Closure* done) {
        bool ok = replicator_.AppendEntries(request, response);
        if (ok) {
            PDLOG(INFO, "receive log entry from leader ok");
            response->set_code(0);
        }else {
            PDLOG(INFO, "receive log entry from leader error");
            response->set_code(1);
        }
        done->Run();
        replicator_.Notify();
    }

    void SetMode(bool follower) {
        follower_.store(follower);
    }

    bool GetMode() {
        return follower_.load(std::memory_order_relaxed);
    }
private:
    ReplicatorRole role_;
    std::string path_;
    std::vector<std::string> endpoints_;
    LogReplicator replicator_;
    std::atomic<bool> follower_;
};

bool ReceiveEntry(const ::rtidb::api::LogEntry& entry) {
    return true;
}

class LogReplicatorTest : public ::testing::Test {

public:
    LogReplicatorTest() {}

    ~LogReplicatorTest() {}
};

inline std::string GenRand() {
    return std::to_string(rand() % 10000000 + 1);
}

TEST_F(LogReplicatorTest,  Init) {
    std::vector<std::string> endpoints;
    std::string folder = "/tmp/" + GenRand() + "/";
    std::map<std::string, uint32_t> mapping;
    std::atomic<bool> follower(false);
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> table = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    table->Init();
    LogReplicator replicator(folder, endpoints, kLeaderNode, table, &follower);
    bool ok = replicator.Init();
    ASSERT_TRUE(ok);
}

TEST_F(LogReplicatorTest,  BenchMark) {
    std::vector<std::string> endpoints;
    std::string folder = "/tmp/" + GenRand() + "/";
    std::map<std::string, uint32_t> mapping;
    std::atomic<bool> follower(false);
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> table = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    table->Init();
    LogReplicator replicator(folder, endpoints, kLeaderNode, table, &follower);
    bool ok = replicator.Init();
    ::rtidb::api::LogEntry entry;
    entry.set_term(1);
    entry.set_pk("test");
    entry.set_value("test");
    entry.set_ts(9527);
    ok = replicator.AppendEntry(entry);
    ASSERT_TRUE(ok);
}

TEST_F(LogReplicatorTest,   LeaderAndFollowerMulti) {
	brpc::ServerOptions options;
	brpc::Server server0;
	brpc::Server server1;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("card", 0));
    mapping.insert(std::make_pair("merchant", 1));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:17527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
		if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
    	}
		if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
    	}
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:17527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    // put the first row
    {
        ::rtidb::api::LogEntry entry;
        ::rtidb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card0");
        d1->set_idx(0);
        ::rtidb::api::Dimension* d2 = entry.add_dimensions();
        d2->set_key("merchant0");
        d2->set_idx(1);
        entry.set_ts(9527);
        entry.set_value("value 1");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    } 
    // the second row
    {
        ::rtidb::api::LogEntry entry;
        ::rtidb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card1");
        d1->set_idx(0);
        ::rtidb::api::Dimension* d2 = entry.add_dimensions();
        d2->set_key("merchant0");
        d2->set_idx(1);
        entry.set_ts(9526);
        entry.set_value("value 2");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    } 
    // the third row
    {
        ::rtidb::api::LogEntry entry;
        ::rtidb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card0");
        d1->set_idx(0);
        entry.set_ts(9525);
        entry.set_value("value 3");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    } 
    leader.Notify();
    std::vector<std::string> vec;
    vec.push_back("127.0.0.1:17528");
    leader.AddReplicateNode(vec);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:17528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
		if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
    	}
		if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
    	}
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    leader.DelAllReplicateNode();
    ASSERT_EQ(3, t8->GetRecordCnt());
    ASSERT_EQ(5, t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator(0, "card0", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value 1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value 3", value_str1);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_FALSE(it->Valid());
    }
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator(1, "merchant0", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value 1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value 2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_FALSE(it->Valid());
    }

}

TEST_F(LogReplicatorTest,  LeaderAndFollower) {
	brpc::ServerOptions options;
	brpc::Server server0;
	brpc::Server server1;
    brpc::Server server2;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:18527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
		if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
    	}
		if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
    	}
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:18527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    ::rtidb::api::LogEntry entry;
    entry.set_pk("test_pk");
    entry.set_value("value1");
    entry.set_ts(9527);
    ok = leader.AppendEntry(entry);
    entry.set_value("value2");
    entry.set_ts(9526);
    ok = leader.AppendEntry(entry);
    entry.set_value("value3");
    entry.set_ts(9525);
    ok = leader.AppendEntry(entry);
    entry.set_value("value4");
    entry.set_ts(9524);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();
    std::vector<std::string> vec;
    vec.push_back("127.0.0.1:18528");
    leader.AddReplicateNode(vec);
    vec.clear();
    vec.push_back("127.0.0.1:18529");
    leader.AddReplicateNode(vec, 2);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:18528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
		if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
    	}
		if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
    	}
        PDLOG(INFO, "start follower");
    }
    std::shared_ptr<MemTable> t9 = std::make_shared<MemTable>("test", 2, 1, 8, mapping, 0);
    t9->Init();
    {
        std::string follower_addr = "127.0.0.1:18529";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, folder, g_endpoints, t9);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        follower->SetMode(true);
        if (server2.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server2.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    int result = server1.Stop(10000);
    ASSERT_EQ(0, result);
    sleep(2);
    entry.Clear();
    entry.set_pk("test_pk");
    entry.set_value("value5");
    entry.set_ts(9523);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();

    sleep(2);
    leader.DelAllReplicateNode();
    ASSERT_EQ(4, t8->GetRecordCnt());
    ASSERT_EQ(4, t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, it->GetKey());
    }
    ASSERT_EQ(4, t9->GetRecordCnt());
    ASSERT_EQ(4, t9->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t9->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, it->GetKey());
    }
}

TEST_F(LogReplicatorTest,  Leader_Remove_local_follower) {
    brpc::ServerOptions options;
    brpc::Server server0;
    brpc::Server server1;
    brpc::Server server2;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:18527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode,
                                                      folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:18527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    ::rtidb::api::LogEntry entry;
    entry.set_pk("test_pk");
    entry.set_value("value1");
    entry.set_ts(9527);
    ok = leader.AppendEntry(entry);
    entry.set_value("value2");
    entry.set_ts(9526);
    ok = leader.AppendEntry(entry);
    entry.set_value("value3");
    entry.set_ts(9525);
    ok = leader.AppendEntry(entry);
    entry.set_value("value4");
    entry.set_ts(9524);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();
    std::vector<std::string> vec;
    vec.push_back("127.0.0.1:18528");
    leader.AddReplicateNode(vec);
    vec.clear();
    vec.push_back("127.0.0.1:18529");
    leader.AddReplicateNode(vec, 2);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>("test", 1, 1, 8, mapping, 0);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:18528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    std::shared_ptr<MemTable> t9 = std::make_shared<MemTable>("test", 2, 1, 8, mapping, 0);
    t9->Init();
    {
        std::string follower_addr = "127.0.0.1:18529";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, folder, g_endpoints, t9);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        follower->SetMode(true);
        if (server2.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server2.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    leader.DelReplicateNode("127.0.0.1:18528");
    int result = server1.Stop(10000);
    ASSERT_EQ(0, result);
    sleep(2);
    entry.Clear();
    entry.set_pk("test_pk");
    entry.set_value("value5");
    entry.set_ts(9523);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();

    sleep(4);
    leader.DelAllReplicateNode();
    ASSERT_EQ(4, t8->GetRecordCnt());
    ASSERT_EQ(4, t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, it->GetKey());
    }
    ASSERT_EQ(5, t9->GetRecordCnt());
    ASSERT_EQ(5, t9->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t9->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::rtidb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, it->GetKey());


        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str4(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9523, it->GetKey());
    }
}

}
}

int main(int argc, char** argv) {
    srand (time(NULL));
    ::baidu::common::SetLogLevel(::baidu::common::INFO);
    ::testing::InitGoogleTest(&argc, argv);
    int ok = RUN_ALL_TESTS();
    return ok; 
}

