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

#include "uv.h"
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// for linux

static void uv__trans_done(struct uv__work* w, int status) {
  uv_trans_c_t* req;

  req = container_of(w, uv_trans_c_t, work_req);
  uv__req_unregister(req->loop, req);

  if (req->cb != NULL)
    req->cb(req);
}

void uv_trans_taskcomplete(uv_loop_t* loop, uv_trans_c_t *req) {
  struct uv__work* w = &req->work_req;
  uv_mutex_lock(&w->loop->wq_mutex);
  QUEUE_INSERT_TAIL(&w->loop->wq, &w->wq);
  uv_async_send(&w->loop->wq_async);
  uv_mutex_unlock(&w->loop->wq_mutex);
}

void uv_trans_init(uv_loop_t* loop, uv_trans_c_t* req,
  uv_req_type tans_type, const uv_trans_c_cb cb) {
  req->type = tans_type;
  req->loop = loop;
  req->ptr = NULL;
  req->result = 0;
  assert(NULL != cb);
  req->cb = cb;
  (&req->work_req)->loop = loop;
  (&req->work_req)->work = NULL;
  (&req->work_req)->done = uv__trans_done;
}

void uv_trans_req_register(uv_loop_t* loop, uv_trans_c_t* req) {
  uv__req_register(loop, req);
}

void uv_trans_c_req_cleanup(uv_trans_c_t* req) {
  if(req->ptr) {
    free(req->ptr);
    req->ptr = NULL;
  }
}