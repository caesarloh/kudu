// Copyright (c) 2013, Cloudera, inc.

#include <algorithm>
#include <boost/foreach.hpp>

#include "kudu/consensus/raft_consensus_state.h"
#include "kudu/consensus/log_util.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"
#include "kudu/util/trace.h"

// Convenience macros to prefix log messages with the id of the tablet and peer.
// Do not obtain the state lock and should be used when holding the state_ lock
#define LOG_WITH_PREFIX(severity) LOG(severity) << LogPrefixUnlocked()
#define VLOG_WITH_PREFIX(verboselevel) LOG_IF(INFO, VLOG_IS_ON(verboselevel)) \
  << LogPrefixUnlocked()
// Same as the above, but obtain the lock
#define LOG_WITH_PREFIX_LK(severity) LOG(severity) << LogPrefix()
#define VLOG_WITH_PREFIX_LK(verboselevel) LOG_IF(INFO, VLOG_IS_ON(verboselevel)) \
  << LogPrefix()

namespace kudu {
namespace consensus {

using metadata::QuorumPB;
using metadata::QuorumPeerPB;
using std::string;
using std::tr1::shared_ptr;
using std::tr1::unordered_set;
using strings::Substitute;
using strings::SubstituteAndAppend;

//////////////////////////////////////////////////
// QuorumState
//////////////////////////////////////////////////

gscoped_ptr<QuorumState> QuorumState::Build(const QuorumPB& quorum, const string& self_uuid) {
  // Default this peer's role to non-participant.
  QuorumPeerPB::Role role = QuorumPeerPB::NON_PARTICIPANT;

  // Try to find the role set in the provided quorum.
  std::tr1::unordered_set<string> voting_peers;
  string leader_uuid;
  BOOST_FOREACH(const QuorumPeerPB& peer_pb, quorum.peers()) {
    if (peer_pb.permanent_uuid() == self_uuid) {
      role = peer_pb.role();
    }
    if (peer_pb.role() == QuorumPeerPB::LEADER ||
        peer_pb.role() == QuorumPeerPB::FOLLOWER) {
      voting_peers.insert(peer_pb.permanent_uuid());
    }
    if (peer_pb.role() == QuorumPeerPB::LEADER) {
      leader_uuid = peer_pb.permanent_uuid();
    }

  }

  // TODO: Calculating the majority from the number of peers can cause problems
  // without joint consensus. We should add a configuration parameter to
  // QuorumPB defining what constitutes the majority.
  int majority_size = (voting_peers.size() / 2) + 1;
  int quorum_size = quorum.peers_size();
  int64_t config_seqno = quorum.seqno();

  gscoped_ptr<QuorumState> state(new QuorumState(role, leader_uuid, voting_peers,
                                                 majority_size, quorum_size, config_seqno));
  return state.Pass();
}

QuorumState::QuorumState(metadata::QuorumPeerPB::Role role,
                         const std::string& leader_uuid,
                         const std::tr1::unordered_set<std::string>& voting_peers,
                         int majority_size,
                         int quorum_size,
                         int64_t config_seqno)
  : role(role),
    leader_uuid(leader_uuid),
    voting_peers(voting_peers),
    majority_size(majority_size),
    quorum_size(quorum_size),
    config_seqno(config_seqno) {
}

//////////////////////////////////////////////////
// ReplicaState
//////////////////////////////////////////////////

ReplicaState::ReplicaState(const ConsensusOptions& options,
                           ThreadPool* callback_pool,
                           const string& peer_uuid,
                           gscoped_ptr<ConsensusMetadata> cmeta,
                           ReplicaTransactionFactory* txn_factory)
  : options_(options),
    peer_uuid_(peer_uuid),
    callback_pool_(callback_pool),
    cmeta_(cmeta.Pass()),
    next_index_(0),
    txn_factory_(txn_factory),
    in_flight_applies_latch_(0),
    replicate_watchers_(callback_pool),
    commit_watchers_(callback_pool),
    state_(kInitialized) {
  CHECK(cmeta_) << "ConsensusMeta passed as NULL";
  UniqueLock l(&update_lock_);
  // Now that we know the peer UUID, refresh acting state from persistent state.
  ResetActiveQuorumStateUnlocked(GetCommittedQuorumUnlocked());
}

Status ReplicaState::StartUnlocked(const OpId& initial_id) {
  DCHECK(update_lock_.is_locked());

  // Handle term changes.
  uint64_t current_term = cmeta_->mutable_pb()->current_term();
  if (initial_id.term() < current_term) {
    return Status::InvalidArgument(Substitute("Cannot start in older term. "
                                              "Current term: $0, passed term: $1",
                                              current_term, initial_id.term()));
  }

  CHECK_EQ(initial_id.term(), current_term)
      << "Starting with term " << initial_id.term()
      << " which is greater than last recorded term "
      << current_term;

  next_index_ = initial_id.index() + 1;
  replicated_op_id_.CopyFrom(initial_id);
  received_op_id_.CopyFrom(initial_id);
  last_triggered_apply_.CopyFrom(initial_id);
  return Status::OK();
}

Status ReplicaState::LockForStart(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  CHECK_EQ(state_, kInitialized) << "Illegal state for Start()."
      << " Replica is not in kInitialized state";
  lock->swap(&l);
  return Status::OK();
}

Status ReplicaState::LockForRead(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  lock->swap(&l);
  return Status::OK();
}

Status ReplicaState::LockForReplicate(UniqueLock* lock, const ReplicateMsg& msg) {
  DCHECK(!msg.has_id()) << "Should not have an ID yet: " << msg.ShortDebugString();
  UniqueLock l(&update_lock_);
  if (PREDICT_FALSE(state_ != kRunning)) {
    return Status::IllegalState("Replica not in running state");
  }
  switch (active_quorum_state_->role) {
    case QuorumPeerPB::LEADER:
      lock->swap(&l);
      return Status::OK();
    case QuorumPeerPB::CANDIDATE:
      if (msg.op_type() != CHANGE_CONFIG_OP) {
        return Status::IllegalState("Only a change config round can be pushed while CANDIDATE.");
      }
      // TODO support true config change. Right now we only allow
      // replicate calls while CANDIDATE if our term is 0, meaning
      // we're the first CANDIDATE/LEADER of the quorum.
      CHECK_EQ(GetCurrentTermUnlocked(), 0);
      lock->swap(&l);
      return Status::OK();
    default:
      return Status::IllegalState(Substitute("Replica $0 is not leader of this quorum. Role: $1",
                                             peer_uuid_,
                                             QuorumPeerPB::Role_Name(active_quorum_state_->role)));
  }
}

Status ReplicaState::LockForCommit(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  if (PREDICT_FALSE(state_ != kRunning && state_ != kShuttingDown)) {
    return Status::IllegalState("Replica not in running state");
  }
  lock->swap(&l);
  return Status::OK();
}

Status ReplicaState::LockForConfigChange(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  // Can only change the config on non-initialized replicas for
  // now, later kRunning state will also be a valid state for
  // config changes.
  CHECK(state_ == kInitialized || state_ == kRunning) << "Unexpected state: " << state_;
  lock->swap(&l);
  state_ = kChangingConfig;
  return Status::OK();
}

Status ReplicaState::LockForElection(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  if (!(state_ == kInitialized || state_ == kRunning)) {
    return Status::IllegalState(Substitute("Unexpected ReplicaState for LockForElection: $0",
                                           state_));
  }
  lock->swap(&l);
  return Status::OK();
}

Status ReplicaState::LockForUpdate(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  if (PREDICT_FALSE(state_ != kRunning)) {
    return Status::IllegalState("Replica not in running state");
  }
  switch (active_quorum_state_->role) {
    case QuorumPeerPB::LEADER:
      return Status::IllegalState("Replica is leader of the quorum.");
    case QuorumPeerPB::NON_PARTICIPANT:
      return Status::IllegalState("Replica is not a participant of this quorum.");
    default:
      lock->swap(&l);
      return Status::OK();
  }
}

Status ReplicaState::LockForShutdown(UniqueLock* lock) {
  UniqueLock l(&update_lock_);
  if (state_ == kShutDown) {
    return Status::IllegalState("Replica is already shutdown");
  }
  if (state_ != kShuttingDown) {
    state_ = kShuttingDown;
    in_flight_applies_latch_.Reset(in_flight_commits_.size());
  }
  lock->swap(&l);
  return Status::OK();
}

Status ReplicaState::Shutdown() {
  CHECK_EQ(state_, kShuttingDown);
  UniqueLock l(&update_lock_);
  state_ = kShutDown;
  return Status::OK();
}

Status ReplicaState::SetConfigDoneUnlocked() {
  DCHECK(update_lock_.is_locked());
  CHECK_EQ(state_, kChangingConfig);
  state_ = kRunning;
  return Status::OK();
}

const QuorumState& ReplicaState::GetActiveQuorumStateUnlocked() const {
  DCHECK(update_lock_.is_locked());
  DCHECK(active_quorum_state_) << "Quorum state is not set";
  return *active_quorum_state_;
}

bool ReplicaState::IsQuorumChangePendingUnlocked() const {
  return pending_quorum_.get() != NULL;
}

// TODO check that the role change is legal.
Status ReplicaState::SetPendingQuorumUnlocked(const metadata::QuorumPB& new_quorum) {
  DCHECK(update_lock_.is_locked());
  CHECK(!IsQuorumChangePendingUnlocked()) // TODO: Allow rollback of failed config chg txn?
      << "Attempting to make pending quorum change while another is already pending: "
      << "Pending quorum: " << pending_quorum_->ShortDebugString() << "; "
      << "New quorum: " << new_quorum.ShortDebugString();
  pending_quorum_.reset(new metadata::QuorumPB(new_quorum));
  ResetActiveQuorumStateUnlocked(new_quorum);
  return Status::OK();
}

const metadata::QuorumPB& ReplicaState::GetPendingQuorumUnlocked() const {
  DCHECK(update_lock_.is_locked());
  CHECK(IsQuorumChangePendingUnlocked()) << "No pending quorum";
  return *pending_quorum_;
}

Status ReplicaState::SetCommittedQuorumUnlocked(const metadata::QuorumPB& new_quorum) {
  DCHECK(update_lock_.is_locked());

  // TODO: check that the role change is legal.

  // Check that if pending quorum is set, new_quorum is equivalent.
  if (IsQuorumChangePendingUnlocked()) {
    // TODO: Prevent this from being possible once we have proper config change.
    // See KUDU-513 for more details.
    CHECK(pending_quorum_->SerializeAsString() == new_quorum.SerializeAsString())
      << "Attempting to persist quorum change while a different one is pending: "
      << "Pending quorum: " << pending_quorum_->ShortDebugString() << "; "
      << "New quorum: " << new_quorum.ShortDebugString();
  } else {
    // Only update acting quorum members if this is a net-new transaction.
    ResetActiveQuorumStateUnlocked(new_quorum);
  }

  cmeta_->mutable_pb()->mutable_committed_quorum()->CopyFrom(new_quorum);
  RETURN_NOT_OK(cmeta_->Flush());
  pending_quorum_.reset();

  return Status::OK();
}

const metadata::QuorumPB& ReplicaState::GetCommittedQuorumUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return cmeta_->pb().committed_quorum();
}

Status ReplicaState::IncrementTermUnlocked() {
  DCHECK(update_lock_.is_locked());
  cmeta_->mutable_pb()->set_current_term(cmeta_->pb().current_term() + 1);
  cmeta_->mutable_pb()->clear_voted_for();
  RETURN_NOT_OK(cmeta_->Flush());
  return Status::OK();
}

Status ReplicaState::SetCurrentTermUnlocked(uint64_t new_term) {
  DCHECK(update_lock_.is_locked());
  if (PREDICT_FALSE(new_term < GetCurrentTermUnlocked())) {
    return Status::IllegalState(
        Substitute("Cannot change term to a term that is lower than the current one. "
            "Current: $0, Proposed: $1", GetCurrentTermUnlocked(), new_term));
  }
  cmeta_->mutable_pb()->set_current_term(new_term);
  cmeta_->mutable_pb()->clear_voted_for();
  RETURN_NOT_OK(cmeta_->Flush());
  return Status::OK();
}

const uint64_t ReplicaState::GetCurrentTermUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return cmeta_->pb().current_term();
}

