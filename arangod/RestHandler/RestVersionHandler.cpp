////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#include "RestVersionHandler.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/V8SecurityFeature.h"
#include "Cluster/ServerState.h"
#include "Rest/HttpRequest.h"
#include "Rest/Version.h"
#include "RestServer/ServerFeature.h"
#include "Utils/ExecContext.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief ArangoDB server
////////////////////////////////////////////////////////////////////////////////

RestVersionHandler::RestVersionHandler(GeneralRequest* request, GeneralResponse* response)
    : RestBaseHandler(request, response) {}

RestStatus RestVersionHandler::execute() {
  VPackBuilder result;

  V8SecurityFeature* v8security =
      application_features::ApplicationServer::getFeature<V8SecurityFeature>(
          "V8Security");
  TRI_ASSERT(v8security != nullptr);

  bool hardened = v8security->isDenyedHardenedApi(nullptr);
  bool allowInfo = !hardened;  // allow access if harden flag was not given

  ExecContext const* exec = ExecContext::CURRENT;
  if (exec == nullptr || exec->isAdminUser()) {
    // also allow access if there is not authentication
    // enabled or when the user is an administrator
    allowInfo = true;
  }

  result.add(VPackValue(VPackValueType::Object));
  result.add("server", VPackValue("arango"));

  if (allowInfo) {
    result.add("version", VPackValue(ARANGODB_VERSION));
#ifdef USE_ENTERPRISE
    result.add("license", VPackValue("enterprise"));
#else
    result.add("license", VPackValue("community"));
#endif

    bool found;
    std::string const& detailsStr = _request->value("details", found);
    if (found && StringUtils::boolean(detailsStr)) {
      result.add("details", VPackValue(VPackValueType::Object));
      Version::getVPack(result);

      if (application_features::ApplicationServer::server != nullptr) {
        auto server = application_features::ApplicationServer::server->getFeature<ServerFeature>(
            "Server");
        result.add("mode", VPackValue(server->operationModeString()));
        auto serverState = ServerState::instance();
        if (serverState != nullptr) {
          result.add("role", VPackValue(ServerState::roleToString(serverState->getRole())));
        }
      }

      std::string host = ServerState::instance()->getHost();
      if (!host.empty()) {
        result.add("host", VPackValue(host));
      }
      result.close();
    }  // found
  }    // allowInfo
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
  return RestStatus::DONE;
}
