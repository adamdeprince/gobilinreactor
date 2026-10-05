#pragma once
#include <functional>
#include <string>
#include <android/asset_manager.h>
bool RunSessionTests(const std::string& root, AAssetManager* assets, const std::function<void(const std::string&)>& log);
