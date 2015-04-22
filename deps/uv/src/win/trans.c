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

#include <assert.h>
#include <malloc.h>
#include "uv.h"
#include "internal.h"
#include "req-inl.h"


void uv_trans_taskcomplete(uv_loop_t* loop, uv_trans_c_t *req){
  POST_COMPLETION_FOR_REQ(loop, req);
}

void uv_trans_init(uv_loop_t* loop, uv_trans_c_t* req,
  uv_req_type tans_type, const uv_trans_c_cb cb){
  uv_req_init(loop, (uv_req_t*)req);
  req->type = tans_type;
  req->loop = loop;
  req->ptr = NULL;
  assert(NULL != cb);
  req->cb = cb;
}

void uv_trans_req_register(uv_loop_t* loop, uv_trans_c_t* req){
  uv__req_register(loop, req);
}

void uv_process_trans_c_req(uv_loop_t* loop, uv_trans_c_t* req){
  assert(req->cb);
  uv__req_unregister(loop, req);
  req->cb(req);
}

void uv_trans_c_req_cleanup(uv_trans_c_t* req){
  if(req->ptr){
    free(req->ptr);
    req->ptr = NULL;
  }
}