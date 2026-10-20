#pragma once

#include <string>

namespace japanese_dictionary
{
struct BuildStatus
{
    std::string state; // "absent" | "building" | "ready" | "error"
    std::string message;
};

BuildStatus GetStatus();
void StartBuild();
} // namespace japanese_dictionary
