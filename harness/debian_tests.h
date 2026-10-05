#pragma once
#include <functional>
#include <string>
bool RunDebianTests(const std::string& root, const std::function<void(const std::string&)>& log);
