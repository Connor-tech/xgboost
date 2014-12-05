/*!
 * \file allreduce_robust.cc
 * \brief Robust implementation of Allreduce
 *
 * \author Tianqi Chen, Ignacio Cano, Tianyi Zhou
 */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE
#define NOMINMAX
#include <limits>
#include <utility>
#include "./io.h"
#include "./utils.h"
#include "./allreduce_robust.h"

namespace rabit {
namespace engine {
AllreduceRobust::AllreduceRobust(void) {
  result_buffer_round = 1;
  num_local_replica = 2;
  seq_counter = 0;
}
/*! \brief shutdown the engine */
void AllreduceRobust::Shutdown(void) {
  // need to sync the exec before we shutdown, do a pesudo check point
  // execute checkpoint, note: when checkpoint existing, load will not happen
  utils::Assert(RecoverExec(NULL, 0, ActionSummary::kCheckPoint, ActionSummary::kMaxSeq),
                "check point must return true");
  // reset result buffer
  resbuf.Clear(); seq_counter = 0;  
  // execute check ack step, load happens here
  utils::Assert(RecoverExec(NULL, 0, ActionSummary::kCheckAck, ActionSummary::kMaxSeq),
                "check ack must return true");    
  AllreduceBase::Shutdown();
}
/*!
 * \brief set parameters to the engine 
 * \param name parameter name
 * \param val parameter value
 */
void AllreduceRobust::SetParam(const char *name, const char *val) {
  AllreduceBase::SetParam(name, val);
  if (!strcmp(name, "result_buffer_round")) result_buffer_round = atoi(val);
  if (!strcmp(name, "result_replicate")) {
    result_buffer_round = std::max(world_size / atoi(val), 1);
  }
}
/*!
 * \brief perform in-place allreduce, on sendrecvbuf 
 *        this function is NOT thread-safe
 * \param sendrecvbuf_ buffer for both sending and recving data
 * \param type_nbytes the unit number of bytes the type have
 * \param count number of elements to be reduced
 * \param reducer reduce function
 */
void AllreduceRobust::Allreduce(void *sendrecvbuf_,
                                size_t type_nbytes,
                                size_t count,
                                ReduceFunction reducer) {
  bool recovered = RecoverExec(sendrecvbuf_, type_nbytes * count, 0, seq_counter);
  // now we are free to remove the last result, if any
  if (resbuf.LastSeqNo() != -1 &&
      (resbuf.LastSeqNo() % result_buffer_round != rank % result_buffer_round)) {
    resbuf.DropLast();
  }
  void *temp = resbuf.AllocTemp(type_nbytes, count);
  while (true) {
    if (recovered) {
      std::memcpy(temp, sendrecvbuf_, type_nbytes * count); break;
    } else {
      std::memcpy(temp, sendrecvbuf_, type_nbytes * count);
      if (CheckAndRecover(TryAllreduce(temp, type_nbytes, count, reducer))) {
        std::memcpy(sendrecvbuf_, temp, type_nbytes * count); break;
      } else {
        recovered = RecoverExec(sendrecvbuf_, type_nbytes * count, 0, seq_counter);
      }
    } 
  }
  resbuf.PushTemp(seq_counter, type_nbytes, count);
  seq_counter += 1;
}
/*!
 * \brief broadcast data from root to all nodes
 * \param sendrecvbuf_ buffer for both sending and recving data
 * \param size the size of the data to be broadcasted
 * \param root the root worker id to broadcast the data
 */
void AllreduceRobust::Broadcast(void *sendrecvbuf_, size_t total_size, int root) {
  bool recovered = RecoverExec(sendrecvbuf_, total_size, 0, seq_counter);
  // now we are free to remove the last result, if any
  if (resbuf.LastSeqNo() != -1 &&
      (resbuf.LastSeqNo() % result_buffer_round != rank % result_buffer_round)) {
    resbuf.DropLast();
  }
  void *temp = resbuf.AllocTemp(1, total_size);
  while (true) {
    if (recovered) {
      std::memcpy(temp, sendrecvbuf_, total_size); break;
    } else {
      if (CheckAndRecover(TryBroadcast(sendrecvbuf_, total_size, root))) {
        std::memcpy(temp, sendrecvbuf_, total_size); break;
      } else {
        recovered = RecoverExec(sendrecvbuf_, total_size, 0, seq_counter);
      }
    } 
  }
  resbuf.PushTemp(seq_counter, 1, total_size);
  seq_counter += 1;
}
/*!
 * \brief load latest check point
 * \param global_model pointer to the globally shared model/state
 *   when calling this function, the caller need to gauranttees that global_model
 *   is the same in all nodes
 * \param local_model pointer to local model, that is specific to current node/rank
 *   this can be NULL when no local model is needed
 *
 * \return the version number of check point loaded
 *     if returned version == 0, this means no model has been CheckPointed
 *     the p_model is not touched, user should do necessary initialization by themselves
 *   
 *   Common usage example:
 *      int iter = rabit::LoadCheckPoint(&model);
 *      if (iter == 0) model.InitParameters();
 *      for (i = iter; i < max_iter; ++i) {
 *        do many things, include allreduce
 *        rabit::CheckPoint(model);
 *      } 
 *
 * \sa CheckPoint, VersionNumber
 */
int AllreduceRobust::LoadCheckPoint(utils::ISerializable *global_model,
                                    utils::ISerializable *local_model) {
  utils::Check(local_model == NULL, "CheckPoint local_model is not yet supported");
  // check if we succesfll
  if (RecoverExec(NULL, 0, ActionSummary::kLoadCheck, ActionSummary::kMaxSeq)) {
    // reset result buffer
    resbuf.Clear(); seq_counter = 0;
    // load from buffer
    utils::MemoryBufferStream fs(&global_checkpoint);
    fs.Read(&version_number, sizeof(version_number));
    if (version_number == 0) return version_number;
    global_model->Load(fs);
    // run another phase of check ack, if recovered from data
    utils::Assert(RecoverExec(NULL, 0, ActionSummary::kCheckAck, ActionSummary::kMaxSeq),
                  "check ack must return true");
    return version_number;
  } else {
    // reset result buffer
    resbuf.Clear(); seq_counter = 0;
    // nothing loaded, a fresh start, everyone init model
    return false;
  }
}
/*!
 * \brief checkpoint the model, meaning we finished a stage of execution
 *  every time we call check point, there is a version number which will increase by one
 *
 * \param global_model pointer to the globally shared model/state
 *   when calling this function, the caller need to gauranttees that global_model
 *   is the same in all nodes
 * \param local_model pointer to local model, that is specific to current node/rank
 *   this can be NULL when no local state is needed
 *
 * NOTE: local_model requires explicit replication of the model for fault-tolerance, which will
 *       bring replication cost in CheckPoint function. global_model do not need explicit replication.
 *       So only CheckPoint with global_model if possible
 *
 * \sa LoadCheckPoint, VersionNumber
 */
void AllreduceRobust::CheckPoint(const utils::ISerializable *global_model,
                                 const utils::ISerializable *local_model) {
  utils::Assert(local_model == NULL, "CheckPoint local model is not supported yet");
  // execute checkpoint, note: when checkpoint existing, load will not happen
  utils::Assert(RecoverExec(NULL, 0, ActionSummary::kCheckPoint, ActionSummary::kMaxSeq),
                "check point must return true");
  // this is the critical region where we will change all the stored models
  // increase version number
  version_number += 1;
  // save model
  global_checkpoint.resize(0);
  utils::MemoryBufferStream fs(&global_checkpoint);
  fs.Write(&version_number, sizeof(version_number));
  global_model->Save(fs);
  // reset result buffer
  resbuf.Clear(); seq_counter = 0;  
  // execute check ack step, load happens here
  utils::Assert(RecoverExec(NULL, 0, ActionSummary::kCheckAck, ActionSummary::kMaxSeq),
                "check ack must return true");  
}
/*!
 * \brief reset the all the existing links by sending Out-of-Band message marker
 *  after this function finishes, all the messages received and sent before in all live links are discarded,
 *  This allows us to get a fresh start after error has happened
 *
 * \return this function can return kSuccess or kSockError
 *         when kSockError is returned, it simply means there are bad sockets in the links,
 *         and some link recovery proceduer is needed
 */
AllreduceRobust::ReturnType AllreduceRobust::TryResetLinks(void) {
  // number of links
  const int nlink = static_cast<int>(links.size());
  for (int i = 0; i < nlink; ++i) {
    links[i].InitBuffer(sizeof(int), 1 << 10, reduce_buffer_size);
    links[i].ResetSize();
  }  
  // read and discard data from all channels until pass mark
  while (true) {
    for (int i = 0; i < nlink; ++i) {
      if (links[i].sock.BadSocket()) continue;
      if (links[i].size_write == 0) {
        char sig = kOOBReset;
        ssize_t len = links[i].sock.Send(&sig, sizeof(sig), MSG_OOB);
        // error will be filtered in next loop
        if (len == sizeof(sig)) links[i].size_write = 1;
      }
      if (links[i].size_write == 1) {
        char sig = kResetMark;
        ssize_t len = links[i].sock.Send(&sig, sizeof(sig));
        if (len == sizeof(sig)) links[i].size_write = 2;
      }
    }
    utils::SelectHelper rsel;
    bool finished = true;
    for (int i = 0; i < nlink; ++i) {
      if (links[i].size_write != 2 && !links[i].sock.BadSocket()) {
        rsel.WatchWrite(links[i].sock); finished = false;
      }
    }
    if (finished) break;
    // wait to read from the channels to discard data
    rsel.Select();
  }
  for (int i = 0; i < nlink; ++i) {
    if (!links[i].sock.BadSocket()) {
      utils::SelectHelper::WaitExcept(links[i].sock);
    }
  }
  while (true) {
    for (int i = 0; i < nlink; ++i) {
      if (links[i].size_read == 0) {
        int atmark = links[i].sock.AtMark();
        if (atmark < 0) {
          utils::Assert(links[i].sock.BadSocket(), "must already gone bad");
        } else if (atmark > 0) {
          links[i].size_read = 1;
        } else {
          // no at mark, read and discard data
          ssize_t len = links[i].sock.Recv(links[i].buffer_head, links[i].buffer_size);
          if (links[i].sock.AtMark()) links[i].size_read = 1;
          // zero length, remote closed the connection, close socket
          if (len == 0) links[i].sock.Close();
        }
      }
    }
    utils::SelectHelper rsel;
    bool finished = true;    
    for (int i = 0; i < nlink; ++i) {
      if (links[i].size_read == 0 && !links[i].sock.BadSocket()) {
        rsel.WatchRead(links[i].sock); finished = false;
      }
    }
    if (finished) break;
    rsel.Select();
  }

  // start synchronization, use blocking I/O to avoid select
  for (int i = 0; i < nlink; ++i) {
    if (!links[i].sock.BadSocket()) {
      char oob_mark;
      links[i].sock.SetNonBlock(false);
      ssize_t len = links[i].sock.Recv(&oob_mark, sizeof(oob_mark), MSG_WAITALL);
      if (len == 0) {
        links[i].sock.Close(); continue;
      } else if (len > 0) {
        utils::Assert(oob_mark == kResetMark, "wrong oob msg");
        utils::Assert(links[i].sock.AtMark() != 1, "should already read past mark");
      } else {
        utils::Assert(errno != EAGAIN|| errno != EWOULDBLOCK, "BUG");
      }
      // send out ack
      char ack = kResetAck;
      while (true) {
        len = links[i].sock.Send(&ack, sizeof(ack));
        if (len == sizeof(ack)) break;
        if (len == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        }
      }
    }
  }
  // wait all ack
  for (int i = 0; i < nlink; ++i) {
    if (!links[i].sock.BadSocket()) {
      char ack;
      ssize_t len = links[i].sock.Recv(&ack, sizeof(ack), MSG_WAITALL);
      if (len == 0) {
        links[i].sock.Close(); continue;
      } else if (len > 0) {
        utils::Assert(ack == kResetAck, "wrong Ack MSG");
      } else {
        utils::Assert(errno != EAGAIN|| errno != EWOULDBLOCK, "BUG");
      }
      // set back to nonblock mode
      links[i].sock.SetNonBlock(true);
    }
  }
  for (int i = 0; i < nlink; ++i) {
    if (links[i].sock.BadSocket()) return kSockError;
  }
  return kSuccess;
}
/*!
 * \brief if err_type indicates an error
 *         recover links according to the error type reported
 *        if there is no error, return true
 * \param err_type the type of error happening in the system
 * \return true if err_type is kSuccess, false otherwise 
 */
bool AllreduceRobust::CheckAndRecover(ReturnType err_type) {
  if (err_type == kSuccess) return true;
  // simple way, shutdown all links
  for (size_t i = 0; i < links.size(); ++i) {
    if (!links[i].sock.BadSocket()) links[i].sock.Close();
  }
  ReConnectLinks("recover");  
  return false;
  // this was old way
  while(err_type != kSuccess) {
    switch(err_type) {
      case kGetExcept: err_type = TryResetLinks(); break;
      case kSockError: {
        TryResetLinks();
        ReConnectLinks();
        err_type = kSuccess;
        break;
      }
      default: utils::Assert(false, "RecoverLinks: cannot reach here");
    }
  }
  return false;
}
/*!
 * \brief message passing function, used to decide the
 *        shortest distance to the possible source of data
 * \param node_value a pair of have_data and size
 *           have_data whether current node have data
 *           size gives the size of data, if current node is kHaveData
 * \param dist_in the shorest to any data source distance in each direction
 * \param out_index the edge index of output link
 * \return the shorest distance result of out edge specified by out_index
 */
inline std::pair<int,size_t>
ShortestDist(const std::pair<bool, size_t> &node_value,
             const std::vector< std::pair<int, size_t> > &dist_in,
             size_t out_index) {
  if (node_value.first) {
    return std::make_pair(1, node_value.second);
  }
  size_t size = 0;
  int res = std::numeric_limits<int>::max();
  for (size_t i = 0; i < dist_in.size(); ++i) {
    if (i == out_index) continue;
    if (dist_in[i].first == std::numeric_limits<int>::max()) continue;
    if (dist_in[i].first + 1 < res) {
      res = dist_in[i].first + 1;
      size = dist_in[i].second;
    } 
  }
  // add one hop

  return std::make_pair(res, size);
}
/*!
 * \brief message passing function, used to decide the
 *    data request from each edge, whether need to request data from certain edge
 * \param node_value a pair of request_data and best_link
 *           request_data stores whether current node need to request data 
 *           best_link gives the best edge index to fetch the data
 * \param req_in the data request from incoming edges
 * \param out_index the edge index of output link
 * \return the request to the output edge
 */
inline char DataRequest(const std::pair<bool, int> &node_value,
                        const std::vector<char> &req_in,
                        size_t out_index) {
  // whether current node need to request data
  bool request_data = node_value.first;
  // which edge index is the best link to request data
  // can be -1, which means current node contains data
  const int best_link = node_value.second;
  if (static_cast<int>(out_index) == best_link) {
    if (request_data) return 1;
    for (size_t i = 0; i < req_in.size(); ++i) {
      if (i == out_index) continue;
      if (req_in[i] != 0) return 1;
    }
  }
  return 0;
}
/*!
 * \brief try to decide the recovery message passing request
 * \param role the current role of the node
 * \param p_size used to store the size of the message, for node in state kHaveData,
 *               this size must be set correctly before calling the function
 *               for others, this surves as output parameter
 *
 * \param p_recvlink used to store the link current node should recv data from, if necessary
 *          this can be -1, which means current node have the data
 * \param p_req_in used to store the resulting vector, indicating which link we should send the data to
 *
 * \return this function can return kSuccess/kSockError/kGetExcept, see ReturnType for details
 * \sa ReturnType
 */  
AllreduceRobust::ReturnType
AllreduceRobust::TryDecideRouting(AllreduceRobust::RecoverType role,
                                  size_t *p_size,
                                  int *p_recvlink,
                                  std::vector<bool> *p_req_in) {
  int best_link = -2;
  {// get the shortest distance to the request point
    std::vector< std::pair<int,size_t> > dist_in, dist_out;    
    ReturnType succ = MsgPassing(std::make_pair(role == kHaveData, *p_size),
                                 &dist_in, &dist_out, ShortestDist);
    if (succ != kSuccess) return succ;
    if (role != kHaveData) {
      for (size_t i = 0; i < dist_in.size(); ++i) {
        if (dist_in[i].first != std::numeric_limits<int>::max()) {
          utils::Check(best_link == -2 || *p_size == dist_in[i].second,
                       "[%d] Allreduce size inconsistent, distin=%lu, size=%lu, reporting=%lu\n",
                       rank, dist_in[i].first, *p_size, dist_in[i].second);
          if (best_link == -2 || dist_in[i].first < dist_in[best_link].first) {
            best_link = static_cast<int>(i);
            *p_size = dist_in[i].second;
          }
        }
      }
      utils::Check(best_link != -2, "Too many nodes went down and we cannot recover..");
    } else {
      best_link = -1;
    }
  }
  // get the node request
  std::vector<char> req_in, req_out;
  ReturnType succ = MsgPassing(std::make_pair(role == kRequestData, best_link),
                               &req_in, &req_out, DataRequest);
  if (succ != kSuccess) return succ;
  // set p_req_in
  p_req_in->resize(req_in.size());  
  for (size_t i = 0; i < req_in.size(); ++i) {
    // set p_req_in
    (*p_req_in)[i] = (req_in[i] != 0);
    if (req_out[i] != 0) {
      utils::Assert(req_in[i] == 0, "cannot get and receive request");
      utils::Assert(static_cast<int>(i) == best_link, "request result inconsistent");
    }
  }
  *p_recvlink = best_link;
  return kSuccess;
}
/*!
 * \brief try to finish the data recovery request,
 *        this function is used together with TryDecideRouting
 * \param role the current role of the node
 * \param sendrecvbuf_ the buffer to store the data to be sent/recived
 *          - if the role is kHaveData, this stores the data to be sent
 *          - if the role is kRequestData, this is the buffer to store the result
 *          - if the role is kPassData, this will not be used, and can be NULL
 * \param size the size of the data, obtained from TryDecideRouting
 * \param recv_link the link index to receive data, if necessary, obtained from TryDecideRouting
 * \param req_in the request of each link to send data, obtained from TryDecideRouting
 *
 * \return this function can return kSuccess/kSockError/kGetExcept, see ReturnType for details
 * \sa ReturnType, TryDecideRouting
 */  
AllreduceRobust::ReturnType
AllreduceRobust::TryRecoverData(RecoverType role,
                                void *sendrecvbuf_,
                                size_t size,
                                int recv_link,
                                const std::vector<bool> &req_in) {
  // no need to run recovery for zero size message
  if (links.size() == 0 || size == 0) return kSuccess;
  utils::Assert(req_in.size() == links.size(), "TryRecoverData");
  const int nlink = static_cast<int>(links.size());
  {
    bool req_data = role == kRequestData;
    for (int i = 0; i < nlink; ++i) {
      if (req_in[i]) {
        utils::Assert(i != recv_link, "TryDecideRouting");
        req_data = true;
      }
    }
    // do not need to provide data or receive data, directly exit
    if (!req_data) return kSuccess;
  }
  utils::Assert(recv_link >= 0 || role == kHaveData, "recv_link must be active");
  for (int i = 0; i < nlink; ++i) {
    links[i].ResetSize();
  }
  while (true) {
    bool finished = true;
    utils::SelectHelper selecter;
    for (int i = 0; i < nlink; ++i) {
      if (i == recv_link && links[i].size_read != size) {
        selecter.WatchRead(links[i].sock);
        finished = false;
      }      
      if (req_in[i] && links[i].size_write != size) {
        if (role == kHaveData ||
            (role == kPassData && links[recv_link].size_read != links[i].size_write)) {
          selecter.WatchWrite(links[i].sock);
        }
        finished = false;
      }
      selecter.WatchException(links[i].sock);
    }
    if (finished) break;
    selecter.Select();
    // exception handling
    for (int i = 0; i < nlink; ++i) {
      if (selecter.CheckExcept(links[i].sock)) return kGetExcept;
    }
    if (role == kRequestData) {
      const int pid = recv_link;
      if (selecter.CheckRead(links[pid].sock)) {
        if(!links[pid].ReadToArray(sendrecvbuf_, size)) return kSockError;
      }
      for (int i = 0; i < nlink; ++i) {
        if (req_in[i] && links[i].size_write != links[pid].size_read &&
            selecter.CheckWrite(links[i].sock)) {
          if(!links[i].WriteFromArray(sendrecvbuf_, links[pid].size_read)) return kSockError;
        }
      }
    }
    if (role == kHaveData) {
      for (int i = 0; i < nlink; ++i) {
        if (req_in[i] && selecter.CheckWrite(links[i].sock)) {
          if(!links[i].WriteFromArray(sendrecvbuf_, size)) return kSockError;
        }
      }
    }
    if (role == kPassData) {
      const int pid = recv_link;
      const size_t buffer_size = links[pid].buffer_size;
      if (selecter.CheckRead(links[pid].sock)) {
        size_t min_write = size;
        for (int i = 0; i < nlink; ++i) {
          if (req_in[i]) min_write = std::min(links[i].size_write, min_write);
        }
        utils::Assert(min_write <= links[pid].size_read, "boundary check");
        if (!links[pid].ReadToRingBuffer(min_write)) return kSockError;    
      }
      for (int i = 0; i < nlink; ++i) {        
        if (req_in[i] && selecter.CheckWrite(links[i].sock) && links[pid].size_read != links[i].size_write) {
          size_t start = links[i].size_write % buffer_size;
          // send out data from ring buffer
          size_t nwrite = std::min(buffer_size - start, links[pid].size_read - links[i].size_write);          
          ssize_t len = links[i].sock.Send(links[pid].buffer_head + start, nwrite);
          if (len != -1) {
            links[i].size_write += len;
          } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) return kSockError;
          }
        }
      }
    }
  }
  return kSuccess;
}
/*!
 * \brief try to load check point
 *
 *        This is a collaborative function called by all nodes
 *        only the nodes with requester set to true really needs to load the check point
 *        other nodes acts as collaborative roles to complete this request
 *
 * \param requester whether current node is the requester
 * \return this function can return kSuccess/kSockError/kGetExcept, see ReturnType for details
 * \sa ReturnType
 */
