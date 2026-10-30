// main.cpp
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (protocol factory, from nchat)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// braidsd - braids core daemon.
//
// Milestone M0 skeleton: verifies that the enabled protocol libraries build and
// load. The daemon itself (chat model, JSON API) is milestone M1/M2, see
// doc/ARCHITECTURE.md.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "apputil.h"
#include "fileutil.h"
#include "protocol.h"

#ifdef HAS_DUMMY
#include "duchat.h"
#endif

#ifdef HAS_TELEGRAM
#include "tgchat.h"
#endif

#ifdef HAS_WHATSAPP
#include "wmchat.h"
#endif

#ifdef HAS_SIGNAL
#include "sgchat.h"
#endif

#ifdef HAS_MATRIX
#include "mxchat.h"
#endif

class ProtocolBaseFactory
{
public:
  virtual ~ProtocolBaseFactory() { }
  virtual std::string GetName() const = 0;
  virtual std::shared_ptr<Protocol> Create() const = 0;
};

template<typename T>
class ProtocolFactory : public ProtocolBaseFactory
{
public:
  std::string GetName() const override
  {
    return T::GetName();
  }

  std::shared_ptr<Protocol> Create() const override
  {
    std::shared_ptr<T> protocol;
#ifdef HAS_DYNAMICLOAD
    const std::string libPath =
      FileUtil::DirName(FileUtil::GetSelfPath()) + "/../" CMAKE_INSTALL_LIBDIR "/" + T::GetLibName() +
      FileUtil::GetLibSuffix();
    void* handle = dlopen(libPath.c_str(), RTLD_LAZY);
    if (handle == nullptr)
    {
      const char* dlerr = dlerror();
      std::cerr << "failed to load " << libPath << ": " << (dlerr ? dlerr : "unknown error") << "\n";
      return protocol;
    }

    // intentionally leak handle, dlclose may crash at exit as go routines are not explicitly managed
    T* (* createFunc)() = (T * (*)())dlsym(handle, T::GetCreateFunc().c_str());
    if (createFunc == nullptr)
    {
      std::cerr << "failed to find " << T::GetCreateFunc() << " in " << libPath << "\n";
      return protocol;
    }

    protocol.reset(createFunc());
#else
    protocol = std::make_shared<T>();
#endif
    return protocol;
  }
};

static std::vector<std::shared_ptr<ProtocolBaseFactory>> GetProtocolFactories()
{
  return std::vector<std::shared_ptr<ProtocolBaseFactory>>
  {
#ifdef HAS_DUMMY
    std::make_shared<ProtocolFactory<DuChat>>(),
#endif
#ifdef HAS_TELEGRAM
    std::make_shared<ProtocolFactory<TgChat>>(),
#endif
#ifdef HAS_WHATSAPP
    std::make_shared<ProtocolFactory<WmChat>>(),
#endif
#ifdef HAS_SIGNAL
    std::make_shared<ProtocolFactory<SgChat>>(),
#endif
#ifdef HAS_MATRIX
    std::make_shared<ProtocolFactory<MxChat>>(),
#endif
  };
}

static void ShowHelp()
{
  std::cout <<
    "braidsd is the core daemon of braids, a multi-protocol messenger.\n"
    "\n"
    "Usage: braidsd [OPTION]\n"
    "\n"
    "    -l, --list-protocols   load and list the enabled protocols\n"
    "    -h, --help             display this help and exit\n"
    "    -v, --version          output version information and exit\n"
    "\n"
    "The daemon and its JSON API are not implemented yet (milestone M1/M2).\n";
}

static int ListProtocols()
{
  int failed = 0;
  for (const auto& factory : GetProtocolFactories())
  {
    std::shared_ptr<Protocol> protocol = factory->Create();
    std::cout << factory->GetName() << ": " << (protocol ? "ok" : "failed to load") << "\n";
    failed += protocol ? 0 : 1;
  }

  return (failed == 0) ? 0 : 1;
}

int main(int argc, char* argv[])
{
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if ((arg == "-h") || (arg == "--help"))
    {
      ShowHelp();
      return 0;
    }
    else if ((arg == "-v") || (arg == "--version"))
    {
      std::cout << "braidsd " << AppUtil::GetAppVersion() << "\n";
      return 0;
    }
    else if ((arg == "-l") || (arg == "--list-protocols"))
    {
      return ListProtocols();
    }
    else
    {
      ShowHelp();
      return 1;
    }
  }

  ShowHelp();
  return 0;
}
