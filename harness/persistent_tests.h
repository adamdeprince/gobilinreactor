#pragma once
#include <functional>
#include <string>
#include <android/asset_manager.h>
bool RunPersistentTests(const std::string& data, AAssetManager* assets, const std::function<void(const std::string&)>& log);
