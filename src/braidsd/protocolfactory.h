// protocolfactory.h
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (protocol factory, from nchat)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

class ProtocolBaseFactory
{
public:
  virtual ~ProtocolBaseFactory() { }
  virtual std::string GetName() const = 0;
  virtual std::shared_ptr<Protocol> Create() const = 0;
};

class ProtocolFactories
{
public:
  // All protocols enabled at build time.
  static std::vector<std::shared_ptr<ProtocolBaseFactory>> GetAll();

  // Factory by protocol name (e.g. "Matrix"), or null.
  static std::shared_ptr<ProtocolBaseFactory> Get(const std::string& p_Name);
};
