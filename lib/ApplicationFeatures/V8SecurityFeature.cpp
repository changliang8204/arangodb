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

#include "ApplicationFeatures/V8SecurityFeature.h"

#include "Basics/FileUtils.h"
#include "Basics/MutexLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/files.h"
#include "Basics/tri-strings.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "V8/v8-globals.h"

#include <v8.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::options;

V8SecurityFeature::V8SecurityFeature(application_features::ApplicationServer& server)
    : ApplicationFeature(server, "V8Security"), _allowExecutionOfBinaries(false) {
  setOptional(false);
  startsAfter("V8Platform");
}

void V8SecurityFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("javascript", "Configure the Javascript engine");

  options->addOption(
      "--javascript.execute-binaries",
      "allow execution of external binaries. default set to false",
      new BooleanParameter(&_allowExecutionOfBinaries));

  options->addOption("--javascript.startup-options-white-list",
                     "startup options whose names match this regular "
                     "expression will not be exposed to JavaScript actions",
                     new VectorParameter<StringParameter>(&_startupOptionsWhiteListVec));

  options->addOption("--javascript.startup-options-black-list",
                     "startup options whose names match this regular "
                     "expression will not be exposed to JavaScript actions",
                     new VectorParameter<StringParameter>(&_startupOptionsBlackListVec));

  options->addOption("--javascript.environment-variables-white-list",
                     "environment variables whose names match this regular "
                     "expression will not be exposed to JavaScript actions",
                     new VectorParameter<StringParameter>(&_environmentVariablesWhiteListVec));

  options->addOption("--javascript.environment-variables-black-list",
                     "environment variables whose names match this regular "
                     "expression will not be exposed to JavaScript actions",
                     new VectorParameter<StringParameter>(&_environmentVariablesBlackListVec));

  options->addOption(
      "--javascript.endpoints-white-list",
      "endpoints that match this regular expression cannot be connected to via "
      "internal.download() in JavaScript actions",
      new VectorParameter<StringParameter>(&_endpointsWhiteListVec));

  options->addOption(
      "--javascript.endpoints-black-list",
      "endpoints that match this regular expression cannot be connected to via "
      "internal.download() in JavaScript actions",
      new VectorParameter<StringParameter>(&_endpointsBlackListVec));

  options->addOption("--javascript.files-white-list",
                     "paths to be added to files-white-list-expression",
                     new VectorParameter<StringParameter>(&_filesWhiteListVec));

  options->addOption("--javascript.files-black-list",
                     "paths to be added to files-black-list-expression",
                     new VectorParameter<StringParameter>(&_filesBlackListVec));
}

namespace {
void convertToRe(std::vector<std::string>& files, std::string& target_re) {
  if (!files.empty()) {
    std::stringstream ss;
    std::string last = std::move(files.back());
    files.pop_back();

    while (!files.empty()) {
      ss << files.back() << "|";
      files.pop_back();
    }

    ss << last;

    target_re = ss.str();
  }
};

bool checkBlackAndWhiteList(std::string const& value, bool hasWhiteList,
                            std::regex const& whiteList, bool hasBlacklist,
                            std::regex const& blackList) {
  if (!hasWhiteList && !hasBlacklist) {
    return true;
  }

  if (!hasBlacklist) {
    // must be white listed
    return std::regex_search(value, whiteList);
  }

  if (!hasWhiteList) {
    // must be white listed
    return !std::regex_search(value, blackList);
  }

  if (std::regex_search(value, whiteList)) {
    return true;  // white-list wins - simple implementation
  } else {
    return !std::regex_search(value, blackList);
  }
}
}  // namespace

