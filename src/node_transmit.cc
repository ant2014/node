/* Copyright classfellow@qq.com  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "node.h"
#include <malloc.h>
#include <stdlib.h>
#include <string>
#include <memory>
#include <list>
#include <queue>
#include <stack>
#include "../multithread/macros.h"
#include "../multithread/aligned_memory.h"
#include "../multithread/lazy_instance.h"
#include "../multithread/ref_counted.h"
#include "../multithread/WrapperObj.h"
#include "../multithread/WeakPtr.h"
#include "../multithread/FastDelegateImpl.h"
#include "../multithread/time/time.h"
#include "../multithread/MessagePump.h"
#include "../multithread/util_tools.h"
#include "../multithread/Event/WaitableEvent.h"
#include "../multithread/PendingTask.h"
#include "../multithread/observer_list.h"
#include "../multithread/MessagePumpDefault.h"
#include "../multithread/MessageLoop.h"
#include "../multithread/Thread.h"
#include "../multithread/at_exist.h"
#include "node_transmit.h"

#include "uv.h"
#include "env.h"
#include "env-inl.h"
#include "req_wrap.h"

#define SET_REQ_RESULT(req, result_value)                              \
  do{                                                                  \
       req->result = (result_value);                                   \
    }while(0);

#define TYPE_ERROR(msg) env->ThrowTypeError(msg)

base::LazyInstance<base::Thread> local_thread_ = LAZY_INSTANCE_INITIALIZER;

class PostTaskHelper{
public:
  explicit PostTaskHelper();
  static PostTaskHelper& GetInstance();
  void Exejs(INT64 taskid, std::string fun, std::string param, uv_trans_c_t *req);
  INT64 &taskid();

private:
  INT64 taskid_;
};

PostTaskHelper::PostTaskHelper()
  :taskid_(0) {
}

INT64 &PostTaskHelper::taskid() {
  ++taskid_;
  return taskid_;
}

PostTaskHelper& PostTaskHelper::GetInstance() {
  static PostTaskHelper This;
  return This;
}

void PostTaskHelper::Exejs(INT64 taskid, std::string fun, std::string param, uv_trans_c_t *req) {
  v8::Isolate *isolate = local_thread_.Get().isolate();
  assert(isolate);
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Object> globalObj = isolate->GetCurrentContext()->Global();
  v8::Handle<v8::Value> _globalFunction = globalObj->Get(v8::String::NewFromUtf8(isolate, fun.c_str()));
  if (_globalFunction.IsEmpty() || !_globalFunction->IsFunction()) {
    SET_REQ_RESULT(req, -1);
  }
  else {	
    v8::Handle<v8::Function> func = v8::Handle<v8::Function>::Cast(_globalFunction);
    v8::Local<v8::Value> v1 = v8::String::NewFromUtf8(isolate, param.c_str());
    v8::Handle<v8::Value> args[1] = { v1 };
    v8::Handle<v8::Value> tmpCallVal = func->Call(globalObj, 1, args);
    if (tmpCallVal->IsString()) {
      v8::Local<v8::String> V8String= tmpCallVal->ToString();
      int len = 1 + V8String->Utf8Length();
      char *p = (char *)malloc(len);
      V8String->WriteUtf8(p);
      //set result
      req->ptr = p;
      SET_REQ_RESULT(req, 1);
    }
    else {
      SET_REQ_RESULT(req, -1);
    }
  }
  uv_trans_taskcomplete(req->loop, req);
}

namespace node{
  using namespace v8;
  using namespace fastdelegate;

  class TransReqWrap: public ReqWrap<uv_trans_c_t>{
  public:
    void* operator new(size_t size) { return new char[size]; }
    void* operator new(size_t size, char* storage) { return storage; }
    TransReqWrap(Environment* env)
      : ReqWrap<uv_trans_c_t>(env, Object::New(env->isolate()), AsyncWrap::PROVIDER_REQWRAP){
    }
    //virtual funs has vptr,  assert(&req_wrap->req_ == req) will false;
  private:
  };

  static void After(uv_trans_c_t *req) {
    TransReqWrap* req_wrap = static_cast<TransReqWrap*>(req->data);
    assert(&req_wrap->req_ == req);

    Environment* env = req_wrap->env();
    HandleScope handle_scope(env->isolate());
    Context::Scope context_scope(env->context());

    // there is always at least one argument. "error"
    int argc = 1;

    // Allocate space for two args. We may only use one depending on the case.
    // (Feel free to increase this if you need more)
    Local<Value> argv[2];
    // NOTE: This may be needed to be changed if something returns a -1
    // for a success, which is possible.
    if (-1 == req->result || !req->ptr) {
      argv[0] = UVException(-1,
        NULL,
        NULL);
    }
    else {
      // error value is empty or null for non-error.
      argv[0] = Null(env->isolate());
      // the data to pass is a string , use json
      argc = 2;
      char *data = (char *)req->ptr;
      std::string strdata(data);
      argv[1] = String::NewFromUtf8(env->isolate(), data);
    }

    req_wrap->MakeCallback(env->oncomplete_string(), argc, argv);

    uv_trans_c_req_cleanup(&req_wrap->req_);
    delete req_wrap;
  }

  int uv_trans(uv_loop_t* loop, uv_trans_c_t* req, uv_trans_c_cb cb) {
    uv_trans_init(loop, req, UV_TRANS_C, cb);
    return S_OK;
  }

  // function name  , param,  callback
  static void JsTask(const FunctionCallbackInfo<Value>& args) { 
    if(!local_thread_.Get().IsRunning()){
      //Create  working thread, focus on cup intensive task
      local_thread_.Get().set_thread_name("js_task_thread");
      local_thread_.Get().StartWithOptions(base::Thread::Options());
    }

    Environment* env = Environment::GetCurrent(args.GetIsolate());
    HandleScope scope(env->isolate());

    if(args.Length() != 3 || !args[0]->IsString() || !args[1]->IsString() || !args[2]->IsFunction()) {
      return TYPE_ERROR("Bad Param"); 
    }

    INT64 taskid = PostTaskHelper::GetInstance().taskid();

    TransReqWrap *req_wrap;
    char* storage = new char[sizeof(*req_wrap)];
    req_wrap = new(storage) TransReqWrap(env);
    uv_trans_c_t* req = &req_wrap->req_;
    int r = uv_trans(env->event_loop(),
                      req,
                      After);
    req_wrap->object()->Set(env->oncomplete_string(), args[2]);
    req_wrap->Dispatched(); 
    if(r < 0) {
      req->result = -1;
      After(req);
      return TYPE_ERROR("r < 0");
    }
    String::Utf8Value fun(args[0]);
    std::string strfun = (*fun)?*fun:"(null)";
    String::Utf8Value param(args[1]);
    std::string strparam = (*param)? *param : "(null)";

    local_thread_.Get().message_loop()->PostTask(base::Bind(base::Unretained(&PostTaskHelper::GetInstance()),
      &PostTaskHelper::Exejs, taskid, strfun, strparam, req));

    uv_trans_req_register(env->event_loop(), req);

    return args.GetReturnValue().Set(req_wrap->persistent());
  }

  void Init_Post_Task(Handle<Object> target,
    Handle<Value> unused,
    Handle<Context> context,
    void* priv) {

    Environment* env = Environment::GetCurrent(context);
	// post a task that run a cpu-intensive function defined in backgroundjs
    NODE_SET_METHOD(target, "jstask", JsTask);
  }
} // end namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(pt_c, node::Init_Post_Task)
