#pragma once
#include <android/asset_manager.h>
#include <functional>
#include <string>
void RunHarnessForService(const std::string& data, AAssetManager* assets, int mode,
                          const std::function<void(const std::string&)>& output);
