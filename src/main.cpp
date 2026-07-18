#include <iostream>
#include <vector>
#include <set>
#include "common/net.h"
#include "logger/logger.h"

using namespace std;

int main()
{
    TLSSLOG::Logger::initFile("net", "../logs/net.log", spdlog::level::err);
    int sockfd = -1;
    TLSSNET::tcp_init("127.0.0.1",12345,sockfd);
    cout << "hello world!!!!!" << endl;
    TLSSLOG:: Logger::shutdown();
    return 0;
}

