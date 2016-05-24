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
#include "application/ntrace/Application.h"

#include <cassert>

#include <vector>

#include "application/ntrace/MemoryTerminal.h"
#include "application/ntrace/ProcessorTerminal.h"
#include "event/Simulator.h"
#include "network/Network.h"
#include "application/NullTerminal.h"

#define ISPOW2INT(X) (((X) != 0) && !((X) & ((X) - 1)))  /*glibc trick*/
#define ISPOW2(X) (ISPOW2INT(X) == 0 ? false : true)

namespace Ntrace {

Application::Application(const std::string& _name, const Component* _parent,
                         MetadataHandler* _metadataHandler,
                         Json::Value _settings)
    : ::Application(_name, _parent, _metadataHandler, _settings) {
  network_ = static_cast<Torus::Network *>(gSim->getNetwork());
  std::vector<u32> dimensionWidths = network_->getDimensionWidths();
  u32 concentration = network_->getConcentration();
  numVcs_ = network_->numVcs();
  assert(numVcs_ > 0);
  bytesPerFlit_ = _settings["bytes_per_flit"].asUInt();
  assert(bytesPerFlit_ > 0);
  headerOverhead_ = _settings["header_overhead"].asUInt();
  maxPacketSize_ = _settings["max_packet_size"].asUInt();

  numSrams_ = _settings["num_srams"].asUInt();
  assert(numSrams_ == dimensionWidths[0]);
  assert(_settings["dim_pe"].isArray());
  rowsPE_ = _settings["dim_pe"][0].asUInt();
  colsPE_ = _settings["dim_pe"][1].asUInt();
  numPEs_ = rowsPE_ * colsPE_;
  assert(dimensionWidths.size() == 2);

  routerCols_ = colsPE_ / dimensionWidths[0];
  routerRows_ = rowsPE_ / (dimensionWidths[1]-1);

  assert(rowsPE_ % (dimensionWidths[1] - 1) == 0);
  assert(colsPE_ %  dimensionWidths[0] == 0);

  assert(numPEs_ == ((dimensionWidths[1] - 1)* dimensionWidths[0]
                      * concentration));

  // Initialize the queue for each processor node
  traceRequests_ = new std::queue<TraceOp> [numPEs_];

  traceFile_ = _settings["trace_file"].asString();

  // check the memory system setup
  memorySlice_ = _settings["memory_slice"].asUInt();
  totalMemory_ = memorySlice_ * numSrams_;
  blockSize_ = _settings["block_size"].asUInt();
  assert(ISPOW2(blockSize_));
  assert((memorySlice_ % blockSize_) == 0);

  // Parse the trace file
  dbgprintf("Trace file: ", traceFile_.c_str());
  parseTraceFile();

  // create terminals
  remainingProcessors_ = 0;
  for (u32 t = 0; t < numTerminals(); t++) {
    std::vector<u32> address;
    network_->translateIdToAddress(t, &address);

    if (t < PeIdBase()) {
      if (t % concentration == 0) {
        std::string tname = "MemoryTerminal_" + std::to_string(t);
        MemoryTerminal* terminal = new MemoryTerminal(
          tname, this, t, address, memorySlice_, this,
          _settings["memory_terminal"]);
        setTerminal(t, terminal);
      } else {
        std::string tname = "NullTerminal_" + std::to_string(t);
        NullTerminal* terminal = new NullTerminal(tname, this, t,
          address, this);
        setTerminal(t, terminal);
      }
    } else {
      std::string tname = "ProcessorTerminal_" +
        std::to_string(remainingProcessors_);
      ProcessorTerminal* terminal = new ProcessorTerminal(
        tname, this, t, address, this, _settings["processor_terminal"]);
      setTerminal(t, terminal);
      remainingProcessors_++;
    }
  }
  assert(remainingProcessors_ == numPEs_);

  // this application always wants monitor
  addEvent(0, 0, nullptr, 0);
}

Application::~Application() {
  delete[] traceRequests_;
}

f64 Application::percentComplete() const {
  f64 percentSum = 0.0;
  u32 processorCount = 0;
  for (u32 idx = PeIdBase(); idx < numTerminals(); idx++) {
    ProcessorTerminal* t =
        reinterpret_cast<ProcessorTerminal*>(getTerminal(idx));
    percentSum += t->percentComplete();
    processorCount++;
  }
  return percentSum / processorCount;
}

void Application::split(const std::string &s, char delim,
             std::vector<std::string> *elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems->push_back(item);
    }
}

std::vector<std::string> Application::split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, &elems);
    return elems;
}

void Application::parseTraceFile() {
  std::ifstream file(traceFile_);
  std::string line;

  assert(file.good());
  while (true) {
    std::getline(file, line);
    if (!file) break;

    auto fields = split(line, ' ');
    assert(fields.size() == 5);

    // Data flows from src to dest
    u32 srcId = traceNameToId(fields[2]);
    u32 destId = traceNameToId(fields[1]);

    u32 initiator;
    TraceOp op;
    op.size = std::stoi(fields[3]);

    if (fields[4] == "R") {
      initiator = destId;
      op.target = srcId;
      op.op = MemoryOp::eOp::kReadReq;
    } else if (fields[4] == "W") {
      initiator = srcId;
      op.target = destId;
      op.op = MemoryOp::eOp::kWriteReq;
    } else {
      assert(false);
    }

    // Initiator is always a PE
    assert(initiator >= PeIdBase());
    traceRequests_[initiator - PeIdBase()].push(op);
  }
}

u32 Application::traceNameToId(std::string name) {
  // Examples
  // 2-3 represents node (2,3)
  // m-0 represents SRAM 0
  // SRAMs start from ID 0
  auto fields = split(name, '-');
  assert(fields.size() == 2);
  u32 row, col;

  if (fields[0] == "m") {
    col = std::stoi(fields[1]);
    assert(col < numSrams_);
    return col * network_->getConcentration();
  } else {
    row = std::stoi(fields[0]);
    col = std::stoi(fields[1]);
    assert(row < rowsPE_);
    assert(col < colsPE_);
    return row * colsPE_ + col + PeIdBase();
  }
}

u32 Application::numVcs() const {
  return numVcs_;
}

u32 Application::numSrams() const {
  return numSrams_;
}

u32 Application::numPEs() const {
  return numPEs_;
}

u32 Application::totalMemory() const {
  return totalMemory_;
}

u32 Application::memorySlice() const {
  return memorySlice_;
}

u32 Application::blockSize() const {
  return blockSize_;
}

u32 Application::bytesPerFlit() const {
  return bytesPerFlit_;
}

u32 Application::headerOverhead() const {
  return headerOverhead_;
}

u32 Application::maxPacketSize() const {
  return maxPacketSize_;
}

void Application::processorComplete(u32 _id) {
  remainingProcessors_--;
  if (remainingProcessors_ == 0) {
    dbgprintf("Processing complete");
    gSim->endMonitoring();
  }
}

void Application::processEvent(void* _event, s32 _type) {
  dbgprintf("Ntrace application starting\n");
  gSim->startMonitoring();
}

std::queue<Application::TraceOp>* Application::getTraceQ(u32 pe) {
  assert(pe < numPEs_);
  return &traceRequests_[pe];
}

const std::queue<Application::TraceOp>* Application::getTraceQ(u32 pe) const {
  assert(pe < numPEs_);
  return &traceRequests_[pe];
}

u32 Application::PeIdBase() const {
  return network_->getConcentration() * numSrams_;
}

}  // namespace Ntrace