const bool ReplicaState::HasVotedCurrentTermUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return cmeta_->pb().has_voted_for();
}

Status ReplicaState::SetVotedForCurrentTermUnlocked(const std::string& uuid) {
  DCHECK(update_lock_.is_locked());
  cmeta_->mutable_pb()->set_voted_for(uuid);
  RETURN_NOT_OK_PREPEND(cmeta_->Flush(),
                        "Unable to flush consensus metadata after recording vote");
  return Status::OK();
}

const std::string& ReplicaState::GetVotedForCurrentTermUnlocked() const {
  DCHECK(update_lock_.is_locked());
  DCHECK(cmeta_->pb().has_voted_for());
  return cmeta_->pb().voted_for();
}

ReplicaTransactionFactory* ReplicaState::GetReplicaTransactionFactoryUnlocked() const {
  return txn_factory_;
}

Status ReplicaState::IncrementConfigSeqNoUnlocked() {
  DCHECK(update_lock_.is_locked());
  cmeta_->mutable_pb()->mutable_committed_quorum()->set_seqno(
      cmeta_->pb().committed_quorum().seqno() + 1);
  RETURN_NOT_OK(cmeta_->Flush());
  return Status::OK();
}

const string& ReplicaState::GetPeerUuid() const {
  return peer_uuid_;
}

