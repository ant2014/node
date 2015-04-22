#include <new>
#include <vector>
#include<memory>
#include <stack>
#include "macros.h"
#include "ref_counted.h"
#include "WrapperObj.h"
#include "WeakPtr.h"
#include "FastDelegateImpl.h"
#ifdef _WIN32
#include "windows.h"
#else
#include <assert.h>
#endif
#include "util_tools.h"
#include "aligned_memory.h"
#include "at_exist.h"
#include "lazy_instance.h"

namespace base{
  void CompleteLazyInstance(intptr_t* state,
    intptr_t new_instance,
    void* lazy_instance,
    void (*dtor)(void*)) {

      *state = new_instance;

      // Make sure that the lazily instantiated object will get destroyed at exit.
      if (dtor)
        AtExitManager::RegisterCallback(dtor, lazy_instance);
  }
} //end base
