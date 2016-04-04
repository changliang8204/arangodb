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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Agent.h"
#include "Basics/ConditionLocker.h"

#include <velocypack/Iterator.h>    
#include <velocypack/velocypack-aliases.h> 

#include <chrono>
#include <iostream>

using namespace arangodb::velocypack;

namespace arangodb {
namespace consensus {

Agent::Agent () : Thread ("Agent"), _last_commit_index(0) {}

/// @brief Agent configuration
Agent::Agent (config_t const& config) :
  Thread ("Agent"), _config(config), _last_commit_index(0) {
  _state.setEndPoint(_config.end_point);
  _constituent.configure(this);
  _confirmed.resize(size(),0); // agency's size and reset to 0
}

/// @brief This agent's id
id_t Agent::id() const {
  return _config.id;
}

/// @brief Shutdown
Agent::~Agent () {
  shutdown();
}

/// @brief State machine
State const& Agent::state () const {
  return _state;
}

/// @brief Start all agent thread
bool Agent::start() {
  LOG_TOPIC(INFO, Logger::AGENCY) << "Starting agency comm worker.";
  Thread::start();
  return true;
}

/// @brief This agent's term
term_t Agent::term () const {
  return _constituent.term();
}

/// @brief Agency size
inline size_t Agent::size() const {
  return _config.size();
}

/// @brief Handle vote request
priv_rpc_ret_t Agent::requestVote(term_t t, id_t id, index_t lastLogIndex,
                                  index_t lastLogTerm, query_t const& query) {

  /// Are we receiving new endpoints
  if (query != nullptr) { // record new endpoints
    if (query->slice().hasKey("endpoints") && 
        query->slice().get("endpoints").isArray()) {
      size_t j = 0;
      for (auto const& i : VPackArrayIterator(query->slice().get("endpoints"))) {
        _config.end_points[j++] = i.copyString();
      }
    }
  }

  /// Constituent handles this
  return priv_rpc_ret_t( 
    _constituent.vote(t, id, lastLogIndex, lastLogTerm), this->term());
  
}

/// @brief Get configuration
config_t const& Agent::config () const {
  return _config;
}

/// @brief Leader's id
id_t Agent::leaderID () const {
  return _constituent.leaderID();
}

/// @brief Are we leading?
bool Agent::leading() const {
  return _constituent.leading();
}

/// @brief Persist term and id we vote for
void Agent::persist(term_t t, id_t i) {
//  _state.persist(t, i);
}

/// @brief Waits here for confirmation of log's commits up to index
bool Agent::waitFor (index_t index, duration_t timeout) {

  if (size() == 1) // single host agency
    return true;
    
  CONDITION_LOCKER(guard, _rest_cv);
  auto start = std::chrono::system_clock::now();

  // Wait until woken up through AgentCallback 
  while (true) {

    _rest_cv.wait();

    // shutting down
    if (this->isStopping()) {      
      return false;
    }
    // timeout?
    if (std::chrono::system_clock::now() - start > timeout) {
      return false;
    }
    /// success?
    if (_last_commit_index >= index) {
      return true;
    }
  }
  // We should never get here
  TRI_ASSERT(false);
}

/// @brief AgentCallback reports id of follower and its highest processed index
void Agent::reportIn (id_t id, index_t index) {
  MUTEX_LOCKER(mutexLocker, _ioLock);

  if (index > _confirmed[id])      // progress this follower?
    _confirmed[id] = index;
  
  if(index > _last_commit_index) { // progress last commit?
    size_t n = 0;
    for (size_t i = 0; i < size(); ++i) {
      n += (_confirmed[i]>=index);
    }
    if (n>size()/2) { // catch up read database and commit index
      LOG_TOPIC(INFO, Logger::AGENCY) << "Critical mass for commiting " <<
        _last_commit_index+1 << " through " << index << " to read db";
      
      _read_db.apply(_state.slices(_last_commit_index+1, index));
      _last_commit_index = index;
    }
  }

  _rest_cv.broadcast();            // wake up REST handlers
}

/// @brief Append entries for followers
bool Agent::recvAppendEntriesRPC (term_t term, id_t leaderId, index_t prevIndex,
  term_t prevTerm, index_t leaderCommitIndex, query_t const& queries) {
  //Update commit index

  if (queries->slice().type() != VPackValueType::Array) {
    LOG_TOPIC(WARN, Logger::AGENCY)
      << "Received malformed entries for appending. Discarting!";  
    return false;
  }
  if (queries->slice().length()) {
    LOG_TOPIC(INFO, Logger::AGENCY) << "Appending "<< queries->slice().length()
              << " entries to state machine.";
  } else { 
    // heart-beat
  }
    
  if (_last_commit_index < leaderCommitIndex) {
    LOG_TOPIC(INFO, Logger::AGENCY) <<  "Updating last commited index to " << leaderCommitIndex;
  }
  _last_commit_index = leaderCommitIndex;
  
  // Sanity
  if (this->term() > term) {                 // (§5.1)
    LOG_TOPIC(WARN, Logger::AGENCY) << "I have a higher term than RPC caller.";
    throw LOWER_TERM_APPEND_ENTRIES_RPC; 
  }
  
  // Delete conflits and append (§5.3)
  _state.log (queries, term, leaderId, prevIndex, prevTerm);
  return true;

}

#include <iostream>
append_entries_t Agent::sendAppendEntriesRPC (id_t follower_id) {

  index_t last_confirmed = _confirmed[follower_id];
  std::vector<log_t> unconfirmed = _state.get(last_confirmed);

  // RPC path
  std::stringstream path;
  path << "/_api/agency_priv/appendEntries?term=" << term() << "&leaderId="
       << id() << "&prevLogIndex=" << unconfirmed[0].index << "&prevLogTerm="
       << unconfirmed[0].term << "&leaderCommit=" << _last_commit_index;

  // Headers
	std::unique_ptr<std::map<std::string, std::string>> headerFields =
	  std::make_unique<std::map<std::string, std::string> >();

  // Body
  Builder builder;
  index_t last = unconfirmed[0].index;
  builder.add(VPackValue(VPackValueType::Array));
  for (size_t i = 1; i < unconfirmed.size(); ++i) {
    builder.add (VPackValue(VPackValueType::Object));
    builder.add ("index", VPackValue(unconfirmed[i].index));
    builder.add ("query", VPackSlice(unconfirmed[i].entry->data()));
    builder.close();
    last = unconfirmed[i].index;
  }
  builder.close();

  // Send request
  if (unconfirmed.size() > 1) {
    LOG_TOPIC(INFO, Logger::AGENCY)
      << "Appending " << unconfirmed.size()-1 << " entries up to index " << last
      << " to follower " << follower_id;
  }

  arangodb::ClusterComm::instance()->asyncRequest
    ("1", 1, _config.end_points[follower_id],
     arangodb::GeneralRequest::RequestType::POST,
     path.str(), std::make_shared<std::string>(builder.toJson()), headerFields,
     std::make_shared<AgentCallback>(this, follower_id, last),
     0, true);

  return append_entries_t(this->term(), true);
  
}

bool Agent::load () {
  LOG_TOPIC(INFO, Logger::AGENCY) << "Loading persistent state.";
  if (!_state.loadCollections()) {
    LOG_TOPIC(WARN, Logger::AGENCY) << "Failed to load persistent state on statup.";
  }

  LOG_TOPIC(INFO, Logger::AGENCY) << "Reassembling spearhead and read stores.";
  _read_db.apply(_state.slices());
  _spearhead.apply(_state.slices(_last_commit_index+1));

  LOG_TOPIC(INFO, Logger::AGENCY) << "Starting spearhead worker.";
  _spearhead.start(this);
  _read_db.start(this);

  LOG_TOPIC(INFO, Logger::AGENCY) << "Starting constituent personality.";
  _constituent.update(0,0);
  _constituent.start();
  
  return true;
}

write_ret_t Agent::write (query_t const& query)  {

  if (_constituent.leading()) {                    // Leading 
    MUTEX_LOCKER(mutexLocker, _ioLock);
    std::vector<bool> applied = _spearhead.apply(query); // Apply to spearhead
    std::vector<index_t> indices = 
      _state.log (query, applied, term(), id()); // Append to log w/ indicies
    for (size_t i = 0; i < applied.size(); ++i) {
      if (applied[i]) {
        _confirmed[id()] = indices[i];           // Confirm myself
      }
    }
    _cv.signal();                                // Wake up run
    return write_ret_t(true,id(),applied,indices); // Indices to wait for to rest
  } else {                                       // Leading else redirect
    return write_ret_t(false,_constituent.leaderID());
  }
}

read_ret_t Agent::read (query_t const& query) const {
  if (_constituent.leading()) {     // We are leading
    query_t result = std::make_shared<arangodb::velocypack::Builder>();
    std::vector<bool> success= (_config.size() == 1) ?
      _spearhead.read(query, result) : _read_db.read (query, result);
    return read_ret_t(true, _constituent.leaderID(), success, result);
  } else {                          // We redirect
    return read_ret_t(false, _constituent.leaderID());
  }
}

void Agent::run() {

  CONDITION_LOCKER(guard, _cv);
  
  while (!this->isStopping()) {
    if (leading())
      _cv.wait(250000);
    else
      _cv.wait();

    // Collect all unacknowledged
    for (id_t i = 0; i < size(); ++i) {
      if (i != id()) {
        sendAppendEntriesRPC(i);
      }
    }
  }

}

void Agent::beginShutdown() {
  Thread::beginShutdown();
  _constituent.beginShutdown();
  _spearhead.beginShutdown();
  _read_db.beginShutdown();
  CONDITION_LOCKER(guard, _cv);
  guard.broadcast();
}

bool Agent::lead () {
  rebuildDBs();
  _cv.signal();
  return true;
}

bool Agent::rebuildDBs() {
  MUTEX_LOCKER(mutexLocker, _ioLock);
  _spearhead.apply(_state.slices());
  _read_db.apply(_state.slices());
  return true;
}

log_t const& Agent::lastLog() const {
  return _state.lastLog();
}

Store const& Agent::spearhead () const {
  return _spearhead;
}

Store const& Agent::readDB () const {
  return _read_db;
}

}}