const ConsensusOptions& ReplicaState::GetOptions() const {
  return options_;
}

int ReplicaState::GetNumPendingTxnsUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return pending_txns_.size();
}

Status ReplicaState::CancelPendingTransactions() {
  {
    UniqueLock lock(&update_lock_);
    if (state_ != kShuttingDown) {
      return Status::IllegalState("Can only wait for pending commits on kShuttingDown state.");
    }
    LOG_WITH_PREFIX(INFO) << "Trying to abort " << pending_txns_.size() << " pending transactions.";
    for (OpIdToRoundMap::iterator iter = pending_txns_.begin();
         iter != pending_txns_.end(); iter++) {
      ConsensusRound* round = (*iter).second;
      // We cancel only transactions whose applies have not yet been triggered.
      if (in_flight_commits_.count((*iter).first) == 0) {
        LOG_WITH_PREFIX(INFO) << "Aborting transaction as it isn't in flight: "
            << (*iter).second->replicate_msg()->ShortDebugString();
        round->GetReplicaCommitContinuation()->Abort();
      } else {
        LOG_WITH_PREFIX(INFO) << "Skipping txn abort as the apply already in flight: "
            << (*iter).second->replicate_msg()->ShortDebugString();
      }
    }
  }
  return Status::OK();
}

Status ReplicaState::WaitForOustandingApplies() {
  {
    UniqueLock lock(&update_lock_);
    if (state_ != kShuttingDown) {
      return Status::IllegalState("Can only wait for pending commits on kShuttingDown state.");
    }
    LOG_WITH_PREFIX(INFO) << "Waiting on " << in_flight_applies_latch_.count()
        << " outstanding applies:";
  }
  in_flight_applies_latch_.Wait();
  LOG_WITH_PREFIX_LK(INFO) << "All local commits completed.";
  return Status::OK();
}