AllreduceRobust::ReturnType AllreduceRobust::TryLoadCheckPoint(bool requester) {
  RecoverType role =  requester ? kRequestData : kHaveData;
  size_t size = this->global_checkpoint.length();
  int recv_link;
  std::vector<bool> req_in;
  ReturnType succ = TryDecideRouting(role, &size, &recv_link, &req_in);
  if (succ != kSuccess) return succ;
  if (role == kRequestData) {
    global_checkpoint.resize(size);
  }
  if (size == 0) return kSuccess;
  return TryRecoverData(role, &global_checkpoint[0], size, recv_link, req_in);
}
/*!
 * \brief try to get the result of operation specified by seqno
 *
 *        This is a collaborative function called by all nodes
 *        only the nodes with requester set to true really needs to get the result
 *        other nodes acts as collaborative roles to complete this request
 *
 * \param buf the buffer to store the result, this parameter is only used when current node is requester
 * \param size the total size of the buffer, this parameter is only used when current node is requester
 * \param seqno sequence number of the operation, this is unique index of a operation in current iteration
 * \param requester whether current node is the requester
 * \return this function can return kSuccess/kSockError/kGetExcept, see ReturnType for details
 * \sa ReturnType
 */
AllreduceRobust::ReturnType
AllreduceRobust::TryGetResult(void *sendrecvbuf, size_t size, int seqno, bool requester) {  RecoverType role;
  if (!requester) {
    sendrecvbuf = resbuf.Query(seqno, &size);
    role = sendrecvbuf != NULL ? kHaveData : kPassData;
  } else {
    role = kRequestData;
  }
  int recv_link;
  std::vector<bool> req_in;
  ReturnType succ = TryDecideRouting(role, &size, &recv_link, &req_in);
  if (succ != kSuccess) return succ;
  utils::Check(size != 0, "zero size check point is not allowed");  
  return TryRecoverData(role, sendrecvbuf, size, recv_link, req_in);
}
/*!
 * \brief try to run recover execution for a request action described by flag and seqno,
 *        the function will keep blocking to run possible recovery operations before the specified action,
 *        until the requested result is received by a recovering procedure,
 *        or the function discovers that the requested action is not yet executed, and return false
 *
 * \param buf the buffer to store the result
 * \param size the total size of the buffer
 * \param flag flag information about the action \sa ActionSummary
 * \param seqno sequence number of the action, if it is special action with flag set, 
 *              seqno needs to be set to ActionSummary::kMaxSeq
 *
 * \return if this function can return true or false 
 *    - true means buf already set to the
 *           result by recovering procedure, the action is complete, no further action is needed
 *    - false means this is the lastest action that has not yet been executed, need to execute the action
 */
