#include <iostream>
#include <vector>
#include <set>
#include "logger/logger.h"

using namespace std;

int main()
{
    TLSSLOG::Logger::initFile("net", "../logs/net.log", spdlog::level::err);
    int sockfd = -1;
    cout << "hello world!!!!!" << endl;
    TLSSLOG:: Logger::shutdown();
    return 0;
}