Status ReplicaState::AddPendingOperation(ConsensusRound* round) {
  DCHECK(update_lock_.is_locked());
  if (PREDICT_FALSE(state_ != kRunning)) {
    // Special case when we're configuring and this is a config change, refuse
    // everything else.
    if (round->replicate_msg()->op_type() != CHANGE_CONFIG_OP) {
      return Status::IllegalState("Cannot trigger prepare. Replica is not in kRunning state.");
    }
  }
  InsertOrDie(&pending_txns_, round->replicate_msg()->id(), round);
  return Status::OK();
}


Status ReplicaState::MarkConsensusCommittedUpToUnlocked(const OpId& id) {
  DCHECK(update_lock_.is_locked());
  if (PREDICT_FALSE(state_ == kShuttingDown || state_ == kShutDown)) {
    return Status::ServiceUnavailable("Cannot trigger apply. Replica is shutting down.");
  }
  if (PREDICT_FALSE(state_ != kRunning)) {
    return Status::IllegalState("Cannot trigger apply. Replica is not in kRunning state.");
  }

  // If we already committed up to (or past) 'id' return.
  // This can happen in the case that multiple UpdateConsensus() calls end
  // up in the RPC queue at the same time, and then might get interleaved out
  // of order.
  if (OpIdCompare(last_triggered_apply_, id) >= 0) {
    VLOG_WITH_PREFIX(2)
      << "Already marked ops through " << last_triggered_apply_ << " as committed. "
      << "Now trying to mark " << id << " which would be a no-op.";
    return Status::OK();
  }

  // Start at the operation after the last committed one.
  OpIdToRoundMap::iterator iter = pending_txns_.upper_bound(last_triggered_apply_);
  // Stop at the operation after the last one we must commit.
  OpIdToRoundMap::iterator end_iter = pending_txns_.upper_bound(id);

  for (; iter != end_iter; iter++) {
    ConsensusRound* round = DCHECK_NOTNULL((*iter).second);
    InsertOrDie(&in_flight_commits_, round->id());

    // TODO this is hacky right now, but we should fix this when
    // todd's merged transaction drivers get in.
    if (round->GetReplicaCommitContinuation() != NULL) {
      RETURN_NOT_OK(round->GetReplicaCommitContinuation()->ConsensusCommitted());
    } else {
      RETURN_NOT_OK(callback_pool_->Submit(shared_ptr<Runnable>(
          new OperationCallbackRunnable(round->replicate_callback()))));
    }
  }

  last_triggered_apply_.CopyFrom(id);

  return Status::OK();
}

