// cacheutil.h
//
// Copyright (c) 2024 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>

#include "protocol.h"

class CacheUtil
{
public:
  static bool IsDefaultReactions(const Reactions& p_Reactions);
  static std::string ReactionsToString(const Reactions& p_Reactions);
  static void UpdateReactions(const Reactions& p_Source, Reactions& p_Target);
  // braids: a poll sent again by the protocol (p_New) stays ended once ended
  static std::string MergePoll(const std::string& p_Old, const std::string& p_New);
};