void V8SecurityFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  // check if the regexes compile properly

  // startup options
  convertToRe(_startupOptionsWhiteListVec, _startupOptionsWhiteList);
  try {
    std::regex(_startupOptionsWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab9d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.startup-options-white-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  convertToRe(_startupOptionsBlackListVec, _startupOptionsBlackList);
  try {
    std::regex(_startupOptionsBlackList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab8d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.startup-options-black-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  // environment variables
  convertToRe(_environmentVariablesWhiteListVec, _environmentVariablesWhiteList);
  try {
    std::regex(_environmentVariablesWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab9d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.environment-variables-white-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  convertToRe(_environmentVariablesBlackListVec, _environmentVariablesBlackList);
  try {
    std::regex(_environmentVariablesBlackList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab8d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.environment-variables-black-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  // endpoints
  convertToRe(_endpointsWhiteListVec, _endpointsWhiteList);
  try {
    std::regex(_endpointsWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab9d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.endpoints-white-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  convertToRe(_endpointsBlackListVec, _endpointsBlackList);
  try {
    std::regex(_endpointsBlackList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab8d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.endpoints-black-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  // file access
  convertToRe(_filesWhiteListVec, _filesWhiteList);
  try {
    std::regex(_filesWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab9d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.files-white-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }

  convertToRe(_filesBlackListVec, _filesBlackList);
  try {
    std::regex(_filesBlackList, std::regex::nosubs | std::regex::ECMAScript);
  } catch (std::exception const& ex) {
    LOG_TOPIC("ab8d5", FATAL, arangodb::Logger::FIXME)
        << "value for '--javascript.files-black-list' is not a "
           "valid regular "
           "expression: "
        << ex.what();
    FATAL_ERROR_EXIT();
  }
}

void V8SecurityFeature::start() {
  // initialize regexes for filtering options. the regexes must have been validated before
  _startupOptionsWhiteListRegex =
      std::regex(_startupOptionsWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  _startupOptionsBlackListRegex =
      std::regex(_startupOptionsBlackList, std::regex::nosubs | std::regex::ECMAScript);

  _environmentVariablesWhiteListRegex =
      std::regex(_environmentVariablesWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  _environmentVariablesBlackListRegex =
      std::regex(_environmentVariablesBlackList, std::regex::nosubs | std::regex::ECMAScript);

  _endpointsWhiteListRegex =
      std::regex(_endpointsWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  _endpointsBlackListRegex =
      std::regex(_endpointsBlackList, std::regex::nosubs | std::regex::ECMAScript);

  _filesWhiteListRegex =
      std::regex(_filesWhiteList, std::regex::nosubs | std::regex::ECMAScript);
  _filesBlackListRegex =
      std::regex(_filesBlackList, std::regex::nosubs | std::regex::ECMAScript);
}

bool V8SecurityFeature::isAllowedToExecuteExternalBinaries(v8::Isolate* isolate) const {
  TRI_GET_GLOBALS();
  // v8g may be a nullptr when we are in arangosh
  if (v8g != nullptr) {
   return _allowExecutionOfBinaries || v8g->_securityContext.canExecuteExternalBinaries();
  }
  return _allowExecutionOfBinaries;
}

bool V8SecurityFeature::isAllowedToDefineHttpAction(v8::Isolate* isolate) const {
  TRI_GET_GLOBALS();
  // v8g may be a nullptr when we are in arangosh
  return v8g != nullptr && v8g->_securityContext.canDefineHttpAction();
}

bool V8SecurityFeature::shouldExposeStartupOption(v8::Isolate* isolate,
                                                  std::string const& name) const {
  return checkBlackAndWhiteList(name, !_startupOptionsWhiteList.empty(),
                                _startupOptionsWhiteListRegex,
                                !_startupOptionsBlackList.empty(),
                                _startupOptionsBlackListRegex);
}

bool V8SecurityFeature::shouldExposeEnvironmentVariable(v8::Isolate* isolate,
                                                        std::string const& name) const {
  return checkBlackAndWhiteList(name, !_environmentVariablesWhiteList.empty(),
                                _environmentVariablesWhiteListRegex,
                                !_environmentVariablesBlackList.empty(),
                                _environmentVariablesBlackListRegex);
}

bool V8SecurityFeature::isAllowedToConnectToEndpoint(v8::Isolate* isolate,
                                                     std::string const& name) const {
  TRI_GET_GLOBALS();
  if (v8g != nullptr && v8g->_securityContext.isInternal()) {
    // internal security contexts are allowed to connect to any endpoint
    // this includes connecting to self or to other instances in a cluster
    return true;
  }

  return checkBlackAndWhiteList(name, !_endpointsWhiteList.empty(), _endpointsWhiteListRegex,
                                !_endpointsBlackList.empty(), _endpointsBlackListRegex);
}

bool V8SecurityFeature::isAllowedToAccessPath(v8::Isolate* isolate, char const* path,
                                              FSAccessType access) const {
  // expects 0 terminated utf-8 string
  TRI_ASSERT(path != nullptr);

  return isAllowedToAccessPath(isolate, std::string(path), access);
}

bool V8SecurityFeature::isAllowedToAccessPath(v8::Isolate* isolate, std::string path,
                                              FSAccessType access) const {
  // check security context first
  TRI_GET_GLOBALS();
  auto& sec = v8g->_securityContext;
  if ((access == FSAccessType::READ && sec.canReadFs()) ||
      (access == FSAccessType::WRITE && sec.canWriteFs())) {
    return true;  // context may read / write without restrictions
  }

  // remove link
  path = TRI_ResolveSymbolicLink(std::move(path));

  // make absolute
  std::string cwd = FileUtils::currentDirectory().result();
  {
    auto absPath = std::unique_ptr<char, void (*)(char*)>(
        TRI_GetAbsolutePath(path.c_str(), cwd.c_str()), &TRI_FreeString);
    if (absPath) {
      path = std::string(absPath.get());
    }
  }

  return checkBlackAndWhiteList(path, !_filesWhiteList.empty(), _filesWhiteListRegex,
                                !_filesBlackList.empty(), _filesBlackListRegex);
}

bool V8SecurityFeature::isAllowedToExecuteJavaScript(v8::Isolate* isolate) const {
  // implement me
  return true;
}
