#pragma once
#include <android/asset_manager.h>
#include <functional>
#include <string>
bool RunDeveloperTests(const std::string& data, AAssetManager* assets,
                       const std::function<void(const std::string&)>& log, bool emacs_only = false);
