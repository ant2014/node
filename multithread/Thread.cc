// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <assert.h>
#include <sys/syscall.h>
#endif
#include <string>
#include <memory>
#include <list>
#include <queue>
#include "macros.h"
#include "ref_counted.h"
#include "WrapperObj.h"
#include "WeakPtr.h"
#include "FastDelegateImpl.h"
#include "time/time.h"
#include "MessagePump.h"
#include "util_tools.h"
#include "Event/WaitableEvent.h"
#include "PendingTask.h"
#include "observer_list.h"
#include "MessagePumpDefault.h"
#include "MessageLoop.h"
#include "v8.h"
#include "Thread.h"

namespace base{
#ifdef _WIN32
  Thread::ScopedCOMInitializer::~ScopedCOMInitializer(){
    assert(GetCurrentThreadId() == thread_id_);
    CoUninitialize();
  }

  void Thread::ScopedCOMInitializer::Initialize(COINIT init){
    thread_id_ = GetCurrentThreadId();
    CoInitializeEx(NULL, init);
  }

  DWORD __stdcall ThreadFunc(void* params);
#else
  void * ThreadFunc(void* params);
#endif
  Thread::Options::Options()
    :message_loop_type_(MessageLoop::TYPE_DEFAULT), stack_size_(0){
  }
  Thread::Options::Options(MessageLoop::Type type, size_t size)
    :message_loop_type_(type), stack_size_(size){

  }

  Thread::Thread():
#ifdef _WIN32
  com_status_(NONE),
#endif
    started_(false),
    stopping_(false),
    running_(false),
    thread_(NULL),
    message_loop_(NULL),
    thread_id_(kInvalidThreadId),
    startup_data_(NULL),
    isolate_(NULL){
  }

  Thread::Thread(const char* name):
#ifdef _WIN32
  com_status_(NONE),
#endif
    started_(false),
    stopping_(false),
    running_(false),
    thread_(NULL),
    message_loop_(NULL),
    name_(name),
    thread_id_(kInvalidThreadId),
    startup_data_(NULL),
    isolate_(NULL){
  }

  Thread::~Thread(){
    Stop();
  }

  bool Thread::StartWithOptions(const Options& options){
    assert(!message_loop_);
    StartupData startup_data(options);
    startup_data_ = &startup_data;
    if(!CreateThreadInternal(options.stack_size_, this, &thread_)){
      startup_data_ = NULL;
      return false;
    }
    startup_data.event_.Wait();
    startup_data_ = NULL;
    started_ = true;
    assert(message_loop_);
    return true;
  }

  //static
  bool Thread::CreateThreadInternal(size_t stack_size, Thread *thread, PlatformThreadHandle *out_thread_handle){
#ifdef _WIN32
    unsigned int flags = 0;
    if(stack_size > 0)
      flags = STACK_SIZE_PARAM_IS_A_RESERVATION;

    Thread::ThreadParams * params = new Thread::ThreadParams;
    params->thread_ = thread;

    PlatformThreadHandle thread_handle = ::CreateThread(NULL, stack_size, ThreadFunc, params, flags, NULL);
    if(NULL == thread_handle){
      delete params;
      return false;
    }
    if(out_thread_handle){
      *out_thread_handle = thread_handle;
    }else{
      CloseHandle(thread_handle);
    }
    return true;
#else
    bool success = false;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stack_size > 0)
      pthread_attr_setstacksize(&attributes, stack_size);

    ThreadParams* params = new ThreadParams;
    params->thread_ = thread;

