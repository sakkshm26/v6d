/** Copyright 2020-2021 Alibaba Group Holding Limited.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef MODULES_GRAPH_UTILS_TABLE_SHUFFLER_BETA_H_
#define MODULES_GRAPH_UTILS_TABLE_SHUFFLER_BETA_H_

#include <mpi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "arrow/buffer.h"
#include "arrow/io/memory.h"
#include "arrow/ipc/dictionary.h"
#include "arrow/ipc/reader.h"
#include "arrow/ipc/writer.h"
#include "arrow/type.h"
#include "arrow/util/string_view.h"
#include "boost/leaf.hpp"

#include "grape/communication/sync_comm.h"
#include "grape/utils/concurrent_queue.h"
#include "grape/worker/comm_spec.h"

#include "basic/ds/arrow_utils.h"
#include "graph/utils/error.h"

namespace grape {

inline InArchive& operator<<(InArchive& in_archive,
                             const arrow::util::string_view& val) {
  in_archive << val.length();
  in_archive.AddBytes(val.data(), val.length());
  return in_archive;
}

inline OutArchive& operator>>(OutArchive& out_archive,
                              arrow::util::string_view& val) {
  size_t length;
  out_archive >> length;
  const char* ptr = static_cast<const char*>(out_archive.GetBytes(length));
  val = arrow::util::string_view(ptr, length);
  return out_archive;
}

}  // namespace grape

namespace vineyard {

namespace beta {

inline void SendArrowBuffer(const std::shared_ptr<arrow::Buffer>& buffer,
                            int dst_worker_id, MPI_Comm comm) {
  int64_t size = buffer->size();
  MPI_Send(&size, 1, MPI_INT64_T, dst_worker_id, 0, comm);
  if (size != 0) {
    grape::send_buffer<uint8_t>(buffer->data(), size, dst_worker_id, comm, 0);
  }
}

inline void RecvArrowBuffer(std::shared_ptr<arrow::Buffer>& buffer,
                            int src_worker_id, MPI_Comm comm) {
  int64_t size;
  MPI_Recv(&size, 1, MPI_INT64_T, src_worker_id, 0, comm, MPI_STATUS_IGNORE);
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
  ARROW_CHECK_OK(
      arrow::AllocateBuffer(arrow::default_memory_pool(), size, &buffer));
#else
  ARROW_CHECK_OK_AND_ASSIGN(
      buffer, arrow::AllocateBuffer(size, arrow::default_memory_pool()));
#endif
  if (size != 0) {
    grape::recv_buffer<uint8_t>(buffer->mutable_data(), size, src_worker_id,
                                comm, 0);
  }
}

inline boost::leaf::result<void> SchemaConsistent(
    const arrow::Schema& schema, const grape::CommSpec& comm_spec) {
  std::shared_ptr<arrow::Buffer> buffer;
  arrow::Status serialized_status;
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
  arrow::ipc::DictionaryMemo out_memo;
  serialized_status = arrow::ipc::SerializeSchema(
      schema, &out_memo, arrow::default_memory_pool(), &buffer);
#elif defined(ARROW_VERSION) && ARROW_VERSION < 2000000
  arrow::ipc::DictionaryMemo out_memo;
  auto ret = arrow::ipc::SerializeSchema(schema, &out_memo,
                                         arrow::default_memory_pool());
  serialized_status = ret.status();
  if (ret.ok()) {
    buffer = std::move(ret).ValueOrDie();
  }
#else
  auto ret = arrow::ipc::SerializeSchema(schema, arrow::default_memory_pool());
  serialized_status = ret.status();
  if (ret.ok()) {
    buffer = std::move(ret).ValueOrDie();
  }
#endif
  if (!serialized_status.ok()) {
    int flag = 1;
    int sum;
    MPI_Allreduce(&flag, &sum, 1, MPI_INT, MPI_SUM, comm_spec.comm());
    RETURN_GS_ERROR(ErrorCode::kArrowError, "Serializing schema failed.");
  } else {
    int flag = 0;
    int sum;
    MPI_Allreduce(&flag, &sum, 1, MPI_INT, MPI_SUM, comm_spec.comm());
    if (sum != 0) {
      RETURN_GS_ERROR(ErrorCode::kArrowError, "Serializing schema failed.");
    }
  }

  int worker_id = comm_spec.worker_id();
  int worker_num = comm_spec.worker_num();

  std::thread send_thread([&]() {
    for (int i = 1; i < worker_num; ++i) {
      int dst_worker_id = (worker_id + i) % worker_num;
      SendArrowBuffer(buffer, dst_worker_id, comm_spec.comm());
    }
  });
  bool consistent = true;
  std::thread recv_thread([&]() {
    for (int i = 1; i < worker_num; ++i) {
      int src_worker_id = (worker_id + worker_num - i) % worker_num;
      std::shared_ptr<arrow::Buffer> got_buffer;
      RecvArrowBuffer(got_buffer, src_worker_id, comm_spec.comm());
      arrow::ipc::DictionaryMemo in_memo;
      arrow::io::BufferReader reader(got_buffer);
      std::shared_ptr<arrow::Schema> got_schema;
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
      ARROW_CHECK_OK(arrow::ipc::ReadSchema(&reader, &in_memo, &got_schema));
#else
      ARROW_CHECK_OK_AND_ASSIGN(got_schema,
                                arrow::ipc::ReadSchema(&reader, &in_memo));
#endif
      consistent &= (got_schema->Equals(schema));
    }
  });

  send_thread.join();
  recv_thread.join();

  MPI_Barrier(comm_spec.comm());

  if (!consistent) {
    RETURN_GS_ERROR(ErrorCode::kInvalidOperationError,
                    "Schemas of edge tables are not consistent.");
  }

  return {};
}

template <typename T>
inline void serialize_selected_typed_items(
    grape::InArchive& arc, std::shared_ptr<arrow::Array> array) {
  auto ptr =
      std::dynamic_pointer_cast<typename ConvertToArrowType<T>::ArrayType>(
          array)
          ->raw_values();
  for (int64_t x = 0; x < array->length(); ++x) {
    arc << ptr[x];
  }
}

template <typename T>
inline void serialize_selected_typed_items(grape::InArchive& arc,
                                           std::shared_ptr<arrow::Array> array,
                                           const std::vector<int64_t>& offset) {
  auto ptr =
      std::dynamic_pointer_cast<typename ConvertToArrowType<T>::ArrayType>(
          array)
          ->raw_values();
  for (auto x : offset) {
    arc << ptr[x];
  }
}

void serialize_string_items(grape::InArchive& arc,
                            std::shared_ptr<arrow::Array> array,
                            const std::vector<int64_t>& offset) {
  auto* ptr = std::dynamic_pointer_cast<arrow::LargeStringArray>(array).get();
  for (auto x : offset) {
    arc << ptr->GetView(x);
  }
}

void serialize_null_items(grape::InArchive& arc,
                          std::shared_ptr<arrow::Array> array,
                          const std::vector<int64_t>& offset) {
  return;
}

template <typename T>
void serialize_list_items(grape::InArchive& arc,
                          std::shared_ptr<arrow::Array> array,
                          const std::vector<int64_t>& offset) {
  auto* ptr = std::dynamic_pointer_cast<arrow::LargeListArray>(array).get();
  for (auto x : offset) {
    arrow::LargeListArray::offset_type length = ptr->value_length(x);
    arc << length;
    auto value = ptr->value_slice(x);
    serialize_selected_typed_items<T>(arc, value);
  }
}

void SerializeSelectedItems(grape::InArchive& arc,
                            std::shared_ptr<arrow::Array> array,
                            const std::vector<int64_t>& offset) {
  if (array->type()->Equals(arrow::float64())) {
    serialize_selected_typed_items<double>(arc, array, offset);
  } else if (array->type()->Equals(arrow::float32())) {
    serialize_selected_typed_items<float>(arc, array, offset);
  } else if (array->type()->Equals(arrow::int64())) {
    serialize_selected_typed_items<int64_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::int32())) {
    serialize_selected_typed_items<int32_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::uint64())) {
    serialize_selected_typed_items<uint64_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::uint32())) {
    serialize_selected_typed_items<uint32_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_utf8())) {
    serialize_string_items(arc, array, offset);
  } else if (array->type()->Equals(arrow::null())) {
    serialize_null_items(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::float64()))) {
    serialize_list_items<double>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::float32()))) {
    serialize_list_items<float>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::int64()))) {
    serialize_list_items<int64_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::int32()))) {
    serialize_list_items<int32_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::uint64()))) {
    serialize_list_items<uint64_t>(arc, array, offset);
  } else if (array->type()->Equals(arrow::large_list(arrow::uint32()))) {
    serialize_list_items<uint32_t>(arc, array, offset);
  } else {
    LOG(FATAL) << "Unsupported data type - " << array->type()->ToString();
  }
}

void SerializeSelectedRows(grape::InArchive& arc,
                           std::shared_ptr<arrow::RecordBatch> record_batch,
                           const std::vector<int64_t>& offset) {
  int col_num = record_batch->num_columns();
  arc << static_cast<int64_t>(offset.size());
  for (int col_id = 0; col_id != col_num; ++col_id) {
    SerializeSelectedItems(arc, record_batch->column(col_id), offset);
  }
}

template <typename T>
inline void deserialize_selected_typed_items(grape::OutArchive& arc,
                                             int64_t num,
                                             arrow::ArrayBuilder* builder) {
  auto casted_builder =
      dynamic_cast<typename ConvertToArrowType<T>::BuilderType*>(builder);
  T val;
  for (int64_t i = 0; i != num; ++i) {
    arc >> val;
    casted_builder->Append(val);
  }
}

inline void deserialize_string_items(grape::OutArchive& arc, int64_t num,
                                     arrow::ArrayBuilder* builder) {
  auto casted_builder = dynamic_cast<arrow::LargeStringBuilder*>(builder);
  arrow::util::string_view val;
  for (int64_t i = 0; i != num; ++i) {
    arc >> val;
    casted_builder->Append(val);
  }
}

inline void deserialize_null_items(grape::OutArchive& arc, int64_t num,
                                   arrow::ArrayBuilder* builder) {
  auto casted_builder = dynamic_cast<arrow::NullBuilder*>(builder);
  casted_builder->AppendNulls(num);
}

template <typename T>
inline void deserialize_list_items(grape::OutArchive& arc, int64_t num,
                                   arrow::ArrayBuilder* builder) {
  auto casted_builder = dynamic_cast<arrow::LargeListBuilder*>(builder);
  auto value_builder = casted_builder->value_builder();
  arrow::LargeListArray::offset_type length;
  for (int64_t i = 0; i != num; ++i) {
    arc >> length;
    deserialize_selected_typed_items<T>(arc, length, value_builder);
    casted_builder->Append(true);
  }
}

void DeserializeSelectedItems(grape::OutArchive& arc, int64_t num,
                              arrow::ArrayBuilder* builder) {
  if (builder->type()->Equals(arrow::float64())) {
    deserialize_selected_typed_items<double>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::float32())) {
    deserialize_selected_typed_items<float>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::int64())) {
    deserialize_selected_typed_items<int64_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::int32())) {
    deserialize_selected_typed_items<int32_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::uint64())) {
    deserialize_selected_typed_items<uint64_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::uint32())) {
    deserialize_selected_typed_items<uint32_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_utf8())) {
    deserialize_string_items(arc, num, builder);
  } else if (builder->type()->Equals(arrow::null())) {
    deserialize_null_items(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::float64()))) {
    deserialize_list_items<double>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::float32()))) {
    deserialize_list_items<float>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::int64()))) {
    deserialize_list_items<int64_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::int32()))) {
    deserialize_list_items<int32_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::uint64()))) {
    deserialize_list_items<uint64_t>(arc, num, builder);
  } else if (builder->type()->Equals(arrow::large_list(arrow::uint32()))) {
    deserialize_list_items<uint32_t>(arc, num, builder);
  } else {
    LOG(FATAL) << "Unsupported data type - " << builder->type()->ToString();
  }
}

void DeserializeSelectedRows(grape::OutArchive& arc,
                             std::shared_ptr<arrow::Schema> schema,
                             std::shared_ptr<arrow::RecordBatch>& batch_out) {
  int64_t row_num;
  arc >> row_num;
  std::unique_ptr<arrow::RecordBatchBuilder> builder;
  ARROW_CHECK_OK(arrow::RecordBatchBuilder::Make(
      schema, arrow::default_memory_pool(), row_num, &builder));
  int col_num = builder->num_fields();
  for (int col_id = 0; col_id != col_num; ++col_id) {
    DeserializeSelectedItems(arc, row_num, builder->GetField(col_id));
  }
  ARROW_CHECK_OK(builder->Flush(&batch_out));
}

template <typename T>
inline void select_typed_items(std::shared_ptr<arrow::Array> array,
                               arrow::ArrayBuilder* builder) {
  auto ptr =
      std::dynamic_pointer_cast<typename ConvertToArrowType<T>::ArrayType>(
          array)
          ->raw_values();
  auto casted_builder =
      dynamic_cast<typename ConvertToArrowType<T>::BuilderType*>(builder);
  casted_builder->AppendValues(ptr, array->length());
}

template <typename T>
inline void select_typed_items(std::shared_ptr<arrow::Array> array,
                               const std::vector<int64_t>& offset,
                               arrow::ArrayBuilder* builder) {
  auto ptr =
      std::dynamic_pointer_cast<typename ConvertToArrowType<T>::ArrayType>(
          array)
          ->raw_values();
  auto casted_builder =
      dynamic_cast<typename ConvertToArrowType<T>::BuilderType*>(builder);
  for (auto x : offset) {
    casted_builder->Append(ptr[x]);
  }
}

inline void select_string_items(std::shared_ptr<arrow::Array> array,
                                const std::vector<int64_t>& offset,
                                arrow::ArrayBuilder* builder) {
  auto* ptr = std::dynamic_pointer_cast<arrow::LargeStringArray>(array).get();
  auto casted_builder = dynamic_cast<arrow::LargeStringBuilder*>(builder);
  for (auto x : offset) {
    casted_builder->Append(ptr->GetView(x));
  }
}

inline void select_null_items(std::shared_ptr<arrow::Array> array,
                              const std::vector<int64_t>& offset,
                              arrow::ArrayBuilder* builder) {
  arrow::NullBuilder* casted_builder =
      dynamic_cast<arrow::NullBuilder*>(builder);
  casted_builder->AppendNulls(offset.size());
}

template <typename T>
inline void select_list_items(std::shared_ptr<arrow::Array> array,
                              const std::vector<int64_t>& offset,
                              arrow::ArrayBuilder* builder) {
  auto ptr = std::dynamic_pointer_cast<arrow::LargeListArray>(array).get();

  auto casted_builder = dynamic_cast<arrow::LargeListBuilder*>(builder);
  auto value_builder = casted_builder->value_builder();
  for (auto x : offset) {
    select_typed_items<T>(ptr->value_slice(x), value_builder);
    casted_builder->Append(true);
  }
}

inline void SelectItems(std::shared_ptr<arrow::Array> array,
                        const std::vector<int64_t> offset,
                        arrow::ArrayBuilder* builder) {
  if (array->type()->Equals(arrow::float64())) {
    select_typed_items<double>(array, offset, builder);
  } else if (array->type()->Equals(arrow::float32())) {
    select_typed_items<float>(array, offset, builder);
  } else if (array->type()->Equals(arrow::int64())) {
    select_typed_items<int64_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::int32())) {
    select_typed_items<int32_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::uint64())) {
    select_typed_items<uint64_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::uint32())) {
    select_typed_items<uint32_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_utf8())) {
    select_string_items(array, offset, builder);
  } else if (array->type()->Equals(arrow::null())) {
    select_null_items(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::float64()))) {
    select_list_items<double>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::float32()))) {
    select_list_items<float>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::int64()))) {
    select_list_items<int64_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::int32()))) {
    select_list_items<int32_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::uint64()))) {
    select_list_items<uint64_t>(array, offset, builder);
  } else if (array->type()->Equals(arrow::large_list(arrow::uint32()))) {
    select_list_items<uint32_t>(array, offset, builder);
  } else {
    LOG(FATAL) << "Unsupported data type - " << builder->type()->ToString();
  }
}

inline void SelectRows(std::shared_ptr<arrow::RecordBatch> record_batch_in,
                       const std::vector<int64_t>& offset,
                       std::shared_ptr<arrow::RecordBatch>& record_batch_out) {
  int64_t row_num = offset.size();
  std::unique_ptr<arrow::RecordBatchBuilder> builder;
  ARROW_CHECK_OK(arrow::RecordBatchBuilder::Make(record_batch_in->schema(),
                                                 arrow::default_memory_pool(),
                                                 row_num, &builder));
  int col_num = builder->num_fields();
  for (int col_id = 0; col_id != col_num; ++col_id) {
    SelectItems(record_batch_in->column(col_id), offset,
                builder->GetField(col_id));
  }
  ARROW_CHECK_OK(builder->Flush(&record_batch_out));
}

void ShuffleTableByOffsetLists(
    std::shared_ptr<arrow::Schema> schema,
    std::vector<std::shared_ptr<arrow::RecordBatch>>& record_batches_out,
    const std::vector<std::vector<std::vector<int64_t>>>& offset_lists,
    std::vector<std::shared_ptr<arrow::RecordBatch>>& record_batches_in,
    const grape::CommSpec& comm_spec) {
  int worker_id = comm_spec.worker_id();
  int worker_num = comm_spec.worker_num();
  size_t record_batches_out_num = record_batches_out.size();
#if 1
  int thread_num =
      (std::thread::hardware_concurrency() + comm_spec.local_num() - 1) /
      comm_spec.local_num();
  int deserialize_thread_num = std::max(1, (thread_num - 2) / 2);
  int serialize_thread_num =
      std::max(1, thread_num - 2 - deserialize_thread_num);
  std::vector<std::thread> serialize_threads(serialize_thread_num);
  std::vector<std::thread> deserialize_threads(deserialize_thread_num);

  grape::BlockingQueue<std::pair<grape::fid_t, grape::InArchive>> msg_out;
  grape::BlockingQueue<grape::OutArchive> msg_in;

  msg_out.SetProducerNum(serialize_thread_num);
  msg_in.SetProducerNum(1);

  int64_t record_batches_to_send = static_cast<int64_t>(record_batches_out_num);
  int64_t total_record_batches;
  MPI_Allreduce(&record_batches_to_send, &total_record_batches, 1, MPI_INT64_T,
                MPI_SUM, comm_spec.comm());
  int64_t record_batches_to_recv =
      total_record_batches - record_batches_to_send;

  std::thread send_thread([&]() {
    std::pair<grape::fid_t, grape::InArchive> item;
    while (msg_out.Get(item)) {
      int dst_worker_id = comm_spec.FragToWorker(item.first);
      auto& arc = item.second;
      grape::SendArchive(arc, dst_worker_id, comm_spec.comm());
    }
  });

  std::thread recv_thread([&]() {
    int64_t remaining_msg_num = record_batches_to_recv;
    while (remaining_msg_num != 0) {
      MPI_Status status;
      MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_spec.comm(), &status);
      grape::OutArchive arc;
      grape::RecvArchive(arc, status.MPI_SOURCE, comm_spec.comm());
      msg_in.Put(std::move(arc));
      --remaining_msg_num;
    }
    msg_in.DecProducerNum();
  });

  std::atomic<size_t> cur_batch_out(0);
  for (int i = 0; i != serialize_thread_num; ++i) {
    serialize_threads[i] = std::thread([&]() {
      while (true) {
        size_t got_batch = cur_batch_out.fetch_add(1);
        if (got_batch >= record_batches_out_num) {
          break;
        }
        auto cur_rb = record_batches_out[got_batch];
        auto& cur_offset_lists = offset_lists[got_batch];

        for (int i = 1; i != worker_num; ++i) {
          int dst_worker_id = (worker_id + i) % worker_num;
          grape::fid_t dst_fid = comm_spec.WorkerToFrag(dst_worker_id);
          std::pair<grape::fid_t, grape::InArchive> item;
          item.first = dst_fid;
          SerializeSelectedRows(item.second, cur_rb, cur_offset_lists[dst_fid]);
          msg_out.Put(std::move(item));
        }
      }
      msg_out.DecProducerNum();
    });
  }

  std::atomic<int64_t> cur_batch_in(0);
  record_batches_in.resize(record_batches_to_recv);
  for (int i = 0; i != deserialize_thread_num; ++i) {
    deserialize_threads[i] = std::thread([&]() {
      grape::OutArchive arc;
      while (msg_in.Get(arc)) {
        int64_t got_batch = cur_batch_in.fetch_add(1);
        DeserializeSelectedRows(arc, schema, record_batches_in[got_batch]);
      }
    });
  }

  send_thread.join();
  recv_thread.join();
  for (auto& thrd : serialize_threads) {
    thrd.join();
  }
  for (auto& thrd : deserialize_threads) {
    thrd.join();
  }

  for (size_t rb_i = 0; rb_i != record_batches_out_num; ++rb_i) {
    std::shared_ptr<arrow::RecordBatch> rb;
    SelectRows(record_batches_out[rb_i], offset_lists[rb_i][comm_spec.fid()],
               rb);
    record_batches_in.emplace_back(std::move(rb));
  }
#else
  std::thread send_thread([&]() {
    for (int i = 1; i != worker_num; ++i) {
      int dst_worker_id = (worker_id + worker_num - i) % worker_num;
      grape::fid_t dst_fid = comm_spec.WorkerToFrag(dst_worker_id);
      grape::InArchive arc;
      arc << record_batches_out_num;
      for (size_t rb_i = 0; rb_i != record_batches_out_num; ++rb_i) {
        SerializeSelectedRows(arc, record_batches_out[rb_i],
                              offset_lists[rb_i][dst_fid]);
      }
      grape::SendArchive(arc, dst_worker_id, comm_spec.comm());
    }
  });
  std::thread recv_thread([&]() {
    for (int i = 1; i != worker_num; ++i) {
      int src_worker_id = (worker_id + i) % worker_num;
      grape::OutArchive arc;
      grape::RecvArchive(arc, src_worker_id, comm_spec.comm());
      size_t rb_num;
      arc >> rb_num;
      for (size_t rb_i = 0; rb_i != rb_num; ++rb_i) {
        std::shared_ptr<arrow::RecordBatch> rb;
        DeserializeSelectedRows(arc, schema, rb);
        record_batches_in.emplace_back(std::move(rb));
      }
    }

    for (size_t rb_i = 0; rb_i != record_batches_out_num; ++rb_i) {
      std::shared_ptr<arrow::RecordBatch> rb;
      SelectRows(record_batches_out[rb_i], offset_lists[rb_i][comm_spec.fid()],
                 rb);
      record_batches_in.emplace_back(std::move(rb));
    }
  });

  send_thread.join();
  recv_thread.join();
#endif

  MPI_Barrier(comm_spec.comm());
}

template <typename VID_TYPE>
boost::leaf::result<std::shared_ptr<arrow::Table>> ShufflePropertyEdgeTable(
    const grape::CommSpec& comm_spec, IdParser<VID_TYPE>& id_parser,
    int src_col_id, int dst_col_id, std::shared_ptr<arrow::Table>& table_in) {
  BOOST_LEAF_CHECK(SchemaConsistent(*table_in->schema(), comm_spec));

  std::vector<std::shared_ptr<arrow::RecordBatch>> record_batches;
  VY_OK_OR_RAISE(TableToRecordBatches(table_in, &record_batches));

  size_t record_batch_num = record_batches.size();
  // record_batch_num, fragment_num, row_ids
  std::vector<std::vector<std::vector<int64_t>>> offset_lists(record_batch_num);

  int thread_num =
      (std::thread::hardware_concurrency() + comm_spec.local_num() - 1) /
      comm_spec.local_num();
  std::vector<std::thread> scan_threads(thread_num);
  std::atomic<size_t> cur(0);

  for (int i = 0; i < thread_num; ++i) {
    scan_threads[i] = std::thread([&]() {
      while (true) {
        size_t got = cur.fetch_add(1);
        if (got >= record_batch_num) {
          break;
        }

        auto& offset_list = offset_lists[got];
        offset_list.resize(comm_spec.fnum());
        auto cur_batch = record_batches[got];
        int64_t row_num = cur_batch->num_rows();

        const VID_TYPE* src_col =
            std::dynamic_pointer_cast<
                typename ConvertToArrowType<VID_TYPE>::ArrayType>(
                cur_batch->column(src_col_id))
                ->raw_values();
        const VID_TYPE* dst_col =
            std::dynamic_pointer_cast<
                typename ConvertToArrowType<VID_TYPE>::ArrayType>(
                cur_batch->column(dst_col_id))
                ->raw_values();

        for (int64_t row_id = 0; row_id < row_num; ++row_id) {
          VID_TYPE src_gid = src_col[row_id];
          VID_TYPE dst_gid = dst_col[row_id];

          grape::fid_t src_fid = id_parser.GetFid(src_gid);
          grape::fid_t dst_fid = id_parser.GetFid(dst_gid);

          offset_list[src_fid].push_back(row_id);
          if (src_fid != dst_fid) {
            offset_list[dst_fid].push_back(row_id);
          }
        }
      }
    });
  }
  for (auto& thrd : scan_threads) {
    thrd.join();
  }

  std::vector<std::shared_ptr<arrow::RecordBatch>> batches_in;

  ShuffleTableByOffsetLists(table_in->schema(), record_batches, offset_lists,
                            batches_in, comm_spec);

  batches_in.erase(std::remove_if(batches_in.begin(), batches_in.end(),
                                  [](std::shared_ptr<arrow::RecordBatch>& e) {
                                    return e->num_rows() == 0;
                                  }),
                   batches_in.end());

  // N.B.: we need an empty table for non-existing labels.
  std::shared_ptr<arrow::Table> table_out;
  if (batches_in.empty()) {
    VY_OK_OR_RAISE(EmptyTableBuilder::Build(table_in->schema(), table_out));
  } else {
    std::shared_ptr<arrow::Table> tmp_table;
    VY_OK_OR_RAISE(RecordBatchesToTable(batches_in, &tmp_table));
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    ARROW_OK_OR_RAISE(
        tmp_table->CombineChunks(arrow::default_memory_pool(), &table_out));
#else
    ARROW_OK_ASSIGN_OR_RAISE(
        table_out, tmp_table->CombineChunks(arrow::default_memory_pool()));
#endif
  }
  return table_out;
}

template <typename PARTITIONER_T>
boost::leaf::result<std::shared_ptr<arrow::Table>> ShufflePropertyVertexTable(
    const grape::CommSpec& comm_spec, const PARTITIONER_T& partitioner,
    std::shared_ptr<arrow::Table>& table_in) {
  using oid_t = typename PARTITIONER_T::oid_t;
  using internal_oid_t = typename InternalType<oid_t>::type;
  using oid_array_type = typename ConvertToArrowType<oid_t>::ArrayType;

  BOOST_LEAF_CHECK(SchemaConsistent(*table_in->schema(), comm_spec));

  std::vector<std::shared_ptr<arrow::RecordBatch>> record_batches;
  VY_OK_OR_RAISE(TableToRecordBatches(table_in, &record_batches));

  size_t record_batch_num = record_batches.size();
  std::vector<std::vector<std::vector<int64_t>>> offset_lists(record_batch_num);

  int thread_num =
      (std::thread::hardware_concurrency() + comm_spec.local_num() - 1) /
      comm_spec.local_num();
  std::vector<std::thread> scan_threads(thread_num);
  std::atomic<size_t> cur(0);

  for (int i = 0; i < thread_num; ++i) {
    scan_threads[i] = std::thread([&]() {
      while (true) {
        size_t got = cur.fetch_add(1);
        if (got >= record_batch_num) {
          break;
        }

        auto& offset_list = offset_lists[got];
        offset_list.resize(comm_spec.fnum());
        auto cur_batch = record_batches[got];
        int64_t row_num = cur_batch->num_rows();

        std::shared_ptr<oid_array_type> id_col =
            std::dynamic_pointer_cast<oid_array_type>(cur_batch->column(0));

        for (int64_t row_id = 0; row_id < row_num; ++row_id) {
          internal_oid_t rs = id_col->GetView(row_id);
          grape::fid_t fid = partitioner.GetPartitionId(oid_t(rs));
          offset_list[fid].push_back(row_id);
        }
      }
    });
  }
  for (auto& thrd : scan_threads) {
    thrd.join();
  }

  std::vector<std::shared_ptr<arrow::RecordBatch>> batches_in;

  ShuffleTableByOffsetLists(table_in->schema(), record_batches, offset_lists,
                            batches_in, comm_spec);

  batches_in.erase(std::remove_if(batches_in.begin(), batches_in.end(),
                                  [](std::shared_ptr<arrow::RecordBatch>& e) {
                                    return e->num_rows() == 0;
                                  }),
                   batches_in.end());

  std::shared_ptr<arrow::Table> table_out;
  if (batches_in.empty()) {
    VY_OK_OR_RAISE(EmptyTableBuilder::Build(table_in->schema(), table_out));
  } else {
    std::shared_ptr<arrow::Table> tmp_table;
    VY_OK_OR_RAISE(RecordBatchesToTable(batches_in, &tmp_table));
#if defined(ARROW_VERSION) && ARROW_VERSION < 17000
    ARROW_OK_OR_RAISE(
        tmp_table->CombineChunks(arrow::default_memory_pool(), &table_out));
#else
    ARROW_OK_ASSIGN_OR_RAISE(
        table_out, tmp_table->CombineChunks(arrow::default_memory_pool()));
#endif
  }
  return table_out;
}

}  // namespace beta

}  // namespace vineyard

#endif  // MODULES_GRAPH_UTILS_TABLE_SHUFFLER_BETA_H_
