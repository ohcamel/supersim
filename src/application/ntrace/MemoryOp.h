/*
 * Copyright 2016 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the 'License');
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef APPLICATION_NTRACE_MEMORYOP_H_
#define APPLICATION_NTRACE_MEMORYOP_H_

#include <prim/prim.h>

#include "application/ntrace/MemoryOp.h"

namespace Ntrace {

class MemoryOp {
 public:
  enum class eOp {
    kReadReq,
    kReadResp,
    kWriteReq,
    kWriteResp,
    kPrefetchReq,
    kPrefetchResp
  };

  MemoryOp(eOp _op, u32 _address);
  MemoryOp(eOp _op, u32 _address, u32 _blockSize);
  ~MemoryOp();

  eOp op() const;
  u32 address() const;
  u32 blockSize() const;

 private:
  eOp op_;
  u32 address_;
  u32 blockSize_;
};

}  // namespace Ntrace

#endif  // APPLICATION_NTRACE_MEMORYOP_H_
