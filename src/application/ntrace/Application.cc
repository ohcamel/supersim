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
  auto network = gSim->getNetwork();
  u32 concentration = network->numInterfaces() / network->numRouters();
  numVcs_ = network->numVcs();
  assert(numVcs_ > 0);

  bytesPerFlit_ = _settings["bytes_per_flit"].asUInt();
  assert(bytesPerFlit_ > 0);
  headerOverhead_ = _settings["header_overhead"].asUInt();
  maxPacketSize_ = _settings["max_packet_size"].asUInt();

  // processor terminals
  auto dimPE = _settings["processor_terminal"]["dimension"];
  assert(dimPE.isArray());
  assert(dimPE.size() == 2);
  rowsPE_ = dimPE[ROW].asUInt();
  colsPE_ = dimPE[COL].asUInt();
  numPEs_ = rowsPE_ * colsPE_;

  auto sharingPE = _settings["processor_terminal"]["sharing"];
  assert(sharingPE.isArray());
  assert(sharingPE.size() == 2);
  routerRows_ = sharingPE[ROW].asUInt();
  routerCols_ = sharingPE[COL].asUInt();
  assert(rowsPE_ % routerRows_ == 0);
  assert(colsPE_ % routerCols_ == 0);
  assert(routerRows_ * routerCols_ == concentration);

  std::vector<u32> dimensionWidths(2);
  dimensionWidths[ROW] = rowsPE_ / routerRows_;
  dimensionWidths[COL] = colsPE_ / routerCols_;

  assert(numPEs_ ==
      dimensionWidths[ROW] * dimensionWidths[COL] * concentration);

  // memory terminals
  assert(_settings["memory_terminal"]["place"].asString() == "surrounding");
  numSrams_ = (dimensionWidths[ROW] + dimensionWidths[COL]) * 2 + 4;
  dimensionWidths[ROW] += 2;
  dimensionWidths[COL] += 2;

  // compare dimensions with the network
  assert(dimensionWidths[ROW] * dimensionWidths[COL] == network->numRouters());

  // Initialize the queue for each processor node
  traceRequests_ = new std::deque<TraceOp> [numPEs_];

  traceFile_ = _settings["trace"]["file"].asString();

  // check the memory system setup
  memorySlice_ = _settings["memory_slice"].asUInt();
  totalMemory_ = memorySlice_ * numSrams_;
  blockSize_ = _settings["block_size"].asUInt();
  assert(ISPOW2(blockSize_));
  assert((memorySlice_ % blockSize_) == 0);

  // Parse the trace file
  dbgprintf("Trace file: %s", traceFile_.c_str());
  parseTraceFile();

  // (row, col) to network ID lookup table
  std::vector<std::vector<std::vector<u32>>> krc2nid;
  krc2nid.resize(concentration);
  for (auto& rcVec : krc2nid) {
    rcVec.resize(dimensionWidths[ROW]);
    for (auto& cVec : rcVec)
      cVec.resize(dimensionWidths[COL], -1);
  }
  std::vector<u32> address;
  for (u32 id = 0; id < network->numInterfaces(); id++) {
    network->translateIdToAddress(id, &address);
    assert(address.size() == 3);
    auto k = address[0];
    auto r = address[ROW+1];
    auto c = address[COL+1];
    krc2nid[k][r][c] = id;
  }

  // Initialize trace ID to network ID lookup table
  for (u32 i = 0; i < numPEs_ + numSrams_; i++) {
    tid2nid_.push_back(-1);
  }

  u32 tid = 0;
  u32 nullTid = 0;

  // create terminals
  address.resize(3);
  for (u32 r = 0; r < dimensionWidths[ROW]; r++) {
    for (u32 c = 0; c < dimensionWidths[COL]; c++) {
      if (r == 0 || c == 0 ||
          r == dimensionWidths[ROW] - 1 || c == dimensionWidths[COL] - 1) {
        u32 k = 0;

        address[0] = k;
        address[ROW + 1] = r;
        address[COL + 1] = c;

        u32 id = krc2nid[0][r][c];
        dbgprintf("SRAM (%u, %u): tid %u nid %u", r, c, tid, id);

        std::string tname = "MemoryTerminal_" + std::to_string(tid);
        MemoryTerminal* terminal = new MemoryTerminal(
            tname, this, id, tid, address, memorySlice_,
            this, _settings["memory_terminal"]);
        setTerminal(id, terminal);
        tid2nid_[tid] = id;
        tid++;

        for (k = 1; k < concentration; k++) {
          // Connect null terminals to the unused local router ports
          id = krc2nid[k][r][c];
          dbgprintf("NT_%u, nid %u", nullTid, id);

          std::string tname = "NullTerminal_" + std::to_string(nullTid);
          NullTerminal* terminal = new NullTerminal(
              tname, this, id, address,
              this);
          setTerminal(id, terminal);
          nullTid++;
        }
      }
    }
  }
  assert(tid == numSrams_);

  for (u32 per = 0; per < rowsPE_; per++) {
    for (u32 pec = 0; pec < colsPE_; pec++) {
      u32 r = per / routerRows_ + 1;
      u32 c = pec / routerCols_ + 1;
      u32 k = (per % routerRows_) * routerCols_ + pec % routerCols_;

      address[0] = k;
      address[ROW + 1] = r;
      address[COL + 1] = c;

      u32 id = krc2nid[k][r][c];
      dbgprintf("PE (%u, %u): tid %u, nid %u", per, pec, tid, id);

      std::string tname = "ProcessorTerminal_" + std::to_string(tid);
      ProcessorTerminal* terminal = new ProcessorTerminal(
          tname, this, id, tid, address,
          this, _settings["processor_terminal"]);
      setTerminal(id, terminal);
      tid2nid_[tid] = id;
      tid++;
    }
  }
  assert(tid == numSrams_ + numPEs_);

  for (u32 i = 0; i < numSrams_ + numPEs_; i++) {
    nid2tid_[tid2nid_[i]] = i;
  }

  // this application always wants monitor
  addEvent(0, 0, nullptr, 0);
}

