#pragma once
#include <functional>
#include <string>
bool RunCoreTests(const std::function<void(const std::string&)>& log);