bool AllreduceRobust::RecoverExec(void *buf, size_t size, int flag, int seqno) {
  if (flag != 0) {
    utils::Assert(seqno == ActionSummary::kMaxSeq, "must only set seqno for normal operations");
  }
  // request
  ActionSummary req(flag, seqno);
  while (true) {
    // action
    ActionSummary act = req;    
    // get the reduced action
    if (!CheckAndRecover(TryAllreduce(&act, sizeof(act), 1, ActionSummary::Reducer))) continue;
    if (act.check_ack()) {
      if (act.check_point()) {
        // if we also have check_point, do check point first
        utils::Assert(!act.diff_seq(),
                      "check ack & check pt  cannot occur together with normal ops");
        // if we requested checkpoint, we are free to go
        if (req.check_point()) return true;
      } else if (act.load_check()) {
        // if there is only check_ack and load_check, do load_check
        if (!CheckAndRecover(TryLoadCheckPoint(req.load_check()))) continue;
        // if requested load check, then misson complete
        if (req.load_check()) return true;
      } else {
        // there is no check point and no load check, execute check ack
        if (req.check_ack()) return true;
      }
      // if execute to this point
      // this means the action requested has not been completed
      // try next round
    } else {
      if (act.check_point()) {
        if (act.diff_seq()) {
          utils::Assert(act.min_seqno() != ActionSummary::kMaxSeq, "min seq bug");
          bool requester = req.min_seqno() == act.min_seqno();
          if (!CheckAndRecover(TryGetResult(buf, size, act.min_seqno(), requester))) continue;
          if (requester) return true;
        } else  {
          // no difference in seq no, means we are free to check point
          if (req.check_point()) return true;
        }
      } else {
        // no check point
        if (act.load_check()) {
          // all the nodes called load_check, this is an incomplete action
          if (!act.diff_seq()) return false;
          // load check have higher priority, do load_check
          if (!CheckAndRecover(TryLoadCheckPoint(req.load_check()))) continue;
          // if requested load check, then misson complete
          if (req.load_check()) return true;
        } else {
          // no special flags, no checkpoint, check ack, load_check
          utils::Assert(act.min_seqno() != ActionSummary::kMaxSeq, "min seq bug");
          if (act.diff_seq()) {
            bool requester = req.min_seqno() == act.min_seqno();
            if (!CheckAndRecover(TryGetResult(buf, size, act.min_seqno(), requester))) continue;
            if (requester) return true;
          } else {
            // all the request is same, this is most recent command that is yet to be executed
            return false;
          }
        }
      }
      // something is still incomplete try next round
    }
  }
  utils::Assert(false, "RecoverExec: should not reach here");
  return true;
}