Application::~Application() {
  delete[] traceRequests_;
}

f64 Application::percentComplete() const {
  f64 percentSum = 0.0;
  u32 processorCount = 0;
  for (u32 tid = numSrams_; tid < numSrams_ + numPEs_; tid++) {
    ProcessorTerminal* t =
        reinterpret_cast<ProcessorTerminal*>(getTerminal(tid2nid_[tid]));
    percentSum += t->percentComplete();
    processorCount++;
  }
  return percentSum / processorCount;
}

void Application::split(const std::string &s, char delim,
             std::vector<std::string> *elems) {
    if (elems == nullptr) return;
    std::string::size_type lastPos = s.find_first_not_of(delim, 0);
    std::string::size_type pos     = s.find_first_of(delim, lastPos);
    while (std::string::npos != pos || std::string::npos != lastPos) {
        elems->push_back(s.substr(lastPos, pos - lastPos));
        lastPos = s.find_first_not_of(delim, pos);
        pos = s.find_first_of(delim, lastPos);
    }
}

std::vector<std::string> Application::split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    elems.reserve(8);
    split(s, delim, &elems);
    return elems;
}

void Application::parseTraceFile() {
  std::ifstream file(traceFile_);
  std::string line;
  u32 lnCnt = 0;

  assert(file.good());
  while (true) {
    std::getline(file, line);
    lnCnt++;
    if (!file) break;

    auto fields = split(line, ' ');
    assert(fields.size() == 5);

    // Data flows from src to dest
    u32 srcId = traceNameToId(fields[2]);
    u32 destId = traceNameToId(fields[1]);

    u32 initiator;
    TraceOp op;
    op.size = std::stoi(fields[3]);

    // Timestamp
    op.ts = std::stoull(fields[0]);

    if (fields[4] == "R") {
      initiator = destId;
      op.target = srcId;
      op.op = MemoryOp::eOp::kReadReq;
    } else if (fields[4] == "W") {
      initiator = srcId;
      op.target = destId;
      op.op = MemoryOp::eOp::kWriteReq;
    } else if (fields[4] == "P") {
      initiator = destId;
      op.target = srcId;
      op.op = MemoryOp::eOp::kPrefetchReq;
    } else {
      assert(false);
    }

    if (lnCnt % 5000000 == 0) {
      dbgprintf("Trace file %u lines read", lnCnt);
    }

    // Initiator is always a PE
    assert(initiator >= numSrams_);
    traceRequests_[initiator - numSrams_].push_back(op);
  }

  // Sort requests based on timestamp.
  for (u32 idx = 0; idx < numPEs_; idx++) {
    auto& q = traceRequests_[idx];
    std::sort(q.begin(), q.end());
  }
}

u32 Application::traceNameToId(std::string name) {
  // Examples
  // 2-3 represents node (2,3)
  // m-0 represents SRAM 0
  // SRAMs start from ID 0
  u32 dash = 0;
  while (true) {
    auto c = name[dash];
    if (!c) assert(false);
    if (c == '-') break;
    dash++;
  }
  name[dash] = '\0';
  const char* f1 = name.c_str();
  const char* f2 = f1 + dash + 1;
  u32 row, col;

  if (*f1 == 'm') {
    col = std::stoi(f2);
    assert(col < numSrams_);
    return col;
  } else {
    col = std::stoi(f1);
    row = std::stoi(f2);
    assert(row < rowsPE_);
    assert(col < colsPE_);
    return row * colsPE_ + col + numSrams_;
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
  dbgprintf("Ntrace application starting");
  gSim->startMonitoring();
}

std::deque<Application::TraceOp>* Application::getTraceQ(u32 pe) {
  assert(pe < numPEs_);
  return &traceRequests_[pe];
}

const std::deque<Application::TraceOp>* Application::getTraceQ(u32 pe) const {
  assert(pe < numPEs_);
  return &traceRequests_[pe];
}

u32 Application::tid2nid(u32 tid) const {
  assert(tid < numPEs_ + numSrams_);
  return tid2nid_[tid];
}

u32 Application::nid2tid(u32 nid) const {
  u32 ret = nid2tid_.at(nid);
  assert(ret < numPEs_ + numSrams_);
  return ret;
}

}  // namespace Ntrace
