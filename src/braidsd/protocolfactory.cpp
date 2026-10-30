// protocolfactory.cpp
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (protocol factory, from nchat)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "protocolfactory.h"

#include <dlfcn.h>

#include "fileutil.h"
#include "log.h"

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
      LOG_ERROR("failed to load %s: %s", libPath.c_str(), (dlerr ? dlerr : "unknown error"));
      return protocol;
    }

    // intentionally leak handle, dlclose may crash at exit as go routines are not explicitly managed
    T* (* createFunc)() = (T * (*)())dlsym(handle, T::GetCreateFunc().c_str());
    if (createFunc == nullptr)
    {
      LOG_ERROR("failed to find %s in %s", T::GetCreateFunc().c_str(), libPath.c_str());
      return protocol;
    }

    protocol.reset(createFunc());
#else
    protocol = std::make_shared<T>();
#endif
    return protocol;
  }
};

std::vector<std::shared_ptr<ProtocolBaseFactory>> ProtocolFactories::GetAll()
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

std::shared_ptr<ProtocolBaseFactory> ProtocolFactories::Get(const std::string& p_Name)
{
  for (const auto& factory : GetAll())
  {
    if (factory->GetName() == p_Name) return factory;
  }

  return nullptr;
}
