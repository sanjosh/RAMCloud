/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "BackupManager.h"
#include "CoordinatorClient.h"
#include "CoordinatorServer.h"
#include "MasterServer.h"
#include "MockTransport.h"
#include "TransportManager.h"
#include "BindTransport.h"
#include "Recovery.h"
#include "RamCloud.h"

namespace RAMCloud {

class RamCloudTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(RamCloudTest);
    CPPUNIT_TEST(test_multiRead);
    CPPUNIT_TEST_SUITE_END();

    BindTransport* transport;
    CoordinatorServer* coordinatorServer;
    CoordinatorClient* coordinatorClient1;
    CoordinatorClient* coordinatorClient2;
    ServerConfig masterConfig1;
    ServerConfig masterConfig2;
    MasterServer* master1;
    MasterServer* master2;
    RamCloud* ramcloud;

  public:
    RamCloudTest()
        : transport(NULL)
        , coordinatorServer(NULL)
        , coordinatorClient1(NULL)
        , coordinatorClient2(NULL)
        , masterConfig1()
        , masterConfig2()
        , master1(NULL)
        , master2(NULL)
        , ramcloud(NULL)
    {
        masterConfig1.coordinatorLocator = "mock:host=coordinatorServer";
        masterConfig1.localLocator = "mock:host=master1";
        MasterServer::sizeLogAndHashTable("64", "8", &masterConfig1);
        masterConfig2.coordinatorLocator = "mock:host=coordinatorServer";
        masterConfig2.localLocator = "mock:host=master2";
        MasterServer::sizeLogAndHashTable("64", "8", &masterConfig2);
    }

    void setUp() {
        transport = new BindTransport();
        transportManager.registerMock(transport);
        coordinatorServer = new CoordinatorServer();
        transport->addService(*coordinatorServer,
                              "mock:host=coordinatorServer");

        coordinatorClient1 = new CoordinatorClient(
                             "mock:host=coordinatorServer");
        master1 = new MasterServer(masterConfig1, coordinatorClient1, 0);
        transport->addService(*master1, "mock:host=master1");
        master1->serverId.construct(
            coordinatorClient1->enlistServer(MASTER,
                                             masterConfig1.localLocator));

        coordinatorClient2 = new CoordinatorClient(
                             "mock:host=coordinatorServer");
        master2 = new MasterServer(masterConfig2, coordinatorClient2, 0);
        transport->addService(*master2, "mock:host=master2");
        master2->serverId.construct(
            coordinatorClient2->enlistServer(MASTER,
                                             masterConfig2.localLocator));

        ramcloud = new RamCloud("mock:host=coordinatorServer");
        ramcloud->createTable("table1");
        ramcloud->createTable("table2");
        TestLog::enable();
    }

    void tearDown() {
        TestLog::disable();
        delete ramcloud;
        delete master1;
        delete master2;
        delete coordinatorClient1;
        delete coordinatorClient2;
        delete coordinatorServer;
        transportManager.unregisterMock();
        delete transport;
    }

    void test_multiRead() {
        // Create objects to be read later
        uint32_t tableId1 = ramcloud->openTable("table1");
        uint64_t version1;
        ramcloud->create(tableId1, "firstVal", 8, &version1, false);

        uint32_t tableId2 = ramcloud->openTable("table2");
        uint64_t version2;
        ramcloud->create(tableId2, "secondVal", 9, &version2, false);
        uint64_t version3;
        ramcloud->create(tableId2, "thirdVal", 8, &version3, false);

        // Create requests and read
        MasterClient::ReadObject* requests[3];

        Tub<Buffer> readValue1;
        MasterClient::ReadObject request1(tableId1, 0, &readValue1);
        request1.status = STATUS_RETRY;
        requests[0] = &request1;

        Tub<Buffer> readValue2;
        MasterClient::ReadObject request2(tableId2, 0, &readValue2);
        request2.status = STATUS_RETRY;
        requests[1] = &request2;

        Tub<Buffer> readValue3;
        MasterClient::ReadObject request3(tableId2, 1, &readValue3);
        request3.status = STATUS_RETRY;
        requests[2] = &request3;

        ramcloud->multiRead(requests, 3);

        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request1.status));
        CPPUNIT_ASSERT_EQUAL(1, request1.version);
        CPPUNIT_ASSERT_EQUAL("firstVal", toString(readValue1.get()));
        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request2.status));
        CPPUNIT_ASSERT_EQUAL(1, request2.version);
        CPPUNIT_ASSERT_EQUAL("secondVal", toString(readValue2.get()));
        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request3.status));
        CPPUNIT_ASSERT_EQUAL(2, request3.version);
        CPPUNIT_ASSERT_EQUAL("thirdVal", toString(readValue3.get()));
    }

    DISALLOW_COPY_AND_ASSIGN(RamCloudTest);
};
CPPUNIT_TEST_SUITE_REGISTRATION(RamCloudTest);

}  // namespace RAMCloud
