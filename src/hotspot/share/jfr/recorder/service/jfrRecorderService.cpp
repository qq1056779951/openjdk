/*
 * Copyright (c) 2016, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jfrfiles/jfrEventClasses.hpp"
#include "jfr/jni/jfrJavaSupport.hpp"
#include "jfr/leakprofiler/leakProfiler.hpp"
#include "jfr/leakprofiler/checkpoint/objectSampleCheckpoint.hpp"
#include "jfr/leakprofiler/sampling/objectSampler.hpp"
#include "jfr/recorder/jfrRecorder.hpp"
#include "jfr/recorder/checkpoint/jfrCheckpointManager.hpp"
#include "jfr/recorder/checkpoint/jfrMetadataEvent.hpp"
#include "jfr/recorder/repository/jfrChunkRotation.hpp"
#include "jfr/recorder/repository/jfrChunkWriter.hpp"
#include "jfr/recorder/repository/jfrRepository.hpp"
#include "jfr/recorder/service/jfrPostBox.hpp"
#include "jfr/recorder/service/jfrRecorderService.hpp"
#include "jfr/recorder/stacktrace/jfrStackTraceRepository.hpp"
#include "jfr/recorder/storage/jfrStorage.hpp"
#include "jfr/recorder/storage/jfrStorageControl.hpp"
#include "jfr/recorder/stringpool/jfrStringPool.hpp"
#include "jfr/utilities/jfrAllocation.hpp"
#include "jfr/utilities/jfrTime.hpp"
#include "jfr/writers/jfrJavaEventWriter.hpp"
#include "jfr/utilities/jfrTypes.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"

// incremented on each flushpoint
static u8 flushpoint_id = 0;

template <typename E, typename Instance, size_t(Instance::*func)()>
class Content {
 private:
  Instance& _instance;
  u4 _elements;
 public:
  typedef E EventType;
  Content(Instance& instance) : _instance(instance), _elements(0) {}
  bool process() {
    _elements = (u4)(_instance.*func)();
    return true;
  }
  u4 elements() const { return _elements; }
};

template <typename Content>
class WriteContent : public StackObj {
 protected:
  const JfrTicks _start_time;
  JfrTicks _end_time;
  JfrChunkWriter& _cw;
  Content& _content;
  const int64_t _start_offset;
 public:
  typedef typename Content::EventType EventType;

  WriteContent(JfrChunkWriter& cw, Content& content) :
    _start_time(JfrTicks::now()),
    _end_time(),
    _cw(cw),
    _content(content),
    _start_offset(_cw.current_offset()) {
    assert(_cw.is_valid(), "invariant");
  }

  bool process() {
    // invocation
    _content.process();
    _end_time = JfrTicks::now();
    return 0 != _content.elements();
  }

  const JfrTicks& start_time() const {
    return _start_time;
  }

  const JfrTicks& end_time() const {
    return _end_time;
  }

  int64_t start_offset() const {
    return _start_offset;
  }

  int64_t end_offset() const {
    return current_offset();
  }

  int64_t current_offset() const {
    return _cw.current_offset();
  }

  u4 elements() const {
    return (u4) _content.elements();
  }

  u4 size() const {
    return (u4)(end_offset() - start_offset());
  }

  static bool is_event_enabled() {
    return EventType::is_enabled();
  }

  static u8 event_id() {
    return EventType::eventId;
  }

  void write_elements(int64_t offset) {
    _cw.write_padded_at_offset<u4>(elements(), offset);
  }

  void write_size() {
    _cw.write_padded_at_offset<u4>(size(), start_offset());
  }

  void set_last_checkpoint() {
    _cw.set_last_checkpoint_offset(start_offset());
  }

  void rewind() {
    _cw.seek(start_offset());
  }
};

static int64_t write_checkpoint_event_prologue(JfrChunkWriter& cw, u8 type_id) {
  const int64_t last_cp_offset = cw.last_checkpoint_offset();
  const int64_t delta_to_last_checkpoint = 0 == last_cp_offset ? 0 : last_cp_offset - cw.current_offset();
  cw.reserve(sizeof(u4));
  cw.write<u8>(EVENT_CHECKPOINT);
  cw.write(JfrTicks::now());
  cw.write<u8>(0); // duration
  cw.write(delta_to_last_checkpoint);
  cw.write<u4>(GENERIC); // checkpoint type
  cw.write<u4>(1); // nof types in this checkpoint
  cw.write(type_id);
  return cw.reserve(sizeof(u4));
}

template <typename Content>
class WriteCheckpointEvent : public WriteContent<Content> {
 private:
  const u8 _type_id;
 public:
  WriteCheckpointEvent(JfrChunkWriter& cw, Content& content, u8 type_id) :
    WriteContent<Content>(cw, content), _type_id(type_id) {}

  bool process() {
    const int64_t num_elements_offset = write_checkpoint_event_prologue(this->_cw, _type_id);
    if (!WriteContent<Content>::process()) {
      // nothing to do, rewind writer to start
      this->rewind();
      assert(this->current_offset() == this->start_offset(), "invariant");
      return false;
    }
    assert(this->elements() > 0, "invariant");
    assert(this->current_offset() > num_elements_offset, "invariant");
    this->write_elements(num_elements_offset);
    this->write_size();
    this->set_last_checkpoint();
    return true;
  }
};

template <typename Functor>
static u4 invoke(Functor& f) {
  f.process();
  return f.elements();
}

template <typename Functor>
static void write_flush_event(Functor& f) {
  if (Functor::is_event_enabled()) {
    typename Functor::EventType e(UNTIMED);
    e.set_starttime(f.start_time());
    e.set_endtime(f.end_time());
    e.set_flushId(flushpoint_id);
    e.set_elements(f.elements());
    e.set_size(f.size());
    e.commit();
  }
}

template <typename Functor>
static u4 invoke_with_flush_event(Functor& f) {
  const u4 elements = invoke(f);
  write_flush_event(f);
  return elements;
}

class StackTraceRepository : public StackObj {
 private:
  JfrStackTraceRepository& _repo;
  JfrChunkWriter& _cw;
  size_t _elements;
  bool _clear;

 public:
  typedef EventFlushStacktrace EventType;
  StackTraceRepository(JfrStackTraceRepository& repo, JfrChunkWriter& cw, bool clear) :
    _repo(repo), _cw(cw), _elements(0), _clear(clear) {}
  bool process() {
    _elements = _repo.write(_cw, _clear);
    return true;
  }
  size_t elements() const { return _elements; }
  void reset() { _elements = 0; }
};

typedef WriteCheckpointEvent<StackTraceRepository> WriteStackTrace;

static u4 flush_stacktrace(JfrStackTraceRepository& stack_trace_repo, JfrChunkWriter& chunkwriter) {
  StackTraceRepository str(stack_trace_repo, chunkwriter, false);
  WriteStackTrace wst(chunkwriter, str, TYPE_STACKTRACE);
  return invoke_with_flush_event(wst);
}

static u4 write_stacktrace(JfrStackTraceRepository& stack_trace_repo, JfrChunkWriter& chunkwriter, bool clear) {
  StackTraceRepository str(stack_trace_repo, chunkwriter, clear);
  WriteStackTrace wst(chunkwriter, str, TYPE_STACKTRACE);
  return invoke(wst);
}

typedef Content<EventFlushStorage, JfrStorage, &JfrStorage::write> Storage;
typedef WriteContent<Storage> WriteStorage;

static size_t flush_storage(JfrStorage& storage, JfrChunkWriter& chunkwriter) {
  assert(chunkwriter.is_valid(), "invariant");
  Storage fsf(storage);
  WriteStorage fs(chunkwriter, fsf);
  return invoke_with_flush_event(fs);
}

static size_t write_storage(JfrStorage& storage, JfrChunkWriter& chunkwriter) {
  assert(chunkwriter.is_valid(), "invariant");
  Storage fsf(storage);
  WriteStorage fs(chunkwriter, fsf);
  return invoke(fs);
}

typedef Content<EventFlushStringPool, JfrStringPool, &JfrStringPool::write> StringPool;
typedef Content<EventFlushStringPool, JfrStringPool, &JfrStringPool::write_at_safepoint> StringPoolSafepoint;
typedef WriteCheckpointEvent<StringPool> WriteStringPool;
typedef WriteCheckpointEvent<StringPoolSafepoint> WriteStringPoolSafepoint;

static u4 flush_stringpool(JfrStringPool& string_pool, JfrChunkWriter& chunkwriter) {
  StringPool sp(string_pool);
  WriteStringPool wsp(chunkwriter, sp, TYPE_STRING);
  return invoke_with_flush_event(wsp);
}

static u4 write_stringpool(JfrStringPool& string_pool, JfrChunkWriter& chunkwriter) {
  StringPool sp(string_pool);
  WriteStringPool wsp(chunkwriter, sp, TYPE_STRING);
  return invoke(wsp);
}

static u4 write_stringpool_safepoint(JfrStringPool& string_pool, JfrChunkWriter& chunkwriter) {
  StringPoolSafepoint sps(string_pool);
  WriteStringPoolSafepoint wsps(chunkwriter, sps, TYPE_STRING);
  return invoke(wsps);
}

typedef Content<EventFlushTypeSet, JfrCheckpointManager, &JfrCheckpointManager::flush_type_set> FlushTypeSetFunctor;
typedef WriteContent<FlushTypeSetFunctor> FlushTypeSet;

static u4 flush_typeset(JfrCheckpointManager& checkpoint_manager, JfrChunkWriter& chunkwriter) {
  FlushTypeSetFunctor flush_type_set(checkpoint_manager);
  FlushTypeSet fts(chunkwriter, flush_type_set);
  return invoke_with_flush_event(fts);
}

class MetadataEvent : public StackObj {
 private:
  JfrChunkWriter& _cw;
 public:
  typedef EventFlushMetadata EventType;
  MetadataEvent(JfrChunkWriter& cw) : _cw(cw) {}
  bool process() {
    JfrMetadataEvent::write(_cw);
    return true;
  }
  size_t elements() const { return 1; }
};

typedef WriteContent<MetadataEvent> WriteMetadata;

static u4 flush_metadata(JfrChunkWriter& chunkwriter) {
  assert(chunkwriter.is_valid(), "invariant");
  MetadataEvent me(chunkwriter);
  WriteMetadata wm(chunkwriter, me);
  return invoke_with_flush_event(wm);
}

static u4 write_metadata(JfrChunkWriter& chunkwriter) {
  assert(chunkwriter.is_valid(), "invariant");
  MetadataEvent me(chunkwriter);
  WriteMetadata wm(chunkwriter, me);
  return invoke(wm);
}

template <typename Instance, void(Instance::*func)()>
class JfrVMOperation : public VM_Operation {
 private:
  Instance& _instance;
 public:
  JfrVMOperation(Instance& instance) : _instance(instance) {}
  void doit() { (_instance.*func)(); }
  VMOp_Type type() const { return VMOp_JFRCheckpoint; }
  Mode evaluation_mode() const { return _safepoint; } // default
};

JfrRecorderService::JfrRecorderService() :
  _checkpoint_manager(JfrCheckpointManager::instance()),
  _chunkwriter(JfrRepository::chunkwriter()),
  _repository(JfrRepository::instance()),
  _stack_trace_repository(JfrStackTraceRepository::instance()),
  _storage(JfrStorage::instance()),
  _string_pool(JfrStringPool::instance()) {}

static bool recording = false;

static void set_recording_state(bool is_recording) {
  OrderAccess::storestore();
  recording = is_recording;
}

bool JfrRecorderService::is_recording() {
  return recording;
}

void JfrRecorderService::start() {
  MutexLocker lock(JfrStream_lock);
  log_debug(jfr, system)("Request to START recording");
  assert(!is_recording(), "invariant");
  clear();
  set_recording_state(true);
  assert(is_recording(), "invariant");
  open_new_chunk();
  log_debug(jfr, system)("Recording STARTED");
}

void JfrRecorderService::clear() {
  ResourceMark rm;
  HandleMark hm;
  pre_safepoint_clear();
  invoke_safepoint_clear();
  post_safepoint_clear();
}

void JfrRecorderService::pre_safepoint_clear() {
  _string_pool.clear();
  _storage.clear();
  _stack_trace_repository.clear();
}

void JfrRecorderService::invoke_safepoint_clear() {
  JfrVMOperation<JfrRecorderService, &JfrRecorderService::safepoint_clear> safepoint_task(*this);
  VMThread::execute(&safepoint_task);
}

void JfrRecorderService::safepoint_clear() {
  assert(SafepointSynchronize::is_at_safepoint(), "invariant");
  _string_pool.clear();
  _storage.clear();
  _checkpoint_manager.shift_epoch();
  _chunkwriter.set_time_stamp();
  _stack_trace_repository.clear();
}

void JfrRecorderService::post_safepoint_clear() {
  _checkpoint_manager.clear();
}

void JfrRecorderService::open_new_chunk(bool vm_error) {
  JfrChunkRotation::on_rotation();
  const bool valid_chunk = _repository.open_chunk(vm_error);
  _storage.control().set_to_disk(valid_chunk);
  if (valid_chunk) {
    _checkpoint_manager.write_static_type_set_and_threads();
  }
}

static void stop() {
  assert(JfrRecorderService::is_recording(), "invariant");
  log_debug(jfr, system)("Recording STOPPED");
  set_recording_state(false);
  assert(!JfrRecorderService::is_recording(), "invariant");
}

void JfrRecorderService::prepare_for_vm_error_rotation() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  if (!_chunkwriter.is_valid()) {
    open_new_chunk(true);
  }
  _checkpoint_manager.register_service_thread(Thread::current());
}

void JfrRecorderService::vm_error_rotation() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  if (_chunkwriter.is_valid()) {
    Thread* const t = Thread::current();
    _storage.flush_regular_buffer(t->jfr_thread_local()->native_buffer(), t);
    _chunkwriter.mark_chunk_final();
    invoke_flush();
    _chunkwriter.set_time_stamp();
    _repository.close_chunk();
    assert(!_chunkwriter.is_valid(), "invariant");
    _repository.on_vm_error();
  }
}

void JfrRecorderService::rotate(int msgs) {
  assert(!JfrStream_lock->owned_by_self(), "invariant");
  MutexLocker lock(JfrStream_lock);
  static bool vm_error = false;
  if (msgs & MSGBIT(MSG_VM_ERROR)) {
    vm_error = true;
    prepare_for_vm_error_rotation();
  }
  if (!_storage.control().to_disk()) {
    in_memory_rotation();
  } else if (vm_error) {
    vm_error_rotation();
  } else {
    chunk_rotation();
  }
  if (msgs & (MSGBIT(MSG_STOP))) {
    stop();
  }
}

void JfrRecorderService::in_memory_rotation() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  // currently running an in-memory recording
  assert(!_storage.control().to_disk(), "invariant");
  open_new_chunk();
  if (_chunkwriter.is_valid()) {
    // dump all in-memory buffer data to the newly created chunk
    write_storage(_storage, _chunkwriter);
  }
}

void JfrRecorderService::chunk_rotation() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  finalize_current_chunk();
  open_new_chunk();
}

void JfrRecorderService::finalize_current_chunk() {
  assert(_chunkwriter.is_valid(), "invariant");
  write();
}

void JfrRecorderService::write() {
  ResourceMark rm;
  HandleMark hm;
  pre_safepoint_write();
  invoke_safepoint_write();
  post_safepoint_write();
}

void JfrRecorderService::pre_safepoint_write() {
  assert(_chunkwriter.is_valid(), "invariant");
  if (LeakProfiler::is_running()) {
    // Exclusive access to the object sampler instance.
    // The sampler is released (unlocked) later in post_safepoint_write.
    ObjectSampleCheckpoint::on_rotation(ObjectSampler::acquire(), _stack_trace_repository);
  }
  if (_string_pool.is_modified()) {
    write_stringpool(_string_pool, _chunkwriter);
  }
  write_storage(_storage, _chunkwriter);
  if (_stack_trace_repository.is_modified()) {
    write_stacktrace(_stack_trace_repository, _chunkwriter, false);
  }
}

void JfrRecorderService::invoke_safepoint_write() {
  JfrVMOperation<JfrRecorderService, &JfrRecorderService::safepoint_write> safepoint_task(*this);
  VMThread::execute(&safepoint_task);
}

void JfrRecorderService::safepoint_write() {
  assert(SafepointSynchronize::is_at_safepoint(), "invariant");
  if (_string_pool.is_modified()) {
    write_stringpool_safepoint(_string_pool, _chunkwriter);
  }
  _checkpoint_manager.on_rotation();
  _storage.write_at_safepoint();
  _checkpoint_manager.shift_epoch();
  _chunkwriter.set_time_stamp();
  write_stacktrace(_stack_trace_repository, _chunkwriter, true);
}

void JfrRecorderService::post_safepoint_write() {
  assert(_chunkwriter.is_valid(), "invariant");
  // During the safepoint tasks just completed, the system transitioned to a new epoch.
  // Type tagging is epoch relative which entails we are able to write out the
  // already tagged artifacts for the previous epoch. We can accomplish this concurrently
  // with threads now tagging artifacts in relation to the new, now updated, epoch and remain outside of a safepoint.
  _checkpoint_manager.write_type_set();
  if (LeakProfiler::is_running()) {
    // The object sampler instance was exclusively acquired and locked in pre_safepoint_write.
    // Note: There is a dependency on write_type_set() above, ensure the release is subsequent.
    ObjectSampler::release();
  }
  // serialize the metadata descriptor event and close out the chunk
  write_metadata(_chunkwriter);
  _repository.close_chunk();
}

static JfrBuffer* thread_local_buffer(Thread* t) {
  assert(t != NULL, "invariant");
  return t->jfr_thread_local()->native_buffer();
}

static void reset_buffer(JfrBuffer* buffer, Thread* t) {
  assert(buffer != NULL, "invariant");
  assert(t != NULL, "invariant");
  assert(buffer == thread_local_buffer(t), "invariant");
  buffer->set_pos(const_cast<u1*>(buffer->top()));
}

static void reset_thread_local_buffer(Thread* t) {
  reset_buffer(thread_local_buffer(t), t);
}

static void write_thread_local_buffer(JfrChunkWriter& chunkwriter, Thread* t) {
  JfrBuffer * const buffer = thread_local_buffer(t);
  assert(buffer != NULL, "invariant");
  if (!buffer->empty()) {
    chunkwriter.write_unbuffered(buffer->top(), buffer->pos() - buffer->top());
    reset_buffer(buffer, t);
  }
}

size_t JfrRecorderService::flush() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  size_t total_elements = flush_metadata(_chunkwriter);
  const size_t storage_elements = flush_storage(_storage, _chunkwriter);
  if (0 == storage_elements) {
    return total_elements;
  }
  total_elements += storage_elements;
  if (_string_pool.is_modified()) {
    total_elements += flush_stringpool(_string_pool, _chunkwriter);
  }
  if (_stack_trace_repository.is_modified()) {
    total_elements += flush_stacktrace(_stack_trace_repository, _chunkwriter);
  }
  if (_checkpoint_manager.is_type_set_required()) {
    total_elements += flush_typeset(_checkpoint_manager, _chunkwriter);
  } else if (_checkpoint_manager.is_static_type_set_required()) {
    // don't tally this, it is only in order to flush the waiting constants
    _checkpoint_manager.flush_static_type_set();
  }
  return total_elements;
}

typedef Content<EventFlush, JfrRecorderService, &JfrRecorderService::flush> FlushFunctor;
typedef WriteContent<FlushFunctor> Flush;

void JfrRecorderService::invoke_flush() {
  assert(JfrStream_lock->owned_by_self(), "invariant");
  assert(_chunkwriter.is_valid(), "invariant");
  Thread* const t = Thread::current();
  ResourceMark rm(t);
  HandleMark hm(t);
  ++flushpoint_id;
  reset_thread_local_buffer(t);
  FlushFunctor flushpoint(*this);
  Flush fl(_chunkwriter, flushpoint);
  invoke_with_flush_event(fl);
  write_thread_local_buffer(_chunkwriter, t);
  _repository.flush_chunk();
}

void JfrRecorderService::flushpoint() {
  MutexLocker lock(JfrStream_lock);
  invoke_flush();
}

void JfrRecorderService::process_full_buffers() {
  if (_chunkwriter.is_valid()) {
    _storage.write_full();
  }
}

void JfrRecorderService::scavenge() {
  _storage.scavenge();
}

void JfrRecorderService::evaluate_chunk_size_for_rotation() {
  JfrChunkRotation::evaluate(_chunkwriter);
}