/*!
 * \brief perform a ring passing to receive data from prev link, and sent data to next link
 *  this allows data to stream over a ring structure
 *  sendrecvbuf[0:read_ptr] are already provided by current node
 *  current node will recv sendrecvbuf[read_ptr:read_end] from prev link
 *  current node will send sendrecvbuf[write_ptr:write_end] to next link
 *  write_ptr will wait till the data is readed before sending the data
 *  this function requires read_end >= write_end
 *
 * \param sendrecvbuf_ the place to hold the incoming and outgoing data
 * \param read_ptr the initial read pointer
 * \param read_end the ending position to read
 * \param write_ptr the initial write pointer
 * \param write_end the ending position to write
 * \param prev pointer to link to previous position in ring
 * \param prev pointer to link of next position in ring
 */
AllreduceRobust::ReturnType
AllreduceRobust::RingPassing(void *sendrecvbuf_,
                             size_t read_ptr,
                             size_t read_end,
                             size_t write_ptr,
                             size_t write_end,
                             LinkRecord *prev_link,
                             LinkRecord *next_link) {
  if (links.size() == 0 || read_end == 0) return kSuccess;
  utils::Assert(read_end <= write_end, "boundary check");
  utils::Assert(read_ptr <= read_end, "boundary check");
  utils::Assert(write_ptr <= write_end, "boundary check");
  // take reference
  LinkRecord &prev = *prev_link, &next = *next_link;
  // send recv buffer
  char *buf = reinterpret_cast<char*>(sendrecvbuf_);  
  while (true) {
    bool finished = true;
    utils::SelectHelper selecter;
    if (read_ptr != read_end) {
      selecter.WatchRead(prev.sock);
      finished = false;
    }
    if (write_ptr < read_ptr && write_ptr != write_end) {
      selecter.WatchWrite(next.sock);
      finished = false;      
    }
    selecter.WatchException(prev.sock);
    selecter.WatchException(next.sock);
    if (finished) break;
    selecter.Select();
    if (selecter.CheckExcept(prev.sock)) return kGetExcept;
    if (selecter.CheckExcept(next.sock)) return kGetExcept;
    if (read_ptr != read_end && selecter.CheckRead(prev.sock)) {
      ssize_t len = prev.sock.Recv(buf + read_ptr, read_end - read_ptr);
      if (len == 0) {
        prev.sock.Close(); return kSockError;
      } 
      if (len != -1) {
        read_ptr += static_cast<size_t>(len);
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return kSockError;
      }
    }
    if (write_ptr != write_end && write_ptr < read_ptr &&
        selecter.CheckWrite(next.sock)) {
      size_t nsend = std::min(write_end - write_ptr, read_ptr - write_ptr);
      ssize_t len = next.sock.Send(buf + write_ptr, nsend);
      if (len != -1) {
        write_ptr += static_cast<size_t>(len);
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return kSockError;
      }
    }
  }
  return kSuccess;
}
}  // namespace engine
}  // namespace rabit