    int err = pthread_create(out_thread_handle, &attributes, ThreadFunc, params);
    success = !err;
    if (!success) {
      errno = err;
    }
    pthread_attr_destroy(&attributes);
    if (!success)
      delete params;
    return success;
#endif
  }

  void Thread::ThreadMain(){
    std::auto_ptr<MessageLoop> message_loop;
    message_loop.reset(new MessageLoop(startup_data_->options_.message_loop_type_));
    assert(message_loop.get());
#ifdef _WIN32
    thread_id_ = GetCurrentThreadId();
#else
    thread_id_ = syscall(__NR_gettid);
#endif
    message_loop->set_thread_name(name_);
    message_loop_ = message_loop.get();

#ifdef _WIN32
    std::auto_ptr<ScopedCOMInitializer> com_initializer;
    if(com_status_ != NONE){
      com_initializer.reset((com_status_ == STA) ? 
        new ScopedCOMInitializer() :
      new ScopedCOMInitializer(MTA));
    }

    char szPath[MAX_PATH + 1] = {0};
    GetModuleFileNameA(NULL, szPath, MAX_PATH);
    PathRemoveFileSpecA(szPath);
    std::string strPath = szPath;

    strPath += "\\background\\background.js";

#else

    int pathmax = pathconf("/", _PC_PATH_MAX);
    if(pathmax < 0){
      printf("pathconf error\n");
      return;
    }
    pathmax++;
    char *szbuf = new char[pathmax];
    if(getcwd(szbuf, pathmax) == NULL) {
      printf("getcwd error");
      return;
    }
    std::string strPath = szbuf;
    delete []szbuf;
    strPath += "/background/background.js";
    printf("background.js path is: %s\n", strPath.c_str());

#endif
    std::string strfile = tk::getFileContents(strPath);
    if(0 == strfile.length()){
      printf("Error: cannot find background.js\n");
      return ;
    }
    //init v8
    v8::Isolate *isolate = v8::Isolate::New();
    v8::Locker locker(isolate);
    isolate->Enter();

    running_ = true;
    startup_data_->event_.Signal();
    do {
      v8::HandleScope handle_scope(isolate);
      // Create a new context.
      v8::Handle<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::TryCatch catcher;
      v8::Handle<v8::Script> script = v8::Script::Compile(v8::String::NewFromUtf8(isolate, strfile.c_str()));
      if (catcher.HasCaught())  {
        printf("background.js Compile error\n");
        return ;
      }
      //executive js 
      v8::Handle<v8::Value> result = script->Run();
      if (catcher.HasCaught())  {
        printf("background.js Run error\n");
        return ;
      }
      //clear useless 
      strPath.clear();
      strfile.clear();
      isolate_ = isolate;

      Run(message_loop_);
    } while (false);

    running_ = false;

    isolate->Exit();
    isolate->Dispose();
    isolate_ = NULL;
    // Let the thread do extra cleanup.
    CleanUp();
#ifdef _WIN32
    com_initializer.reset();
#endif
    message_loop_ = NULL;

  }

  void Thread::Run(MessageLoop *message_loop) {
    message_loop->Run();
  }

  bool Thread::IsRunning() const {
    return running_;
  }

  void Thread::SetPriority(ThreadPriority priority) {
#ifdef _WIN32
    switch (priority) {
    case kThreadPriority_Normal:
      ::SetThreadPriority(thread_, THREAD_PRIORITY_NORMAL);
      break;
    case kThreadPriority_RealtimeAudio:
      ::SetThreadPriority(thread_, THREAD_PRIORITY_TIME_CRITICAL);
      break;
    default:
      break;
    }
#else
#endif
  }

  void Thread::Stop() {
    if(!started_)
      return;

    if(stopping_ || !message_loop_)
      return;

    stopping_ = true;

    message_loop_->Quit();

    PlatformThreadHandle thread_handle = thread_;
#ifdef _WIN32
    DWORD result = WaitForSingleObject(thread_handle, INFINITE);
    if(result != WAIT_OBJECT_0){
      assert(0);
    }
    CloseHandle(thread_handle);
#else
    pthread_join(thread_handle, NULL);
#endif
    assert(!message_loop_);
    started_ = false;
    stopping_ = false;
  }

  bool Thread::set_thread_name(std::string name) {
    if(name_.empty()){
      name_ = name;
      return true;
    }
    return false;
  }
}//end base

namespace base{
#ifdef _WIN32
  DWORD __stdcall ThreadFunc(void* params) {
    Thread::ThreadParams *thread_params = static_cast<Thread::ThreadParams*>(params);
    base::Thread *thread = thread_params->thread_;
    delete thread_params;

    thread->ThreadMain();

    return NULL;
  }
#else
  void* ThreadFunc(void* params) {
    Thread::ThreadParams *thread_params = static_cast<Thread::ThreadParams*>(params);
    base::Thread *thread = thread_params->thread_;
    delete thread_params;

    thread->ThreadMain();

    return NULL;
  }
#endif
} //end base