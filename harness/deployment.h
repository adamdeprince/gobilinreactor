#pragma once
#include "session.h"
#include <android/asset_manager.h>
bool DeployGuestFeatures(const std::string& root, AAssetManager* assets,
                         const goblin::RunLimits& limits, goblin::SessionControl* control, std::string* error);