const OpId& ReplicaState::GetCommittedOpIdUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return last_triggered_apply_;
}

void ReplicaState::UpdateLastReplicatedOpIdUnlocked(const OpId& op_id) {
  DCHECK(update_lock_.is_locked());
  replicated_op_id_.CopyFrom(op_id);
  replicate_watchers_.MarkFinished(op_id, OpIdWaiterSet::MARK_ALL_OPS_BEFORE);
}

const OpId& ReplicaState::GetLastReplicatedOpIdUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return replicated_op_id_;
}

void ReplicaState::UpdateLastReceivedOpIdUnlocked(const OpId& op_id) {
  DCHECK(update_lock_.is_locked());
  DCHECK_LE(OpIdCompare(received_op_id_, op_id), 0)
    << "Previously received OpId: " << received_op_id_.ShortDebugString()
    << ", updated OpId: " << op_id.ShortDebugString()
    << ", Trace:" << std::endl << Trace::CurrentTrace()->DumpToString(true);
  received_op_id_ = op_id;
  next_index_ = op_id.index() + 1;
}

const OpId& ReplicaState::GetLastReceivedOpIdUnlocked() const {
  DCHECK(update_lock_.is_locked());
  return received_op_id_;
}

void ReplicaState::UpdateCommittedOpIdUnlocked(const OpId& committed_op_id) {
  DCHECK(update_lock_.is_locked());
  CHECK_EQ(in_flight_commits_.erase(committed_op_id), 1)
    << "Trying to mark " << committed_op_id.ShortDebugString() << " as committed, but not "
    << "in the in-flight set";
  CHECK(EraseKeyReturnValuePtr(&pending_txns_, committed_op_id))
    << "Couldn't remove " << committed_op_id.ShortDebugString() << " from the pending set";
  commit_watchers_.MarkFinished(committed_op_id, OpIdWaiterSet::MARK_ONLY_THIS_OP);
}

