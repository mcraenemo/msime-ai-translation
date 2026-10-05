#pragma once

#include <string>
#include <unordered_map>

namespace ApiCredentialTest
{
struct Request
{
    std::string service;
    std::unordered_map<std::string, std::string> config;
};

struct Result
{
    bool ok = false;
    std::string message;
};

Result Run(const Request &request);
} // namespace ApiCredentialTest
