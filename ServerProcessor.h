#pragma once
#pragma comment(lib, "ws2_32.lib") // 비주얼에서 소켓프로그래밍 하기 위한 것
#pragma comment(lib,"mswsock.lib") //AcceptEx를 사용하기 위한 것

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9090
#define PACKET_SIZE 1024

#include <jwt-cpp/jwt.h>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <string>
#include <thread>
#include <sw/redis++/redis++.h>

#include "Packet.h"
#include "Define.h"
#include "MySQLManager.h"

class ServerProcessor {
public:
    ~ServerProcessor() {
        if (serverProcThread.joinable()) {
            serverProcThread.join();
        }
        CloseHandle(IOCPHandle);
        closesocket(serverIOSkt);
        WSACleanup();
        std::cout << "Server Proc Thread End" << std::endl;
    }

	bool init(int16_t threadCnt_, std::shared_ptr<sw::redis::RedisCluster> redis_, MySQLManager* mysqlManager_) {
        WSADATA wsadata;
        int check = 0;
        threadCnt = threadCnt_;

        check = WSAStartup(MAKEWORD(2, 2), &wsadata);
        if (check) {
            std::cout << "WSAStartup() Fail" << std::endl;
            return false;
        }

        serverIOSkt = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSA_FLAG_OVERLAPPED);
        if (serverIOSkt == INVALID_SOCKET) {
            std::cout << "Server Socket Make Fail" << std::endl;
            return false;
        }

        IOCPHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, threadCnt);

        if (IOCPHandle == NULL) {
            std::cout << "Iocp Handle Make Fail" << std::endl;
            return false;
        }

        auto bIOCPHandle = CreateIoCompletionPort((HANDLE)serverIOSkt, IOCPHandle, (ULONG_PTR)this, 0);
        if (bIOCPHandle == nullptr) {
            std::cout << "Iocp Handle Bind Fail" << std::endl;
            return false;
        }

        SOCKADDR_IN addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);

        std::cout << "Connect To Game Server" << std::endl;

        connect(serverIOSkt, (SOCKADDR*)&addr, sizeof(addr));

        std::cout << "Connect Game Server Success" << std::endl;

        redis = redis_;

        IM_WEB_REQUEST iwReq;
        iwReq.PacketId = (UINT16)PACKET_ID::IM_WEB_REQUEST;
        iwReq.PacketLength = sizeof(IM_WEB_REQUEST);

        webToken = jwt::create()
            .set_issuer("web_server")
            .set_subject("ServerConnect")
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{ 3600 }) // 한시간으로 세팅
            .sign(jwt::algorithm::hs256{ JWT_SECRET });

        std::string tag = "{webserver}";
        std::string key = "jwtcheck:" + tag;

        auto pipe = redis->pipeline("{webserver}");

        pipe.hset(key, webToken, std::to_string(4))
            .expire(key, 15); // set ttl 1 hour

        pipe.exec();

        auto value = redis->hget(key, webToken);

		strncpy_s(iwReq.webToken, webToken.c_str(), 256);

        send(serverIOSkt, (char*)&iwReq, sizeof(iwReq), 0);
        recv(serverIOSkt, recvBuf, PACKET_SIZE, 0);
    
        std::cout << "Success to Check Token in Server" << std::endl;

        auto iwRes = reinterpret_cast<IM_WEB_RESPONSE*>(recvBuf);

        if (!iwRes->isSuccess) {
            std::cout << "Fail to Check Token in Server" << std::endl;

            return false;
        }
        tempOvLap = new OverlappedTCP;

        CreateServerProcThread(threadCnt_);

        ConnUserRecv();

        mysqlManager = mysqlManager_;

        mysqlManager->SetRankingInRedis();

        std::cout << "Game Server Connect" << std::endl;

        return true;
	}

    bool ConnUserRecv() {
        DWORD dwFlag = 0;
        DWORD dwRecvBytes = 0;

        ZeroMemory(tempOvLap, sizeof(OverlappedTCP));
		memset(recvBuf, 0, PACKET_SIZE);   

        tempOvLap->wsaBuf.len = MAX_SOCK;
        tempOvLap->wsaBuf.buf = recvBuf;
        tempOvLap->a = 1;
        tempOvLap->taskType = TaskType::RECV;
        tempOvLap->serverProcessor = this;

        int tempR = WSARecv(serverIOSkt, &(tempOvLap->wsaBuf), 1, &dwRecvBytes, &dwFlag, (LPWSAOVERLAPPED)(tempOvLap), NULL);

        if (tempR == SOCKET_ERROR && (WSAGetLastError() != ERROR_IO_PENDING))
        {
            std::cout << serverIOSkt << " WSARecv() Fail : " << WSAGetLastError() << std::endl;
            return false;
        }

        return true;
    }

    void CreateServerProcThread(uint16_t threadCnt_) {
        serverProcRun = true;
        serverProcThread = std::thread([this]() {WorkRun();});
    }

    void WorkRun() {
        LPOVERLAPPED lpOverlapped = NULL;
        DWORD dwIoSize = 0;
        bool gqSucces = TRUE;
        ULONG_PTR compKey = 0;

        while (serverProcRun) {
            gqSucces = GetQueuedCompletionStatus(
                IOCPHandle,
                &dwIoSize,
                &compKey,
                &lpOverlapped,
                INFINITE
            );

            auto overlappedTCP = (OverlappedTCP*)lpOverlapped;

            std::cout << "싱크로 요청 들어옴" << std::endl;

            if (overlappedTCP->a == 1) { // Recv Data From Server Only

                auto k = reinterpret_cast<PACKET_HEADER*>(overlappedTCP->wsaBuf.buf);

                if (k->PacketId == (uint16_t)PACKET_ID::SYNCRONIZE_LEVEL_REQUEST) {
                    std::cout << "Get Sync UserLevel" << std::endl;
                    auto pakcet = reinterpret_cast<SYNCRONIZE_LEVEL_REQUEST*>(overlappedTCP->wsaBuf.buf);
                    
                    if (mysqlManager->SyncLevel(pakcet->userPk, pakcet->level, pakcet->currentExp)) { 
                        std::cout << "Sync UserLevel Success" << std::endl;
                    }
                    else {
                        std::cout << "Sync UserLevel Success" << std::endl;
                    }

                }
                else if (k->PacketId == (uint16_t)PACKET_ID::SYNCRONIZE_LOGOUT_REQUEST) {
                    std::cout << "Get Sync UserInfo" << std::endl;
                    auto pakcet = reinterpret_cast<SYNCRONIZE_LOGOUT_REQUEST*>(overlappedTCP->wsaBuf.buf);

                    if (mysqlManager->SyncUserInfo(pakcet->userPk)) {
                        std::cout << "Sync UserInfo Success" << std::endl;
                    }
                    else {
                        std::cout << "Sync UserInfo Success" << std::endl;
                    }

                }

               ConnUserRecv();
            }

        }
    }

private:
    bool serverProcRun = false;

    uint16_t threadCnt = 0;

    SOCKET serverIOSkt;
    HANDLE IOCPHandle;
    MySQLManager* mysqlManager;

    OverlappedTCP* tempOvLap;

    std::thread serverProcThread; 

    std::shared_ptr<sw::redis::RedisCluster> redis;

    std::string webToken;

    char recvBuf[PACKET_SIZE];
};