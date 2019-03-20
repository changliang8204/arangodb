////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_TRANSACTION_MANAGER_FEATURE_H
#define ARANGODB_TRANSACTION_MANAGER_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"

#include "Scheduler/Scheduler.h"

namespace arangodb {
namespace transaction {

class Manager;

class ManagerFeature final : public application_features::ApplicationFeature {
 public:
  explicit ManagerFeature(application_features::ApplicationServer& server);

  void prepare() override final;
  void start() override;
  void beginShutdown() override;
  void unprepare() override final;

  static transaction::Manager* manager() {
    TRI_ASSERT(MANAGER != nullptr);
    return MANAGER.get();
  }

 private:
  static std::unique_ptr<transaction::Manager> MANAGER;
  
 private:
  Scheduler::WorkHandle _workItem;
  std::function<void(bool)> _gcfunc;
};

}  // namespace transaction
}  // namespace arangodb

#endif
