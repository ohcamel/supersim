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
#ifndef APPLICATION_NTRACE_APPLICATION_H_
#define APPLICATION_NTRACE_APPLICATION_H_

#include <json/json.h>
#include <prim/prim.h>

#include <string>
#include <iostream>
#include <fstream>
#include <regex>
#include <vector>
#include <queue>

#include "network/torus/Network.h"
#include "event/Component.h"
#include "application/Application.h"
#include "application/ntrace/MemoryOp.h"

class MetadataHandler;

namespace Ntrace {

class Application : public ::Application {
 public:
  Application(const std::string& _name, const Component* _parent,
              MetadataHandler* _metadataHandler, Json::Value _settings);
  ~Application();
  f64 percentComplete() const override;
  u32 numVcs() const;
  u32 numSrams() const;
  u32 numPEs() const;
  u32 totalMemory() const;
  u32 memorySlice() const;
  u32 blockSize() const;
  u32 bytesPerFlit() const;
  u32 headerOverhead() const;
  u32 maxPacketSize() const;
  void processorComplete(u32 _id);
  void processEvent(void* _event, s32 _type) override;

  struct TraceOp {
    MemoryOp::eOp op;
    u32 target;
    u32 size;  // Size in bits
  };
  std::queue<TraceOp> * getTraceQ(u32 pe);
  const std::queue<TraceOp> * getTraceQ(u32 pe) const;

  u32 PeIdBase() const;

  u32 tid2nid(u32 tid) const;
  u32 nid2tid(u32 nid) const;

 private:
  u32 numVcs_;

  u32 totalMemory_;
  u32 memorySlice_;
  u32 blockSize_;

  u32 bytesPerFlit_;
  u32 headerOverhead_;
  u32 maxPacketSize_;

  u32 remainingProcessors_;

  u32 numPEs_;
  u32 numSrams_;
  u32 rowsPE_;
  u32 colsPE_;

  // How many rows and columns of PE array per router
  u32 routerRows_;
  u32 routerCols_;
  std::string traceFile_;
  std::queue<TraceOp> * traceRequests_;
  std::vector<int> tid2nid_;
  std::unordered_map<u32, u32> nid2tid_;

  Torus::Network *network_;

  void parseTraceFile();
  u32 traceNameToId(std::string name);
  void split(const std::string &s, char delim,
             std::vector<std::string> *elems);
  std::vector<std::string> split(const std::string &s, char delim);
};

}  // namespace Ntrace

#endif  // APPLICATION_NTRACE_APPLICATION_H_