void ReplicaState::CountDownOutstandingCommitsIfShuttingDown() {
  if (PREDICT_FALSE(state_ == kShuttingDown)) {
    in_flight_applies_latch_.CountDown();
  }
}

Status ReplicaState::RegisterOnReplicateCallback(
    const OpId& replicate_op_id,
    const shared_ptr<FutureCallback>& repl_callback) {
  UniqueLock lock(&update_lock_);
  if (PREDICT_TRUE(OpIdCompare(replicate_op_id, replicated_op_id_) > 0)) {
    replicate_watchers_.RegisterCallback(replicate_op_id, repl_callback);
    return Status::OK();
  }
  return Status::AlreadyPresent("The operation has already been replicated.");
}

Status ReplicaState::RegisterOnCommitCallback(const OpId& op_id,
                                              const shared_ptr<FutureCallback>& commit_callback) {
  UniqueLock lock(&update_lock_);
  if (PREDICT_TRUE(OpIdCompare(op_id, replicated_op_id_) > 0)) {
    commit_watchers_.RegisterCallback(op_id, commit_callback);
    return Status::OK();
  }
  if (FindOrNull(pending_txns_, op_id) != NULL) {
    commit_watchers_.RegisterCallback(op_id, commit_callback);
    return Status::OK();
  }
  return Status::AlreadyPresent("The operation has already been committed.");
}

void ReplicaState::NewIdUnlocked(OpId* id) {
  DCHECK(update_lock_.is_locked());
  id->set_term(GetCurrentTermUnlocked());
  id->set_index(next_index_++);
}

void ReplicaState::CancelPendingOperation(const OpId& id) {
  DCHECK(update_lock_.is_locked());
  CHECK_EQ(GetCurrentTermUnlocked(), id.term());
  CHECK_EQ(next_index_, id.index() + 1);
  next_index_ = id.index();
  ignore_result(DCHECK_NOTNULL(EraseKeyReturnValuePtr(&pending_txns_, id)));
}

string ReplicaState::LogPrefix() {
  ReplicaState::UniqueLock lock;
  CHECK_OK(LockForRead(&lock));
  return LogPrefixUnlocked();
}

string ReplicaState::LogPrefixUnlocked() const {
  return Substitute("T $0 P $1 [$2]: ",
                    options_.tablet_id,
                    GetPeerUuid(),
                    QuorumPeerPB::Role_Name(active_quorum_state_->role));
}

ReplicaState::State ReplicaState::state() const {
  DCHECK(update_lock_.is_locked());
  return state_;
}

string ReplicaState::ToString() const {
  ReplicaState::UniqueLock lock(&update_lock_);
  return ToStringUnlocked();
}

