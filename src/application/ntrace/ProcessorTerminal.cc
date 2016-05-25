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
#include "application/ntrace/ProcessorTerminal.h"

#include <cassert>

#include "application/ntrace/Application.h"
#include "application/ntrace/MemoryOp.h"
#include "event/Simulator.h"
#include "types/Message.h"
#include "types/Packet.h"
#include "types/Flit.h"

namespace Ntrace {

ProcessorTerminal::ProcessorTerminal(
    const std::string& _name, const Component* _parent, u32 _id, u32 _tid,
    const std::vector<u32>& _address, ::Application* _app,
    Json::Value _settings)
    : ::Terminal(_name, _parent, _id, _address, _app) {
  latency_ = _settings["latency"].asUInt();
  assert(latency_ > 0);
  const Application* app = dynamic_cast<const Application*>(_parent);
  assert(app);
  tid_ = _tid;
  remainingAccesses_ = app->getTraceQ(_tid - app->numSrams())->size();
  numMemoryAccesses_ = remainingAccesses_;
  fsm_ = pState::kDone;
  mfsm_ = mState::kWaiting;
  if (remainingAccesses_ > 0) {
    startProcessing();
  }
  curTimestamp_ = 0;
  waitingResps_ = 0;
}

ProcessorTerminal::~ProcessorTerminal() {
}

void ProcessorTerminal::processEvent(void* _event, s32 _type) {
  switch (_type) {
  case static_cast<s32>(pEvents::kSendReq):
    startNextMemoryAccess();
    break;
  case static_cast<s32>(pEvents::kSendResp):
    sendMemoryResponse();
    break;
  }
}

void ProcessorTerminal::handleMessage(Message* _message) {
  dbgprintf("received message");

  // log the message
  Application* app = reinterpret_cast<Application*>(gSim->getApplication());
  app->getMessageLog()->logMessage(_message);

  // verify the message
  MemoryOp* memOp = reinterpret_cast<MemoryOp*>(_message->getData());
  assert(memOp != nullptr);

  if (memOp->op() == MemoryOp::eOp::kReadResp ||
    memOp->op() == MemoryOp::eOp::kWriteResp) {
    // Received response
    // end the transaction
    endTransaction(_message->getTransaction());
    waitingResps_--;

    delete memOp;
    delete _message;

    if (waitingResps_ == 0) {
      startProcessing();
    }
  } else {
    // Received request
    if ((memOp->op() == MemoryOp::eOp::kReadReq) ||
      (memOp->op() == MemoryOp::eOp::kWriteReq)) {
      messages_.push(_message);
    } else {
      assert(false);
    }

    if (mfsm_ == mState::kWaiting) {
      startMemoryAccess();
    }
  }
}

void ProcessorTerminal::messageEnteredInterface(Message* _message) {
}

void ProcessorTerminal::messageExitedNetwork(Message* _message) {
  // any override of this function must call the base class's function
  ::Terminal::messageExitedNetwork(_message);
}

f64 ProcessorTerminal::percentComplete() const {
  f64 pc = 1.0 - ((f64)remainingAccesses_ / (f64)numMemoryAccesses_);
  dbgprintf("Percent complete: %f", pc);
  return pc;
}

void ProcessorTerminal::startMemoryAccess() {
  dbgprintf("starting memory access");
  addEvent(gSim->futureCycle(latency_), 0, nullptr,
          static_cast<s32>(pEvents::kSendResp));
  mfsm_ = mState::kAccessing;
}

void ProcessorTerminal::startProcessing() {
  dbgprintf("starting processing");
  addEvent(gSim->futureCycle(latency_), 0, nullptr,
          static_cast<s32>(pEvents::kSendReq));
  fsm_ = pState::kProcessing;
}

void ProcessorTerminal::startNextMemoryAccess() {
  if (remainingAccesses_ == 0) {
    Application* app = reinterpret_cast<Application*>(gSim->getApplication());
    app->processorComplete(getId());
    return;
  }
  dbgprintf("remaining accesses = %u", remainingAccesses_);

  Application* app = reinterpret_cast<Application*>(gSim->getApplication());
  u32 blockSize = app->blockSize();
  u32 bytesPerFlit = app->bytesPerFlit();
  u32 headerOverhead = app->headerOverhead();
  u32 maxPacketSize = app->maxPacketSize();

  // generate a memory request
  auto op_queue = app->getTraceQ(tid_ - app->numSrams());
  Application::TraceOp op = op_queue->front();
  op_queue->pop_front();
  remainingAccesses_--;

  assert(curTimestamp_ <= op.ts);
  curTimestamp_ = op.ts;

  MemoryOp* memOp = new MemoryOp(op.op, 0, (op.size + 7)/8);
  if (op.op == MemoryOp::eOp::kWriteReq) {
    fsm_ = pState::kWaitingForWriteResp;
  } else {
    fsm_ = pState::kWaitingForReadResp;
  }

  // determine the proper memory terminal
  u64 memoryTerminalId = app->tid2nid(op.target);

  // determine message length
  u32 messageLength = headerOverhead + 1 + sizeof(u32) + blockSize;
  messageLength /= bytesPerFlit;
  u32 numPackets = messageLength / maxPacketSize;
  if ((messageLength % maxPacketSize) > 0) {
    numPackets++;
  }

  // create network message, packets, and flits
  Message* message = new Message(numPackets, memOp);
  message->setTransaction(createTransaction());

  u32 flitsLeft = messageLength;
  for (u32 p = 0; p < numPackets; p++) {
    u32 packetLength = flitsLeft > maxPacketSize ? maxPacketSize : flitsLeft;
    Packet* packet = new Packet(p, packetLength, message);
    message->setPacket(p, packet);

    for (u32 f = 0; f < packetLength; f++) {
      bool headFlit = f == 0;
      bool tailFlit = f == (packetLength - 1);
      Flit* flit = new Flit(f, headFlit, tailFlit, packet);
      packet->setFlit(f, flit);
    }
    flitsLeft -= packetLength;
  }

  // send the request to the memory terminal
  dbgprintf("sending %s request to %u",
            (op.op == MemoryOp::eOp::kWriteReq) ?
            "write" : "read", memoryTerminalId);
  waitingResps_++;
  sendMessage(message, memoryTerminalId);
}

void ProcessorTerminal::sendMemoryResponse() {
  // get the next request
  Message* request = messages_.front();
  messages_.pop();
  MemoryOp* memOpReq = reinterpret_cast<MemoryOp*>(request->getData());
  assert(memOpReq != nullptr);
  MemoryOp::eOp reqOp = memOpReq->op();

  Application* app = reinterpret_cast<Application*>(gSim->getApplication());
  u32 blockSize = app->blockSize();
  u32 bytesPerFlit = app->bytesPerFlit();
  u32 headerOverhead = app->headerOverhead();
  u32 maxPacketSize = app->maxPacketSize();

  // get a pointer into the memory array
  u32 address = memOpReq->address();
  address &= ~(blockSize - 1);  // align to blockSize

  // create the response
  MemoryOp::eOp respOp = reqOp == MemoryOp::eOp::kReadReq ?
    MemoryOp::eOp::kReadResp : MemoryOp::eOp::kWriteResp;
  MemoryOp* memOpResp = new MemoryOp(respOp, address,
    (reqOp == MemoryOp::eOp::kReadReq ?
      blockSize : 0));

  // determine the message length
  //  perform memory operation
  u32 messageLength = headerOverhead + 1 + sizeof(u32);
  if (reqOp == MemoryOp::eOp::kReadReq) {
    messageLength += blockSize;
  }
  messageLength /= bytesPerFlit;
  u32 numPackets = messageLength / maxPacketSize;
  if ((messageLength % maxPacketSize) > 0) {
    numPackets++;
  }

  // create the outgoing message, packets, and flits
  Message* response = new Message(numPackets, memOpResp);
  response->setTransaction(request->getTransaction());

  u32 flitsLeft = messageLength;
  for (u32 p = 0; p < numPackets; p++) {
    u32 packetLength = flitsLeft > maxPacketSize ? maxPacketSize : flitsLeft;
    Packet* packet = new Packet(p, packetLength, response);
    response->setPacket(p, packet);

    for (u32 f = 0; f < packetLength; f++) {
      bool headFlit = f == 0;
      bool tailFlit = f == (packetLength - 1);
      Flit* flit = new Flit(f, headFlit, tailFlit, packet);
      packet->setFlit(f, flit);
    }
    flitsLeft -= packetLength;
  }

  // send the response to the requester
  u32 requesterId = request->getSourceId();
  dbgprintf("sending %s response to %u (address %u)",
    (respOp == MemoryOp::eOp::kWriteResp) ?
    "write" : "read", requesterId, address);
  sendMessage(response, requesterId);

  // delete the request
  delete memOpReq;
  delete request;

  // if more memory requests are outstanding, continue accessing memory
  if (messages_.size() > 0) {
    startMemoryAccess();
  } else {
    mfsm_ = mState::kWaiting;
  }
}

}  // namespace Ntrace
