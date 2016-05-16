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
#include "application/ntrace/MemoryOp.h"

namespace Ntrace {

MemoryOp::MemoryOp(MemoryOp::eOp _op, u32 _address)
    : MemoryOp(_op, _address, 0) {}

MemoryOp::MemoryOp(MemoryOp::eOp _op, u32 _address, u32 _blockSize)
    : op_(_op), address_(_address), blockSize_(_blockSize) {
}

MemoryOp::~MemoryOp() {
}

MemoryOp::eOp MemoryOp::op() const {
  return op_;
}

u32 MemoryOp::address() const {
  return address_;
}

u32 MemoryOp::blockSize() const {
  return blockSize_;
}

}  // namespace Ntrace
