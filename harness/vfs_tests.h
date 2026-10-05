#pragma once
#include <functional>
#include <string>
bool RunVfsTests(const std::string& root, const std::function<void(const std::string&)>& log);