string ReplicaState::ToStringUnlocked() const {
  DCHECK(update_lock_.is_locked());
  QuorumPeerPB::Role role = QuorumPeerPB::NON_PARTICIPANT;
  if (active_quorum_state_) {
    role = active_quorum_state_->role;
  }
  string ret;
  SubstituteAndAppend(&ret, "Replica: $0, State: $1, Role: $2\n",
                      peer_uuid_, state_,
                      QuorumPeerPB::Role_Name(role));

  SubstituteAndAppend(&ret, "Watermarks: {Received: $0 Replicated: $1 Committed: $2}\n",
                      received_op_id_.ShortDebugString(),
                      replicated_op_id_.ShortDebugString(),
                      last_triggered_apply_.ShortDebugString());

  SubstituteAndAppend(&ret, "Num. outstanding commits: $0 IsLocked: $1",
                      in_flight_commits_.size(), update_lock_.is_locked());
  return ret;
}

void ReplicaState::ResetActiveQuorumStateUnlocked(const metadata::QuorumPB& quorum) {
  DCHECK(update_lock_.is_locked());
  active_quorum_state_ = QuorumState::Build(quorum, peer_uuid_).Pass();
}

//////////////////////////////////////////////////
// OperationCallbackRunnable
//////////////////////////////////////////////////

OperationCallbackRunnable::OperationCallbackRunnable(const shared_ptr<FutureCallback>& callback)
  : callback_(callback) {}

void OperationCallbackRunnable::set_error(const Status& error) {
  lock_guard<simple_spinlock> lock(&lock_);
  error_ = error;
}

void OperationCallbackRunnable::Run() {
  lock_guard<simple_spinlock> lock(&lock_);
  if (PREDICT_TRUE(error_.ok())) {
    callback_->OnSuccess();
    return;
  }
  callback_->OnFailure(error_);
}


MajorityOpStatusTracker::MajorityOpStatusTracker(gscoped_ptr<ReplicateMsg> replicate_msg,
                                                 const unordered_set<string>& voting_peers,
                                                 int majority,
                                                 int total_peers_count)
    : OperationStatusTracker(replicate_msg.Pass()),
      majority_(majority),
      voting_peers_(voting_peers),
      total_peers_count_(total_peers_count),
      replicated_count_(0),
      completion_latch_(majority) {
}

void MajorityOpStatusTracker::AckPeer(const string& uuid) {
  CHECK(!uuid.empty()) << "Peer acked with empty uuid";
  lock_guard<simple_spinlock> lock(&lock_);
  if (voting_peers_.count(uuid) != 0) {
    completion_latch_.CountDown();
  }
  replicated_count_++;
  if (PREDICT_FALSE(VLOG_IS_ON(2))) {
    VLOG(2) << "Peer: " << uuid << " ACK'd " << ToStringUnlocked();
  }
  DCHECK_LE(replicated_count_, total_peers_count_)
    << "More replicates than expected. " << ToStringUnlocked()
    << "; Quorum: " << JoinStringsIterator(voting_peers_.begin(), voting_peers_.end(), ", ");
}

bool MajorityOpStatusTracker::IsDone() const {
  return completion_latch_.count() == 0;
}

bool MajorityOpStatusTracker::IsAllDone() const {
  lock_guard<simple_spinlock> lock(&lock_);
  return replicated_count_ >= total_peers_count_;
}

void MajorityOpStatusTracker::Wait() {
  completion_latch_.Wait();
}

MajorityOpStatusTracker::~MajorityOpStatusTracker() {
  if (!IsDone()) {
    LOG(WARNING) << "Deleting incomplete Operation: " << ToString();
  }
}

std::string MajorityOpStatusTracker::ToString() const {
  lock_guard<simple_spinlock> lock(&lock_);
  return ToStringUnlocked();
}

std::string MajorityOpStatusTracker::ToStringUnlocked() const {
  return Substitute("MajorityOpStatusTracker: Id: $0 IsDone: $1 All Peers: $2, Voting Peers: $3, "
                    "ACK'd Peers: $4, Majority: $5",
                    op_id().ShortDebugString(),
                    IsDone(),
                    total_peers_count_,
                    voting_peers_.size(),
                    replicated_count_,
                    majority_);
}

}  // namespace consensus
}  // namespace kudu

